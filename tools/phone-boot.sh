#!/bin/sh
# Headless boot of the emulated HD2 on the phone, and nothing else: no app,
# no screen, no touching it.  Everything comes back through adb - QMP for
# screenshots and registers, the guest's own console for boot progress.
#
# usage: phone-boot.sh [soft|gles] [trace|nodec|trace-nodec]
#
#   soft|gles   renderer (default gles).  Always the real baseband (LLE); the
#               hle stub is not used, and never should be: it does not boot.
#   trace       log every draw and its vertices to files/emu/run/draw.log
#   nodec       renderer experiment: copy each bin out as it is issued rather
#               than at the end of the kick
#
# A boot takes about four minutes to the launcher, and is unattended: the
# bootloader is sent F2 by phone-keys.py, which is what makes it go at all.
# See android/HEADLESS.md.
PKG=org.htcleorevivalproject.hd2emu
BACKEND=${1:-gles}
MODE=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
D=/data/user/0/$PKG/files

S=$(adb devices | awk '$2 == "device" { print $1; exit }' 2>/dev/null)
[ -n "$S" ] || { echo "no phone over adb"; exit 1; }
adb() { command adb -s "$S" "$@"; }

# Build first: a headless run uses files/emu/bin/qemu-arm, which only the APK
# refreshes, so without this it can be testing any build but this one.
sh "$HERE/phone-deploy.sh" || exit 1

adb shell "su -c 'for p in \$(ps -A | grep -E \"qemu-arm|qemu-q6\" | tr -s \" \" | cut -d\" \" -f2); do kill -9 \$p 2>/dev/null; done'"
sleep 3
adb shell am force-stop $PKG 2>/dev/null
adb forward --remove-all >/dev/null 2>&1
adb forward tcp:4444 tcp:4444 >/dev/null
adb forward tcp:5555 tcp:5555 >/dev/null

adb push "$HERE/glesrun.sh" /data/local/tmp/glesrun.sh.new >/dev/null
adb shell "su -c 'cp /data/local/tmp/glesrun.sh.new /data/local/tmp/glesrun.sh; chmod 755 /data/local/tmp/glesrun.sh'"

ENV=""
case "$MODE" in
    *trace*) ENV="$ENV GTRACE=1" ;;
esac
case "$MODE" in
    *nodec*) ENV="$ENV GNODEFER=1" ;;
esac
case "$MODE" in
    *snap*) ENV="$ENV GDEBUG=1" ;;
esac

echo "$BACKEND headless start $(date +%T) ${ENV:-(no trace)}"
adb shell "run-as $PKG sh -c '$ENV setsid sh /data/local/tmp/glesrun.sh $D/emu/bin/qemu-arm $D/emu/bin/qemu-q6 $BACKEND > $D/gemu.out 2>&1 < /dev/null &'"

# The cCLK bootloader waits at its menu for a key press before it boots
# anything.  Without this the guest sits in the bootloader forever, ramcon
# stays empty, and the PC parks at 0x28007b92 - which looks like a hang and
# is not one.  The app does the same thing from startBootKeys().
sleep 12
python3 "$HERE/phone-keys.py" f2 >/dev/null 2>&1 || true