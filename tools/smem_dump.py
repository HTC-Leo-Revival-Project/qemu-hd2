#!/usr/bin/env python3
"""Dump the SMEM heap table of contents from a raw SMEM image."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
pc = struct.unpack_from('<16I', d, 0)
print('proc_comm:', ' '.join('%08x' % x for x in pc))
ver = struct.unpack_from('<32I', d, 0x40)
print('version:', ' '.join('%08x' % x for x in ver))
init, free, rem, res = struct.unpack_from('<4I', d, 0xC0)
print('heap_info: init=%x free_offset=%x remaining=%x' % (init, free, rem))
for i in range(512):
    a, off, size, r = struct.unpack_from('<4I', d, 0xD0 + i * 16)
    if a or off or size:
        print('item %3d alloc=%x off=%06x size=%06x' % (i, a, off, size))
