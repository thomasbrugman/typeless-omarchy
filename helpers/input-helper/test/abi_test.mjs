// Loads libInputHelper.so exactly like Typeless' main process does (koffi 2.14.1,
// same prototypes / async flags / callback type) and exercises the exports that
// cannot type into the user's session. Keystroke-sending exports are covered by
// e2e_test.mjs, which only runs with a grabbed virtual keyboard.
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const koffi = require('koffi');

const libPath = process.argv[2] ?? new URL('../../out/input-helper/build/linux/arm64/libInputHelper.so', import.meta.url).pathname;
const lib = koffi.load(libPath);

const InputHelperCallback = koffi.proto('void InputHelperCallback(str)');
const cbPtr = koffi.pointer(InputHelperCallback);

const f = {
  insertText: lib.func('insertText', 'int32_t', ['str'], { async: true }),
  insertRichText: lib.func('insertRichText', 'int32_t', ['str', 'str'], { async: true }),
  deleteBackward: lib.func('int32_t deleteBackward(int32_t count)'),
  getSelectedText: lib.func('getSelectedText', 'str', []),
  getCurrentInputState: lib.func('getCurrentInputState', 'str', []),
  getSelectedTextBySimulateCopyAsync: lib.func('getSelectedTextBySimulateCopyAsync', 'void', [cbPtr], { async: true }),
  getSelectedTextAsync: lib.func('getSelectedTextAsync', 'void', [cbPtr], { async: true }),
  getCurrentInputStateAsync: lib.func('getCurrentInputStateAsync', 'void', [cbPtr], { async: true }),
  cleanup: lib.func('cleanup_input_helper', 'void', [], { async: true }),
  recover: lib.func('recover_input_helper_after_resume', 'void', []),
  warmup: lib.func('warmup_voice_ime', 'int32_t', [], { async: true }),
  restore: lib.func('restore_voice_ime', 'int32_t', [], { async: true }),
  testTSF: lib.func('testTSFFunction', 'str', []),
};

const callAsync = (fn, ...args) => new Promise((resolve, reject) => fn.async(...args, (err, res) => (err ? reject(err) : resolve(res))));

// Same shape as the app's invokeStringCallback(): register, pass, unregister in the callback.
function invokeStringCallback(fn, timeoutMs) {
  return new Promise((resolve) => {
    let done = false;
    let calls = 0;
    const timer = setTimeout(() => { if (!done) { done = true; resolve({ value: '', timedOut: true, calls }); } }, timeoutMs);
    let ptr = koffi.register((value) => {
      calls++;
      if (!done) { clearTimeout(timer); done = true; resolve({ value: value || '', timedOut: false, calls, mainThread: true }); }
      if (ptr) { koffi.unregister(ptr); ptr = null; }
    }, cbPtr);
    fn(ptr);
  });
}

const results = [];
const check = (name, fn) => results.push((async () => { try { await fn(); console.log(`ok   ${name}`); } catch (e) { console.log(`FAIL ${name}: ${e.message}`); process.exitCode = 1; } })());

// Sequential: some checks share the helper state.
async function main() {
  const tsf = JSON.parse(f.testTSF());
  assert.equal(tsf.platform, 'linux');
  assert.equal(tsf.tsf_initialized, false);
  assert.equal(tsf.is_in_editable_tsf, false);
  assert.match(tsf.timestamp, /^\d{13}$/);
  console.log('ok   testTSFFunction', JSON.stringify(tsf));

  assert.equal(f.deleteBackward(0), -1);
  assert.equal(f.deleteBackward(-5), -1);
  console.log('ok   deleteBackward(<=0) == -1 (no keys sent)');

  assert.equal(await f.insertText(''), 0);
  assert.equal(await f.insertText(null), 0);
  assert.equal(await f.insertRichText(null, null), 0);
  assert.equal(await f.insertRichText('', ''), 0);
  console.log('ok   insertText/insertRichText reject empty input with 0');

  assert.equal(await f.warmup(), 0);
  assert.equal(await f.restore(), 0);
  console.log('ok   warmup_voice_ime/restore_voice_ime == 0');

  const t0 = Date.now();
  const st = await invokeStringCallback(f.getCurrentInputStateAsync, 500);
  console.log('     getCurrentInputStateAsync ->', JSON.stringify(st), `${Date.now() - t0}ms`);
  assert.equal(st.timedOut, false);
  assert.equal(st.calls, 1);
  const parsed = JSON.parse(st.value);
  assert.ok(typeof parsed === 'object');
  assert.ok('error' in parsed || ('startIndex' in parsed && 'beforeText' in parsed));
  console.log('ok   getCurrentInputStateAsync callback fired once with JSON');

  const sel = await invokeStringCallback(f.getSelectedTextAsync, 1000);
  console.log('     getSelectedTextAsync ->', JSON.stringify(sel));
  assert.equal(sel.timedOut, false);
  assert.equal(sel.calls, 1);
  console.log('ok   getSelectedTextAsync callback fired once');

  // Eviction: two queued requests of the same kind -> the older gets {"error":"evicted"}.
  const [a, b, c] = await Promise.all([
    invokeStringCallback(f.getCurrentInputStateAsync, 2000),
    invokeStringCallback(f.getCurrentInputStateAsync, 2000),
    invokeStringCallback(f.getCurrentInputStateAsync, 2000),
  ]);
  console.log('     burst ->', a.value, '|', b.value, '|', c.value);
  for (const r of [a, b, c]) { assert.equal(r.timedOut, false); assert.equal(r.calls, 1); JSON.parse(r.value); }
  console.log('ok   burst of 3 async calls all resolved exactly once');

  const syncState = f.getCurrentInputState();
  JSON.parse(syncState);
  console.log('ok   getCurrentInputState (sync) ->', syncState);
  console.log('ok   getSelectedText (sync) ->', JSON.stringify(f.getSelectedText()));

  const r0 = Date.now();
  f.recover();
  assert.ok(Date.now() - r0 < 50, 'recover must not block');
  console.log(`ok   recover_input_helper_after_resume returned in ${Date.now() - r0}ms`);

  await f.cleanup();
  const after = await invokeStringCallback(f.getCurrentInputStateAsync, 500);
  console.log('     after cleanup getCurrentInputStateAsync ->', JSON.stringify(after));
  assert.equal(after.timedOut, false);
  console.log('ok   cleanup_input_helper; post-cleanup calls still resolve');
}

main().catch((e) => { console.error("FAIL", e); process.exitCode = 1; }).finally(() => process.exit());
