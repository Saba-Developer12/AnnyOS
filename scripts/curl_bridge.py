#!/usr/bin/env python3
"""Serial bridge from the AnnyOS QEMU/VirtualBox guest to upstream host curl."""

import argparse
import os
import selectors
import shlex
import socketserver
import subprocess
import sys
import time
from urllib.parse import urlsplit

MAX_REQUEST = 512
MAX_RESPONSE = 8192
MAX_DIAGNOSTIC = 2048
MAX_CURL_SECONDS = 20


def valid_url(value):
    if not value or len(value) > 255 or any(ch.isspace() for ch in value):
        return False
    try:
        parsed = urlsplit(value)
        return parsed.scheme.lower() in ("http", "https") and bool(parsed.hostname)
    except ValueError:
        return False


def parse_curl_arguments(text):
    """Accept a small safe curl option set; file output is always sent to guest."""
    try:
        tokens = shlex.split(text, posix=True)
    except ValueError:
        return None
    if not tokens:
        return None

    curl_options = []
    url = None
    output_seen = False
    remote_name_seen = False
    index = 0
    while index < len(tokens):
        token = tokens[index]
        index += 1
        if token in ("-L", "--location"):
            curl_options.append("--location")
        elif token in ("-v", "--verbose"):
            curl_options.append("--verbose")
        elif token in ("-s", "--silent"):
            curl_options.append("--silent")
        elif token in ("-k", "--insecure"):
            curl_options.append("--insecure")
        elif token in ("-X", "--request", "-d", "--data", "-H", "--header", "-u", "--user", "-o", "--output"):
            if index >= len(tokens):
                return None
            value = tokens[index]
            index += 1
            if any(ord(ch) < 32 or ord(ch) > 126 for ch in value):
                return None
            if token in ("-o", "--output"):
                if output_seen or remote_name_seen:
                    return None
                if not value:
                    return None
                output_seen = True
                if value != "-" and ("/" in value or "\\" in value):
                    return None
                continue
            if token in ("-X", "--request"):
                if not value:
                    return None
                curl_options.extend(("--request", value))
            elif token in ("-d", "--data"):
                curl_options.extend(("--data", value))
            elif token in ("-H", "--header"):
                if "\r" in value or "\n" in value:
                    return None
                curl_options.extend(("--header", value))
            else:
                curl_options.extend(("--user", value))
        elif token in ("-O", "--remote-name"):
            if remote_name_seen or output_seen:
                return None
            remote_name_seen = True
        elif token.startswith("-"):
            return None
        elif valid_url(token) and url is None:
            url = token
        else:
            return None

    if url is None:
        return None
    return curl_options, url


class CurlHandler(socketserver.StreamRequestHandler):
    def handle(self):
        while True:
            try:
                raw = self.rfile.readline(MAX_REQUEST + 1)
                if not raw:
                    return
                if len(raw) > MAX_REQUEST or not raw.endswith(b"\n"):
                    self.reply(2, b"Invalid or oversized curl request.\n", b"", False)
                    return
                request = raw.decode("ascii").rstrip("\r\n")
                if not request.startswith("CURL "):
                    self.reply(2, b"Invalid bridge command.\n", b"", False)
                    continue
                parsed = parse_curl_arguments(request[5:])
                if parsed is None:
                    self.reply(2, b"Unsupported curl arguments or invalid HTTP(S) URL.\n", b"", False)
                    continue
                options, url = parsed
                code, body, diagnostic, truncated = self.run_curl(options, url)
                self.reply(code, body, diagnostic, truncated)
            except (BrokenPipeError, ConnectionError, OSError, UnicodeDecodeError):
                return

    def run_curl(self, options, url):
        command = [
            self.server.curl_path,
            "--no-progress-meter", "--show-error", "--fail-with-body",
            "--max-redirs", "5", "--connect-timeout", "5",
            "--max-time", str(MAX_CURL_SECONDS),
            "--max-filesize", str(MAX_RESPONSE),
            "--proto", "=http,https", "--proto-redir", "=http,https",
            "--output", "-",
        ] + options + [url]
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                close_fds=True,
            )
            selector = selectors.DefaultSelector()
            streams = ((process.stdout, "body"), (process.stderr, "diagnostic"))
            body = bytearray()
            diagnostic = bytearray()
            truncated = False
            for stream, kind in streams:
                os.set_blocking(stream.fileno(), False)
                selector.register(stream, selectors.EVENT_READ, kind)

            deadline = time.monotonic() + MAX_CURL_SECONDS + 2
            timed_out = False
            while selector.get_map():
                if time.monotonic() >= deadline:
                    timed_out = True
                    process.kill()
                    deadline = time.monotonic() + 2
                events = selector.select(0.1)
                for key, _ in events:
                    try:
                        chunk = os.read(key.fileobj.fileno(), 4096)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        selector.unregister(key.fileobj)
                        key.fileobj.close()
                        continue
                    destination = body if key.data == "body" else diagnostic
                    limit = MAX_RESPONSE if key.data == "body" else MAX_DIAGNOSTIC
                    remaining = limit - len(destination)
                    if remaining > 0:
                        destination.extend(chunk[:remaining])
                    if len(chunk) > remaining:
                        truncated = True
                        if key.data == "body" and process.poll() is None:
                            process.kill()

            selector.close()
            try:
                code = process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                code = 28
                timed_out = True
            if timed_out:
                code = 28
                if not diagnostic:
                    diagnostic.extend(b"curl request timed out\n")
            elif truncated and len(body) >= MAX_RESPONSE:
                code = 63  # CURLE_FILESIZE_EXCEEDED
            if code != 0 and not diagnostic:
                diagnostic.extend(("curl exited with code %d\n" % code).encode("ascii"))
            return int(code) & 0xFF, bytes(body), bytes(diagnostic), truncated
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            message = ("curl bridge error: %s\n" % error).encode("utf-8", "replace")
            return 127, b"", message[:MAX_DIAGNOSTIC], False

    def reply(self, code, body, diagnostic, truncated):
        body = body[:MAX_RESPONSE]
        diagnostic = diagnostic[:MAX_DIAGNOSTIC]
        header = "R %d %d %d %d\n" % (
            int(code) & 0xFF, len(body), len(diagnostic), 1 if truncated else 0
        )
        try:
            self.wfile.write(header.encode("ascii"))
            self.wfile.write(diagnostic)
            self.wfile.write(body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionError):
            return


class CurlBridgeServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, handler, curl_path):
        self.curl_path = curl_path
        super().__init__(address, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=43210)
    parser.add_argument("--curl", required=True, help="path to the upstream curl CLI")
    options = parser.parse_args()
    curl_path = os.path.abspath(options.curl)
    if not os.path.isfile(curl_path) or not os.access(curl_path, os.X_OK):
        parser.error("upstream curl executable is missing or not executable: " + curl_path)
    if options.port < 1024 or options.port > 65535:
        parser.error("port must be between 1024 and 65535")

    try:
        with CurlBridgeServer(("127.0.0.1", options.port), CurlHandler, curl_path) as server:
            print("AnnyOS curl host bridge listening on 127.0.0.1:%d" % options.port, flush=True)
            server.serve_forever(poll_interval=0.25)
    except OSError as error:
        print("curl bridge: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
