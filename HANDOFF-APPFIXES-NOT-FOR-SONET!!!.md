# Handoff: app / performance / audio work (2026-11-05/06)

Written by Claude (Opus) for picking this up later.  Everything below is
committed on branch `spl_boot` (bb01217..6e63992, see "Commits").  Test everything on the S25 (the laptop is far too
slow and its Turks GPU is useless for the GLES renderer).

## Phone access

- Wireless adb: `10.0.0.28:<port>` (port changes; `adb mdns services` /
  `adb devices`).  USB adb also works when plugged in.
- Root: `adb shell su -c '...'`.  Never reboot the phone, never write
  `sys.usb.config`.
- `tools/phone-deploy.sh` builds `qemu-system-arm` for Android, pushes it and
  (with `PHONE_DEPLOY_APK=1`) rebuilds/installs the APK.  It picks the first
  adb device.  Needs `JAVA_HOME=/home/j0sh1x/toolchain/jdk` (the shell's own
  JAVA_HOME points at a dead path).
- The DSP binary is NOT rebuilt by phone-deploy: build it yourself and stage it:
  ```
  cd android
  ninja -C build/arm64-v8a/qemu qemu-system-hexagon
  cp build/arm64-v8a/qemu/qemu-system-hexagon app/src/main/jniLibs/arm64-v8a/libhtcleo-q6.so
  $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all app/src/main/jniLibs/arm64-v8a/libhtcleo-q6.so
  ```
  then run phone-deploy with `PHONE_DEPLOY_APK=1`.
- App settings live in `/data/user/0/org.htcleorevivalproject.hd2emu/shared_prefs/htcleo.xml`
  (force-stop the app before editing).  The user's own settings are backed up
  in `/data/local/tmp/htcleo.user2.xml`.  Current settings: `wm6nand.img` or
  `nand-cm9-gpu.img`, gpu `gles`, dsp `lle`, `icountShift -1`,
  **`glesBarrier 1`**, extraArgs `-qmp tcp:127.0.0.1:4444,server=on,wait=off`
  (debug QMP; `adb forward tcp:4444 tcp:4444`).
