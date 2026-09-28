#!/usr/bin/env bash
# The upstream update feed only ships x64 packages; report "up to date" instead.
set -euo pipefail
f="$1/node_modules/electron-updater/out/AppUpdater.js"
grep -q 'typeless-omarchy: updates disabled' "$f" && exit 0
python3 - "$f" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "    checkForUpdates() {\n        if (!this.isUpdaterActive()) {"
new = ("    checkForUpdates() {\n"
       "        // typeless-omarchy: updates disabled\n"
       "        const info = { version: this.currentVersion.version, files: [], path: '', sha512: '', releaseDate: new Date().toISOString() };\n"
       "        setImmediate(() => { this.emit('checking-for-update'); this.emit('update-not-available', info); });\n"
       "        return Promise.resolve(null);\n"
       "        if (!this.isUpdaterActive()) {")
assert s.count(old) == 1, "anchor not found"
s = s.replace(old, new)
old2 = "    async downloadUpdate(cancellationToken = null) {"
if old2 in s:
    s = s.replace(old2, old2 + "\n        throw new Error('Updates are disabled in the arm64 port');", 1)
open(p, "w").write(s)
PY
