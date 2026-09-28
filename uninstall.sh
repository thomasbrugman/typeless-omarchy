#!/usr/bin/env bash
# Remove everything install.sh put in place. Typeless's own data
# (~/.config/Typeless: sign-in, history, settings) is kept unless --purge.
set -euo pipefail

pkill -x typeless 2>/dev/null || true
systemctl --user disable --now typeless-mic-button.service 2>/dev/null || true

rm -rf "$HOME/.local/opt/Typeless"
rm -f "$HOME/.local/bin/typeless" "$HOME/.local/bin/typeless-mic-button" \
  "$HOME/.config/systemd/user/typeless-mic-button.service" \
  "$HOME/.config/typeless-mic-button.env" \
  "$HOME/.local/share/applications/typeless.desktop" \
  "$HOME/.local/share/applications/typeless-url-handler.desktop" \
  "$HOME/.config/autostart/typeless.desktop"
systemctl --user daemon-reload 2>/dev/null || true

conf=$HOME/.config/hypr
if [ -f "$conf/typeless.lua" ]; then
  rm -f "$conf/typeless.lua"
  sed -i '/^-- Typeless (typeless-omarchy)$/d; /^require("hypr.typeless")$/d' "$conf/hyprland.lua"
  hyprctl reload >/dev/null 2>&1 || true
fi

if [ "${1:-}" = --purge ]; then
  rm -rf "$HOME/.config/Typeless"
fi
echo "Typeless removed."
