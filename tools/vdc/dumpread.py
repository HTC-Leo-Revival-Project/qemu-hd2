import struct, sys
sys.path.insert(0, '.')
from cmdparse import packets
def load(path):
    d = open(path, 'rb').read()
    addr, n = struct.unpack_from('<II', d, 0)
    w = list(struct.unpack_from('<%dI' % n, d, 8))
    o = 8 + 4 * n; mem = {}
    while o + 8 <= len(d):
        a, c = struct.unpack_from('<II', d, o); o += 8
        mem[a] = list(struct.unpack_from('<%dI' % c, d, o)); o += 4 * c
    return addr, w, mem
