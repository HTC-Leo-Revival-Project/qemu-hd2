#!/bin/sh
# Start the emulated HD2 headless in the background and pick BOOT FROM NAND.
# usage: [AUDIODEV="-audiodev wav,id=snd0,path=out.wav"] start.sh [extra qemu args]
#   CLK_KEYS="pgdn pgdn f2" picks RECOVERY in the cLK menu instead; CLK_KEYS=
#   presses nothing (MAGLDR).  NAND=/SDCARD= pick other images.
#   (adb: adb connect 127.0.0.1:5555)
cd "$(dirname "$0")/../images" || exit 1
rm -f qmp.sock ramcon.log qemu.log
SD=; [ -f "${SDCARD:-sdcard.img}" ] && SD="-drive if=sd,format=raw,file=${SDCARD:-sdcard.img}"
# WiFi and mobile data each get a user-mode network (real internet)
NET="-nic user,model=bcm4329 -nic user,model=msm-modem,net=10.0.3.0/24"
ADB="-chardev socket,id=adb,host=127.0.0.1,port=${ADB_PORT:-5555},server=on,wait=off -global msm-hsusb.adb=adb"
../src/qemu/build/qemu-system-arm -M htcleo -drive if=mtd,format=raw,file=${NAND:-nand.img} \
  -display none -serial file:uart1.log -serial file:ramcon.log \
  -qmp unix:qmp.sock,server,nowait -d unimp,guest_errors -D qemu.log \
  ${AUDIODEV:--audiodev none,id=snd0} -global msm-modem.audiodev=snd0 $SD $NET $ADB "$@" > qemu.out 2>&1 &
for i in $(seq 20); do [ -S qmp.sock ] && break; sleep 0.3; done
sleep 2.5
[ -n "${CLK_KEYS-f2}" ] && python3 ../tools/keys.py qmp.sock ${CLK_KEYS-f2}
