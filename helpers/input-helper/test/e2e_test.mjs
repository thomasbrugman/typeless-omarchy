// End-to-end test of the paste / copy paths without touching the user's session:
//   - run with LD_PRELOAD=grab_shim.so: the virtual keyboard is EVIOCGRAB'ed by this
//     process the moment it is created, so injected keys are only logged;
//   - run with WAYLAND_DISPLAY pointing at a headless compositor, so every clipboard
//     operation happens there.
// Usage: GRAB_SHIM_LOG=/tmp/x LD_PRELOAD=/tmp/grab_shim.so WAYLAND_DISPLAY=wayland-N node e2e_test.mjs
import { createRequire } from 'node:module';
import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const koffi = require('koffi');

const OUT = new URL('../../out/input-helper/build/linux/arm64', import.meta.url).pathname;
const TOOL = `${OUT}/typeless-clipboard`;
const LOG = process.env.GRAB_SHIM_LOG;
assert.ok(process.env.LD_PRELOAD?.includes('grab_shim'), 'refusing to run without the grab shim');
assert.ok(LOG, 'GRAB_SHIM_LOG required');
assert.ok(process.env.WAYLAND_DISPLAY && process.env.WAYLAND_DISPLAY !== 'wayland-1', 'use a headless compositor');

const lib = koffi.load(`${OUT}/libInputHelper.so`);
const InputHelperCallback = koffi.proto('void InputHelperCallback(str)');
const cbPtr = koffi.pointer(InputHelperCallback);
const insertText = lib.func('insertText', 'int32_t', ['str'], { async: true });
const insertRichText = lib.func('insertRichText', 'int32_t', ['str', 'str'], { async: true });
const copyAsync = lib.func('getSelectedTextBySimulateCopyAsync', 'void', [cbPtr], { async: true });
const cleanup = lib.func('cleanup_input_helper', 'void', [], { async: true });
const warmup = lib.func('warmup_voice_ime', 'int32_t', [], { async: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function frames(entries) {
  const bufs = [];
  for (const [mime, data] of entries) {
    const m = Buffer.from(mime);
    const d = Buffer.isBuffer(data) ? data : Buffer.from(data);
    const h = Buffer.alloc(4); h.writeUInt32LE(m.length);
    const l = Buffer.alloc(8); l.writeBigUInt64LE(BigInt(d.length));
    bufs.push(h, m, l, d);
  }
  return Buffer.concat(bufs);
}
function parse(buf) {
  const out = new Map();
  let i = 0;
  while (i < buf.length) {
    const ml = buf.readUInt32LE(i); i += 4;
    const m = buf.subarray(i, i + ml).toString(); i += ml;
    const dl = Number(buf.readBigUInt64LE(i)); i += 8;
    out.set(m, buf.subarray(i, i + dl)); i += dl;
  }
  return out;
}
const clipSet = (entries) => { const r = spawnSync(TOOL, ['set'], { input: frames(entries) }); assert.equal(r.status, 0); };
const clipGet = () => { const r = spawnSync(TOOL, ['get']); return r.status === 0 ? parse(r.stdout) : new Map(); };
const clipText = () => clipGet().get('text/plain;charset=utf-8')?.toString() ?? '';

const keyLog = () => (fs.existsSync(LOG) ? fs.readFileSync(LOG, 'utf8').trim().split('\n').filter(Boolean) : []);
let logMark = 0;
const newKeys = () => { const all = keyLog(); const fresh = all.slice(logMark); logMark = all.length; return fresh.join(','); };

function activeIsTerminal() {
  try {
    const w = JSON.parse(execFileSync('hyprctl', ['activewindow', '-j'], { encoding: 'utf8' }));
    return (w.tags ?? []).some((t) => t.replace(/\*$/, '') === 'terminal');
  } catch { return false; }
}
const pasteChord = (term) => (term ? '29 1,42 1,47 1,47 0,42 0,29 0' : '29 1,47 1,47 0,29 0');
const copyChord = (term) => (term ? '29 1,42 1,46 1,46 0,42 0,29 0' : '29 1,46 1,46 0,29 0');

const PNG = Buffer.from('89504e470d0a1a0a0000000d4948445200000001000000010806000000', 'hex');
const ORIGINAL = [['text/plain;charset=utf-8', 'ORIGINAL clipboard'], ['text/plain', 'ORIGINAL clipboard'], ['image/png', PNG]];
const assertRestored = () => {
  const c = clipGet();
  assert.equal(c.get('text/plain;charset=utf-8')?.toString(), 'ORIGINAL clipboard');
  assert.ok(c.get('image/png')?.equals(PNG), 'image/png restored byte-exact');
};

async function run(name, fn) {
  try { await fn(); console.log(`ok   ${name}`); } catch (e) { console.log(`FAIL ${name}: ${e.stack}`); process.exitCode = 1; }
}

await run('warmup_voice_ime returns 0 immediately and pre-creates the virtual keyboard', async () => {
  const t0 = Date.now();
  assert.equal(await warmup(), 0);
  assert.ok(Date.now() - t0 < 50, 'warmup must not block the JS thread');
  await sleep(400);
  assert.equal(newKeys(), '', 'no keys sent by warmup');
});

await run('insertText pastes via clipboard + chord, then restores all MIME types', async () => {
  clipSet(ORIGINAL);
  newKeys();
  const term = activeIsTerminal();
  const text = 'héllo 🌍 world — "quotes" & <tags>\nsecond line';
  const t0 = Date.now();
  const rc = await insertText(text);
  const took = Date.now() - t0;
  const during = clipGet();
  assert.equal(rc, 1);
  assert.equal(during.get('text/plain;charset=utf-8')?.toString(), text);
  for (const m of ['text/plain', 'UTF8_STRING', 'STRING', 'TEXT']) assert.equal(during.get(m)?.toString(), text, m);
  assert.equal(newKeys(), pasteChord(term));
  console.log(`     rc=${rc} took=${took}ms terminal=${term}`);
  await sleep(700);
  assertRestored();
});

await run('insertRichText offers text/html + text/plain, restores after 1s', async () => {
  clipSet(ORIGINAL);
  newKeys();
  const term = activeIsTerminal();
  const rc = await insertRichText("<meta charset='utf-8'><b>bold</b> text", 'bold text');
  const during = clipGet();
  assert.equal(rc, 1);
  assert.equal(during.get('text/html')?.toString(), "<meta charset='utf-8'><b>bold</b> text");
  assert.equal(during.get('text/plain;charset=utf-8')?.toString(), 'bold text');
  assert.equal(newKeys(), pasteChord(term));
  await sleep(600);
  assert.equal(clipText(), 'bold text', 'still ours before 1000ms');
  await sleep(600);
  assertRestored();
});

await run('insertRichText with empty html derives <p>escaped</p>', async () => {
  clipSet(ORIGINAL);
  newKeys();
  assert.equal(await insertRichText('', 'a < b & c'), 1);
  assert.equal(clipGet().get('text/html')?.toString(), '<p>a &lt; b &amp; c</p>');
  newKeys();
  await sleep(1200);
  assertRestored();
});

await run('two inserts inside the restore window still restore the ORIGINAL clipboard', async () => {
  clipSet(ORIGINAL);
  newKeys();
  assert.equal(await insertText('one'), 1);
  assert.equal(await insertText('two'), 1);
  assert.equal(clipText(), 'two');
  newKeys();
  await sleep(750);
  assertRestored();
});

await run('user copy during the restore window is not clobbered', async () => {
  clipSet(ORIGINAL);
  newKeys();
  assert.equal(await insertText('dictated'), 1);
  clipSet([['text/plain;charset=utf-8', 'USER COPY']]);
  newKeys();
  await sleep(750);
  assert.equal(clipText(), 'USER COPY');
});

await run('empty clipboard before paste -> cleared afterwards', async () => {
  spawnSync(TOOL, ['clear']);
  newKeys();
  assert.equal(await insertText('temp'), 1);
  newKeys();
  await sleep(700);
  assert.equal(clipGet().size, 0);
});

function copyWithFakeApp(respondWith) {
  // Act as the focused app: when the copy chord's key-down shows up, publish a selection.
  return new Promise((resolve) => {
    let answered = false;
    const watcher = setInterval(() => {
      if (!answered && respondWith !== null && keyLog().slice(logMark).includes('46 1')) {
        answered = true;
        clipSet([['text/plain;charset=utf-8', respondWith], ['text/plain', respondWith]]);
      }
    }, 5);
    let done = false;
    const timer = setTimeout(() => { if (!done) { done = true; clearInterval(watcher); resolve({ value: '', timedOut: true }); } }, 2500);
    let ptr = koffi.register((v) => {
      if (!done) { done = true; clearTimeout(timer); clearInterval(watcher); resolve({ value: v || '', timedOut: false }); }
      if (ptr) { koffi.unregister(ptr); ptr = null; }
    }, cbPtr);
    copyAsync(ptr);
  });
}

await run('getSelectedTextBySimulateCopyAsync returns copied text and restores clipboard', async () => {
  clipSet(ORIGINAL);
  newKeys();
  const term = activeIsTerminal();
  const t0 = Date.now();
  const r = await copyWithFakeApp('the ​selected text');
  console.log(`     -> ${JSON.stringify(r)} in ${Date.now() - t0}ms terminal=${term}`);
  assert.equal(r.timedOut, false);
  assert.equal(r.value, 'the selected text', 'zero-width space stripped');
  assert.equal(newKeys(), copyChord(term));
  assertRestored();
});

await run('getSelectedTextBySimulateCopyAsync with nothing selected -> "" and clipboard intact', async () => {
  clipSet(ORIGINAL);
  newKeys();
  const r = await copyWithFakeApp(null);
  assert.equal(r.timedOut, false);
  assert.equal(r.value, '');
  newKeys();
  assertRestored();
});

// ---- terminal targets, via a fake Hyprland IPC socket (own instance signature) ----
// The library's koffi calls are synchronous on the JS thread (the app passes an
// `{async: true}` 4th argument that koffi ignores), so the fake server must live
// in another process.
import { spawn } from 'node:child_process';
async function withFakeHypr(activewindow, fn) {
  const sig = `tltest_${process.pid}`;
  const child = spawn('python3', [new URL('./fake_hypr.py', import.meta.url).pathname, sig, JSON.stringify(activewindow)], { stdio: ['ignore', 'pipe', 'inherit'] });
  await new Promise((r) => child.stdout.once('data', r));
  const saved = process.env.HYPRLAND_INSTANCE_SIGNATURE;
  process.env.HYPRLAND_INSTANCE_SIGNATURE = sig;
  try { await fn(); } finally {
    if (saved === undefined) delete process.env.HYPRLAND_INSTANCE_SIGNATURE; else process.env.HYPRLAND_INSTANCE_SIGNATURE = saved;
    child.kill('SIGINT');
    await new Promise((r) => child.once('exit', r));
  }
}
const isTerminalFake = { class: 'kitty', initialClass: 'kitty', title: 'x', pid: 1, tags: ['terminal*'] };
await run('terminal (fake Hyprland: kitty, tag terminal*) -> Ctrl+Shift+V paste, Ctrl+Shift+C copy', async () => {
  await withFakeHypr(isTerminalFake, async () => {
    clipSet(ORIGINAL);
    newKeys();
    const t0 = Date.now();
    assert.equal(await insertText('ls -la'), 1);
    console.log(`     steady-state insertText took ${Date.now() - t0}ms`);
    assert.equal(newKeys(), pasteChord(true));
    await sleep(700);
    assertRestored();
    newKeys();
    const r = await copyWithFakeApp('term selection');
    assert.equal(r.value, 'term selection');
    assert.equal(newKeys(), copyChord(true));
  });
});
await run('class-only terminal (foot, no tags) and non-terminal (firefox)', async () => {
  await withFakeHypr({ class: 'foot', tags: [] }, async () => {
    newKeys(); assert.equal(await insertText('a'), 1); assert.equal(newKeys(), pasteChord(true));
  });
  await withFakeHypr({ class: 'firefox', tags: [] }, async () => {
    newKeys(); assert.equal(await insertText('b'), 1); assert.equal(newKeys(), pasteChord(false));
  });
  await sleep(700);
});

await cleanup();
console.log(process.exitCode ? 'SOME TESTS FAILED' : 'all e2e tests passed');
process.exit();
