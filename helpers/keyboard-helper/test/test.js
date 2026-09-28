// Headless koffi harness for libKeyboardHelper.so. Only touches virtual
// uinput devices it creates itself (KEYBOARD_HELPER_DEVICE_FILTER=kh-test);
// injects only F22/F23/F24 and KEY_VOICECOMMAND.
process.env.KEYBOARD_HELPER_DEVICE_FILTER = process.env.KEYBOARD_HELPER_DEVICE_FILTER || 'kh-test';
const koffi = require('koffi');
const path = require('path');
const { spawn } = require('child_process');

const LIB = process.argv[2] || path.join(__dirname, '../../out/keyboard-helper/build/linux/arm64/libKeyboardHelper.so');
const lib = koffi.load(LIB);
const proto = koffi.proto('bool KeyboardCallback(int32_t, int32_t, int32_t, char*)');
const f = {
  hello: lib.func('keyboard_say_hello', 'string', []),
  update: lib.func('updateTargetShortcuts', 'void', ['char*']),
  reset: lib.func('resetPressingKeycodes', 'void', []),
  start: lib.func('startMonitor', 'bool', [koffi.pointer(proto)]),
  proc: lib.func('processEvents', 'void', []),
  stop: lib.func('stopMonitor', 'void', []),
  interval: lib.func('setWatcherInterval', 'void', ['double']),
  layout: lib.func('getKeyboardLayoutInfo', 'string', ['bool']),
  devices: lib.func('getKeyboardDeviceList', 'string', []),
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const check = (cond, msg) => { console.log((cond ? 'PASS ' : 'FAIL ') + msg); if (!cond) failures++; };

const events = [];
const cb = koffi.register((a, b, c, json) => {
  events.push({ keyCode: a, isDown: b, isBlocked: c, keys: JSON.parse(json) });
  return true;
}, koffi.pointer(proto));

const vdev = spawn('python3', [path.join(__dirname, 'vdev.py')], { stdio: ['pipe', 'pipe', 'inherit'] });
const cloneEvents = [];
let cloneKeys = null;
let buf = '';
vdev.stdout.on('data', (d) => {
  buf += d;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i); buf = buf.slice(i + 1);
    if (line.startsWith('clone ')) { const [, node, t, c, v] = line.split(' '); cloneEvents.push({ node, t: +t, c: +c, v: +v }); }
    else if (line.startsWith('clonekeys')) cloneKeys = line.slice(10);
    else if (line.startsWith('err')) console.log('vdev:', line);
  }
});
const cmd = (s) => vdev.stdin.write(s + '\n');
const lastEvent = () => events[events.length - 1];
const names = (e) => (e ? e.keys.map((k) => k.keyName) : null);

