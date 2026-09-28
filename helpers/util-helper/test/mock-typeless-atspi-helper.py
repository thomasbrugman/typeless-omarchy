#!/usr/bin/env python3
# Mock of the typeless-atspi-helper wire protocol (test only).
import os, socket, struct, json, sys
d = os.path.join(os.environ.get('XDG_RUNTIME_DIR') or f"/tmp", 'typeless-atspi')
os.makedirs(d, mode=0o700, exist_ok=True)
sp = os.path.join(d, 'helper.sock')
try: os.unlink(sp)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind(sp); s.listen(4)
open(os.path.join(d, 'helper.pid'), 'w').write(f"{os.getpid()}\n{os.getppid()}\n")
log = open(os.path.join(d, 'mock-requests.log'), 'a')
log.write(f"host_pid_env={os.environ.get('TYPELESS_A11Y_HOST_PID')}\n"); log.flush()
while True:
    c, _ = s.accept()
    while True:
        h = c.recv(4)
        if len(h) < 4: break
        n = struct.unpack('<I', h)[0]; buf = b''
        while len(buf) < n: buf += c.recv(n - len(buf))
        req = json.loads(buf); log.write(buf.decode() + "\n"); log.flush()
        resp = json.dumps({"id": req["id"], "ok": True, "error": "none", "failReason": "", "generation": 0,
                           "payload": {"reachable": os.environ.get('MOCK_REACHABLE', '1') == '1'}}, separators=(',', ':')).encode()
        c.sendall(struct.pack('<I', len(resp)) + resp)
    c.close()
