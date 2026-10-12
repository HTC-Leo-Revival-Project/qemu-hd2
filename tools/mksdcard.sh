#!/bin/sh
# Create a microSD image with one FAT32 partition, as Android's vold expects.
# usage: mksdcard.sh sdcard.img [SIZE_MiB, power of two, default 1024]
set -e
IMG=$1; MB=${2:-1024}
rm -f "$IMG"; truncate -s "${MB}M" "$IMG"
echo 'start=2048, type=c' | sfdisk -q "$IMG"
mkfs.vfat -F 32 -n SDCARD --offset 2048 -h 2048 "$IMG" $(( (MB * 1024 * 1024 - 2048 * 512) / 1024 )) >/dev/null
echo "wrote $IMG (${MB} MiB, FAT32)"