(async () => {
  try {
    check(f.hello() === 'Hello from C++ shared library - Keyboard Helper (Linux)!', 'keyboard_say_hello');
    const lay = JSON.parse(f.layout(false));
    check(Array.isArray(lay) && lay[0].layoutId && lay[0].keys === undefined, 'layout info (simple): ' + JSON.stringify(lay));
    const layd = JSON.parse(f.layout(true));
    check(layd[0].keys.length === 48 && layd[0].keys[0].keyName === '1', 'layout info (detail) 48 keys, first=' + JSON.stringify(layd[0].keys[0]));
    const devs = JSON.parse(f.devices());
    check(Array.isArray(devs), 'device list (filtered to kh-test, before devices exist): ' + JSON.stringify(devs));
    f.proc();

    cmd('create kh-test-kbd kbd');
    cmd('create kh-test-mic mic');
    cmd('watch');
    await sleep(700);
    const devs2 = JSON.parse(f.devices());
    check(devs2.some((d) => d.deviceName === 'kh-test-kbd' && d.isBuiltIn === false) && !devs2.some((d) => d.deviceName === 'kh-test-mic'),
      'device list lists only full keyboards: ' + JSON.stringify(devs2));

    check(f.start(null) === false, 'startMonitor(null) -> false');
    check(f.start(cb) === true, 'startMonitor(cb) -> true');
    check(f.start(cb) === false, 'second startMonitor -> false');
    f.update(JSON.stringify([['F23'], ['F24', 'F22'], ['VoiceCommand']]));
    await sleep(300);

    // plain key: reported and forwarded to the clone
    events.length = 0; cloneEvents.length = 0;
    cmd('key kh-test-kbd 192 1'); await sleep(150);
    check(lastEvent() && lastEvent().keyCode === 192 && lastEvent().isDown === 1 && lastEvent().isBlocked === 0 &&
      JSON.stringify(lastEvent().keys) === '[{"keyCode":192,"keyName":"F22","enKeyName":"F22"}]', 'F22 down reported: ' + JSON.stringify(lastEvent()));
    cmd('key kh-test-kbd 192 0'); await sleep(150);
    check(lastEvent() && lastEvent().isDown === 0 && lastEvent().keys.length === 0, 'F22 up reported with empty list');
    check(cloneEvents.some((e) => e.t === 1 && e.c === 192 && e.v === 1) && cloneEvents.some((e) => e.t === 1 && e.c === 192 && e.v === 0),
      'F22 forwarded through uinput clone: ' + JSON.stringify(cloneEvents));

    // single-key target shortcut F23 is swallowed
    events.length = 0; cloneEvents.length = 0;
    cmd('key kh-test-kbd 193 1'); await sleep(150);
    check(lastEvent() && lastEvent().keyCode === 193 && lastEvent().isBlocked === 1 && names(lastEvent()).join() === 'F23', 'F23 (target) down reported as blocked');
    cmd('key kh-test-kbd 193 2'); await sleep(100);
    cmd('key kh-test-kbd 193 0'); await sleep(150);
    check(lastEvent() && lastEvent().isDown === 0 && lastEvent().isBlocked === 1, 'F23 up reported as blocked');
    check(!cloneEvents.some((e) => e.c === 193), 'F23 NOT forwarded to clone: ' + JSON.stringify(cloneEvents));

    // two-key shortcut F22+F24: F22 goes through (pressed before match), F24 swallowed
    events.length = 0; cloneEvents.length = 0;
    cmd('key kh-test-kbd 192 1'); await sleep(80);
    cmd('key kh-test-kbd 194 1'); await sleep(120);
    check(names(lastEvent()).join('+') === 'F22+F24' && lastEvent().isBlocked === 1, 'F22+F24 pressing list order and F24 blocked: ' + JSON.stringify(lastEvent()));
    cmd('key kh-test-kbd 194 0'); await sleep(80);
    cmd('key kh-test-kbd 192 0'); await sleep(150);
    check(cloneEvents.some((e) => e.c === 192 && e.v === 1) && cloneEvents.some((e) => e.c === 192 && e.v === 0) && !cloneEvents.some((e) => e.c === 194),
      'F22 forwarded, F24 swallowed: ' + JSON.stringify(cloneEvents));

    // mic button (non-keyboard device, passive)
    events.length = 0; cloneEvents.length = 0;
    cmd('key kh-test-mic 582 1'); await sleep(150);
    check(lastEvent() && lastEvent().keyCode === 582 && lastEvent().isBlocked === 0 && names(lastEvent()).join() === 'VoiceCommand', 'VoiceCommand from mic-like device reported: ' + JSON.stringify(lastEvent()));
    cmd('key kh-test-mic 582 0'); await sleep(150);
    check(lastEvent() && lastEvent().isDown === 0 && lastEvent().keys.length === 0, 'VoiceCommand release reported');
    check(cloneEvents.length === 0, 'passive device not cloned');

    // hotplug
    cmd('create kh-test-kbd2 kbd'); await sleep(600);
    events.length = 0;
    cmd('key kh-test-kbd2 192 1'); await sleep(150); cmd('key kh-test-kbd2 192 0'); await sleep(150);
    check(events.length === 2 && events[0].keyCode === 192, 'hot-plugged keyboard monitored');

    cmd('destroy kh-test-kbd2'); await sleep(300);
    // lost release (SYN_DROPPED): resync must drop the stale key and release it on the clone
    f.interval(0.3);
    events.length = 0; cloneEvents.length = 0;
    cmd('key kh-test-kbd 194 1'); await sleep(150);
    check(names(lastEvent()).join() === 'F24', 'F24 held');
    cmd('flood kh-test-kbd 194 250'); await sleep(800);
    const rec = events.find((e) => e.keyCode === -9999);
    check(rec && rec.isDown === 0 && rec.isBlocked === 0 && !names(rec).includes('F24'), 'stale F24 reconciled after SYN_DROPPED: ' + JSON.stringify(rec));
    cloneKeys = null; cmd('clonekeys'); await sleep(200);
    check(cloneKeys === '', 'no key left down on the uinput clone (EVIOCGKEY): "' + cloneKeys + '"');
    check(!names(lastEvent()).includes('F24'), 'final pressing list has no F24: ' + JSON.stringify(lastEvent()));

    // resetPressingKeycodes -> synchronous -9999 x3 "[]"
    events.length = 0;
    cmd('key kh-test-kbd 192 1'); await sleep(150);
    f.reset();
    check(events.some((e) => e.keyCode === -9999 && e.isDown === -9999 && e.isBlocked === -9999 && e.keys.length === 0), 'resetPressingKeycodes sentinel');
    cmd('key kh-test-kbd 192 0'); await sleep(150);

    f.stop();
    await sleep(300);
    events.length = 0;
    cmd('key kh-test-kbd 192 1'); await sleep(100); cmd('key kh-test-kbd 192 0'); await sleep(150);
    check(events.length === 0, 'no callbacks after stopMonitor');

    // restart works, shortcuts were cleared by start (original behaviour)
    check(f.start(cb) === true, 'restart');
    await sleep(300);
    events.length = 0;
    cmd('key kh-test-kbd 193 1'); await sleep(120); cmd('key kh-test-kbd 193 0'); await sleep(120);
    check(events.length === 2 && events[0].isBlocked === 0, 'targets cleared by start -> F23 not blocked');
    // stop while the JS thread is busy must not deadlock
    cmd('key kh-test-kbd 192 1'); cmd('key kh-test-kbd 192 0');
    const t0 = Date.now(); while (Date.now() - t0 < 200) {} // block JS thread while callbacks queue
    f.stop();
    check(true, 'stopMonitor returned while dispatcher had pending callbacks');
  } catch (e) {
    console.error(e); failures++;
  } finally {
    cmd('quit');
    await sleep(300);
    console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASSED');
    process.exit(failures ? 1 : 0);
  }
})();
