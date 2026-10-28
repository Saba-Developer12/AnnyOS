# Upstream curl in the AnnyOS shell

The project keeps the upstream curl source checkout in `curl/`. The AnnyOS
shell command is dispatched through `apps/CURL.aadl`, compiled as `CURL.AP`.
That AP v1 bytecode wrapper calls the kernel's argument-aware `CURL` API
service, which forwards the request through the serial bridge to the upstream
curl CLI built for the development host. The AP wrapper is native to the
AnnyOS application format; the curl executable itself runs on the host.

## Use the shell command in QEMU

Run QEMU through the project launcher:

```sh
make run
```

The launcher builds `build/bin/curl` from the cloned upstream source if it is
missing, starts `scripts/curl_bridge.py` on localhost, and connects it to the
guest COM2 UART. The shell supports these options:

```text
C:\> curl [-X METHOD] [-d DATA] [-H HEADER] [-u USER:PASS] [-L] [-v] [-s] [-k] URL
C:\> curl -o PAGE.HTM URL
C:\> curl -O URL
```

`-X` selects the request method, `-d` sends request data, `-H` adds a header,
`-u` supplies basic authentication credentials, `-L` follows redirects, `-v`
prints curl diagnostics, `-s` suppresses diagnostics, and `-k` skips TLS
certificate validation. `-o` saves the body in the current guest FAT32
directory; `-O` saves it using the URL's final path component. Output names
must fit FAT32 short-name format (8.3). `-o -` prints the body in the shell.

The host curl output is returned to the guest. Requests are limited to
HTTP/HTTPS, 20 seconds, and 8 KiB of response data. Non-ASCII and binary bytes
are rendered as `?` when printed; file downloads preserve their original bytes.
Long text responses are trimmed to the shell's visible history.
`ANNYOS_CURL_BRIDGE_PORT` can change the default bridge port (`43210`) when
running from WSL/Linux.

For the registered VirtualBox VM named `AnnyOS`, first power it off, then run
this on Windows:

```powershell
VBoxManage.exe modifyvm "AnnyOS" --uart2 0x2f8 3 --uartmode2 tcpclient 127.0.0.1:43210
```

Start the bridge before powering on the VM. VirtualBox's second UART then
connects COM2 to the helper, which is the guest interface used by this shell
command. The installed VirtualBox VM currently has UART2 disabled, so this
setting is needed before testing `curl` there.

`make curl` still builds the actual upstream host CLI by itself. The guest
command depends on the QEMU launcher and is unavailable when AnnyOS boots
without the host bridge, including bare-metal use. A native curl port would
require a guest socket ABI and a freestanding libcurl build; the current kernel
uses lwIP in single-threaded polling mode and does not provide POSIX sockets.
HTTPS in the QEMU shell is handled by the host curl build and its OpenSSL
backend, not by the guest kernel.

The bridge binds only to `127.0.0.1`; it is a local development aid, not a
network service for remote guests.
