# Known quirks

Open problems and workarounds of the real-firmware (LLE) modes. The README
describes what works.

## Real baseband + DSP (tools/start-q6.sh): reliability

Use `tools/boot-q6.sh` to boot this mode in scripts. It waits for Android
and fails within seconds when a QEMU dies, or when a proc_comm command stays
unanswered (below), printing the apps and modem cores' state, the pending
command, the modem VIC and every DSP thread's interrupt state.

### Rare boot hang: a proc_comm command AMSS never answers

- Symptom: the kernel console stops (often during a YAFFS mount); Linux
  spins in its proc_comm wait loop (apps PC in `__delay`, return address
  0xc0079500) with `APP_COMMAND` at SMEM 0x100000 stuck at 0x32/0x33
  (`CLK_REGIME_SEC_ENABLE`/`DISABLE`, clock 0x13, the clock Linux's NAND
  driver votes for around every NAND command). AMSS sits in its idle loop
  with nothing pending at its VIC; the DSP is idle with nothing pending
  either. Recover by rebooting.
- Frequency: about 1 boot in 10 in testing. It needs heavy proc_comm
  traffic, which a YAFFS mount without a checkpoint makes (thousands of
  calls); since `boot-q6.sh` remounts the YAFFS partitions read-only before
  killing the emulator (writing the checkpoints), it is rarer: the last 11
  boots of testing were clean. `NO_CHECKPOINT=1` skips the remount to
  provoke it.
- What is known: the apps doorbell (A2M 6, modem VIC line 62, edge
  triggered) arrives; normally AMSS takes it as: L4 kernel reads the vector
  twice (PC 0xf0012cbc), AMSS's interrupt code disables the line (0x9d18cc),
  clears it twice (0x9d1924) and re-enables it (0x9d1852). In a hang the
  doorbell came 2 us after the previous one's last clear, while AMSS was
  still finishing it; the vector was read once and the line cleared, and
  the command was never processed. Probably a race that the real ARM9's
  timing hides (Linux on its own host thread answers within microseconds).
- Tools: `-global msm-vic.watch=62` keeps the last 256 events of that line
  (edges, vector reads, clears, enables, with the accessing PC); read them
  with `qom-get /machine/unattached/device[0] watch-log` (tools/qmon.py).

### Slow boots after an unclean stop

Without a YAFFS checkpoint the first mount scans the whole NAND, and in
this mode each NAND command costs a proc_comm round trip to AMSS: 40-100 s
of boot instead of 25-40 s. `boot-q6.sh` remounts read-only before it
kills the emulator; do the same (or shut Android down) in other scripts.

### First video recording after boot

The first `recordvideo` (or camcorder session) right after boot can leave an
unfinished MP4 without its index: the SD card is mounted late, so a file
removed or written before the mount is not the one read back afterwards.
Wait until `mount` lists /mnt/sdcard, or make a throwaway recording first.

### DSP speed and timing

- The DSP runs at about 38 fps on a 320x240 H.264 clip (stagefright -r),
  estimated 5-10x slower than the real QDSP6; larger videos are likely
  slower than real time.
- With a fixed `-icount shift=1` the DSP's clock runs at about half of real
  time while it is busy, which halves the audio rate; `shift=auto` (the web
  UI's default) keeps time but gives the firmware fewer instructions per
  5 ms audio period, and playback then has 5 ms holes (open).
- Fine round-robin slices (`smt-slice` below 8) slow the DSP so much that
  AMSS's boot-time exchanges with it time out.

## DSP video encoder: motion vectors

`venc-me` (htcleo-q6 machine property) selects the encoder core's motion
search:

- `0` (default): no search; every macroblock of a P picture is coded intra.
  Always decodes correctly, but P pictures are as large as I pictures.
- `1` / `2` (experimental): a full-pel / half-pel search. The firmware codes
  the vectors as intended (checked with ffmpeg's exported motion vectors),
  but its inter frames overflow the 25 KB output buffer ("Buffer overrun" in
  logcat) and the recording stops. Not yet understood.

What is reverse-engineered (hw/hexagon/htcleo_q6_venc.c):
- word 2 of an encoder command is the reference picture (the previous
  picture's reconstruction), not this picture's reconstruction;
- MB information record, word 2: bits 0-1 = 3 intra; else bits 2-3 = 3
  inter with one vector, 0 inter with four (1 and 2 leave the type unset);
  words 3-4: vectors as signed bytes in quarter pels;
- the firmware takes the residual against the prediction stream (stream 2)
  and reads chroma from the reference picture itself.
