// Simulates the app's steady state: 1 s focus polling + periodic full-context
// fetches, verifying every callback arrives exactly once and memory is stable.
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const koffi = require('koffi');
const lib = koffi.load(new URL('../../out/context-helper/build/linux/arm64/libContextHelper.so', import.meta.url).pathname);
const cbPtr = koffi.pointer(koffi.proto('void CStringCallback(str)'));
const appAsync = lib.func('getFocusedAppInfoAsync', 'void', [cbPtr], { async: true });
const elemAsync = lib.func('getFocusedElementInfoAsync', 'void', [cbPtr], { async: true });
const visAsync = lib.func('getFocusedVisibleTextAsync', 'void', ['int32_t', 'int32_t', cbPtr], { async: true });
const relAsync = lib.func('getFocusedElementRelatedContentAsync', 'void', ['int32_t', 'int32_t', 'int32_t', cbPtr], { async: true });
const enhance = lib.func('setFocusedWindowEnhancedUserInterface', 'int32_t', []);
const cleanup = lib.func('cleanup_context_helper', 'void', [], { async: true });

const secs = Number(process.argv[2] || 20);
let issued = 0, answered = 0, timeouts = 0, dup = 0, badJson = 0, maxMainBlock = 0;
function call(fn, args, timeoutMs, json = true) {
  issued++;
  return new Promise((resolve) => {
    let done = false, calls = 0;
    const timer = setTimeout(() => { if (!done) { done = true; timeouts++; resolve(null); } }, timeoutMs);
    let reg = koffi.register((s) => {
      calls++; if (calls > 1) dup++;
      answered++;
      if (json) { try { JSON.parse(s); } catch { badJson++; } }
      if (!done) { done = true; clearTimeout(timer); resolve(s); }
      if (reg) { koffi.unregister(reg); reg = null; }
    }, cbPtr);
    const t0 = performance.now();
    fn(...args, reg);
    maxMainBlock = Math.max(maxMainBlock, performance.now() - t0);
  });
}
const rss0 = process.memoryUsage().rss;
const end = Date.now() + secs * 1000;
let tick = 0;
while (Date.now() < end) {
  await call(appAsync, [], 500);
  if (tick++ % 3 === 0) {
    const t0 = performance.now();
    enhance();
    maxMainBlock = Math.max(maxMainBlock, performance.now() - t0);
    await new Promise((r) => setTimeout(r, 30));
    const app = await call(appAsync, [], 500);
    await Promise.all([
      call(visAsync, [20000, 2000], 2000, false),
      call(elemAsync, [], 500).then(() => call(relAsync, [1000, 1000, 500], 500)),
    ]);
  }
  await new Promise((r) => setTimeout(r, 1000));
}
await new Promise((r) => setTimeout(r, 2500)); // let late callbacks land
const rss1 = process.memoryUsage().rss;
console.log(JSON.stringify({ issued, answered, timeouts, dup, badJson, maxMainBlockMs: +maxMainBlock.toFixed(2), rssDeltaKB: Math.round((rss1 - rss0) / 1024) }));
await cleanup();
process.exit(issued === answered && !dup && !badJson ? 0 : 1);
