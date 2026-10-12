#!/bin/sh
# Dump guest RAM over QMP and recover logcat into logcat.txt
# usage: logcat.sh [GREP-REGEX]
cd "$(dirname "$0")/../images" || exit 1
python3 ../tools/qmon.py qmp.sock "pmemsave 0 0x1E800000 ram.bin" > /dev/null
python3 ../tools/logcat_dump.py ram.bin > logcat.txt
[ -n "$1" ] && grep -aE "$1" logcat.txt
