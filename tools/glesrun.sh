#!/bin/sh
# The headless emulator runner.  This runs *on the phone*: it is pushed to
# /data/local/tmp/glesrun.sh and started by phone-boot.sh under run-as, so it
# has the app's uid and can reach files/emu/{bin,images,run}.
#
# It is the same QEMU command line the app builds, minus the display: -display
# none and no VNC client.  The QMP monitor is also on TCP 127.0.0.1:4444,
# which is what makes the phone debuggable from a workstation over
# `adb forward` - see phone-qmp.py.
#
# usage (on the phone, as the app's uid):
#   glesrun.sh <qemu-arm> <qemu-q6> <soft|gles>
#
# environment:
#   GTRACE=1   ADRENO_TRACE + ADRENO_TRACE_ALL: log every draw and its
#              vertices to files/emu/run/draw.log.  On a phone there is no
#              monitor to turn the trace on later, so without TRACE_ALL it
#              only covers the first 2000 draws - the boot animation, never
#              the launcher.
#   ADRENO_STATS=5      the per-draw counters and timing breakdown
#   GNODEFER=1          (renderer experiments only; see android/adreno-divider.md)
set -u
PKG=org.htcleorevivalproject.hd2emu
ARM="$1"
Q6="$2"
BACKEND="${3:-soft}"
D=/data/user/0/$PKG/files
R=$D/emu/run

cd "$R" || exit 1
rm -f dram.bin vnc.sock qmp.sock q6link.sock qemu.log qemu.out "$PKG.out"

# The guest's RAM comes up through this file and has to start out empty: last
# boot's leftovers - the ram_console ring among them - make a guest still
# sitting in the bootloader look like one that booted.
truncate -s 1073741824 dram.bin 2>/dev/null || dd if=/dev/zero of=dram.bin bs=1M count=1024 2>/dev/null

export HOME=$D/emu
export TMPDIR=$R
export ADRENO_GLES_PBUFFER=1
export ADRENO_STATS=5
export TOUCH_DEBUG=1
if [ -n "${GDEBUG:-}" ]; then
    export ADRENO_GLES_DEBUG=1
    export ADRENO_SNAP_DEBUG=1
fi
if [ -n "${GNODEFER:-}" ]; then
    export ADRENO_GLES_NODEFER=1
fi
if [ -n "${GTRACE:-}" ]; then
    export ADRENO_TRACE=$R/draw.log
    export ADRENO_TRACE_ALL=1
fi
unset LD_LIBRARY_PATH

"$ARM" \
  -L "$D/emu/pc-bios" \
  -M "htcleo,baseband=lle,baseband-firmware=$D/images/amss.elf,baseband-trace=off" \
  -drive "if=mtd,format=raw,file=$D/images/nand-lle.img" \
  -display none \
  -vnc "unix:$R/vnc.sock" \
  -serial "file:$R/uart1" \
  -serial "file:$R/ramcon.log" \
  -qmp "unix:$R/qmp.sock,server=on,wait=off" \
  -qmp "tcp:127.0.0.1:4444,server=on,wait=off" \
  -audiodev none,id=snd0 \
  -global msm-modem.audiodev=snd0 \
  -global "msm-adreno.backend=$BACKEND" \
  -global msm-modem.clk-regime-fake=on \
  -d unimp,guest_errors \
  -D "$R/qemu.log" \
  -drive "if=sd,format=raw,file=$D/images/sdcard.img" \
  -nic user,model=bcm4329 \
  -nic user,model=msm-modem,net=10.0.3.0/24 \
  -chardev socket,id=adb,host=127.0.0.1,port=5555,server=on,wait=off \
  -global msm-hsusb.adb=adb \
  -object memory-backend-file,id=htcleo.ram,size=1G,mem-path="$R/dram.bin",share=on \
  -machine memory-backend=htcleo.ram,qdsp6=lle,qdsp6-link=q6link \
  -chardev socket,id=q6link,path="$R/q6link.sock",server=on,wait=off &
ARM_PID=$!

# The Q6 connects to the socket the phone just made, so wait for it: starting
# them together leaves the DSP with no link, and the guest's audio driver
# faults on it.
i=0
while [ ! -e "$R/q6link.sock" ] && [ $i -lt 150 ]; do
    sleep 0.1
    i=$((i + 1))
done

"$Q6" \
  -L "$D/emu/pc-bios" \
  -M "htcleo-q6,link=q6link,apps-irq=11,modem-irq=2,trace=off" \
  -kernel "$D/images/dsp1.elf" \
  -icount shift=auto \
  -object memory-backend-file,id=htcleo.ram,size=1G,mem-path="$R/dram.bin",share=on \
  -machine memory-backend=htcleo.ram \
  -chardev socket,id=q6link,path="$R/q6link.sock" \
  -audiodev none,id=q6snd \
  -global htcleo-q6-audif.audiodev=q6snd \
  -display none \
  -d guest_errors,unimp \
  -D "$R/q6.log" &
Q6_PID=$!

wait $ARM_PID
kill $Q6_PID 2>/dev/null
wait $Q6_PID 2>/dev/null