#!/usr/bin/env python3
"""Copy files into a FAT32 SD card image made by mksdcard.sh, no mounting.

usage: fat_put.py sdcard.img DIR FILE...
Creates DIR in the root directory if needed and writes each FILE into it,
in contiguous clusters.  Every entry gets its 8.3 name (upper case, which
HD2 kernels look files up by: IOS7/ROOTFS.HFS) and, when the real name
differs from it (ios7, zImage, initrd.gz), a long-name entry the way
Windows writes one; MAGLDR matches the long names.  Names that do not fit
8.3 are refused.
"""
import os
import struct
import sys

EOC = 0x0FFFFFFF


class Fat32:
    def __init__(self, path):
        self.f = open(path, 'r+b')
        mbr = self.f.read(512)
        self.base = struct.unpack_from('<I', mbr, 446 + 8)[0] * 512
        self.f.seek(self.base)
        bpb = self.f.read(512)
        (self.bps,) = struct.unpack_from('<H', bpb, 11)
        self.spc = bpb[13]
        (res,) = struct.unpack_from('<H', bpb, 14)
        nfats = bpb[16]
        (fatsz,) = struct.unpack_from('<I', bpb, 36)
        (self.root,) = struct.unpack_from('<I', bpb, 44)
        (fsinfo,) = struct.unpack_from('<H', bpb, 48)
        (total,) = struct.unpack_from('<I', bpb, 32)
        if fatsz == 0 or bpb[82:87] != b'FAT32':
            sys.exit('not a FAT32 volume')
        self.fat_off = [self.base + (res + i * fatsz) * self.bps
                        for i in range(nfats)]
        self.data_off = self.base + (res + nfats * fatsz) * self.bps
        self.csize = self.spc * self.bps
        self.nclusters = (total - res - nfats * fatsz) // self.spc
        self.f.seek(self.fat_off[0])
        self.fat = bytearray(self.f.read(fatsz * self.bps))
        self.fsinfo = self.base + fsinfo * self.bps

    def get(self, c):
        return struct.unpack_from('<I', self.fat, 4 * c)[0] & 0x0FFFFFFF

    def set(self, c, v):
        struct.pack_into('<I', self.fat, 4 * c, v)

    def alloc(self, n):
        """n contiguous free clusters, chained."""
        run, start = 0, 2
        for c in range(2, self.nclusters + 2):
            if self.get(c) == 0:
                if run == 0:
                    start = c
                run += 1
                if run == n:
                    for i in range(n):
                        self.set(start + i, start + i + 1 if i < n - 1
                                 else EOC)
                    return start
            else:
                run = 0
        sys.exit('no room for %d contiguous clusters' % n)

    def coff(self, c):
        return self.data_off + (c - 2) * self.csize

    def chain(self, c):
        while c < 0x0FFFFFF8:
            yield c
            c = self.get(c)

    def entries(self, dcl):
        for c in self.chain(dcl):
            self.f.seek(self.coff(c))
            blob = self.f.read(self.csize)
            for i in range(0, self.csize, 32):
                yield self.coff(c) + i, blob[i:i + 32]

    def find(self, dcl, name):
        for off, e in self.entries(dcl):
            if e[0] == 0:
                return None
            if e[:11] == name and not e[11] & 0x08:
                return off, e
        return None

    def add_entry(self, dcl, raw):
        """raw: one or more consecutive 32-byte entries (LFN parts first)."""
        need = len(raw) // 32
        run = []
        for off, e in self.entries(dcl):
            if e[0] in (0, 0xE5):
                run.append(off)
                if len(run) == need:
                    for i, o in enumerate(run):
                        self.f.seek(o)
                        self.f.write(raw[32 * i:32 * i + 32])
                    return
            else:
                run = []
        # directory full: extend it by one cluster
        last = list(self.chain(dcl))[-1]
        new = self.alloc(1)
        self.set(last, new)
        self.f.seek(self.coff(new))
        self.f.write(bytes(self.csize))
        self.add_entry(dcl, raw)

    @staticmethod
    def lfn(longname, short):
        """VFAT long-name entries for longname, last part first."""
        if longname == short_display(short):
            return b''
        csum = 0
        for c in short:
            csum = (((csum & 1) << 7) + (csum >> 1) + c) & 0xFF
        units = list(longname.encode('utf-16-le'))
        chars = [units[i] | units[i + 1] << 8 for i in range(0, len(units), 2)]
        chars.append(0)
        while len(chars) % 13:
            chars.append(0xFFFF)
        parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
        out = b''
        for n in range(len(parts), 0, -1):
            c = parts[n - 1]
            seq = n | (0x40 if n == len(parts) else 0)
            out += struct.pack('<B5HBBB6HH2H', seq, *c[0:5], 0x0F, 0, csum,
                               *c[5:11], 0, *c[11:13])
        return out

    @staticmethod
    def entry(name, attr, cluster, size):
        # 2026-10-12 12:00:00 in FAT date/time format
        t = (12 << 11)
        d = ((2026 - 1980) << 9) | (10 << 5) | 12
        return struct.pack('<11sBBBHHHHHHHI', name, attr, 0, 0, t, d, d,
                           cluster >> 16, t, d, cluster & 0xFFFF, size)

    def mkdir(self, longname):
        name = short_name(longname)
        hit = self.find(self.root, name)
        if hit:
            e = hit[1]
            return (struct.unpack_from('<H', e, 20)[0] << 16 |
                    struct.unpack_from('<H', e, 26)[0])
        c = self.alloc(1)
        blob = bytearray(self.csize)
        blob[0:32] = self.entry(b'.          ', 0x10, c, 0)
        blob[32:64] = self.entry(b'..         ', 0x10, 0, 0)
        self.f.seek(self.coff(c))
        self.f.write(blob)
        self.add_entry(self.root, self.lfn(longname, name) +
                       self.entry(name, 0x10, c, 0))
        return c

    def put(self, dcl, longname, path):
        name = short_name(longname)
        if self.find(dcl, name):
            sys.exit('%s already exists' % name.decode())
        size = os.path.getsize(path)
        n = max(1, (size + self.csize - 1) // self.csize)
        c = self.alloc(n)
        with open(path, 'rb') as src:
            self.f.seek(self.coff(c))
            while True:
                chunk = src.read(1 << 22)
                if not chunk:
                    break
                self.f.write(chunk)
        self.add_entry(dcl, self.lfn(longname, name) +
                       self.entry(name, 0x20, c, size))

    def close(self):
        for off in self.fat_off:
            self.f.seek(off)
            self.f.write(self.fat)
        # free count and next-free hint: unknown
        self.f.seek(self.fsinfo + 488)
        self.f.write(struct.pack('<II', 0xFFFFFFFF, 0xFFFFFFFF))
        self.f.close()


def short_display(short):
    base, ext = short[:8].decode().rstrip(), short[8:].decode().rstrip()
    return base + ('.' + ext if ext else '')


def short_name(name):
    base, _, ext = name.upper().rpartition('.') if '.' in name else \
        (name.upper(), '', '')
    if not base:
        base, ext = ext, ''
    ok = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&()@^{}')
    if len(base) > 8 or len(ext) > 3 or not set(base + ext) <= ok:
        sys.exit('%s has no 8.3 name' % name)
    return (base.ljust(8) + ext.ljust(3)).encode()


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    fs = Fat32(sys.argv[1])
    d = fs.mkdir(sys.argv[2])
    for p in sys.argv[3:]:
        fs.put(d, os.path.basename(p), p)
        print('%s/%s: %d bytes' % (sys.argv[2].upper(),
                                   os.path.basename(p).upper(),
                                   os.path.getsize(p)))
    fs.close()


if __name__ == '__main__':
    main()
