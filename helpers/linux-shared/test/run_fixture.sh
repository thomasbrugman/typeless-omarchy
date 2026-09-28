#!/usr/bin/env bash
# Starts a headless sway (no visible output, separate Wayland socket) running
# the GTK fixture (X11 via Xwayland, so it gets input focus without a
# keyboard device) in the given mode. Prints the fixture pid. Stop with:
#   kill $(cat /tmp/typeless-fixture-sway.pid)
set -euo pipefail
MODE="${1:-entry}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF=/tmp/typeless-fixture-sway.conf
cat > "$CONF" <<CFG
output HEADLESS-1 resolution 800x600
xwayland enable
exec env -u NO_AT_BRIDGE GDK_BACKEND=x11 GTK_MODULES=gail:atk-bridge $HERE/build/gtk_fixture $MODE
CFG
env -u WAYLAND_DISPLAY -u DISPLAY -u HYPRLAND_INSTANCE_SIGNATURE -u SWAYSOCK \
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    sway -c "$CONF" >/tmp/typeless-fixture-sway.log 2>&1 &
echo $! > /tmp/typeless-fixture-sway.pid
for i in $(seq 1 50); do
    pid=$(pgrep -n -x gtk_fixture || true)
    [[ -n "$pid" ]] && { sleep 1; echo "$pid"; exit 0; }
    sleep 0.1
done
echo "fixture did not start" >&2; cat /tmp/typeless-fixture-sway.log >&2; exit 1
