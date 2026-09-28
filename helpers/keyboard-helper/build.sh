#!/usr/bin/env bash
# Builds the arm64 replacement for Typeless' libKeyboardHelper.so.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${OUT_DIR:-$here/../out/keyboard-helper/build/linux/arm64}"
mkdir -p "$out"
CXX="${CXX:-g++}"
"$CXX" -std=c++17 -O2 -g -fPIC -shared -pthread \
  -fvisibility=hidden -fvisibility-inlines-hidden \
  -Wall -Wextra -Wno-unused-parameter \
  -Wl,-z,max-page-size=65536 -Wl,-z,relro,-z,now -Wl,--as-needed \
  -Wl,-soname,libKeyboardHelper.so -Wl,--version-script="$here/exports.map" \
  -o "$out/libKeyboardHelper.so" "$here/keyboard_helper.cpp"
echo "built $out/libKeyboardHelper.so"
