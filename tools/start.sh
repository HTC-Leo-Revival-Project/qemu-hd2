#!/bin/sh
# Start the emulated HD2 headless in the background and pick BOOT FROM NAND.
# usage: [AUDIODEV="-audiodev wav,id=snd0,path=out.wav"] start.sh [extra qemu args]
#   CLK_KEYS="pgdn pgdn f2" picks RECOVERY in the cLK menu instead; CLK_KEYS=
#   presses nothing (MAGLDR).  NAND=/SDCARD= pick other images.
#   BASEBAND=lle runs the real AMSS (amss.elf) on the modem core; TRACE=on
#   logs its peripheral accesses to qemu.log, AMSSLOG=1 its debug messages
#   (contrib/plugins/amsslog.c; AMSSLOG="hook=0x... ..." adds hooks).
#   (adb: adb connect 127.0.0.1:5555)
cd "$(dirname "$0")/../images" || exit 1
rm -f qmp.sock ramcon.log qemu.log
SD=; [ -f "${SDCARD:-sdcard.img}" ] && SD="-drive if=sd,format=raw,file=${SDCARD:-sdcard.img}"
# WiFi and mobile data each get a user-mode network (real internet)
NET="-nic user,model=bcm4329 -nic user,model=msm-modem,net=10.0.3.0/24"
ADB="-chardev socket,id=adb,host=127.0.0.1,port=${ADB_PORT:-5555},server=on,wait=off -global msm-hsusb.adb=adb"
M=htcleo
D=${QEMU_D:-unimp,guest_errors} PLUGIN=
[ "$BASEBAND" = lle ] && M="htcleo,baseband=lle,baseband-firmware=${AMSS:-amss.elf},baseband-trace=${TRACE:-off}"
if [ -n "$AMSSLOG" ]; then
  # AMSS 2.15.50.14's msg_send entry points
  P="../src/qemu/build/contrib/plugins/libamsslog.so,msg=0x526604,msg=0xc55d88,msg=0xc57028"
  for h in $AMSSLOG; do [ "$h" = 1 ] || P="$P,$h"; done
  PLUGIN="-plugin $P" D="$D,plugin"
fi
../src/qemu/build/qemu-system-arm -M $M -drive if=mtd,format=raw,file=${NAND:-nand.img} \
  -display none -serial file:uart1.log -serial file:ramcon.log \
  -qmp unix:qmp.sock,server,nowait -d $D -D qemu.log $PLUGIN \
  ${AUDIODEV:--audiodev none,id=snd0} -global msm-modem.audiodev=snd0 $SD $NET $ADB "$@" > qemu.out 2>&1 &
for i in $(seq 20); do [ -S qmp.sock ] && break; sleep 0.3; done
# wait for LK's menu (a slow -icount guest takes longer than real time)
for i in $(seq 200); do grep -q "fastboot: processing" uart1.log 2>/dev/null && break; sleep 0.3; done
# press it until the kernel starts (LK may not be reading keys yet)
if [ -n "${CLK_KEYS-f2}" ]; then
  for i in $(seq 20); do
    sleep 0.5; python3 ../tools/keys.py qmp.sock $(for k in ${CLK_KEYS-f2}; do printf "%s:%s " "${k%%:*}" "$((i == 1 ? 200 : 3000))"; done)
    for j in $(seq 6); do [ -s ramcon.log ] && break 2; sleep 0.5; done
  done
fi
