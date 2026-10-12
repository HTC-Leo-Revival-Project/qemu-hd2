#!/usr/bin/env python3
"""Take periodic screendumps through QMP: capture.py SOCK OUTPREFIX INTERVAL COUNT"""
import json, socket, sys, time, os
from PIL import Image
sock, prefix, interval, count = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4])
for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sock); break
    except OSError:
        time.sleep(0.1)
f = s.makefile('rw')
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        m = json.loads(f.readline())
        if 'return' in m or 'error' in m:
            return m
json.loads(f.readline()); cmd("qmp_capabilities")
for i in range(count):
    p = os.path.abspath("%s%03d.ppm" % (prefix, i))
    cmd("screendump", filename=p)
    try:
        Image.open(p).save(p[:-4] + ".png"); os.unlink(p)
    except Exception:
        pass
    time.sleep(interval)
if len(sys.argv) > 5 and sys.argv[5] == "quit":
    cmd("quit")
