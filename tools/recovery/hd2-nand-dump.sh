#!/sbin/sh
# Back up the HD2's radio area to the SD card: NAND blocks 0-0x211 (the
# radio partitions, APPSBL and the factory data blocks), every page with
# its spare bytes, plus a report and an md5 sum.  Read only.

OUT=/sdcard/HD2-NAND

mtd=$(grep '"hd2radio"' /proc/mtd | cut -d: -f1)
if [ -z "$mtd" ]; then
	echo "No hd2radio partition: this recovery kernel cannot read"
	echo "the radio area."
	exit 1
fi
dev=/dev/mtd/$mtd
[ -e "$dev" ] || dev=/dev/$mtd

mount /sdcard 2>/dev/null
if ! grep -q " /sdcard " /proc/mounts; then
	echo "The SD card is not mounted."
	exit 1
fi
free_kb=$(df /sdcard | awk 'NR > 1 { a = $(NF - 2) } END { print a }')
if [ "${free_kb:-0}" -lt 80000 ]; then
	echo "Needs 80 MB free on the SD card (has ${free_kb} KB)."
	exit 1
fi

mkdir -p $OUT
base=hd2radio-$(date +%Y%m%d-%H%M%S)
echo "Reading $dev into $OUT/$base.nand"
/sbin/hd2nanddump $dev $OUT/$base || exit 1
cat /proc/mtd >> $OUT/$base.txt
cd $OUT && md5sum $base.nand > $base.md5
sync
echo ""
echo "Saved $OUT/$base.nand"
echo "It holds this phone's IMEI and radio calibration:"
echo "keep it private."
