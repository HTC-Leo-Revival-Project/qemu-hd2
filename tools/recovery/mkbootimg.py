#!/usr/bin/env python3
"""Minimal Android boot image (v0) writer, as AOSP mkbootimg does it.

usage: mkbootimg.py KERNEL RAMDISK OUT [--base 0x11800000] [--pagesize 2048]
                    [--cmdline '...']
Offsets follow the HD2 cLK images: kernel base+0x8000, ramdisk
base+0x01000000, tags base+0x100.
"""
import argparse
import hashlib
import struct


def pad(b, n):
    return b + b'\0' * ((n - len(b) % n) % n)


ap = argparse.ArgumentParser()
ap.add_argument('kernel')
ap.add_argument('ramdisk')
ap.add_argument('out')
ap.add_argument('--base', type=lambda x: int(x, 0), default=0x11800000)
ap.add_argument('--pagesize', type=int, default=2048)
ap.add_argument('--cmdline', default='')
ap.add_argument('--name', default='')
a = ap.parse_args()

k = open(a.kernel, 'rb').read()
r = open(a.ramdisk, 'rb').read()
sha = hashlib.sha1()
for blob in (k, r, b''):
    sha.update(blob)
    sha.update(struct.pack('<I', len(blob)))
hdr = struct.pack('<8s10I16s512s32s', b'ANDROID!',
                  len(k), a.base + 0x8000, len(r), a.base + 0x01000000,
                  0, a.base + 0x00F00000, a.base + 0x100, a.pagesize, 0, 0,
                  a.name.encode(), a.cmdline.encode(), sha.digest())
with open(a.out, 'wb') as f:
    f.write(pad(hdr, a.pagesize))
    f.write(pad(k, a.pagesize))
    f.write(pad(r, a.pagesize))
