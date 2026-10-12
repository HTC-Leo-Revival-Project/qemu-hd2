#!/usr/bin/env python3
"""Write an image into a partition of an existing emulator NAND image.

usage: nand_put.py nand.img START_BLOCK BLOCKS image
The partition (BLOCKS erase blocks from START_BLOCK) is erased first, then
the image is written page by page in the controller's raw layout, like
flash_image would.  Example, the recovery partition of the cLK layout:
    nand_put.py images/nand.img 0x221 40 recovery.img
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hd2_mknand import Nand, PAGE, PPB, RAW_PAGE  # noqa: E402

path, start, blocks, img = sys.argv[1], int(sys.argv[2], 0), \
    int(sys.argv[3], 0), sys.argv[4]
data = open(img, 'rb').read()
if len(data) > blocks * PPB * PAGE:
    sys.exit('image larger than the partition')
with open(path, 'r+b') as f:
    f.seek(start * PPB * RAW_PAGE)
    f.write(b'\xff' * (blocks * PPB * RAW_PAGE))
    for i in range(0, len(data), PAGE):
        f.seek((start * PPB + i // PAGE) * RAW_PAGE)
        f.write(Nand.encode(data[i:i + PAGE]))
print('%s: %d bytes at block %#x' % (img, len(data), start))
