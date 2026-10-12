#!/usr/bin/env python3
"""Touch the emulated screen via QMP: touch.py SOCK x,y[,holdms] | swipe:x0,y0,x1,y1[,ms] | sleep:ms ...
Coordinates are screen pixels (480x800)."""
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rw')
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        m = json.loads(f.readline())
        if 'return' in m or 'error' in m:
            return m
json.loads(f.readline()); cmd("qmp_capabilities")
W, H = 480, 800
def move(x, y):
    cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7fff / (W - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7fff / (H - 1))}}])
def btn(down):
    cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
for a in sys.argv[2:]:
    if a.startswith('sleep:'):
        time.sleep(int(a[6:]) / 1000); continue
    if a.startswith('swipe:'):
        v = [int(t) for t in a[6:].split(',')]
        x0, y0, x1, y1 = v[:4]; ms = v[4] if len(v) > 4 else 300
        move(x0, y0); btn(True); n = max(2, ms // 20)
        for i in range(1, n + 1):
            time.sleep(ms / 1000 / n)
            move(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n)
        btn(False); time.sleep(0.3); continue
    v = [int(t) for t in a.split(',')]
    move(v[0], v[1]); btn(True); time.sleep((v[2] if len(v) > 2 else 120) / 1000)
    btn(False); time.sleep(0.3)
