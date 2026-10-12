#!/bin/sh
# Start the HD2 with the real baseband firmware (AMSS) on the modem core.
# usage: start-lle.sh [extra qemu args]   (images/amss.elf: tools/hd2_radio.py)
#   TRACE=on logs the modem core's peripheral accesses to images/lle.log
cd "$(dirname "$0")/../images" || exit 1
rm -f qmp.sock ramcon.log lle.log
../src/qemu/build/qemu-system-arm \
  -M htcleo,baseband=lle,baseband-firmware=amss.elf,baseband-trace=${TRACE:-off} \
  -drive if=mtd,format=raw,file=${NAND:-nand.img} \
  -display none -serial file:uart1.log -serial file:ramcon.log \
  -qmp unix:qmp.sock,server,nowait -d unimp,guest_errors -D lle.log \
  -audiodev none,id=snd0 -global msm-modem.audiodev=snd0 "$@" > lle.out 2>&1 &
for i in $(seq 20); do [ -S qmp.sock ] && break; sleep 0.3; done
