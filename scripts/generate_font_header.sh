#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT/build"
c++ -std=c++17 "$ROOT/tools/ttf_to_header.cpp" \
  $(pkg-config --cflags --libs freetype2) -o "$ROOT/build/ttf_to_header"
"$ROOT/build/ttf_to_header" \
  "$ROOT/font/segoe-ui/Segoe UI.ttf" \
  "$ROOT/font/segoe-ui/Segoe UI Bold.ttf" \
  "$ROOT/font/segoe-ui/Segoe UI Italic.ttf" \
  "$ROOT/font/segoe-ui/Segoe UI Bold Italic.ttf" \
  "$ROOT/include/annyos/font_data.hpp"
printf 'Generated %s\n' "$ROOT/include/annyos/font_data.hpp"
