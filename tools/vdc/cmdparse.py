#!/usr/bin/env python3
"""Segment a Janus VDC command buffer into packets (RE helper)."""
import struct, sys

def packets(w, start=0):
    i = start
    while i < len(w):
        h = w[i]; op = h >> 24
        if op == 0x04:          # one reg, 24-bit index
            yield i, 'W1', [h & 0xffffff], w[i+1:i+2]; i += 2
        elif op == 0x03:        # two regs, 12-bit indices
            yield i, 'W2', [(h >> 12) & 0xfff, h & 0xfff], w[i+1:i+3]; i += 3
        elif op == 0x02:        # three regs, 8-bit indices
            yield i, 'W3', [(h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff], w[i+1:i+4]; i += 4
        elif op == 0x08:        # memory -> reg
            yield i, 'MEM', [h & 0xffffff], w[i+1:i+3]; i += 3
        elif op == 0x01:        # end of the command buffer
            yield i, 'STOP', [], []; return
        elif op == 0x0a:
            yield i, 'END', [], []; i += 1
        elif op == 0x00 and h:
            yield i, 'OP0', [h & 0xffffff], []; i += 1
        elif h == 0:
            return
        else:
            yield i, '?%02x' % op, [h & 0xffffff], []; i += 1

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    off = int(sys.argv[2], 0); n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x18000
    w = struct.unpack_from('<%dI' % (n // 4), d, off)
    for i, k, regs, vals in packets(w, 4):
        print('%5d %-4s %s %s' % (i, k, ' '.join('%x' % r for r in regs), ' '.join('%08x' % v for v in vals)))
