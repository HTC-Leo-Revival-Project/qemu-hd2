#!/usr/bin/env python3
"""Extract the modem firmware from an HTC Leo radio RUU (RUU_signed.nbh).

A signed NBH ("R000FF") is a chain of blocks, each a 9-byte header
(u32 length, u32 signature length, u8 flag: 1 data, 2 last), the data and
its signature.  The joined data is a 0x200-byte HTCIMAGE header and the
radio payload: the radio part of the NAND with the MIBIB partition table,
DBL/FSBL/OSBL and the AMSS ELF that runs on the QSD8250's ARM9 modem core.  This prints the partition table and
writes the AMSS ELF for `-M htcleo,baseband=lle,baseband-firmware=...`.

The payload is the boot area (NAND blocks 0-3: DBL headers and the MIBIB
partition tables) followed by the partition images back to back: FSBL at
+0x80020, OSBL at +0xC0020, then the AMSS ELF and the DSP ELF.  A real
HD2's NAND (radio 2.15.50.14, read with the nanddump recovery) holds
exactly these bytes at the MIBIB's blocks: boot area 0-3, FSBL 16, OSBL 22
and again 30 (OSBL2), AMSS 38 (as long as its ELF).  --write-nand puts them
into an emulator NAND image the same way, plus the DSP ELF in DSP1 (hidden
from the apps processor on the phone, so its placement is inferred).

usage: hd2_radio.py RUU_signed.nbh [-o amss.elf] [--write-nand nand.img]
"""
import argparse
import os
import struct
import sys

HEADER = 0x200                  # HTCIMAGE header at the start of the data
PART_MAGIC = (0x55EE73AA, 0xE35EBDDB)


def partitions(p):
    """The MIBIB system partition table (the magic also occurs in code)."""
    i = -1
    while True:
        i = p.find(struct.pack('<2I', *PART_MAGIC), i + 1)
        if i < 0:
            return []
        _, _, _, n = struct.unpack_from('<4I', p, i)
        if 0 < n < 32 and p[i + 16:i + 18] == b'0:':
            break
    out, o = [], i + 16
    for _ in range(n):
        name = p[o:o + 16].split(b'\0')[0].decode(errors='replace')
        off, ln, attr = struct.unpack_from('<3I', p, o + 16)
        out.append((name, off, ln, attr))
        o += 28
    return out


def unsign(d):
    """Join the data of the signed blocks that follow the 0x17-byte lead."""
    out, off = bytearray(), 0x17
    while off + 9 <= len(d):
        ln, sl, flag = struct.unpack_from('<IIB', d, off)
        if not ln or off + 9 + ln + sl > len(d):
            break
        out += d[off + 9:off + 9 + ln]
        off += 9 + ln + sl
    return bytes(out)


def elf_size(e):
    phoff, = struct.unpack_from('<I', e, 28)
    phes, phn = struct.unpack_from('<HH', e, 42)
    end = phoff + phes * phn        # the DSP ELF has them at its end
    for k in range(phn):
        _, off, _, _, fsz, _, _, _ = struct.unpack_from('<8I', e, phoff + 32 * k)
        end = max(end, off + fsz)
    return end


def write_nand(path, p, amss_at, amss_len, parts):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from hd2_mknand import Nand, PAGE, PPB, RAW_PAGE
    blk = {name: start for name, start, _, _ in parts}
    dsp_at = amss_at + amss_len
    dsp_len = elf_size(p[dsp_at:]) if p[dsp_at:dsp_at + 4] == b'\x7fELF' else 0
    images = [('boot area', 0, p[0x20:0x80020]),
              ('FSBL', blk['0:FSBL'], p[0x80020:0xC0020]),
              ('OSBL', blk['0:OSBL'], p[0xC0020:0x140020]),
              ('OSBL2', blk['0:OSBL2'], p[0xC0020:0x140020]),
              ('AMSS', blk['0:AMSS'], p[amss_at:amss_at + amss_len])]
    if dsp_len:
        images.append(('DSP1', blk['0:DSP1'], p[dsp_at:dsp_at + dsp_len]))
    with open(path, 'r+b') as f:
        for name, start, data in images:
            data = data.rstrip(b'\xff')
            for i in range(0, len(data), PAGE):
                f.seek((start * PPB + i // PAGE) * RAW_PAGE)
                f.write(Nand.encode(data[i:i + PAGE]))
            print('  %-9s block %3d  %d bytes' % (name, start, len(data)))


def find_amss(p):
    """The first ARM executable ELF whose program headers all fit."""
    i = 0
    while True:
        i = p.find(b'\x7fELF\x01\x01\x01', i)
        if i < 0:
            return None
        e = p[i:]
        etype, mach = struct.unpack_from('<HH', e, 16)
        phoff, = struct.unpack_from('<I', e, 28)
        phes, phn = struct.unpack_from('<HH', e, 42)
        if etype == 2 and mach == 40 and phes == 32 and 0 < phn < 64:
            end = 0
            ok = True
            for k in range(phn):
                _, off, _, _, fsz, _, _, _ = struct.unpack_from(
                    '<8I', e, phoff + 32 * k)
                end = max(end, off + fsz)
                ok &= off + fsz <= len(e)
            if ok:
                return i, e[:end]
        i += 4


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('nbh')
    ap.add_argument('-o', '--output', default='amss.elf')
    ap.add_argument('--dsp', metavar='DSP_ELF',
                    help='also write the QDSP6 firmware (DSP1) ELF here')
    ap.add_argument('--write-nand', metavar='NAND_IMG',
                    help='also write the radio partitions into this image')
    a = ap.parse_args()

    d = open(a.nbh, 'rb').read()
    if not d.startswith(b'R000FF\n'):
        sys.exit('not a signed HTC NBH')
    d = unsign(d)
    version = d[0x1e0:0x1f0].split(b'\0')[0].decode()
    plen, = struct.unpack_from('<I', d, 0x140)
    p = d[HEADER:HEADER + plen]
    print(f'radio {version}, payload {plen:#x} bytes')
    for name, off, ln, attr in partitions(p):
        size = 'rest' if ln == 0xFFFFFFFF else f'{ln:4d}'
        print(f'  {name:12s} block {off:4d}  blocks {size}')
    found = find_amss(p)
    if not found:
        sys.exit('no AMSS ELF in the payload')
    at, elf = found
    entry, = struct.unpack_from('<I', elf, 24)
    open(a.output, 'wb').write(elf)
    print(f'AMSS ELF at payload {at:#x}: {len(elf)} bytes, entry {entry:#x}'
          f' -> {a.output}')
    if a.dsp:
        dsp = p[at + len(elf):]
        if dsp[:4] != b'\x7fELF':
            sys.exit('no DSP ELF after the AMSS one')
        dsp = dsp[:elf_size(dsp)]
        open(a.dsp, 'wb').write(dsp)
        print(f'DSP1 ELF at payload {at + len(elf):#x}: {len(dsp)} bytes'
              f' -> {a.dsp}')
    if a.write_nand:
        print('radio partitions -> %s' % a.write_nand)
        write_nand(a.write_nand, p, at, len(elf), partitions(p))


if __name__ == '__main__':
    main()
