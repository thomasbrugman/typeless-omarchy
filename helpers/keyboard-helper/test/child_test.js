// Drives the app's real keyboard-helper-child-process.js over IPC (virtual devices only).
const { fork, spawn } = require('child_process');
const path = require('path');
const CHILD = process.argv[2];
const LIB = path.resolve(__dirname, '../../out/keyboard-helper/build/linux/arm64/libKeyboardHelper.so');
const vdev = spawn('python3', [path.join(__dirname, 'vdev.py')], { stdio: ['pipe', 'ignore', 'inherit'] });
const cmd = (s) => vdev.stdin.write(s + '\n');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const msgs = [];
(async () => {
  cmd('create kh-test-kbd kbd'); cmd('create kh-test-mic mic'); await sleep(500);
  const child = fork(CHILD, [], { env: { ...process.env, KEYBOARD_HELPER_LIB_PATH: LIB, KEYBOARD_HELPER_DEVICE_FILTER: 'kh-test' } });
  child.on('message', (m) => msgs.push(m));
  await sleep(500);
  child.send({ type: 'start' });
  child.send({ type: 'setWatcherInterval', payload: 5 });
  child.send({ type: 'setTargetShortcuts', payload: [['F23'], ['VoiceCommand']] });
  child.send({ type: 'getKeyboardLayoutInfo', payload: { needDetail: false, requestId: 7 } });
  child.send({ type: 'getKeyboardDeviceList' });
  await sleep(400);
  cmd('key kh-test-mic 582 1'); await sleep(150); cmd('key kh-test-mic 582 0'); await sleep(150);
  cmd('key kh-test-kbd 193 1'); await sleep(150); cmd('key kh-test-kbd 193 0'); await sleep(150);
  child.send({ type: 'resetPressingKeycodes' });
  await sleep(200);
  child.send({ type: 'destroy' });
  await sleep(500);
  for (const m of msgs) console.log(JSON.stringify(m));
  cmd('quit'); await sleep(200); process.exit(0);
})();
