#!/usr/bin/env bash
# Builds the arm64 typeless-atspi-helper and copies it next to every lib
# that spawns it. Usage: ./build.sh [test]
#   test  - also build test/a11y_client_test (client-module smoke test)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_ROOT="$(cd "$HERE/.." && pwd)/out"
ARCH_DIR="build/linux/arm64"
OUT_DIR="$OUT_ROOT/linux-shared/$ARCH_DIR"
OBJ_DIR="$HERE/.obj"

CC="${CC:-gcc}"
CXX="${CXX:-g++}"
COMMON_FLAGS=(-O2 -g -fPIE -Wall -Wextra -Wno-unused-parameter -fstack-protector-strong
              -D_FORTIFY_SOURCE=2 -fno-omit-frame-pointer)
PKGS=(atspi-2 gio-2.0 gobject-2.0 glib-2.0 dbus-1)
PKG_CFLAGS=($(pkg-config --cflags "${PKGS[@]}"))
PKG_LIBS=($(pkg-config --libs "${PKGS[@]}"))

mkdir -p "$OUT_DIR" "$OBJ_DIR"

HELPER_CXX=(a11y_helper_main.cpp a11y_handlers.cpp atspi_accessible.cpp a11y_ipc.cpp)
HELPER_C=(native_child_guard.c)
OBJS=()
for src in "${HELPER_CXX[@]}"; do
    obj="$OBJ_DIR/${src%.cpp}.o"
    "$CXX" -std=c++17 "${COMMON_FLAGS[@]}" "${PKG_CFLAGS[@]}" -c "$HERE/$src" -o "$obj"
    OBJS+=("$obj")
done
for src in "${HELPER_C[@]}"; do
    obj="$OBJ_DIR/${src%.c}.o"
    "$CC" -std=gnu11 "${COMMON_FLAGS[@]}" -c "$HERE/$src" -o "$obj"
    OBJS+=("$obj")
done

"$CXX" -pie -Wl,-z,relro,-z,now -Wl,--as-needed -o "$OUT_DIR/typeless-atspi-helper" \
    "${OBJS[@]}" "${PKG_LIBS[@]}" -ldl -lpthread
echo "built $OUT_DIR/typeless-atspi-helper"

for mod in input-helper context-helper util-helper; do
    dest="$OUT_ROOT/$mod/$ARCH_DIR"
    mkdir -p "$dest"
    install -m 0755 "$OUT_DIR/typeless-atspi-helper" "$dest/typeless-atspi-helper"
    echo "copied -> $dest/typeless-atspi-helper"
done

if [[ "${1:-}" == "test" ]]; then
    TEST_DIR="$HERE/test/build"
    mkdir -p "$TEST_DIR"
    # The client module is what libInputHelper.so etc. compile in.
    CLIENT_SRCS=(a11y_client.cpp native_child.cpp a11y_ipc.cpp)
    "$CXX" -std=c++17 "${COMMON_FLAGS[@]}" -fvisibility=hidden -I"$HERE" \
        "$HERE/test/a11y_client_test.cpp" "${CLIENT_SRCS[@]/#/$HERE/}" \
        -x c "$HERE/native_child_guard.c" -x none \
        -o "$TEST_DIR/a11y_client_test" -ldl -lpthread
    echo "built $TEST_DIR/a11y_client_test"
fi
