#!/bin/sh
# Boot the emulated HD2 headless: select BOOT FROM NAND in cLK and poll the
# kernel log until UNTIL (regex) shows up, a panic happens, or MAX seconds.
# usage: boot.sh MAX [UNTIL] [-- extra qemu args]
cd "$(dirname "$0")/../images" || exit 1
MAX=$1; shift
UNTIL='boot_completed|Boot completed|BOOT_COMPLETED'
if [ $# -gt 0 ] && [ "$1" != "--" ]; then UNTIL=$1; shift; fi
[ "$1" = "--" ] && shift
rm -f qmp.sock ramcon.log qemu.log
../src/qemu/build/qemu-system-arm -M htcleo -drive if=mtd,format=raw,file=nand.img \
  -display none -serial file:uart1.log -serial file:ramcon.log \
  -qmp unix:qmp.sock,server,nowait -d unimp,guest_errors -D qemu.log "$@" > qemu.out 2>&1 &
QPID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S qmp.sock ] && break; sleep 0.3; done
sleep 2.5
python3 ../tools/keys.py qmp.sock f2
T=0
while [ $T -lt "$MAX" ]; do
  sleep 2; T=$((T+2))
  kill -0 $QPID 2>/dev/null || { echo "qemu exited"; cat qemu.out; exit 1; }
  if grep -qaE "$UNTIL" ramcon.log 2>/dev/null; then echo "reached '$UNTIL' after ${T}s"; break; fi
  if grep -qaE "Kernel panic|Internal error: Oops" ramcon.log 2>/dev/null; then echo "kernel panic after ${T}s"; sleep 1; break; fi
done
python3 ../tools/capture.py qmp.sock k 0.1 1
python3 ../tools/qmon.py qmp.sock quit > /dev/null 2>&1
wait
