#!/usr/bin/env bash
# Builds the arm64 reimplementation of libUtilHelper.so.
# Output: ../out/util-helper/build/linux/arm64/libUtilHelper.so
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${OUT_DIR:-$HERE/../out/util-helper/build/linux/arm64}"
CXX="${CXX:-g++}"
mkdir -p "$OUT_DIR"

PKGS="libpulse libsecret-1 glib-2.0 gio-2.0"
CFLAGS_PKG="$(pkg-config --cflags $PKGS)"
LIBS_PKG="$(pkg-config --libs $PKGS)"

# shellcheck disable=SC2086
"$CXX" -std=gnu++17 -O2 -g -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden \
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
  $CFLAGS_PKG \
  "$HERE"/src/*.cpp \
  -o "$OUT_DIR/libUtilHelper.so" \
  -Wl,-soname,libUtilHelper.so -Wl,-z,defs -Wl,-z,now -Wl,-z,relro -Wl,--as-needed \
  -Wl,-rpath,'$ORIGIN' \
  $LIBS_PKG -pthread

echo "built $OUT_DIR/libUtilHelper.so"
nm -D --defined-only "$OUT_DIR/libUtilHelper.so" | awk '$2=="T"{print "  export", $3}'
