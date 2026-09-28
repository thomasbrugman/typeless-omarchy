#!/usr/bin/env python3
"""typeless-clipboard checks against a headless compositor.
usage: WAYLAND_DISPLAY=<headless> tool_test.py [path/to/typeless-clipboard]"""
import os
import struct
import subprocess
import sys

TOOL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "../../out/input-helper/build/linux/arm64/typeless-clipboard")
assert os.environ.get("WAYLAND_DISPLAY") not in (None, "", "wayland-1"), "use a headless compositor"


def frames(entries):
    out = b""
    for mime, data in entries:
        m = mime.encode()
        out += struct.pack("<I", len(m)) + m + struct.pack("<Q", len(data)) + data
    return out


def parse(buf):
    out, i = [], 0
    while i < len(buf):
        (ml,) = struct.unpack_from("<I", buf, i); i += 4
        m = buf[i:i + ml].decode(); i += ml
        (dl,) = struct.unpack_from("<Q", buf, i); i += 8
        out.append((m, buf[i:i + dl])); i += dl
    return out


def run(*args, data=b""):
    return subprocess.run([TOOL, *args], input=data, capture_output=True, timeout=10)


fails = 0


def check(name, cond):
    global fails
    print(("ok   " if cond else "FAIL ") + name)
    fails += 0 if cond else 1


big = os.urandom(3 * 1024 * 1024)
r = run("set", data=frames([("text/plain", b"a"), ("text/plain", b"a"), ("image/bmp", b"BMP"),
                            ("image/png", big), ("image/jpeg", b"JPG"), ("text/uri-list", b"file:///x\r\n")]))
check("set multi-type source", r.returncode == 0)
snap = parse(run("get").stdout)
mimes = [m for m, _ in snap]
check("snapshot dedupes repeated types", mimes.count("text/plain") == 1)
check("snapshot keeps only image/png among images", [m for m in mimes if m.startswith("image/")] == ["image/png"])
check("snapshot orders text first", mimes[0] == "text/plain" and "text/uri-list" in mimes)
check("3 MiB image round-trips byte-exact", dict(snap).get("image/png") == big)
r = run("get", "--mime", "text/html", "--mime", "text/uri-list")
check("--mime picks first available", r.returncode == 0 and parse(r.stdout) == [("text/uri-list", b"file:///x\r\n")])
check("--mime missing -> exit 1", run("get", "--mime", "application/x-nope").returncode == 1)
check("clear", run("clear").returncode == 0)
r = run("get")
check("get on empty clipboard -> exit 1, no frames", r.returncode == 1 and r.stdout == b"")
check("types on empty clipboard -> exit 1", run("types").returncode == 1)
check("set with no frames -> exit 2", run("set").returncode == 2)
check("garbage frames -> exit 2", run("set", data=b"\xff\xff\xff\xff junk").returncode == 2)
env = dict(os.environ, WAYLAND_DISPLAY="")
check("no Wayland display -> exit 3", subprocess.run([TOOL, "types"], env=env, capture_output=True).returncode == 3)
# replace: the old serving process must exit once another client owns the selection
run("set", data=frames([("text/plain", b"first")]))
before = subprocess.run(["pgrep", "-fc", "typeless-clipboard set"], capture_output=True, text=True).stdout.strip()
run("set", data=frames([("text/plain", b"second")]))
subprocess.run(["sleep", "0.2"])
after = subprocess.run(["pgrep", "-fc", "typeless-clipboard set"], capture_output=True, text=True).stdout.strip()
check(f"replaced source exits (serving processes {before} -> {after})", before == after == "1")
run("clear")
subprocess.run(["sleep", "0.2"])
left = subprocess.run(["pgrep", "-fc", "typeless-clipboard set"], capture_output=True, text=True).stdout.strip()
check("clear ends the serving process", left == "0")
sys.exit(1 if fails else 0)
