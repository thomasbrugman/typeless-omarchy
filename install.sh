#!/usr/bin/env bash
# Install the staged build for the current user (no root needed).
#
# Usage: ./install.sh [--hyprland] [--mic-button[=<pulse-source>]] [--all]
#   (no flags)     the app: ~/.local/opt/Typeless, launcher, typeless:// handler
#   --hyprland     window rules for the floating bar (~/.config/hypr/typeless.lua)
#   --mic-button   watcher that turns a USB mic's hardware mute button into the
#                  dictation key (systemd user service). Optionally pass the
#                  PulseAudio/PipeWire source name of the mic (pactl list short sources).
#   --all          all of the above
set -euo pipefail
cd "$(dirname "$0")"

HYPR=0 MIC=0 MIC_SOURCE=
for arg in "$@"; do
  case "$arg" in
    --hyprland) HYPR=1 ;;
    --mic-button) MIC=1 ;;
    --mic-button=*) MIC=1; MIC_SOURCE=${arg#*=} ;;
    --all) HYPR=1; MIC=1 ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 1 ;;
  esac
done

DEST=$HOME/.local/opt/Typeless
APPS=$HOME/.local/share/applications
ICONS=$HOME/.local/share/icons
[ -x stage/Typeless/typeless ] || { echo "run ./build.sh first" >&2; exit 1; }

# App -----------------------------------------------------------------------
pkill -x typeless 2>/dev/null && sleep 1 || true
rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")" "$APPS" "$HOME/.local/bin"
cp -a stage/Typeless "$DEST"
ln -sf "$DEST/typeless" "$HOME/.local/bin/typeless"
[ -d stage/icons ] && cp -a stage/icons/. "$ICONS/"

# Typeless rewrites this entry itself on first start; this one covers the
# first launch and the typeless:// sign-in callback.
cat > "$APPS/typeless.desktop" <<EOF
[Desktop Entry]
Name=Typeless
Exec=env -u ELECTRON_RUN_AS_NODE "$DEST/typeless" %u
Terminal=false
Type=Application
Icon=typeless
StartupWMClass=Typeless
Comment=AI voice dictation (unofficial arm64 port)
MimeType=x-scheme-handler/typeless;
Categories=Utility;Office;
EOF
xdg-mime default typeless.desktop x-scheme-handler/typeless
update-desktop-database "$APPS" 2>/dev/null || true
gtk-update-icon-cache -q "$ICONS/hicolor" 2>/dev/null || true
echo "Installed Typeless $(cat "$DEST/.typeless-omarchy-version" 2>/dev/null) to $DEST"

# Hyprland ------------------------------------------------------------------
if [ "$HYPR" = 1 ]; then
  conf=$HOME/.config/hypr
  install -Dm644 config/hypr/typeless.lua "$conf/typeless.lua"
  if ! grep -q 'require("hypr.typeless")' "$conf/hyprland.lua"; then
    printf '\n-- Typeless (typeless-omarchy)\nrequire("hypr.typeless")\n' >> "$conf/hyprland.lua"
  fi
  if command -v hyprctl >/dev/null && hyprctl version >/dev/null 2>&1; then
    hyprctl reload >/dev/null
    errors=$(hyprctl configerrors)
    [ -z "$errors" ] || { echo "Hyprland config errors:" >&2; echo "$errors" >&2; exit 1; }
  fi
  echo "Installed Hyprland rules to $conf/typeless.lua"
fi

# Mic button ----------------------------------------------------------------
if [ "$MIC" = 1 ]; then
  install -Dm755 extras/mic-button/typeless-mic-button "$HOME/.local/bin/typeless-mic-button"
  install -Dm644 extras/mic-button/typeless-mic-button.service \
    "$HOME/.config/systemd/user/typeless-mic-button.service"
  if [ -n "$MIC_SOURCE" ]; then
    printf 'MIC_BUTTON_SOURCE=%s\n' "$MIC_SOURCE" > "$HOME/.config/typeless-mic-button.env"
  fi
  systemctl --user daemon-reload
  systemctl --user enable typeless-mic-button.service
  systemctl --user restart typeless-mic-button.service
  echo "Installed and started typeless-mic-button.service"
fi
