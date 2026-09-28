#!/usr/bin/env bash
# Build an arm64 Typeless from the official x64 .deb:
#   1. compile the clean-room arm64 helper libraries in helpers/
#   2. download the upstream x64 .deb and swap in an arm64 Electron runtime of
#      the same version (fuses mirrored), arm64 better-sqlite3, koffi's
#      bundled linux_arm64 build
#   3. apply patches/ to app.asar (updater off, forced XWayland)
# Result: stage/Typeless, installed by ./install.sh.
#
# Usage: ./build.sh [typeless-version]      (default: the version tested here)
#        SKIP_HELPERS=1 ./build.sh           reuse helpers/out from a previous run
set -euo pipefail
shopt -s nullglob
cd "$(dirname "$0")"
ROOT=$PWD

VERSION=${1:-${TYPELESS_VERSION:-2.2.0}}
DEB=Typeless-$VERSION-x64.deb
FEED=https://typeless-static.com/desktop-release
STAGE=$ROOT/stage/Typeless
WORK=$ROOT/work

log() { printf '\n==> %s\n' "$*"; }

[ "$(uname -m)" = aarch64 ] || echo "warning: this port targets aarch64; building on $(uname -m)" >&2
for tool in curl bsdtar npx python3 gcc g++ pkg-config sha256sum sha512sum; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool (see README)" >&2; exit 1; }
done

# 1. Helpers ---------------------------------------------------------------
if [ -z "${SKIP_HELPERS:-}" ]; then
  log "Building arm64 helper libraries"
  rm -rf helpers/out
  helpers/input-helper/build.sh        # also builds helpers/linux-shared (AT-SPI helper)
  helpers/keyboard-helper/build.sh
  helpers/context-helper/build.sh
  helpers/util-helper/build.sh
  helpers/libopusenc/build.sh
fi

# 2. Upstream package --------------------------------------------------------
mkdir -p dl
if [ ! -f "dl/$DEB" ]; then
  log "Downloading $DEB"
  curl -fL --retry 3 -o "dl/$DEB.part" "$FEED/$DEB" && mv "dl/$DEB.part" "dl/$DEB"
fi
# The update feed publishes the sha512 of the current release only.
feed_version=$(curl -fsSL "$FEED/latest-linux.yml" | sed -n 's/^version: //p' || true)
if [ "$feed_version" = "$VERSION" ]; then
  want=$(curl -fsSL "$FEED/latest-linux.yml" | awk '/^path:/{p=1} p&&/^sha512:/{print $2; exit}')
  have=$(sha512sum "dl/$DEB" | cut -d' ' -f1 | python3 -c 'import sys,base64;print(base64.b64encode(bytes.fromhex(sys.stdin.read().strip())).decode())')
  [ "$want" = "$have" ] || { echo "sha512 mismatch for dl/$DEB" >&2; exit 1; }
  echo "sha512 verified against the update feed"
else
  echo "note: latest upstream is ${feed_version:-unknown}; cannot verify $VERSION against the feed" >&2
fi

log "Unpacking $DEB"
rm -rf "$WORK" && mkdir -p "$WORK/x64"
(cd "$WORK/x64" && bsdtar -xf "$ROOT/dl/$DEB" && bsdtar -xf data.tar.* && bsdtar -xf control.tar.*)
X64=$WORK/x64/opt/Typeless

ELECTRON=$(grep -aoE 'Chrome/[0-9.]+ Electron/[0-9.]+' "$X64/typeless" | head -1 | sed 's/.*Electron\///')
[ -n "$ELECTRON" ] || { echo "could not detect the Electron version" >&2; exit 1; }
echo "upstream Electron: $ELECTRON"

EZIP=electron-v$ELECTRON-linux-arm64.zip
if [ ! -f "dl/$EZIP" ]; then
  log "Downloading $EZIP"
  curl -fL --retry 3 -o "dl/$EZIP" "https://github.com/electron/electron/releases/download/v$ELECTRON/$EZIP"
fi
curl -fsSL "https://github.com/electron/electron/releases/download/v$ELECTRON/SHASUMS256.txt" \
  | grep " \*\?$EZIP\$" | sed 's/ \*/  /' | (cd dl && sha256sum -c -)

# 3. Assemble ----------------------------------------------------------------
log "Assembling stage/Typeless"
rm -rf "$ROOT/stage" && mkdir -p "$STAGE"
bsdtar -xf "dl/$EZIP" -C "$STAGE"
mv "$STAGE/electron" "$STAGE/typeless"
rm -rf "$STAGE/resources/default_app.asar" "$STAGE/clang_x64_v8_arm64"
cp -a "$X64/LICENSE.electron.txt" "$X64/LICENSES.chromium.html" "$STAGE/"
# Chromium falls back to the user-namespace sandbox only if chrome-sandbox is
# absent; a per-user install can't make it setuid root.
rm -f "$STAGE/chrome-sandbox"

mkdir -p "$STAGE/resources"
for f in "$X64"/resources/*; do
  case "$(basename "$f")" in app.asar|app.asar.unpacked|lib) ;; *) cp -a "$f" "$STAGE/resources/" ;; esac
done

log "Rebuilding app.asar with arm64 native modules"
npx -y @electron/asar extract "$X64/resources/app.asar" "$WORK/asar"
ABI=$(ELECTRON_RUN_AS_NODE=1 "$STAGE/typeless" -p process.versions.modules)
SQLITE=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["version"])' "$WORK/asar/node_modules/better-sqlite3/package.json")
SQLITE_TGZ=better-sqlite3-v$SQLITE-electron-v$ABI-linux-arm64.tar.gz
[ -f "dl/$SQLITE_TGZ" ] || curl -fL --retry 3 -o "dl/$SQLITE_TGZ" \
  "https://github.com/WiseLibs/better-sqlite3/releases/download/v$SQLITE/$SQLITE_TGZ"
bsdtar -xzf "dl/$SQLITE_TGZ" -C "$WORK/asar/node_modules/better-sqlite3"
[ -d "$WORK/asar/node_modules/koffi/build/koffi/linux_arm64" ] || { echo "koffi has no linux_arm64 build" >&2; exit 1; }
find "$WORK/asar/node_modules/koffi/build/koffi" -mindepth 1 -maxdepth 1 ! -name linux_arm64 -exec rm -rf {} +
for p in patches/*.sh; do echo "patch: $(basename "$p")"; bash "$p" "$WORK/asar"; done
npx -y @electron/asar pack "$WORK/asar" "$STAGE/resources/app.asar" --unpack "*.node"

log "Installing arm64 helpers"
for d in helpers/out/*/; do
  name=$(basename "$d")
  mkdir -p "$STAGE/resources/lib/$name"
  cp -a "$d"/. "$STAGE/resources/lib/$name/"
done

log "Mirroring the upstream Electron fuses"
npx -y @electron/fuses write --app "$STAGE/typeless" \
  RunAsNode=on EnableCookieEncryption=off EnableNodeOptionsEnvironmentVariable=on \
  EnableNodeCliInspectArguments=on EnableEmbeddedAsarIntegrityValidation=off \
  OnlyLoadAppFromAsar=on LoadBrowserProcessSpecificV8Snapshot=off GrantFileProtocolExtraPrivileges=on

cp -a "$WORK/x64/usr/share/icons" "$ROOT/stage/" 2>/dev/null || true
echo "$VERSION" > "$STAGE/.typeless-omarchy-version"
log "Built Typeless $VERSION (Electron $ELECTRON) at $STAGE — run ./install.sh"
