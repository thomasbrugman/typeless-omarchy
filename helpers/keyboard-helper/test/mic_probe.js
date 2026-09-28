process.env.KEYBOARD_HELPER_DEVICE_FILTER='JIAYZ'; process.env.KEYBOARD_HELPER_DEBUG='1';
const koffi=require('koffi');
const lib=koffi.load(require('path').resolve(__dirname,'../../out/keyboard-helper/build/linux/arm64/libKeyboardHelper.so'));
const proto=koffi.proto('bool KeyboardCallback(int32_t, int32_t, int32_t, char*)');
const start=lib.func('startMonitor','bool',[koffi.pointer(proto)]), stop=lib.func('stopMonitor','void',[]);
const cb=koffi.register((a,b,c,j)=>{console.log('cb',a,b,c,j);return true;},koffi.pointer(proto));
console.log('start',start(cb));
setTimeout(()=>{stop();console.log('stopped');},1500);
