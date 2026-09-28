#!/usr/bin/env bash
# Build a self-contained arm64 (aarch64) libopusenc.so for Typeless.
#
#   libopus 1.5.2  (static, PIC, float, NEON intrinsics)  -- same version as the x64 original
#   libopusenc 0.2.1 (static, PIC)                         -- same version as the x64 original
#   src/opusenc_unified.c  -- re-implementation of the app's "libopusenc-unified 1.0.0" layer
#                            (opus_convert_buffer / opus_convert_advanced / ...)
#
# All three are linked (--whole-archive) into one shared object with SONAME
# "libopusenc.so" whose only NEEDED entries are libm/libc.
#
# Usage:  ./build.sh [--clean]
#   env: OUT_DIR (default ../out/libopusenc/build/linux/arm64), JOBS (default nproc)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${OUT_DIR:-$HERE/../out/libopusenc/build/linux/arm64}"
JOBS="${JOBS:-$(nproc)}"
WORK="$HERE/work"

OPUS_VER=1.5.2
OPUS_TGZ="opus-$OPUS_VER.tar.gz"
OPUS_URL="https://downloads.xiph.org/releases/opus/$OPUS_TGZ"
OPUS_SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1

OPE_VER=0.2.1
OPE_TGZ="libopusenc-$OPE_VER.tar.gz"
OPE_URL="https://downloads.xiph.org/releases/opus/$OPE_TGZ"
OPE_SHA256=8298db61a8d3d63e41c1a80705baa8ce9ff3f50452ea7ec1c19a564fe106cbb9
# (checksums from https://downloads.xiph.org/releases/opus/SHA256SUMS.txt)

if [[ "${1:-}" == "--clean" ]]; then
  rm -rf "$WORK"
fi

fetch() { # file url sha256
  local f="$HERE/$1"
  if [[ ! -f "$f" ]]; then
    echo ">> downloading $2"
    curl -fL --retry 3 -o "$f.part" "$2"
    mv "$f.part" "$f"
  fi
  echo "$3  $f" | sha256sum -c -
}

fetch "$OPUS_TGZ" "$OPUS_URL" "$OPUS_SHA256"
fetch "$OPE_TGZ" "$OPE_URL" "$OPE_SHA256"

mkdir -p "$WORK"
PREFIX="$WORK/prefix"
# -std=gnu17 avoids C23 __isoc23_strtol (glibc 2.38+); -fno-math-errno lets sqrtf inline
# (avoids sqrtf@GLIBC_2.43), keeping the glibc requirement low for portability.
COMMON_CFLAGS="-O2 -fPIC -g0 -std=gnu17 -fno-math-errno"

# The upstream autotools builds add -fvisibility=hidden. The x64 original exports every
# non-static symbol (it was evidently built without hidden visibility), so we strip that
# flag from the generated Makefiles to mirror its export set.
unhide() { sed -i 's/-fvisibility=hidden//g' Makefile; }

# ---- libopus (static, PIC) ----
if [[ ! -f "$PREFIX/lib/libopus.a" ]]; then
  rm -rf "$WORK/opus-$OPUS_VER"
  tar -C "$WORK" -xzf "$HERE/$OPUS_TGZ"
  (
    cd "$WORK/opus-$OPUS_VER"
    CFLAGS="$COMMON_CFLAGS" ./configure --prefix="$PREFIX" \
      --disable-shared --enable-static --with-pic \
      --disable-doc --disable-extra-programs
    unhide
    make -j"$JOBS"
    make install
  )
fi

# ---- libopusenc (static, PIC) against the static libopus above ----
if [[ ! -f "$PREFIX/lib/libopusenc.a" ]]; then
  rm -rf "$WORK/libopusenc-$OPE_VER"
  tar -C "$WORK" -xzf "$HERE/$OPE_TGZ"
  (
    cd "$WORK/libopusenc-$OPE_VER"
    PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
    CFLAGS="$COMMON_CFLAGS" ./configure --prefix="$PREFIX" \
      --disable-shared --enable-static --with-pic \
      --disable-doc --disable-examples
    unhide
    # the x64 original's resampler symbols are opusenc_resampler_* (upstream: libopusenc_resampler_*)
    sed -i 's/-DRANDOM_PREFIX=libopusenc/-DRANDOM_PREFIX=opusenc/' Makefile
    make -j"$JOBS"
    make install
  )
fi

# ---- unified layer + final shared object ----
gcc $COMMON_CFLAGS -Wall -Wextra -c "$HERE/src/opusenc_unified.c" \
  -I"$PREFIX/include/opus" -o "$WORK/opusenc_unified.o"

mkdir -p "$OUT_DIR"
gcc -shared -o "$OUT_DIR/libopusenc.so" \
  -Wl,-soname,libopusenc.so \
  -Wl,--enable-new-dtags,-rpath,'$ORIGIN' \
  -Wl,-z,now -Wl,-z,relro -Wl,-z,max-page-size=65536 -Wl,--no-undefined \
  "$WORK/opusenc_unified.o" \
  -Wl,--whole-archive "$PREFIX/lib/libopusenc.a" "$PREFIX/lib/libopus.a" -Wl,--no-whole-archive \
  -lm
strip --strip-unneeded "$OUT_DIR/libopusenc.so"

echo ">> built $OUT_DIR/libopusenc.so"
file "$OUT_DIR/libopusenc.so"
readelf -d "$OUT_DIR/libopusenc.so" | grep -E 'NEEDED|SONAME|RUNPATH'
