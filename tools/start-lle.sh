#!/bin/sh
# Start the HD2 with the real baseband firmware (AMSS) on the modem core:
# start.sh with BASEBAND=lle and the NAND image that has the radio partitions.
# usage: start-lle.sh [extra qemu args]
#   images/amss.elf and images/nand-lle.img: tools/hd2_radio.py (README)
#   TRACE=on logs the modem core's peripheral accesses to images/qemu.log
#   CLK_KEYS= stops in the cLK menu; NAND= picks another image
BASEBAND=lle NAND=${NAND:-nand-lle.img} exec "$(dirname "$0")/start.sh" "$@"
