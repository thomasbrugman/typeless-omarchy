// Exercises libContextHelper.so exactly the way Typeless' main process does
// (koffi 2.14.1, same prototypes, register/unregister per call, JS timeouts).
// Usage: node harness.mjs [libpath] [--loops N] [--no-a11y]
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const koffi = require('koffi');

const args = process.argv.slice(2);
const libPath = args.find((a, i) => !a.startsWith('--') && args[i - 1] !== '--loops') ||
  new URL('../../out/context-helper/build/linux/arm64/libContextHelper.so', import.meta.url).pathname;
const loops = Number((args[args.indexOf('--loops') + 1]) || 3) || 3;

const lib = koffi.load(libPath);
const CStringCallback = koffi.proto('void CStringCallback(str)');
const cbPtr = koffi.pointer(CStringCallback);
const f = {
  hello: lib.func('context_say_hello', 'str', []),
  init: lib.func('initialize_context_helper', 'void', []),
  appSync: lib.func('getFocusedAppInfo', 'str', []),
  appAsync: lib.func('getFocusedAppInfoAsync', 'void', [cbPtr], { async: true }),
  elemSync: lib.func('getFocusedElementInfo', 'str', []),
  elemAsync: lib.func('getFocusedElementInfoAsync', 'void', [cbPtr], { async: true }),
  visSync: lib.func('getFocusedVisibleText', 'str', ['int32_t', 'int32_t']),
  visAsync: lib.func('getFocusedVisibleTextAsync', 'void', ['int32_t', 'int32_t', cbPtr], { async: true }),
  relSync: lib.func('getFocusedElementRelatedContent', 'str', ['int32_t', 'int32_t', 'int32_t']),
  relAsync: lib.func('getFocusedElementRelatedContentAsync', 'void', ['int32_t', 'int32_t', 'int32_t', cbPtr], { async: true }),
  enhance: lib.func('setFocusedWindowEnhancedUserInterface', 'int32_t', []),
  cleanup: lib.func('cleanup_context_helper', 'void', [], { async: true }),
  recover: lib.func('recover_context_helper_after_resume', 'void', []),
  free: lib.func('free_string', 'void', ['void *']),
};

// Same pattern as the app: resolve on first callback or timeout, always unregister in the callback.
function callAsync(fn, fnArgs, timeoutMs, label) {
  return new Promise((resolve) => {
    const t0 = performance.now();
    let done = false;
    const timer = setTimeout(() => {
      if (!done) { done = true; resolve({ label, timeout: true, ms: Math.round(performance.now() - t0) }); }
    }, timeoutMs);
    let reg = koffi.register((s) => {
      const ms = Math.round(performance.now() - t0);
      if (!done) { done = true; clearTimeout(timer); resolve({ label, value: s, ms }); }
      else console.log(`  [late callback ${label} after ${ms}ms]`);
      if (reg) { koffi.unregister(reg); reg = null; }
    }, cbPtr);
    fn(...fnArgs, reg);
  });
}

function show(r, parse = true) {
  if (r.timeout) { console.log(`${r.label}: JS TIMEOUT after ${r.ms}ms`); return; }
  let v = r.value;
  if (parse) {
    try { v = JSON.parse(r.value); } catch (e) { console.log(`${r.label}: INVALID JSON (${e.message}):`, r.value); return; }
  }
  const s = typeof v === 'string' ? JSON.stringify(v.length > 600 ? v.slice(0, 600) + '…' : v) : JSON.stringify(v);
  console.log(`${r.label} (${r.ms}ms):`, s.length > 1500 ? s.slice(0, 1500) + '…' : s);
}

console.log('hello:', f.hello());
console.log('sync appInfo:', f.appSync());

for (let i = 0; i < loops; i++) {
  show(await callAsync(f.appAsync, [], 500, `appInfoAsync#${i}`));
}
if (!args.includes('--no-a11y')) {
  console.log('enhance ->', f.enhance());
  await new Promise((r) => setTimeout(r, 30));
  for (let i = 0; i < loops; i++) {
    const [app, el] = await Promise.all([
      callAsync(f.appAsync, [], 500, `appInfoAsync(par)#${i}`),
      callAsync(f.elemAsync, [], 500, `elementInfoAsync#${i}`),
    ]);
    show(app); show(el);
    show(await callAsync(f.relAsync, [1000, 1000, 500], 500, `relatedAsync#${i}`));
  }
  show(await callAsync(f.visAsync, [20000, 2000], 2000, 'visibleTextAsync'), false);
  // eviction: fire 3 element requests back to back
  const ev = await Promise.all([0, 1, 2].map((k) => callAsync(f.elemAsync, [], 800, `evict-elem#${k}`)));
  ev.forEach((r) => show(r));
  console.log('sync element:', f.elemSync());
  console.log('sync related:', f.relSync(200, 200, 500));
  const vt = f.visSync(300, 1500);
  console.log('sync visible (300 max):', JSON.stringify(vt), 'cp=', [...(vt || '')].length);
  f.recover();
  show(await callAsync(f.elemAsync, [], 800, 'elementInfoAsync after recover'));
}
const t0 = performance.now();
await f.cleanup();
console.log(`cleanup done in ${Math.round(performance.now() - t0)}ms`);
// Calls after cleanup must not crash and must not call back.
f.appAsync(koffi.register(() => console.log('UNEXPECTED callback after cleanup'), cbPtr));
await new Promise((r) => setTimeout(r, 300));
console.log('done');
process.exit(0);
