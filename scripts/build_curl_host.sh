#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build/curl-host"

if ! command -v cmake >/dev/null 2>&1; then
  printf 'CMake is required. Run this target from WSL/Linux with CMake 3.18 or newer.\n' >&2
  exit 1
fi

cmake -S "$ROOT/curl" -B "$BUILD_DIR" \
  -DBUILD_CURL_EXE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_STATIC_LIBS=ON \
  -DBUILD_TESTING=OFF \
  -DBUILD_LIBCURL_DOCS=OFF \
  -DBUILD_MISC_DOCS=OFF \
  -DENABLE_CURL_MANUAL=OFF \
  -DCURL_ENABLE_SSL=ON \
  -DCURL_USE_OPENSSL=ON \
  -DCURL_USE_LIBPSL=OFF \
  -DCURL_USE_LIBSSH2=OFF \
  -DUSE_LIBIDN2=OFF \
  -DUSE_NGHTTP2=OFF \
  -DCURL_ZLIB=OFF \
  -DCURL_BROTLI=OFF \
  -DCURL_ZSTD=OFF \
  -DCURL_DISABLE_ALTSVC=ON \
  -DCURL_DISABLE_COOKIES=ON \
  -DCURL_DISABLE_DICT=ON \
  -DCURL_DISABLE_FILE=ON \
  -DCURL_DISABLE_FTP=ON \
  -DCURL_DISABLE_GOPHER=ON \
  -DCURL_DISABLE_HSTS=ON \
  -DCURL_DISABLE_IPFS=ON \
  -DCURL_DISABLE_IMAP=ON \
  -DCURL_DISABLE_LDAP=ON \
  -DCURL_DISABLE_MIME=ON \
  -DCURL_DISABLE_MQTT=ON \
  -DCURL_DISABLE_POP3=ON \
  -DCURL_DISABLE_PROXY=ON \
  -DCURL_DISABLE_RTSP=ON \
  -DCURL_DISABLE_SMTP=ON \
  -DCURL_DISABLE_TELNET=ON \
  -DCURL_DISABLE_TFTP=ON \
  -DCURL_DISABLE_WEBSOCKETS=ON

cmake --build "$BUILD_DIR" --config Release
mkdir -p "$ROOT/build/bin"
cp "$BUILD_DIR/src/curl" "$ROOT/build/bin/curl"
chmod +x "$ROOT/build/bin/curl"
printf 'Built upstream host curl CLI and libcurl under: %s\n' "$BUILD_DIR"
printf 'Run with: %s/build/bin/curl <URL>\n' "$ROOT"
printf 'These artifacts use the host runtime and are not AnnyOS binaries.\n'
