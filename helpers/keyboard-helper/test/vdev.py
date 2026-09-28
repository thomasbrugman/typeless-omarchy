#!/usr/bin/env python3
"""Test helper: creates uinput virtual input devices and injects harmless keys.
Commands on stdin (one per line):
  create <name> <kbd|mic>   destroy <name>
  key <name> <code> <value>  (value 0/1/2; each followed by SYN_REPORT)
  watch                      read events from all "Typeless Keyboard Helper" nodes
  quit
Output lines: "ok ...", "clone <node> <type> <code> <value>".
On exit, every key still down on our devices is released first."""
import fcntl, os, struct, sys, threading, time, glob, select, ctypes

UI_SET_EVBIT=0x40045564; UI_SET_KEYBIT=0x40045565; UI_SET_MSCBIT=0x40045568
UI_DEV_SETUP=0x405c5503; UI_DEV_CREATE=0x5501; UI_DEV_DESTROY=0x5502
EV_SYN,EV_KEY,EV_MSC=0,1,4
KBD_KEYS=[1,28,29,30,42,183,184,185,186,187,188,189,190,191,192,193,194]
MIC_KEYS=[114,115,163,164,165,582]
devs={}; down={}
lock=threading.Lock()
def out(s):
    with lock: sys.stdout.write(s+"\n"); sys.stdout.flush()
def create(name, kind):
    fd=os.open('/dev/uinput', os.O_WRONLY|os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY); fcntl.ioctl(fd, UI_SET_EVBIT, EV_MSC)
    fcntl.ioctl(fd, UI_SET_MSCBIT, 4)
    for k in (KBD_KEYS if kind=='kbd' else MIC_KEYS): fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    setup=struct.pack('HHHH80sI',3,0x1234,0x5678 if kind=='kbd' else 0x5679,1,name.encode(),0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup); fcntl.ioctl(fd, UI_DEV_CREATE)
    devs[name]=fd; down[name]=set()
def emit(fd, t, c, v):
    now=time.time(); s=int(now); us=int((now-s)*1e6)
    os.write(fd, struct.pack('qqHHi',s,us,t,c,v)+struct.pack('qqHHi',s,us,0,0,0))
def key(name,code,val):
    emit(devs[name],EV_KEY,code,val)
    if val==1: down[name].add(code)
    elif val==0: down[name].discard(code)
def destroy(name, release=True):
    fd=devs.pop(name)
    if release:
        for c in list(down.get(name,())): emit(fd,EV_KEY,c,0)
    down.pop(name,None)
    fcntl.ioctl(fd, UI_DEV_DESTROY); os.close(fd)
def devname(p):
    try: return open('/sys/class/input/%s/device/name'%os.path.basename(p)).read().strip()
    except Exception: return ''
def watcher():
    fds={}
    while True:
        for p in glob.glob('/dev/input/event*'):
            if p not in fds and devname(p)=='Typeless Keyboard Helper':
                try: fds[p]=os.open(p, os.O_RDONLY|os.O_NONBLOCK)
                except OSError: pass
        r,_,_=select.select(list(fds.values()),[],[],0.05)
        for p,fd in list(fds.items()):
            if fd in r:
                try: data=os.read(fd,24*64)
                except OSError: os.close(fd); del fds[p]; continue
                for i in range(0,len(data),24):
                    _,_,t,c,v=struct.unpack('qqHHi',data[i:i+24])
                    if t!=EV_SYN: out("clone %s %d %d %d"%(os.path.basename(p),t,c,v))
try:
    for line in sys.stdin:
        a=line.split()
        if not a: continue
        try:
            if a[0]=='create': create(a[1],a[2])
            elif a[0]=='destroy': destroy(a[1], len(a)<3)
            elif a[0]=='key': key(a[1],int(a[2]),int(a[3]))
            elif a[0]=='flood':
                # key <a[2]> released, then enough F21 taps in ONE write to overflow
                # the evdev client buffer so the release above is lost (SYN_DROPPED)
                fd=devs[a[1]]; rel=int(a[2]); pk=b''
                def ev(t,c,v): return struct.pack('qqHHi',0,0,t,c,v)
                pk+=ev(EV_KEY,rel,0)+ev(0,0,0); down[a[1]].discard(rel)
                for _ in range(int(a[3])): pk+=ev(EV_KEY,191,1)+ev(0,0,0)+ev(EV_KEY,191,0)+ev(0,0,0)
                os.write(fd,pk)
            elif a[0]=='clonekeys':
                res=[]
                for p in glob.glob('/dev/input/event*'):
                    if devname(p)=='Typeless Keyboard Helper':
                        fd=os.open(p,os.O_RDONLY|os.O_NONBLOCK); b=bytearray(96)
                        fcntl.ioctl(fd,0x80604518,b); os.close(fd)
                        res+=[i for i in range(768) if b[i//8]>>(i%8)&1]
                out("clonekeys "+",".join(map(str,res)))
            elif a[0]=='watch': threading.Thread(target=watcher,daemon=True).start()
            elif a[0]=='quit': break
            out("ok "+line.strip())
        except Exception as e: out("err %s %s"%(line.strip(),e))
finally:
    for n in list(devs): destroy(n)
