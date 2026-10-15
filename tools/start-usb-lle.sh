#!/bin/sh
# Start the HD2 with its USB gadget on *this machine's* USB stack: the
# emulated phone is a real USB device to the host kernel, so lsusb and dmesg
# show it, fastboot works and adb talks to it with no bridge in between.
# usage: start-usb-lle.sh [extra qemu args]
#   Same environment as start.sh (NAND=, CLK_KEYS=, AUDIODEV=, BASEBAND=);
#   the adb TCP bridge is off, since adb is local now.
#   QEMU_D="unimp,guest_errors,trace" logs the USB/IP traffic to qemu.log.
#   Only one emulated phone can hold a vhci_hcd port, so a previous run that
#   was killed still owns it: `sudo rmmod vhci_hcd; sudo modprobe vhci_hcd`
#   releases it.
#
# vhci_hcd's attach attribute is root-only, and the socket fd has to be valid
# in the process that writes it -- so QEMU itself has to be root.  Come back
# through sudo rather than trying to write the attribute from a shell: that is
# the one thing here that cannot be done on the side.  (`/usr/bin/sh` is the
# NOPASSWD entry; `sudo env` and `sudo -E` are not, and both prompt.)
sudo modprobe vhci_hcd 2>/dev/null

# sudo resets the environment, so carry the ones start.sh reads over as
# arguments: '-' for unset, '@' in front for set-but-empty (CLK_KEYS= and
# CLK_KEYS unset mean different things).
if [ "$(id -u)" != 0 ]; then
	exec sudo /usr/bin/sh -c '
		script=$1; shift
		export USB=lle
		[ "$1" != - ] && export NAND="${1#@}"; shift
		[ "$1" != - ] && export CLK_KEYS="${1#@}"; shift
		[ "$1" != - ] && export AUDIODEV="$1"; shift
		[ "$1" != - ] && export BASEBAND="$1"; shift
		[ "$1" != - ] && export QEMU_D="$1"; shift
		exec "$script" "$@"
	' sh "$(cd "$(dirname "$0")" && pwd)/$(basename "$0")" \
	  "$([ "${NAND+set}" = set ] && echo "@$NAND" || echo -)" \
	  "$([ "${CLK_KEYS+set}" = set ] && echo "@$CLK_KEYS" || echo -)" \
	  "$([ "${AUDIODEV+set}" = set ] && echo "$AUDIODEV" || echo -)" \
	  "$([ "${BASEBAND+set}" = set ] && echo "$BASEBAND" || echo -)" \
	  "$([ "${QEMU_D+set}" = set ] && echo "$QEMU_D" || echo -)" \
	  "$@"
fi
USB=lle exec "$(dirname "$0")/start.sh" "$@"