#!/bin/sh
# Boot the iOS 7 SD package with MAGLDR 1.13: start QEMU, set MAGLDR's AD SD
# folder to ios7 and pick "Boot AD SD".  Stop with: tools/qmon.py <IMAGES>/qmp.sock quit
# usage: [BASEBAND=lle] [NAND=nand-magldr.img] [SDCARD=sdcard-ios7.img] [IMAGES=images]
#        tools/ios-boot.sh [extra qemu args]
T=$(cd "$(dirname "$0")" && pwd)
cd "${IMAGES:-$T/../images}" || exit 1
rm -f qmp.sock qemu.log uart1.log ramcon.log
M=htcleo
[ "$BASEBAND" = lle ] && M="htcleo,baseband=lle,baseband-firmware=${AMSS:-amss.elf}"
"$T/../src/qemu/build/qemu-system-arm" -M $M \
  -drive if=mtd,format=raw,file="${NAND:-nand-magldr.img}" \
  -drive if=sd,format=raw,file="${SDCARD:-sdcard-ios7.img}" \
  -display none -serial file:uart1.log -serial file:ramcon.log \
  -qmp unix:qmp.sock,server,nowait -d guest_errors -D qemu.log \
  -audiodev none,id=snd0 -global msm-modem.audiodev=snd0 \
  -nic user,model=bcm4329 -nic user,model=msm-modem,net=10.0.3.0/24 \
  "$@" > qemu.out 2>&1 &
for i in $(seq 20); do [ -S qmp.sock ] && break; sleep 0.3; done
N="python3 $T/magnav.py qmp.sock"
# MAGLDR's main menu is up when magnav finds its highlight bar
for i in $(seq 40); do sleep 1; $N 1 >/dev/null 2>&1 && break; done
# Services (10) -> Boot Settings (1) -> AD SD Dir (3) -> ios7 (2), then Boot AD SD (2).
# MAGLDR forgets the folder on every start, so this is needed each time.
$N 10 select && sleep 1 && $N 1 select && sleep 1 &&
  $N 3 select && sleep 2 && $N 2 select && sleep 2 && $N 2 select ||
  { echo "ios-boot: MAGLDR menu not found (see $PWD/qemu.out)" >&2; exit 1; }
echo "ios-boot: booting AD SD (ios7)"
