#!/usr/bin/env python3
"""Fake Hyprland IPC socket for tests: answers j/activewindow with the given JSON.
usage: fake_hypr.py <signature> <json>   (serves until killed)"""
import json, os, socket, sys
sig, reply = sys.argv[1], sys.argv[2]
d = f"{os.environ['XDG_RUNTIME_DIR']}/hypr/{sig}"
os.makedirs(d, exist_ok=True)
p = f"{d}/.socket.sock"
if os.path.exists(p):
    os.unlink(p)
s = socket.socket(socket.AF_UNIX)
s.bind(p)
s.listen(8)
print("ready", flush=True)
try:
    while True:
        c, _ = s.accept()
        req = c.recv(256)
        c.sendall(reply.encode() if req == b"j/activewindow" else b"unknown request")
        c.close()
finally:
    os.unlink(p)
    os.rmdir(d)
