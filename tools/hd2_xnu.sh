#!/bin/sh
# Build a NAND image whose boot.img kernel is an HD2 GenericBooter uImage,
# booted by the UART-enabled cLK lk from ~/qsd8k/clk/lk/build-htcleo/lk.bin.
#
#   hd2_xnu.sh UIMAGE OUT.nand.img
#
# The stock cLK boot partition is 5 MiB; a GenericBooter uImage with its
# ramdisk img3 is ~32 MiB, so the lk's compiled PTABLE-MB entry for "boot"
# is enlarged (and "system" shrunk by the same amount, which keeps every
# later partition where the modem side already expects nothing in particular).
# The baseband/radio partitions are taken from images/nand-lle.img so LLE
# works unchanged.
set -e
cd "$(dirname "$0")/.." || exit 1
IMG=$1
OUT=$2
UARTLK=${UARTLK:-$HOME/qsd8k/clk/lk/build-htcleo/lk.bin}
# boot size now comes from the lk itself

BASE_LLE=images/nand-lle.img

[ -f "$IMG" ]  || { echo "no uImage: $IMG"; exit 1; }
[ -f "$UARTLK" ] || { echo "no uart lk: $UARTLK"; exit 1; }
[ -f "$BASE_LLE" ] || { echo "no base lle nand: $BASE_LLE"; exit 1; }

# boot.img: kernel = the uImage, empty ramdisk (nothing uses it), a cmdline
# cLK will not choke on.  kernel_addr must match ih_load in the uImage.
# boot.img is prepared separately (TWRP header + our kernel); see hd2_xnu_build


# The cLK lk carries its own compiled partition table (lk/target/htcleo/init.c
# board_part_list); "boot" there is 64 MB so a GenericBooter uImage (~32 MB:
# kernel img3 + device tree img3 + ramdisk img3) fits. Nothing to patch here --
# the table is read back out of the lk and only verified.
python3 - "$UARTLK" "$BASE_LLE" "$OUT" ${BOOTIMG:-/tmp/boot-xnu.img} <<'PY'
import sys, os, struct
sys.path.insert(0, 'tools')
from hd2_mknand import (clk14_ptable, clk14_layout, os_sectors_from_lk,
                        Nand, ROM_BLOCK, PPB, PAGE, RAW_PAGE)

uartlk, baselle, out, bootimg = sys.argv[1:5]

# 1. start from the LLE NAND (has the radio partitions + factory MACs) so the
#    real baseband still finds its firmware and the emulated radio works.
import shutil
shutil.copyfile(baselle, out)

# 2. read the partition table compiled into the lk and check the boot
#    partition can hold the boot.img
lk = bytearray(open(uartlk, 'rb').read())
_, parts = clk14_ptable(lk)
if not parts:
    sys.exit('no PTABLE-MB in %s' % uartlk)
layout = clk14_layout(parts)
for name, blk, size in layout:
    print('  %-9s block 0x%04x  %5d blks  %7.1f MiB'
          % (name, blk, size, size * 131072 / 1048576))
boot = [x for x in layout if x[0] == 'boot'][0]
if boot[2] * 131072 < os.path.getsize(bootimg):
    sys.exit('boot partition (%d MiB) smaller than boot.img (%d bytes)'
             % (boot[2] * 131072 / 1048576, os.path.getsize(bootimg)))

# 3. write the lk's OS area into the NAND, in place
f = open(out, 'r+b')
def write_page(page, data):
    f.seek(page * RAW_PAGE); f.write(Nand.encode(data))
secs, start, count = os_sectors_from_lk(bytes(lk))
for i, s in enumerate(secs):
    write_page(ROM_BLOCK * PPB + i, s)
f.close()
print('OS area: %d sectors from lk (%d bytes)' % (len(secs), len(lk)))

# 4. flash the boot.img into the boot partition, erased first
img = open(bootimg, 'rb').read()
f = open(out, 'r+b')
f.seek(boot[1] * PPB * RAW_PAGE)
f.write(b'\xff' * (boot[2] * PPB * RAW_PAGE))
for i in range(0, len(img), PAGE):
    f.seek((boot[1] * PPB + i // PAGE) * RAW_PAGE)
    f.write(Nand.encode(img[i:i + PAGE]))
f.close()
print('%s: boot.img %d bytes -> block 0x%04x (%d blks)'
      % (out, len(img), boot[1], boot[2]))
PY
echo "built $OUT"