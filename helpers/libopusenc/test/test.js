// Exercises the arm64 libopusenc.so exactly the way Typeless' main/worker/opusWorker.js does
// (koffi 2.14.1, same declarations, same config, same buffer-mode retry loop and file-mode fallback).
// Run: mise exec node@24 -- node test.js
'use strict';
const koffi = require('koffi');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

// ---- library path resolution, same as the app's G() ----
const LIB_PATH = process.env.LIB_PATH || path.join(__dirname, '..', '..', 'out');
const libFile = () => {
  const arch = process.arch === 'x64' ? 'x64' : process.arch === 'arm64' ? 'arm64' : 'x86';
  switch (process.platform) {
    case 'darwin': return path.join(LIB_PATH, 'libopusenc', 'build', 'macos', arch, 'libopusenc.dylib');
    case 'win32': return path.join(LIB_PATH, 'libopusenc', 'build', 'windows', arch, 'libopusenc.dll');
    case 'linux': return path.join(LIB_PATH, 'libopusenc', 'build', 'linux', arch, 'libopusenc.so');
    default: throw new Error('Unsupported platform: ' + process.platform);
  }
};

// ---- declarations copied verbatim from opusWorker.js ----
const lib = koffi.load(libFile());
const P = lib.func('opus_estimate_output_size', 'uint64', ['void*', 'uint64', 'int']);
const C = lib.func('opus_convert_buffer', 'int', ['void*', 'uint64', 'int', 'float', 'int', 'int', 'void*', 'uint64',
  koffi.out(koffi.pointer('uint64')), koffi.out(koffi.pointer('double')), koffi.out(koffi.pointer('int')), koffi.out(koffi.pointer('float'))]);
const z = lib.func('opus_convert_advanced', 'int', ['str', 'str', 'int', 'float', 'int', 'int',
  koffi.out(koffi.pointer('double')), koffi.out(koffi.pointer('uint64')), koffi.out(koffi.pointer('int')), koffi.out(koffi.pointer('float'))]);
// extra (not used by the app, sanity only)
const ver = lib.func('opus_unified_version', 'str', []);
const opeVer = lib.func('ope_get_version_string', 'str', []);
const opusVer = lib.func('opus_get_version_string', 'str', []);

const d = { name: '10x', bitrate: 16000, frameSize: 20, vbr: true, signal: 'auto', channels: 1 };
const signalCode = s => s === 'voice' ? 1 : s === 'music' ? 2 : 0;

let failures = 0;
const check = (cond, msg) => { console.log((cond ? 'PASS ' : 'FAIL ') + msg); if (!cond) failures++; };

// ---- 16 kHz mono s16le WAV, canonical 44-byte header ----
function makeWav(seconds = 2, rate = 16000, freq = 440) {
  const n = Math.round(seconds * rate);
  const b = Buffer.alloc(44 + n * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * 2, 4); b.write('WAVE', 8);
  b.write('fmt ', 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22);
  b.writeUInt32LE(rate, 24); b.writeUInt32LE(rate * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34);
  b.write('data', 36); b.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) b.writeInt16LE(Math.round(Math.sin(2 * Math.PI * freq * i / rate) * 12000), 44 + i * 2);
  return b;
}

// ---- buffer mode, same as the app's V() (incl. -5 retry with doubled buffer) ----
function bufferMode(wav, initialSize) {
  const sig = signalCode(d.signal);
  let size = initialSize ?? Number(P(wav, BigInt(wav.length), d.bitrate));
  let retries = 0;
  for (let attempt = 0; attempt <= 2; attempt++) {
    const out = Buffer.alloc(size);
    const outSize = [BigInt(0)], encTime = [0], actualBitrate = [0], ratio = [0];
    const rc = C(wav, BigInt(wav.length), d.bitrate, d.frameSize, d.vbr ? 1 : 0, sig, out, BigInt(out.length),
      outSize, encTime, actualBitrate, ratio);
    if (rc === -5 && attempt < 2) { retries = attempt + 1; size *= 2; continue; }
    if (rc !== 0) return { success: false, nativeErrorCode: rc, bufferRetryCount: retries };
    const n = Number(outSize[0]);
    return { success: true, encoded: out.subarray(0, n), outputSize: n, encodingTime: encTime[0],
      actualBitrate: actualBitrate[0], ratio: ratio[0], bufferRetryCount: retries, estimate: size };
  }
}

