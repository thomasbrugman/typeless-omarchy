// MANUAL smoke test on the real session: after a countdown, inserts a test
// string into whatever window has focus, exactly as Typeless would (koffi,
// same prototypes). Focus a text field (browser, editor, terminal) before the
// countdown ends. Your clipboard is restored ~0.5 s afterwards.
//   mise exec node@24 -- node manual_paste_test.mjs [seconds] [text]
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const koffi = require('koffi');
const lib = koffi.load(new URL('../../out/input-helper/build/linux/arm64/libInputHelper.so', import.meta.url).pathname);
const warmup = lib.func('warmup_voice_ime', 'int32_t', []);
const insertText = lib.func('insertText', 'int32_t', ['str']);

const seconds = Number(process.argv[2] ?? 5);
const text = process.argv[3] ?? 'Typeless arm64 input-helper test: héllo 🌍 ✓';
warmup();
for (let s = seconds; s > 0; s--) {
  process.stdout.write(`inserting in ${s}s...\r`);
  await new Promise((r) => setTimeout(r, 1000));
}
console.log(`insertText -> ${insertText(text)} (1 = pasted)`);
await new Promise((r) => setTimeout(r, 800)); // let the clipboard restore run
process.exit(0);
