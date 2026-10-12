#!/usr/bin/env python3
"""Send key presses via QMP: keys.py SOCK key[:holdms] ... (qcode names, 'sleep:ms')"""
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rw')
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        m = json.loads(f.readline())
        if 'return' in m or 'error' in m:
            return m
json.loads(f.readline()); cmd("qmp_capabilities")
for k in sys.argv[2:]:
    name, _, ms = k.partition(':')
    if name == 'sleep':
        time.sleep(int(ms) / 1000); continue
    hold = int(ms or 200)
    ev = lambda down: {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": name}}}
    cmd("input-send-event", events=[ev(True)]); time.sleep(hold / 1000)
    cmd("input-send-event", events=[ev(False)]); time.sleep(0.3)
