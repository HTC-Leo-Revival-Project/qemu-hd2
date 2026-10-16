#!/bin/sh
# How often is the guest actually redrawing?
#
# The Adreno stats line is per interval, so counting the lines over a window
# gives intervals per second and summing their kick counts gives the guest's
# own update rate.  This is the measurement that says whether the renderer or
# the present path is the limit: if soft and gles come out the same, the
# renderer is not what is holding the frame rate up.
#
# usage: phone-frame-rate.sh [seconds]
SECS=${1:-30}
export LC_ALL=C   # so awk prints 49.9 and not 49,9
PKG=org.htcleorevivalproject.hd2emu
S=$(adb devices | awk '$2 == "device" { print $1; exit }')
adb() { command adb -s "$S" "$@"; }

# The log does not print the backend; GLES says so in its own words.
if adb shell "run-as $PKG grep -aq 'running on this host' files/gemu.out"; then
    backend=gles
else
    backend=soft
fi
echo "renderer: $backend"

adb shell "run-as $PKG grep -a '^adreno: [0-9]* kicks' files/gemu.out | tail -1" >/dev/null
start=$(adb shell "run-as $PKG wc -l < files/gemu.out")
sleep "$SECS"
lines=$(adb shell "run-as $PKG grep -a '^adreno: [0-9]* kicks' files/gemu.out | tail -$((SECS / 5 + 2))")

kicks=$(echo "$lines" | sed -n 's/^adreno: \([0-9]*\) kicks.*/\1/p' | paste -sd+ | bc 2>/dev/null || echo 0)
intervals=$(echo "$lines" | grep -c '^adreno: ')
draws=$(echo "$lines" | sed -n 's/.*draws \([0-9]*\) gpu.*/\1/p' | paste -sd+ | bc 2>/dev/null || echo 0)
gpus=$(echo "$lines" | sed -n 's/.*draws [0-9]* gpu \([0-9]*\).*/\1/p' | paste -sd+ | bc 2>/dev/null || echo 0)

echo "over ${SECS}s: $intervals stats intervals, $kicks kicks, $draws draws ($gpus on the host GPU)"
[ "$intervals" -gt 0 ] && echo "=> $(awk "BEGIN{printf \"%.1f\", $kicks*$intervals/$SECS}") kicks/s"