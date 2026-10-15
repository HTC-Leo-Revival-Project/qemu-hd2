#!/bin/sh
# Boot stock Windows Mobile 6.5 on the real baseband: start-lle.sh with
# images/nand-wm.img (tools/hd2_wmnand.py, see README) and no cLK menu key,
# since the stock SPL boots the OS straight from NAND.
# usage: start-wm.sh [extra qemu args]
#   WCELOG=1 puts the kernel's debug output in images/qemu.log as "wce:"
#   lines (the NKDbgPrintfW hook is at its address in RUU 3.14.405.2)
#   NAND= picks another image, ADB_PORT= as for start.sh
P=
if [ -n "$WCELOG" ]; then
  P="-plugin ../src/qemu/build/contrib/plugins/libamsslog.so,vcpu=0,wstr=0x80046410"
  QEMU_D=${QEMU_D:-unimp,guest_errors,plugin}; export QEMU_D
fi
CLK_KEYS= NAND=${NAND:-nand-wm.img} exec "$(dirname "$0")/start-lle.sh" $P "$@"
