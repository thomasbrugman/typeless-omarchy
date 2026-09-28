// Exercises checkAccessibilityPermission against a mock helper (spawn + protocol).
const koffi = require('koffi'); const fs = require('fs'); const os = require('os'); const path = require('path');
const rt = fs.mkdtempSync(path.join(os.tmpdir(), 'uh-a11y-'));
process.env.XDG_RUNTIME_DIR = rt;
process.env.TYPELESS_ATSPI_HELPER = path.join(__dirname, 'mock-typeless-atspi-helper.py');
const lib = koffi.load(path.resolve(__dirname, '../../out/util-helper/build/linux/arm64/libUtilHelper.so'));
const a11y = lib.func('checkAccessibilityPermission', 'int', []);
let t = Date.now(); const r1 = a11y(); const d1 = Date.now() - t;
t = Date.now(); const r2 = a11y(); const d2 = Date.now() - t;
const pid = parseInt(fs.readFileSync(path.join(rt, 'typeless-atspi', 'helper.pid'), 'utf8'));
console.log(`first=${r1} (${d1} ms, spawned) second=${r2} (${d2} ms, reused) helperPid=${pid}`);
console.log(fs.readFileSync(path.join(rt, 'typeless-atspi', 'mock-requests.log'), 'utf8'));
process.kill(pid); fs.rmSync(rt, { recursive: true, force: true });
process.exit(r1 === 1 && r2 === 1 ? 0 : 1);
