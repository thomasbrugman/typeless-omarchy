#!/usr/bin/env bash
# Build the arm64 clean-room libInputHelper.so + typeless-clipboard.
#
# Output: helpers/out/input-helper/build/linux/arm64/
#   libInputHelper.so      koffi-loaded library (same C ABI as the x64 original)
#   typeless-clipboard     ext-data-control clipboard tool spawned by the library
#   typeless-atspi-helper  AT-SPI helper (built by ../linux-shared/build.sh when present)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SHARED="$ROOT/helpers/linux-shared"
OUT="$ROOT/helpers/out/input-helper/build/linux/arm64"
GEN="$HERE/build/gen"
OBJ="$HERE/build/obj"
mkdir -p "$OUT" "$GEN" "$OBJ"

CC="${CC:-gcc}"
CXX="${CXX:-g++}"
COMMON_FLAGS=(-O2 -g -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wall -Wextra -Wno-unused-parameter)
# 16K-page kernels need >=16K segment alignment; 64K covers every aarch64 kernel.
LINK_FLAGS=(-Wl,-z,max-page-size=65536 -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now -Wl,--as-needed)

# ---- typeless-clipboard (C, libwayland-client) ----
PROTO_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/staging/ext-data-control/ext-data-control-v1.xml"
wayland-scanner client-header "$PROTO_XML" "$GEN/ext-data-control-v1-client-protocol.h"
wayland-scanner private-code "$PROTO_XML" "$GEN/ext-data-control-v1-protocol.c"
"$CC" "${COMMON_FLAGS[@]}" -std=gnu11 -I"$GEN" \
    "$HERE/clipboard-tool/typeless_clipboard.c" "$GEN/ext-data-control-v1-protocol.c" \
    -o "$OUT/typeless-clipboard" "${LINK_FLAGS[@]}" $(pkg-config --libs wayland-client)

# ---- AT-SPI helper + shared client (helpers/linux-shared) ----
A11Y_SRCS=()
A11Y_CFLAGS=()
A11Y_LIBS=()
if [[ -f "$SHARED/a11y_client.cpp" && -f "$HERE/src/a11y_bridge.cpp" ]]; then
    if [[ -x "$SHARED/build.sh" && -z "${SKIP_SHARED_BUILD:-}" ]]; then
        "$SHARED/build.sh"
    fi
    # shellcheck source=/dev/null
    source "$HERE/a11y_client_sources.sh"   # sets A11Y_SRCS / A11Y_CFLAGS / A11Y_LIBS
    A11Y_SRCS+=("$HERE/src/a11y_bridge.cpp")
else
    echo "linux-shared AT-SPI client not found; building with the stub bridge" >&2
    A11Y_SRCS=("$HERE/src/a11y_bridge_stub.cpp")
fi

# ---- libInputHelper.so ----
SRCS=(
    "$HERE/src/common.cpp"
    "$HERE/src/keyboard_inject.cpp"
    "$HERE/src/focused_window.cpp"
    "$HERE/src/clipboard.cpp"
    "$HERE/src/async_executor.cpp"
    "$HERE/src/accessibility_reader.cpp"
    "$HERE/src/input_helper.cpp"
    "${A11Y_SRCS[@]}"
)
"$CXX" "${COMMON_FLAGS[@]}" -std=gnu++17 -fvisibility=hidden -fvisibility-inlines-hidden \
    -I"$HERE/src" "${A11Y_CFLAGS[@]}" -shared -o "$OUT/libInputHelper.so" "${SRCS[@]}" \
    -Wl,--version-script="$HERE/exports.map" -Wl,-soname,libInputHelper.so \
    -static-libstdc++ -static-libgcc "${LINK_FLAGS[@]}" "${A11Y_LIBS[@]}" -lpthread -ldl

echo "Built:"
ls -l "$OUT"
