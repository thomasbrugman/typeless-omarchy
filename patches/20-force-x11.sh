#!/usr/bin/env bash
# Typeless positions its floating bar with raw Xlib calls on the native window
# handle, so it must run under XWayland. Omarchy exports
# ELECTRON_OZONE_PLATFORM_HINT=wayland, and the app rewrites its own .desktop and
# autostart entries without extra flags, so the app relaunches itself with X11.
# TYPELESS_ALLOW_WAYLAND=1 skips this.
set -euo pipefail
d="$1/dist/main"
cat > "$d/force-x11.js" <<'JS'
// Relaunch once with --ozone-platform=x11 (app.commandLine.appendSwitch is too
// late for the ozone platform). Runs before the app takes its single-instance lock.
import { spawn } from 'node:child_process';
const argv = process.argv.slice(1);
if (process.platform === 'linux' && !process.env.TYPELESS_ALLOW_WAYLAND &&
    !argv.includes('--ozone-platform=x11')) {
  const env = { ...process.env };
  delete env.ELECTRON_RUN_AS_NODE;
  spawn(process.execPath, [...argv, '--ozone-platform=x11'],
        { env, detached: true, stdio: 'inherit' }).unref();
  process.exit(0);
}
JS
grep -q "force-x11.js" "$d/index.js" && exit 0
python3 - "$d/index.js" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
# Entry chunk names are content-hashed and change between releases, so prepend
# the shim as a hoisted import instead of anchoring on the upstream chunk name.
shim = "import'./force-x11.js';"
assert "force-x11.js" not in s, "already patched"
open(p, "w").write(shim + s)
PY
