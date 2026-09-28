# Creates a virtual keyboard with a CapsLock LED; the node side starts the lib.
# Then writes EV_LED to the lib's "Typeless Keyboard Helper" clone node and
# checks the LED write is forwarded back to our (physical-stand-in) device.
import fcntl, os, struct, time, glob, subprocess, sys, select
UI_SET_EVBIT=0x40045564; UI_SET_KEYBIT=0x40045565; UI_SET_LEDBIT=0x40045569
fd=os.open('/dev/uinput', os.O_RDWR|os.O_NONBLOCK)
fcntl.ioctl(fd,UI_SET_EVBIT,1); fcntl.ioctl(fd,UI_SET_EVBIT,0x11)
for k in (28,30,194): fcntl.ioctl(fd,UI_SET_KEYBIT,k)
fcntl.ioctl(fd,UI_SET_LEDBIT,1)
fcntl.ioctl(fd,0x405c5503,struct.pack('HHHH80sI',3,0x1234,0x5680,1,b'kh-test-led',0)); fcntl.ioctl(fd,0x5501)
time.sleep(0.4)
node=subprocess.Popen(['mise','exec','node@24','--','node','-e','''
process.env.KEYBOARD_HELPER_DEVICE_FILTER="kh-test-led";
const koffi=require("koffi");const lib=koffi.load(process.argv[1]);
const p=koffi.proto("bool KeyboardCallback(int32_t, int32_t, int32_t, char*)");
const start=lib.func("startMonitor","bool",[koffi.pointer(p)]),stop=lib.func("stopMonitor","void",[]);
const cb=koffi.register(()=>true,koffi.pointer(p)); start(cb); setTimeout(()=>{stop();process.exit(0)},2500);
''','../../out/keyboard-helper/build/linux/arm64/libKeyboardHelper.so'])
time.sleep(0.8)
clone=None
for p in glob.glob('/dev/input/event*'):
    try:
        if open('/sys/class/input/%s/device/name'%os.path.basename(p)).read().strip()=='Typeless Keyboard Helper': clone=p
    except Exception: pass
print('clone node', clone)
cfd=os.open(clone, os.O_WRONLY)
os.write(cfd, struct.pack('qqHHi',0,0,0x11,1,1)+struct.pack('qqHHi',0,0,0,0,0))
got=[]
end=time.time()+1
while time.time()<end:
    r,_,_=select.select([fd],[],[],0.1)
    if r:
        d=os.read(fd,24*16)
        for i in range(0,len(d),24):
            _,_,t,c,v=struct.unpack('qqHHi',d[i:i+24]); got.append((t,c,v))
print('LED events received by stand-in physical device:', [g for g in got if g[0]==0x11])
os.write(cfd, struct.pack('qqHHi',0,0,0x11,1,0)+struct.pack('qqHHi',0,0,0,0,0))
node.wait(); os.close(cfd); fcntl.ioctl(fd,0x5502); os.close(fd)
