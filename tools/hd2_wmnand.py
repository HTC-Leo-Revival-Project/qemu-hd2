#!/usr/bin/env python3
"""Build an HTC HD2 NAND image with a stock Windows Mobile 6.5 ROM.

What a phone's NAND holds after flashing a stock RUU: the radio partitions
(boot area, FSBL, OSBL, AMSS, DSP1; see hd2_radio.py) in the first blocks,
then the OS area at block 0x212.  The OS section of the stock NBH (type
0x400) is that area page by page, each page 2048 data bytes followed by an
8-byte WinCE SectorInfo:

    u32 logical sector (0xffffffff = unmapped), u8 OEM reserved,
    u8 bad block (0xff = good), u16 reserved2

which the HTC NAND driver (FMD) keeps in the 4 spare bytes of codewords 0
and 1 (raw column 524 on the chip, offset 522 of an ECC-mode read; the
image stores codewords in ECC-read order, see msm_nand.c).  The MBR lists
ULDR (0x20), XIP (0x23, the OS kernel the SPL boots), IMGFS (0x25) and
user storage (0x04, left erased for the OS to format).

The stock RUU's NBH carries the radio too (section 0x301, the same as
radio 2.15.50.14's own RUU), so it is all this needs; --radio takes the
radio partitions from another radio NBH instead.

usage: hd2_wmnand.py --ruu RUU_signed.nbh [--radio radio.nbh] -o nand-wm.img
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hd2_mknand import Nand, PAGE, PPB, RAW_PAGE, NBLOCKS, ROM_BLOCK  # noqa
import hd2_radio  # noqa

NBH_PAGE = PAGE + 8


def nbh_sections(path):
    d = open(path, 'rb').read()
    if not d.startswith(b'R000FF\n'):
        sys.exit('%s: not a signed NBH' % path)
    p = hd2_radio.unsign(d)
    types = struct.unpack_from('<32I', p, 0x40)
    offs = struct.unpack_from('<32I', p, 0xC0)
    lens = struct.unpack_from('<32I', p, 0x140)
    return {t: p[o:o + n] for t, o, n in zip(types, offs, lens) if t}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ruu', required=True, help='stock ROM RUU_signed.nbh')
    ap.add_argument('--radio', help='radio RUU_signed.nbh (default: the '
                    'radio in --ruu)')
    ap.add_argument('-o', required=True)
    a = ap.parse_args()

    os_img = nbh_sections(a.ruu).get(0x400)
    if not os_img or len(os_img) % NBH_PAGE:
        sys.exit('%s: no OS section' % a.ruu)

    # erased NAND, then the radio partitions
    Nand(a.o).close()
    p = nbh_sections(a.radio or a.ruu).get(0x301)
    if not p:
        sys.exit('%s: no radio section' % (a.radio or a.ruu))
    amss_at, elf = hd2_radio.find_amss(p)
    hd2_radio.write_nand(a.o, p, amss_at, len(elf), hd2_radio.partitions(p))

    n = len(os_img) // NBH_PAGE
    if ROM_BLOCK * PPB + n > NBLOCKS * PPB:
        sys.exit('OS image does not fit')
    with open(a.o, 'r+b') as f:
        for i in range(n):
            pg = os_img[i * NBH_PAGE:(i + 1) * NBH_PAGE]
            data, info = pg[:PAGE], pg[PAGE:]
            if data == b'\xff' * PAGE and info == b'\xff' * 8:
                continue
            f.seek((ROM_BLOCK * PPB + i) * RAW_PAGE)
            f.write(Nand.encode(data, info))
    print('  OS        block %d  %d pages' % (ROM_BLOCK, n))


if __name__ == '__main__':
    main()
