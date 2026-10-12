#!/bin/sh
# Start the HD2 with both coprocessors running their real firmware: the
# modem core runs AMSS (start-lle.sh) and the QDSP6 runs DSP1 in
# qemu-system-hexagon -M htcleo-q6.  The two processes share the DRAM
# through a file on disk (images/dram.bin) and carry the interrupt lines
# between them over a unix socket (images/q6link.sock).
# usage: start-q6.sh [extra qemu-system-arm args]
#   images/dsp1.elf: tools/hd2_radio.py RUU_signed.nbh --dsp dsp1.elf
#   Q6TRACE=off stops logging the DSP's peripheral accesses to q6.log;
#   Q6_D= sets its -d flags (default guest_errors,unimp), Q6_ARGS= adds more
T=$(cd "$(dirname "$0")" && pwd)
cd "$T/../images" || exit 1
rm -f dram.bin q6link.sock q6.log q6qmp.sock
truncate -s 1G dram.bin
MEM="-object memory-backend-file,id=htcleo.ram,size=1G,mem-path=dram.bin,share=on"
"$T/start-lle.sh" $MEM -machine memory-backend=htcleo.ram,qdsp6=lle,qdsp6-link=q6link \
  -chardev socket,id=q6link,path=q6link.sock,server=on,wait=off "$@"
for i in $(seq 20); do [ -S q6link.sock ] && break; sleep 0.3; done
../src/qemu/build/qemu-system-hexagon \
  -M htcleo-q6,link=q6link,apps-irq=${Q6_APPS_IRQ:-11},modem-irq=${Q6_MODEM_IRQ:-2},trace=${Q6TRACE:-on} -kernel dsp1.elf \
  -icount shift=${Q6_SHIFT:-1} \
  $MEM -machine memory-backend=htcleo.ram \
  -chardev socket,id=q6link,path=q6link.sock \
  -display none -qmp unix:q6qmp.sock,server,nowait \
  -d ${Q6_D:-guest_errors,unimp} -D q6.log $Q6_ARGS > q6.out 2>&1 &