// ---- file mode, same as the app's F() ----
function fileMode(wav, name) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'opus_temp-'));
  const wavPath = path.join(dir, name + '.wav'), oggPath = path.join(dir, name + '.ogg');
  try {
    fs.writeFileSync(wavPath, wav);
    const encTime = [0], outSize = [0], actualBitrate = [0], ratio = [0];
    const rc = z(wavPath, oggPath, d.bitrate, d.frameSize, d.vbr ? 1 : 0, signalCode(d.signal), encTime, outSize, actualBitrate, ratio);
    if (rc !== 0) return { success: false, nativeErrorCode: rc };
    const encoded = fs.readFileSync(oggPath);
    return { success: true, encoded, outputSize: Number(outSize[0]), encodingTime: encTime[0], actualBitrate: actualBitrate[0], ratio: ratio[0] };
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

const have = bin => { try { execFileSync('sh', ['-c', 'command -v ' + bin], { stdio: 'ignore' }); return true; } catch { return false; } };

function validateOgg(label, ogg, expectEncoder) {
  check(ogg.subarray(0, 4).toString() === 'OggS', `${label}: starts with OggS`);
  check(ogg.includes('OpusHead'), `${label}: contains OpusHead`);
  check(ogg.includes('OpusTags'), `${label}: contains OpusTags`);
  check(ogg.includes('ENCODER=' + expectEncoder), `${label}: ENCODER=${expectEncoder} comment`);
  const head = ogg.indexOf('OpusHead');
  check(ogg.readUInt8(head + 9) === 1 && ogg.readUInt32LE(head + 12) === 16000, `${label}: OpusHead channels=1 input_rate=16000`);
  const tmp = path.join(os.tmpdir(), `opusenc-test-${process.pid}-${label}.ogg`);
  fs.writeFileSync(tmp, ogg);
  try {
    if (have('ffprobe')) {
      const j = JSON.parse(execFileSync('ffprobe', ['-v', 'error', '-show_streams', '-show_format', '-of', 'json', tmp]).toString());
      const s = j.streams[0];
      console.log(`     ffprobe: format=${j.format.format_name} codec=${s.codec_name} ch=${s.channels} rate=${s.sample_rate} dur=${j.format.duration}`);
      check(j.format.format_name === 'ogg' && s.codec_name === 'opus' && s.channels === 1, `${label}: ffprobe sees ogg/opus mono`);
      check(Math.abs(parseFloat(j.format.duration) - 2) < 0.05, `${label}: duration ~2.0 s`);
    }
    if (have('ffmpeg')) {
      const pcm = execFileSync('ffmpeg', ['-v', 'error', '-i', tmp, '-f', 's16le', '-ac', '1', '-ar', '16000', '-'], { maxBuffer: 1 << 26 });
      const n = pcm.length / 2;
      // Goertzel power at 440 Hz vs. total energy over the middle second
      let s1 = 0, s2 = 0, energy = 0; const k = 2 * Math.cos(2 * Math.PI * 440 / 16000);
      const a = Math.floor(n / 4), b = Math.floor(3 * n / 4);
      for (let i = a; i < b; i++) { const x = pcm.readInt16LE(i * 2); const s0 = x + k * s1 - s2; s2 = s1; s1 = s0; energy += x * x; }
      const p440 = (s1 * s1 + s2 * s2 - k * s1 * s2) / (b - a);
      const rms = Math.sqrt(energy / (b - a));
      console.log(`     decoded ${n} samples, rms=${rms.toFixed(0)} (input ~8485), 440Hz energy share=${(2 * p440 / energy).toFixed(3)}`);
      check(n > 31000 && n < 33000 && rms > 6000 && 2 * p440 / energy > 0.9, `${label}: decodes back to a 440 Hz tone`);
    }
  } finally { fs.rmSync(tmp, { force: true }); }
}

console.log(`lib: ${libFile()}`);
console.log(`versions: ${ver()} | ${opeVer()} | ${opusVer()}`);

const wav = makeWav();
const est = Number(P(wav, BigInt(wav.length), d.bitrate));
console.log(`estimate for ${wav.length} B WAV @ ${d.bitrate} bps: ${est}`);
check(est === 8192, 'estimate = max(8192, bitrate*dur/8*1.2+512)');

const r1 = bufferMode(wav);
console.log('buffer mode:', { success: r1.success, rc: r1.nativeErrorCode, outputSize: r1.outputSize, encodingTime: r1.encodingTime, actualBitrate: r1.actualBitrate, ratio: r1.ratio, retries: r1.bufferRetryCount });
check(r1.success, 'buffer mode encode returns 0');
if (r1.success) {
  check(r1.actualBitrate === d.bitrate, 'actual bitrate reported = 16000');
  check(Math.abs(r1.ratio - wav.length / r1.outputSize) < 1e-3, 'compression ratio = input/output');
  validateOgg('buffer', r1.encoded, 'libopusenc-unified-buffer');
}

const r2 = bufferMode(wav, 1024); // force the -5 / doubling retry path
check(!r2.success && r2.nativeErrorCode === -5 && r2.bufferRetryCount === 2, `tiny buffer -> -5 after 2 retries (got ${r2.nativeErrorCode})`);
const r3 = bufferMode(wav, 2048); // 2048 -> 4096 -> 8192
check(r3.success && r3.bufferRetryCount >= 1, `retry-with-doubled-buffer succeeds (retries=${r3.bufferRetryCount}, size=${r3.outputSize})`);
if (r1.success && r3.success) check(r3.outputSize === r1.outputSize, 'retry output identical size to first run');

const bad = Buffer.from(wav); bad.write('JUNK', 0);
check(C(bad, BigInt(bad.length), 16000, 20, 1, 0, Buffer.alloc(8192), 8192n, [0n], [0], [0], [0]) === -2, 'bad header -> -2');
check(C(wav, 10n, 16000, 20, 1, 0, Buffer.alloc(8192), 8192n, [0n], [0], [0], [0]) === -6, 'short input -> -6');

const r4 = fileMode(wav, 'temp_audio_' + Date.now());
console.log('file mode:', { success: r4.success, rc: r4.nativeErrorCode, outputSize: r4.outputSize, encodingTime: r4.encodingTime, actualBitrate: r4.actualBitrate, ratio: r4.ratio });
check(r4.success, 'file mode (opus_convert_advanced) returns 0');
if (r4.success) {
  check(r4.outputSize === r4.encoded.length, 'reported output size matches file');
  validateOgg('file', r4.encoded, 'libopusenc-unified');
  check(r4.encoded.includes('OPUS_BITRATE=16000 bps') && r4.encoded.includes('OPUS_FRAME_SIZE=20.0 ms'), 'file: OPUS_BITRATE / OPUS_FRAME_SIZE comments');
}

console.log(failures ? `\n${failures} FAILURE(S)` : '\nALL CHECKS PASSED');
process.exit(failures ? 1 : 0);
