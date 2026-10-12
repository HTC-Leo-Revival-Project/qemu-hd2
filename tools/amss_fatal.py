#!/usr/bin/env python3
"""Find the ERR_FATAL call sites for FILE:LINE in AMSS.

usage: amss_fatal.py amss.elf sleepctl.c 2490

Looks for literal-pool words pointing at the file name string and, within
a small window around each, the line number; prints the code that loads
them so the failing condition can be read with amss_dis.py.
"""
import re
import struct
import sys

e = open(sys.argv[1], 'rb').read()
name, line = sys.argv[2].encode(), int(sys.argv[3], 0)
phoff, = struct.unpack_from('<I', e, 28)
n, = struct.unpack_from('<H', e, 44)
segs = [struct.unpack_from('<8I', e, phoff + 32 * k) for k in range(n)]


def va_of(fo):
    for t, off, va, pa, fs, ms, fl, al in segs:
        if off <= fo < off + fs:
            return va + fo - off


def fo_of(va):
    for t, off, sva, pa, fs, ms, fl, al in segs:
        if sva <= va < sva + fs:
            return off + va - sva


strs = []
for m in re.finditer(re.escape(name) + b'\0', e):
    s = m.start()
    while s > 0 and 0x20 <= e[s - 1] < 0x7f:
        s -= 1
    for start in {s, m.start()}:
        v = va_of(start)
        if v is not None:
            strs.append(v)
for sv in strs:
    for m in re.finditer(re.escape(struct.pack('<I', sv)), e):
        if m.start() % 2:
            continue
        lit = va_of(m.start())
        win = e[m.start() - 64:m.start() + 64]
        hit = [i for i in range(0, len(win) - 3, 4)
               if struct.unpack_from('<I', win, i)[0] == line]
        if hit:
            print('string %#x literal at %#x (line literal nearby)' % (sv, lit))
        else:
            print('string %#x literal at %#x' % (sv, lit))
