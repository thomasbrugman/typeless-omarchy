// Edge cases: own process in foreground, no a11y bus, bad arguments.
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const koffi = require('koffi');
const lib = koffi.load(new URL('../../out/context-helper/build/linux/arm64/libContextHelper.so', import.meta.url).pathname);
const cbPtr = koffi.pointer(koffi.proto('void CStringCallback(str)'));
const appSync = lib.func('getFocusedAppInfo', 'str', []);
const elemSync = lib.func('getFocusedElementInfo', 'str', []);
const relSync = lib.func('getFocusedElementRelatedContent', 'str', ['int32_t', 'int32_t', 'int32_t']);
const visSync = lib.func('getFocusedVisibleText', 'str', ['int32_t', 'int32_t']);
const relAsync = lib.func('getFocusedElementRelatedContentAsync', 'void', ['int32_t', 'int32_t', 'int32_t', cbPtr], { async: true });
const cleanup = lib.func('cleanup_context_helper', 'void', [], { async: true });

const mode = process.argv[2];
if (mode === 'self') {
  process.env.TYPELESS_CONTEXT_HELPER_TEST_PID = String(process.pid);
  process.env.TYPELESS_CONTEXT_HELPER_TEST_TITLE = 'self';
}
const t = (label, fn) => { const t0 = performance.now(); const v = fn(); console.log(`${label} (${Math.round(performance.now() - t0)}ms):`, v); JSON.parse(v); };
t('app', () => appSync());
t('elem', () => elemSync());
t('rel', () => relSync(-5, 1e9, 0));
console.log('vis:', JSON.stringify(visSync(0, 0)));
console.log('vis neg:', JSON.stringify(visSync(-1, -1)).slice(0, 80));
const keepAlive = setInterval(() => {}, 1000);
await new Promise((res) => {
  let r = koffi.register((s) => { console.log('relAsync:', s); JSON.parse(s); koffi.unregister(r); res(); }, cbPtr);
  relAsync(10, 10, 1, r);
});
clearInterval(keepAlive);
await cleanup();
console.log('ok');
process.exit(0);
