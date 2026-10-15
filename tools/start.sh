#!/bin/sh
# Start the emulated HD2 headless in the background, booting RECOVERY, which is
# where the UEFI payload lives.  usage: [AUDIODEV="-audiodev wav,id=snd0,path=out.wav"] start.sh [extra qemu args]
#   CLK_KEYS= presses nothing (MAGLDR).  A bare f2 lands on the wrong cLK entry
#   and boots Android instead of the firmware, which looks like a dead boot:
#   no output on uart1.log at all, while ramcon.log shows the kernel coming up.
#   NAND=/SDCARD= pick other images.
#   BASEBAND=lle runs the real AMSS (amss.elf) on the modem core; TRACE=on
#   logs its peripheral accesses to qemu.log, AMSSLOG=1 its debug messages
#   (contrib/plugins/amsslog.c; AMSSLOG="hook=0x... ..." adds hooks).
#   USB=lle hands the gadget to this machine's own USB stack instead: the
#   emulated HD2 turns up in lsusb and dmesg, and the host's adb and fastboot
#   talk to it directly.  It needs root and vhci_hcd, so use
#   tools/start-usb-lle.sh, which comes back through sudo.
#   (adb: adb connect 127.0.0.1:5555)
cd "$(dirname "$0")/../images" || exit 1
rm -f qmp.sock ramcon.log qemu.log
SD=; [ -f "${SDCARD:-sdcard.img}" ] && SD="-drive if=sd,format=raw,file=${SDCARD:-sdcard.img}"
# WiFi and mobile data each get a user-mode network (real internet)
NET="-nic user,model=bcm4329 -nic user,model=msm-modem,net=10.0.3.0/24"
# USB=lle is the other end of the cable: the host kernel, not this chardev
ADB=
[ "$USB" != lle ] && ADB="-chardev socket,id=adb,host=127.0.0.1,port=${ADB_PORT:-5555},server=on,wait=off -global msm-hsusb.adb=adb"
M=htcleo
D=${QEMU_D:-unimp,guest_errors} PLUGIN=
[ "$BASEBAND" = lle ] && M="$M,baseband=lle,baseband-firmware=${AMSS:-amss.elf},baseband-trace=${TRACE:-off}"
[ "$USB" = lle ] && M="$M,usb-host=lle"
if [ -n "$AMSSLOG" ]; then
  # AMSS 2.15.50.14's msg_send entry points
  P="../src/qemu/build/contrib/plugins/libamsslog.so,msg=0x526604,msg=0xc55d88,msg=0xc57028"
  for h in $AMSSLOG; do [ "$h" = 1 ] || P="$P,$h"; done
  PLUGIN="-plugin $P" D="$D,plugin"
fi
../src/qemu/build/qemu-system-arm -L ../src/qemu/pc-bios -L ../src/qemu/build/pc-bios \
  -M $M -drive if=mtd,format=raw,file=${NAND:-nand.img} \
  -display none -serial file:uart1.log -serial file:ramcon.log \
  -qmp unix:qmp.sock,server,nowait -d $D -D qemu.log $PLUGIN \
  ${AUDIODEV:--audiodev none,id=snd0} -global msm-modem.audiodev=snd0 $SD $NET $ADB "$@" > qemu.out 2>&1 &
for i in $(seq 20); do [ -S qmp.sock ] && break; sleep 0.3; done
sleep 2.5
[ -n "${CLK_KEYS-pgdn pgdn f2}" ] && python3 ../tools/keys.py qmp.sock ${CLK_KEYS-pgdn pgdn f2}
