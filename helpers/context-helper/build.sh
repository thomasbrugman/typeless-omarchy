#!/usr/bin/env bash
# Builds the arm64 reimplementation of libContextHelper.so.
# Output: ../out/context-helper/build/linux/arm64/libContextHelper.so
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/../out/context-helper/build/linux/arm64"
mkdir -p "$out"

CC="${CC:-cc}"
dbus_cflags="$(pkg-config --cflags dbus-1)"
dbus_libs="$(pkg-config --libs dbus-1)"

# -z max-page-size=65536: loadable on 4K/16K/64K page kernels.
"$CC" -std=gnu11 -O2 -g -fPIC -shared \
    -fvisibility=hidden -fno-strict-aliasing -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    $dbus_cflags \
    -Wl,-soname,libContextHelper.so -Wl,-z,max-page-size=65536 -Wl,-z,relro,-z,now -Wl,--no-undefined \
    -Wl,--as-needed \
    -o "$out/libContextHelper.so" \
    "$here"/src/util.c "$here"/src/json.c "$here"/src/window.c "$here"/src/a11y.c "$here"/src/context_helper.c \
    $dbus_libs -lpthread -ldl

echo "built $out/libContextHelper.so"
nm -D --defined-only "$out/libContextHelper.so" | awk '{print "  " $3}'