- **Guest adb (app's adb bridge on phone 127.0.0.1:5555) is broken**:
  `adb connect` via a forward lists the device, but every command hangs and
  it wedged the laptop's adb server (phone adb hung too).  Recover with
  `adb kill-server`, reconnect.  Not investigated yet.

## Fixed / changed (all uncommitted)

### Radio chain / boot (earlier in the session)
- Crypto engine: `seg_done` flag stops re-running the hash on extra DATA_IN
  words; `adm_copy` allocates word-aligned buffers.  FSBL/OSBL now verify AMSS.
- PS_HOLD dropped after it was raised = PMIC restart (not power-off), with
  watchdog power-on reason (`SRC_WDOG_TOUT`, reg 0x40 bit 1) so the restart
  is not taken for off-mode charging.  PS_HOLD log prints lr + stack.
- QDSP6 released when OSBL writes Q6SS+0x28 bit 0 ("Boot adsp" on GO2AMSS),
  not at reset; held in reset (link byte 0x7e) on every board reset.
- SuperCID everywhere: `msm_modem_lle.c` seed and all NAND generators
  (`hd2_mknand.write_htc_config()`: 0:HTC header + CID record, 0:FOTA mode 0).
- Full stock WinMo RUU (`RUU_signed.nbh` from the RUU exe) flashes through
  HSPL on the real chain and boots; MAGLDR 1.13 boots (no SPL) on the laptop.

### Android app / performance
- **App minSdk 34, native build API 34** (`android/scripts/build-native.sh`
  `API=34`; old API28 build kept in `android/build/*.api28`).  Removes
  emulated TLS (~7.5% of CPU).
- App default clock = Auto (`Config.icountShift = -1`): icount put both guest
  cores on one host thread.
- NAND model caches one erase block (`msm_nand.c`): ~9x fewer syscalls
  (each block-layer read was a coroutine + thread pool + eventfd).
- Silenced idle-loop logging: VIC 0xE4 (`IRQ_IN_STACK`), mtmr 0x708/0x810,
  Hexagon trap0 log (QuRT syscalls).  Each log line was a syscall through
  Samsung DEFEX.
- MDP PPP blitter (`msm_mdp_ppp.c`): LUT for 16-bit unpack, byte/NEON
  swizzle for 32-bit, NEON blend (exact float32 /255, proven for the whole
  range) and NEON pack.  ~3x faster per pixel.  Per-blit-type accounting
  printed every 2 s.
- MDP display refresh (`msm_mdp.c`): converts only changed rows
  (`msm-mdp.diff`, default on), RGB565 LUT.  `msm-mdp.verify=on` checks both
  refresh and every blit row against the plain code -- 0 mismatches on the
  phone.  Stats line every 2 s: blits/s, refreshes, rows, ms.
- GLES renderer: dashed dock line = race with `glesBarrier 0`; level 1 is
  correct (matches soft renderer pixel-for-pixel) and not slower.  Draw
  stats now also count barriers.
- Hexagon: `do_interrupt` with no event pending is now ignored (it hit the
  SSR.EX check and could become a double exception); real double
  exceptions dump the last 32 exceptions + registers to q6.log.
- Q6 audio interface: every 2 s logs DSP clock vs real time, periods,
  silent/stale periods, longest gap (`q6-audif:` lines in q6.log).  Deployed,
  not yet looked at with audio playing.

## Measured state (phone, Android CM9, swiping home screens)
- Guest does ~160-190 blits/s, dips to ~80-120 sometimes.
- Apps vCPU thread ~72% of a core, modem ~20%, main ~11%.  The apps thread
  waits the rest: mostly `adreno_drawctxt_wait` (the phone's GPU, GLES
  read-backs / resolves) and futex (BQL).
- Prime core runs 2.2-2.4 GHz (governor), not 4.47 GHz.
- Profile top now: TCG lookups (`helper_lookup_tb_ptr`, `qht_lookup_custom`),
  TLB refills (QEMU TLB untagged: every guest context switch flushes).
- User reports: up to ~30 fps sometimes, then 1-2 fps; audio better but
  still laggy.  Audio underruns = DSP/guest too slow (bursts with gaps).

## Next steps (in order)
1. **GPU read-back waits** in the GLES renderer (`msm_adreno_gles.c`,
   `adreno_resolves_flush`, `adreno_gl_gmem_to_cpu`): make them async (PBO +
   fences) or avoid them -- ~25% of the apps thread is waiting there.
2. **Audio**: play a ringtone, read the `q6-audif:` lines.  If the DSP clock
   is < 1.0x real time, look at the DSP's speed (its icount is `shift=auto`;
   try a fixed shift or none) and its CPU profile.
3. Find the 1-2 fps moments: run `cpumon.sh`-style sampling (thread CPU +
   per-2s mdp stats) during real use and correlate with GPU waits/thermal.
4. DRAM still on f2fs (`dram.bin`, shared with the DSP process).  memfd via
   `/proc/self/fd` is blocked by SELinux; do it with fd passing over a unix
   socket (SCM_RIGHTS) + `/dev/fdset`, no root.  Code for the memfd is in
   `Emulator.java` but disabled (`dramMem = null`).
5. Modem idle (~20%): AMSS does WFI (0xf00116c4) but wakes at 200 Hz and
   flushes the TLB each time (ARMv5, no ASID); probably inherent.
6. Fix the guest adb bridge.

## Commits
bb01217 crypto engine + data mover (FSBL checks AMSS)
7b1ef96 PS_HOLD let go after boot restarts the phone (+ watchdog reason)
1e8d998 OSBL starts the QDSP6
4bc4fd8 SuperCID (QEMU seed + NAND generators)
38b106b NAND erase-block cache
575c491 AMSS idle loop stops filling the log
314d92d MDP: fast PPP blits, refresh of changed rows only
e32f546 GLES draw stats count barriers
f8bdb06 Hexagon/Q6: DSP crash and audio diagnostics
6e63992 Android 14+, API 34 native build, real-time clock default

Left out on purpose: `.gitignore` (the user's own edit), `tools/hd2_xnu.sh`,
`videolog/`.

Test scripts used are in `build/claude/spl/` (ruuflash.sh, lcycle.sh,
osrun.sh) -- the phone benchmark scripts were in the session scratchpad and
are gone; recreate as needed (swipe via QMP `input-send-event`, see
`tools/touch.py`).
