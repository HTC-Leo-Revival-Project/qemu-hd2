#!/usr/bin/env python3
"""Find code that loads a literal-pool word: amss_xref.py amss.elf LIT_VA

Lists Thumb `ldr rX, [pc, #imm]` and ARM `ldr rX, [pc, #imm]` within reach
of the literal.
"""
import struct
import sys

e = open(sys.argv[1], 'rb').read()
lit = int(sys.argv[2], 16)
phoff, = struct.unpack_from('<I', e, 28)
n, = struct.unpack_from('<H', e, 44)
for k in range(n):
    t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<8I', e, phoff + 32 * k)
    if not (va <= lit < va + fs):
        continue
    for a in range(max(va, lit - 1024) & ~1, lit, 2):
        h, = struct.unpack_from('<H', e, off + a - va)
        if h >> 11 == 0b01001 and ((a + 4) & ~3) + 4 * (h & 0xFF) == lit:
            print('thumb ldr r%d at %#x' % ((h >> 8) & 7, a))
    for a in range(max(va, lit - 4096) & ~3, lit, 4):
        w, = struct.unpack_from('<I', e, off + a - va)
        if (w & 0x0F7F0000) == 0x051F0000 and a + 8 + (w & 0xFFF) == lit:
            print('arm ldr r%d at %#x' % ((w >> 12) & 15, a))
