// Headless koffi harness for the arm64 libUtilHelper.so, using the same
// declarations as Typeless' main-process wrapper.
// Side effects are kept small: getDeviceId uses a throwaway app name and a
// temp XDG_CONFIG_HOME; mute/unmute only touches a silent pacat stream for a
// moment; playSoundFile plays an all-zero (silent) WAV.
const koffi = require('koffi');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execSync, spawn } = require('child_process');

const LIB = process.env.UTIL_LIB ||
  path.resolve(__dirname, '../../out/util-helper/build/linux/arm64/libUtilHelper.so');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'uh-test-'));
process.env.XDG_CONFIG_HOME = tmp; // getDeviceId cache file goes here

let failures = 0;
const check = (cond, msg) => { console.log(`${cond ? 'PASS' : 'FAIL'} ${msg}`); if (!cond) failures++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const lib = koffi.load(LIB);
const proto = koffi.proto('void AudioMonitorCallback(char*)');
const cbPtr = koffi.pointer(proto);
const f = {
  hello: lib.func('util_say_hello', 'str', []),
  launch: lib.func('launchApplicationByName', 'int', ['str']),
  mute: lib.func('muteAudio', 'int', []),
  unmute: lib.func('unmuteAudio', 'int', []),
  isMuted: lib.func('isAudioMuted', 'int', []),
  play: lib.func('playSoundFile', 'int', ['str']),
  lid: lib.func('deviceIsLidOpen', 'int32_t', []),
  devs: lib.func('getAudioDevicesJSON', 'str', []),
  devsAsync: lib.func('getAudioDevicesJSONAsync', 'void', [cbPtr]),
  wmi: lib.func('queryWMIAsync', 'void', ['str', 'str', cbPtr]),
  deviceId: lib.func('getDeviceId', 'str', ['str']),
  a11y: lib.func('checkAccessibilityPermission', 'int', []),
  input: lib.func('checkInputDevicesPermission', 'int', []),
};

function silentWav(file, ms = 150, rate = 48000) {
  const n = Math.round(rate * ms / 1000);
  const b = Buffer.alloc(44 + n * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * 2, 4); b.write('WAVE', 8);
  b.write('fmt ', 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22);
  b.writeUInt32LE(rate, 24); b.writeUInt32LE(rate * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34);
  b.write('data', 36); b.writeUInt32LE(n * 2, 40);
  fs.writeFileSync(file, b);
}

function sinkInputMute(name) {
  const out = execSync('pactl -f json list sink-inputs').toString();
  const si = JSON.parse(out).find((s) => s.properties && s.properties['media.name'] === name);
  return si ? si.mute : undefined;
}

// Keep the event loop alive while native threads may still call back (the
// Electron main process always has a live loop).
const keepAlive = setInterval(() => {}, 1000);

(async () => {
  check(f.hello() === 'Hello from UtilHelper Linux shared library!', 'util_say_hello');

  // --- audio devices (sync) ---
  const t0 = Date.now();
  const json = f.devs();
  const d = JSON.parse(json);
  console.log(`getAudioDevicesJSON (${Date.now() - t0} ms):`);
  for (const k of ['inputDevices', 'outputDevices'])
    for (const e of d[k]) console.log(`  ${k[0]} ${e.isDefault ? '*' : ' '} ${e.deviceID} | ${e.name} | ${e.manufacturer} | ${e.transportType} | ${e.sampleRate}Hz/${e.channels}ch vol=${e.volume} muted=${e.isMuted} alive=${e.isAlive}`);
  console.log(`  defaultInputDeviceID=${d.defaultInputDeviceID} defaultOutputDeviceID=${d.defaultOutputDeviceID}`);
  const defSrc = execSync('pactl get-default-source').toString().trim();
  check(d.defaultInputDeviceID === defSrc, 'defaultInputDeviceID matches pactl default source');
  check(d.inputDevices.every((e) => !e.deviceID.endsWith('.monitor')), 'no .monitor sources in inputDevices');
  check(d.inputDevices.filter((e) => e.isDefault).length === 1, 'exactly one default input');
  console.log('  raw head:', json.slice(0, 200));

  // --- audio devices (async, callback from a native thread) ---
  const asyncJson = await new Promise((resolve) => {
    const cb = koffi.register((s) => resolve(s), cbPtr);
    f.devsAsync(cb);
  });
  check(JSON.parse(asyncJson).inputDevices.length === d.inputDevices.length, 'getAudioDevicesJSONAsync delivers JSON via koffi callback');

  const wmi = await new Promise((resolve) => { const cb = koffi.register((s) => resolve(s), cbPtr); f.wmi('Win32_Foo', null, cb); });
  check(wmi === '{"error": "WMI query is only available on Windows"}', 'queryWMIAsync error callback');

  // --- device id ---
  const app = 'TypelessArm64PortTest';
  const id1 = f.deviceId(app);
  const cache = path.join(tmp, app, 'Cache', 'device.cache');
  check(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(id1), `getDeviceId -> ${id1}`);
  check(fs.existsSync(cache) && fs.readFileSync(cache, 'utf8') === id1, 'device.cache written (no newline)');
  let secret = '';
  try { secret = execSync(`secret-tool lookup service ${app}.deviceIdentifier account ${app}.security.auth_key`).toString(); } catch {}
  check(secret === id1, 'libsecret entry stored');
  check(f.deviceId(app) === id1, 'getDeviceId stable');
  fs.unlinkSync(cache);
  check(f.deviceId(app) === id1 && fs.readFileSync(cache, 'utf8') === id1, 'cache file restored from secret store');
  try { execSync(`secret-tool clear service ${app}.deviceIdentifier account ${app}.security.auth_key`); } catch {}
  check(f.deviceId(app) === id1, 'secret restored from cache file');
  try { execSync(`secret-tool clear service ${app}.deviceIdentifier account ${app}.security.auth_key`); } catch {}

  // --- lid / permissions ---
  console.log('deviceIsLidOpen =', f.lid());
  const inputPerm = f.input();
  let expectInput = 0;
  try { fs.accessSync('/dev/uinput', fs.constants.W_OK); expectInput = 1; } catch {}
  console.log('checkInputDevicesPermission =', inputPerm, '(uinput writable:', !!expectInput, ')');
  if (!expectInput) check(inputPerm === 0, 'input permission 0 without uinput access');
  const ta = Date.now();
  const a11y = f.a11y();
  console.log(`checkAccessibilityPermission = ${a11y} (${Date.now() - ta} ms)`);

  // --- mute / unmute (ducking) with a silent pacat stream ---
  check(f.isMuted() === 0, 'isAudioMuted initially 0');
  const pac = spawn('pacat', ['--playback', '--client-name=uh-porttest', '--stream-name=uh-porttest-silence', '/dev/zero'], { stdio: 'ignore' });
  await sleep(800);
  check(sinkInputMute('uh-porttest-silence') === false, 'silent test stream present & unmuted');
  const mutedRc = f.mute();
  const afterMute = sinkInputMute('uh-porttest-silence');
  const isM = f.isMuted();
  const unmuteRc = f.unmute();
  const afterUnmute = sinkInputMute('uh-porttest-silence');
  check(mutedRc === 1 && afterMute === true && isM === 1, `muteAudio ducks other app streams (rc=${mutedRc}, muted=${afterMute}, isAudioMuted=${isM})`);
  check(unmuteRc === 1 && afterUnmute === false && f.isMuted() === 0, `unmuteAudio restores (rc=${unmuteRc}, muted=${afterUnmute})`);
  pac.kill();

  // --- async (worker thread) variants as the app uses them ---
  const asyncMute = await new Promise((res, rej) => f.isMuted.async((e, r) => (e ? rej(e) : res(r))));
  check(asyncMute === 0, 'isAudioMuted.async');

  // --- playSoundFile ---
  const wav = path.join(tmp, 'silence.wav');
  silentWav(wav);
  const tp = Date.now();
  const playRc = await new Promise((res, rej) => f.play.async(wav, (e, r) => (e ? rej(e) : res(r))));
  check(playRc === 1, `playSoundFile(silent wav) -> ${playRc} (${Date.now() - tp} ms)`);
  fs.writeFileSync(path.join(tmp, 'bad.wav'), 'not a wav file at all');
  check(f.play(path.join(tmp, 'bad.wav')) === 0, 'playSoundFile(invalid) -> 0');
  check(f.play('') === 0, 'playSoundFile("") -> 0');

  fs.rmSync(tmp, { recursive: true, force: true });
  console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
  process.exit(failures ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
