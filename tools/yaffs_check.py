#!/usr/bin/env python3
"""Read files back out of a raw HD2 NAND image (controller ECC-mode view) and
compare them with the ROM zip.  usage: yaffs_check.py nand.img rom.zip first_block nblocks path..."""
import hashlib, struct, sys, zipfile
img, rom, first, nblk = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4])
f = open(img, 'rb')
def page(pg):
    f.seek(pg * 2112); r = f.read(2112)
    data = bytearray(); oob = bytearray()
    for cw in range(4):
        c = bytearray(r[cw*528:(cw+1)*528])
        if cw == 3:
            c[464:466] = c[526:528]
        data += c[:512]; oob += c[522:526]
    return bytes(data), bytes(oob)
objs = {}; chunks = {}
for b in range(first, first + nblk):
    for p in range(64):
        d, o = page(b * 64 + p)
        seq, oid, cid, nb = struct.unpack('<IIII', o)
        if seq == 0xFFFFFFFF:
            continue
        if cid & 0x80000000:
            oid &= 0x0FFFFFFF
            otype, parent = struct.unpack_from('<Ii', d, 0)
            name = d[10:266].split(b'\0')[0].decode()
            size = struct.unpack_from('<i', d, 292)[0]
            objs[oid] = (otype, parent, name, size)
        else:
            chunks.setdefault(oid, {})[cid] = d[:nb]
def path_of(oid):
    parts = []
    while oid in objs:
        t, par, name, _ = objs[oid]; parts.append(name); oid = par
    return '/' + '/'.join(reversed(parts))
paths = {path_of(o): o for o in objs}
z = zipfile.ZipFile(rom)
for p in sys.argv[5:]:
    o = paths.get(p)
    if o is None:
        print('missing', p); continue
    ch = chunks.get(o, {})
    data = b''.join(ch[i] for i in sorted(ch))[:objs[o][3]]
    ref = z.read('system' + p)
    print(p, len(data), len(ref), 'OK' if data == ref else 'MISMATCH')

if len(sys.argv) == 5:
    # listing mode: show every object with its newest data
    for oid, (t, par, name, size) in sorted(objs.items()):
        p = path_of(oid)
        if 'dalvik' in p or t == 1 and size and oid in chunks and len(chunks[oid]) < 3:
            ch = chunks.get(oid, {})
            data = b''.join(ch[i] for i in sorted(ch))[:size]
            print(t, size, len(data), p, data[:16])
