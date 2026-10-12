#!/usr/bin/env python3
"""Sample guest PCs over QMP and histogram them by kernel symbol: pcsample.py SOCK N"""
import bisect, json, socket, sys, time, re
from collections import Counter
sock, n = sys.argv[1], int(sys.argv[2])
syms = []
for l in open('/home/davi/clones/hd2-qemu/build-tmp/syms.txt'):
    p = l.split()
    if len(p) == 3:
        syms.append((int(p[0], 16), p[2]))
syms.sort(); keys = [a for a, _ in syms]
s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile('rw')
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        m = json.loads(f.readline())
        if 'return' in m or 'error' in m:
            return m
json.loads(f.readline()); cmd("qmp_capabilities")
c = Counter()
for _ in range(n):
    r = cmd("human-monitor-command", **{"command-line": "info registers"})['return']
    pc = int(re.search(r'R15=([0-9a-f]+)', r).group(1), 16)
    if pc >= 0xc0000000:
        c[syms[bisect.bisect_right(keys, pc) - 1][1]] += 1
    else:
        c['user:%08x' % (pc & ~0xfff)] += 1
    time.sleep(0.05)
for k, v in c.most_common(20):
    print('%4d %s' % (v, k))
