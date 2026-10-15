#!/bin/sh
# USB LLE regression: start the emulator, one getvar and one flash, reset,
# repeat.  One getvar + one flash per run.
set -u
cd "$(dirname "$0")/.." || exit 1

RUNS=${RUNS:-5}
IMG=${IMG:-images/nand-clk.img}
PRISTINE=${PRISTINE:-images/nand-clk.img.pristine}
BLOB=${BLOB:-images/HD2_TWRP_2.4.0.0-nanddump.img}

for n in $(seq "$RUNS"); do
	# a killed run leaves its port busy, and the new one would fail to attach
	sudo /usr/bin/env pkill -f 'qemu-system-arm.*htcleo' >/dev/null 2>&1
	sleep 2
	sudo /usr/bin/env rmmod vhci_hcd >/dev/null 2>&1
	sudo /usr/bin/env modprobe vhci_hcd >/dev/null 2>&1
	sleep 1
	# the flash writes to the image, and a recovery image would send the next
	# boot into recovery instead of fastboot, so every run starts the same
	[ -f "$PRISTINE" ] && cp "$PRISTINE" "$IMG"

	echo "=== run $n/$RUNS ==="
	# start.sh runs from images/, so NAND= is a name there, not a path
	CLK_KEYS= NAND="$(basename "$IMG")" ./tools/start-usb-lle.sh >/dev/null 2>&1
	sleep 32

	printf '  lsusb : %s\n' "$(lsusb | grep -o '18d1:0d02' || echo MISSING)"
	printf '  getvar: %s\n' \
	    "$(timeout 30 fastboot -s usb:5-2 getvar product 2>&1 | head -1)"
	printf '  flash : %s\n' \
	    "$(timeout 900 fastboot -s usb:5-2 flash recovery "$BLOB" 2>&1 |
		grep -E 'OKAY|FAILED' | tail -1)"
	printf '  clk   : %s\n' "$(tail -1 images/uart1.log)"
done

sudo /usr/bin/env pkill -f 'qemu-system-arm.*htcleo' >/dev/null 2>&1
echo "=== done ==="