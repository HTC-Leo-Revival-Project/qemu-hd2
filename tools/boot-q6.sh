#!/bin/sh
# Boot the HD2 with AMSS and the DSP1 firmware (start-q6.sh) and wait for
# Android, failing fast: exits 0 when sys.boot_completed is 1, else 1 with
# the reason (a QEMU died or refused to start, the boot stalled, timeout).
# A stalled boot is diagnosed: where the apps core spins, the pending
# proc_comm command, and each DSP thread's interrupt state.
# usage: boot-q6.sh [start-q6.sh args]   (env as for start-q6.sh)
#   BOOT_MAX= seconds to wait for Android (default 240), BOOT_STALL= seconds
#   without kernel console output before checking for the proc_comm hang (30)
R=$(cd "$(dirname "$0")/.." && pwd)
I=$R/images
A="adb -s 127.0.0.1:5555"
# a running Android: remount read-only so YAFFS writes its checkpoint (the
# next boot then skips the full NAND scan, which in LLE mode costs a
# proc_comm round trip to AMSS per NAND command)
[ -n "$NO_CHECKPOINT" ] || timeout 15 $A shell 'sync; for m in /data /cache /system; do mount -o remount,ro $m; done' >/dev/null 2>&1
pkill -x qemu-system-arm; pkill -x qemu-system-hex; sleep 1
"$R/tools/start-q6.sh" "$@"

qmon() { timeout 5 python3 "$R/tools/qmon.py" "$@" 2>/dev/null; }
diagnose() {
  echo "  apps core: $(qmon "$I/qmp.sock" 'info registers' | grep -oE 'R14=[0-9a-f]+|R15=[0-9a-f]+' | tr '\n' ' ')"
  echo "  proc_comm: $(qmon "$I/qmp.sock" 'xp/8wx 0x100000' | tail -2 | cut -d: -f2 | tr '\n' ' ')"
  echo "  last kernel line: $(tail -n 1 "$I/ramcon.log")"
  for k in 1 2 3; do
    echo "  modem core: $(QMON_CPU=1 qmon "$I/qmp.sock" 'info registers' | grep -oE 'R14=[0-9a-f]+|R15=[0-9a-f]+|PSR=[0-9a-f]+' | tr '\n' ' ')"
  done
  echo "  modem VIC enable/raw pending (lines 0-63): $(QMON_CPU=1 qmon "$I/qmp.sock" 'xp/2wx 0xb8001010' 'xp/2wx 0xb80010a0' | cut -d: -f2 | tr '\n' ' ')"
  for c in 0 1 2 3 4 5; do
    echo "  dsp$c: $(QMON_CPU=$c qmon "$I/q6qmp.sock" 'info registers' | grep -E ' pc =|ssr =|ipend =' | tr -s ' \n' ' ')"
  done
  echo "  last link events:"; grep 'q6-link' "$I/q6.log" | tail -4 | sed 's/^/    /'
}

adb connect 127.0.0.1:5555 >/dev/null 2>&1
t=0 last=0 still=0
while [ $t -lt "${BOOT_MAX:-240}" ]; do
  sleep 2; t=$((t + 2))
  if ! pgrep -x qemu-system-arm >/dev/null || ! pgrep -x qemu-system-hex >/dev/null; then
    echo "QEMU exited after ${t}s:"; tail -n 3 "$I/qemu.out" "$I/q6.out"; exit 1
  fi
  if grep -qiE 'error|failed|assert|abort' "$I/q6.out" 2>/dev/null; then
    echo "DSP QEMU reported:"; cat "$I/q6.out"; exit 1
  fi
  if [ $((t % 6)) = 0 ] &&
     [ "$(timeout 4 $A shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; then
    echo "booted in ${t}s"; exit 0
  fi
  n=$(stat -c %s "$I/ramcon.log" 2>/dev/null || echo 0)
  if [ "$n" = "$last" ] && [ "$n" -gt 0 ]; then
    still=$((still + 2))
  else
    still=0 last=$n
  fi
  if [ $still -ge "${BOOT_STALL:-30}" ]; then
    # quiet is fine (a YAFFS scan without a checkpoint makes thousands of
    # proc_comm calls) unless one proc_comm command stays unanswered
    pc0=$(qmon "$I/qmp.sock" 'xp/4wx 0x100000' | tail -1 | cut -d: -f2)
    same=1
    for k in 1 2 3 4 5; do
      sleep 2; t=$((t + 2))
      [ "$(qmon "$I/qmp.sock" 'xp/4wx 0x100000' | tail -1 | cut -d: -f2)" = "$pc0" ] || same=0
    done
    if [ $same = 1 ] && [ "$(echo $pc0 | cut -d' ' -f1)" != 0x00000000 ] &&
       [ "$(echo $pc0 | cut -d' ' -f1)" != 0x00000001 ]; then
      echo "boot stalled at ${t}s, proc_comm command unanswered for 10s:"
      diagnose; exit 1
    fi
    still=0
  fi
done
echo "boot timeout after ${t}s:"; diagnose; exit 1
