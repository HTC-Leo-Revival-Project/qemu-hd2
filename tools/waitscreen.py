#!/usr/bin/env python3
"""Poll screendumps until the emulated panel shows something: waitscreen.py SOCK MAX_S"""
import json, os, socket, sys, time
from PIL import Image
sock, mx = sys.argv[1], float(sys.argv[2])
s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile('rw')
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        m = json.loads(f.readline())
        if 'return' in m or 'error' in m:
            return m
json.loads(f.readline()); cmd("qmp_capabilities")
t0 = time.time()
while time.time() - t0 < mx:
    p = os.path.abspath('w.ppm'); cmd("screendump", filename=p)
    im = Image.open(p).convert('L')
    if im.getextrema()[1] > 20:
        Image.open(p).save('w.png'); print('content after %.0fs' % (time.time() - t0)); sys.exit(0)
    time.sleep(10)
Image.open(p).save('w.png'); print('still black'); sys.exit(1)
