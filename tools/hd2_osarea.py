#!/usr/bin/env python3
"""Write the cLK "OS area" of a raw HD2 NAND image, the way the HSPL expects it.

usage: hd2_osarea.py nand.img os.nb [--block 0x212]
       hd2_osarea.py nand.img lk.bin [--block 0x212]

The HSPL does not read a bare lk.bin out of NAND.  It reads the NB image that
cLK's own nbgen wraps it in (clk/nbgen.c): one 2048-byte sector per NAND page
starting at block 0x212, laid out as

    sector 0   WinCE MBR, E9 FD FF .. 55 AA, partition 0x23 starting at
               sector 2
    sector 1   "MSFLSH50" tag
    sector 2+  lk.bin, padded with 0xFF to the end of the partition

An .nb file additionally carries 8 bytes of sector info (number, tag) after
every sector; the HSPL never sees those, so they are stripped here.  A plain
lk.bin is accepted too and wrapped the same way.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hd2_mknand import (Nand, PAGE, PPB, RAW_PAGE, ROM_BLOCK,  # noqa: E402
                        extract_nbh_os, os_sectors_from_lk, nbh_unsign)

NB_SECTOR = 0x800          # nbgen writes 2048-byte sectors
NB_STRIDE = NB_SECTOR + 8  # ... each followed by 8 bytes of sector info


def sectors_from_nb(path):
    """Strip nbgen's 8-byte-per-sector info from an .nb / .payload image."""
    d = open(path, 'rb').read()
    if len(d) % NB_STRIDE:
        sys.exit('%s: size %d is not a multiple of %#x - not an nb image'
                 % (path, len(d), NB_STRIDE))
    secs = [d[i * NB_STRIDE:i * NB_STRIDE + NB_SECTOR]
            for i in range(len(d) // NB_STRIDE)]
    if not secs or secs[0][:3] != b'\xe9\xfd\xff':
        sys.exit('%s: sector 0 is not the WinCE MBR' % path)
    if not secs[1].startswith(b'MSFLSH50'):
        sys.exit('%s: sector 1 is not the MSFLSH50 tag' % path)
    start, count = struct.unpack_from('<II', secs[0], 0x1be + 8)
    return secs, start, count


def sectors_from_any(path):
    d = open(path, 'rb').read()
    if d.startswith(b'R000FF'):
        return extract_nbh_os(path)
    if d[:3] == b'\xe9\xfd\xff':
        return sectors_from_nb(path)
    return os_sectors_from_lk(d)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    block = ROM_BLOCK
    for a in sys.argv[1:]:
        if a.startswith('--block'):
            block = int(a.split('=', 1)[1], 0)
    if len(args) != 2:
        sys.exit(__doc__)
    path, img = args

    secs, start, count = sectors_from_any(img)
    print('%s: %d sectors, image at sector %d, %d sectors'
          % (img, len(secs), start, count))

    nblk = (len(secs) + PPB - 1) // PPB
    with open(path, 'r+b') as f:
        f.seek(block * PPB * RAW_PAGE)
        f.write(b'\xff' * (nblk * PPB * RAW_PAGE))
        for i, s in enumerate(secs):
            f.seek((block * PPB + i) * RAW_PAGE)
            f.write(Nand.encode(s))
    print('%s: OS area written to block %#x..%#x (%d sectors)'
          % (path, block, block + nblk - 1, len(secs)))


if __name__ == '__main__':
    main()
