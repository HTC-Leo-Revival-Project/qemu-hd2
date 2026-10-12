#!/bin/sh
# Boot the emulated HD2 headless for N seconds and capture screen + logs.
# usage: run.sh SECONDS [extra qemu args...]
cd "$(dirname "$0")/../images" || exit 1
Q=../src/qemu/build/qemu-system-arm
T=$1; shift
rm -f qemu.log uart1.log ramcon.log
$Q -M htcleo -drive if=mtd,format=raw,file=nand.img \
   -serial file:uart1.log -serial file:ramcon.log -display none \
   -qmp unix:qmp.sock,server,nowait -d unimp,guest_errors -D qemu.log "$@" > qemu.out 2>&1 &
sleep "$T"
python3 ../tools/qmon.py qmp.sock "info registers" | grep -E "R12|R15|PSR"
python3 ../tools/qmon.py qmp.sock "screendump scr.ppm" quit > /dev/null
python3 -c "from PIL import Image; Image.open('scr.ppm').save('scr.png')"
wait
