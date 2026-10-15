# Known quirks

Open problems and workarounds of the real-firmware (LLE) modes. The README
describes what works.

## Real baseband + DSP (tools/start-q6.sh): reliability

Use `tools/boot-q6.sh` to boot this mode in scripts. It waits for Android
and fails within seconds when a QEMU dies, or when a proc_comm command stays
unanswered (below), printing the apps and modem cores' state, the pending
command, the modem VIC and every DSP thread's interrupt state.

### Boot/run hang: a proc_comm command AMSS never answered (fixed)

- Symptom: Linux spun in its proc_comm wait loop (apps PC in `__delay`,
  return address 0xc0079500) with `APP_COMMAND` at SMEM 0x100000 stuck,
  usually 0x32/0x33 (clock regime enable/disable); AMSS and the DSP idle.
  Without the radio up, GSM never started either.
- Cause: AMSS's interrupt glue (tramp_l4.c) masks a line while its
  handler runs and unmasks it just before marking the handler done; a
  doorbell dispatched in between is taken for a nested interrupt and
  dropped. On the phone the next proc_comm cannot ring that early; here
  the apps core runs on its own host thread and sometimes did.
- Fix: the modem VIC holds apps doorbells (lines 56-63) that ring while
  the line is masked, or that software clears while it is masked, and
  latches them 100 us after it is unmasked (msm_vic.c "redeliver").
  The first version held only the former; a doorbell that rang with the
  line unmasked while its handler still ran was taken for a nested
  interrupt, masked and cleared, and lost (2 of 3 boots with AMSS's debug
  log on, which slows the modem).
- If it ever comes back, `tools/boot-q6.sh` reports it within seconds;
  `-global msm-vic.watch=62` plus `qom-get /machine/unattached/device[0]
  watch-log` shows line 62's last 256 events with the accessing PCs.

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

## GSM network (mDSP model)

The cell and the network behind it are modelled in hw/arm/htcleo/msm_mdsp.c
at the mDSP's command interface (AMSS's GSM L1 drives it as on the phone):

- The cell is on every ARFCN: the RF synthesizer's tuning (SBI writes) is
  not decoded, so power scans find it everywhere and AMSS may camp on any
  ARFCN; channels are assigned on 133 (its cell allocation).
- Registration, calls and texts: RACH, IMMEDIATE ASSIGNMENT (SDCCH/8 on
  timeslot 1), LAPDm on SAPI 0 and 3, location updating without
  authentication or ciphering (no TMSI), CM service, paging by IMSI,
  calls on a TCH/F (timeslot 2, early assignment), SMS both ways.
  Answered calls carry silence: speech frames are not routed anywhere.
- Not yet: speech, supplementary services, handovers.
- GPRS (hw/arm/htcleo/gsm_gprs.c): network mode II, attach, one PDP
  context (IPCP DNS in the PCO), LLC UI and SNDCP unit data, no ciphering
  or compression. The PDCH is on timeslot 5: single-slot TBFs, the
  uplink (USF 1, acknowledged mode, extended uplink TBFs), PACCH and
  polls; downlink TBFs (unacknowledged mode) are widened by a PACKET
  DOWNLINK ASSIGNMENT to timeslots 3-6, a layout the HD2 (multislot
  class 12, type 1) can receive while transmitting on 5. Coding scheme
  `gprs-cs` (default CS-2) both ways; `gprs-slots` downlink slots
  (default 4). The mDSP side: receive commands carry an entry per slot
  (slot index in the result's status bits 15-13, metrics 0x28 words
  apart), transmit commands up to two entries (one per uplink slot, a
  poll's answer first), the coding scheme in the channel numbers.
  `-global msm-mdsp.gprs-trace=on` (or `qom-set`) logs the PDCH's blocks.
- GPRS throughput with slirp as the far end is TCP-bound at small sizes
  (its TCP starts slowly over the ~1 s round trips of a busy downlink);
  measure with transfers of 100 KB or more.
- The cell keeps L1's frame numbers instead of its own time: AMSS restarts
  the GSM frame timer after sleep by a register write, which lands a
  variable time late here, and L1 would drift from an independent cell.
- AMSS sometimes misses a GSM frame tick when the modem core is slow
  ("Missed 1 GSM frames"); a RACH's request reference then names a frame
  AMSS does not expect, and it retries the RACH.
- The first network search can take minutes: AMSS tries WCDMA first, and
  with `AMSSLOG=1` everything is several times slower.
