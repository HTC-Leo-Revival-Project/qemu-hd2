# HD2 QEMU — session handoff (2026-10-12)

## Real DSP firmware (qdsp6=lle), 2026-10-13
`tools/start-q6.sh` runs AMSS on the modem core and the radio ROM's DSP1 on `qemu-system-hexagon -M htcleo-q6`, sharing `images/dram.bin`. README section "Real DSP firmware (QDSP6)" has the device map and tools.
- H.264 decode (Janus VDC, `htcleo_q6_vdc.c`): bit-exact against JM; Stagefright/Gallery play.
- Audio: the AFE's DMA (`htcleo_q6_audif.c`) reaches the host audio backend.
- MPEG-4 encode (`htcleo_q6_venc.c`): `recordvideo` output decodes at 31-37 dB against the test pattern (`Q6_MACH=venc-test-pattern=on`, `tools/vdc/venc_psnr.py`). The core models the MB fetch pipeline; no motion search yet (zero vectors).
- Hexagon fixes that the firmware needed: V2 ALU64 encodings of packhl/add:sat/sub:sat; undecodable packets were silently skipped (cause 0x15 raised as the INT5 event); `Rd = cmpy(Rd, Rt)`-style instructions read their own half-written destination.
- Open: re-check the intermittent decoder crashes (`build-tmp/stress.sh`) after those fixes; motion search; then GSM/3G/GPS in LLE mode.

## Real baseband (baseband=lle), 2026-10-12, latest
`tools/start-lle.sh` boots cLK, Linux and Android to the lock screen on the real AMSS 2.15.50.14 running on the ARM926 modem core. adb works and `sys.boot_completed` is 1. The README section "Real baseband firmware" has the device map, the reproduced OSBL hand-off and the debugging tools (`AMSSLOG=1` = the amsslog TCG plugin). Fixes along the way:
- ADM SD0 page at 0xA9400000;
- NAND BUFFER_STATUS bad-block field and per-codeword ADDR0;
- the mDSP boot handshake;
- modem VIC lines: A2M n = 56+n; M2A = write 1 to 0xB8000594 + 4n;
- SSBI/PM7540 ID (register 0x02 = 0xA2);
- the HTC SMEM config block (VBUS);
- the QDSP6 in "only" mode creates its SMD channels;
- the AKM8973 compass INT storm (also helped HLE).

Later the same day (see README "Real baseband firmware"):
- PMIC (`msm_ssbi.c`): RTC, interrupt controller on GPIO 24, charger/USB and power key.
- SIM: `msm_uim.c` (UART2 in SIM mode) + `sim_card.c`; Android shows `gsm.sim.state=READY`.
- GSM frame timer (`msm_gstmr.c`, line 18), WCDMA STMR (`msm_wstmr.c`, lines 12-14), modem timer match IRQ on line 34 (REX timers).
- mDSP: doorbell bits 19 (MC), 15 (WCDMA sync/search commands at B+0x4958, done event B+0x4E5E), search done = HPQ/LPQ events B+0x4E56/4E54, AGC floor at A+0x1F3C. Image 0x1406 swaps per-app overlays while running, each with its own symbol table ([0x1350B0C]).
- Result: stack cycles WCDMA scan/acq -> GSM pscan -> NO SERVICE -> power save; GPS sessions start and time out without a fix. No crash in long runs.

Open items:
- GSM per-frame DSP interface (doorbell bit 26): power measurements read -70 dBm, so GSM tries a few ACQs per cycle;
- the RF SSBI chips;
- real network/GPS fix are out of reach (no DSP code runs).

HLE Android and the MAGLDR→iOS path were re-tested after these changes.

## Goal in progress
Boot the **iOS 7 HD2 package** (pixeldrain `jnBDwb5c`, `ios7_v2.rar`, an XNU build for the
QSD8250 Leo) in the emulator. The OS is started from the SD card by **MAGLDR 1.13 "Boot AD SD"**
with the folder set to `IOS7`. It runs on the **HLE baseband** (the default, `baseband=hle`).

Status: MAGLDR loads `IOS7/zImage` and `IOS7/initrd.gz` and gets through hand-off stages 1–2.
It then stops with **"FATAL HIT 3"**: its loader fails to load AMSS from NAND (details below).

## Repos (git is now used — commit as you go, never push)
| Path | Branch | What |
|---|---|---|
| `~/clones/hd2-qemu` | `main` | `tools/`, `README.md`, this file. `.gitignore` excludes `src/`, `images/`, `firmware/`, `build-tmp/`, `toolchains/` |
| `src/qemu` | `htcleo` | the board: `hw/arm/htcleo/*` (QEMU v11.1.0 upstream clone) |
| `src/kernel-ics` | `hd2-qemu` | "hd2radio" MTD partition (recovery NAND dump), timeconst.pl fix |
| `src/clk` | `hd2-qemu` | cLK build fix |

The baseline commit in `src/qemu` (82dbb41) contains every emulator change up to and including
this session's work. Uncommitted at the time of writing: a small retry tweak in
`tools/magnav.py`, plus this file.

**Privacy:** `firmware/private/nand-dump/` is the user's real NAND dump. It holds the IMEI and
radio calibration, so never commit, upload or share it.

## Build / run
```bash
ps -eLo comm | grep -qx GameThread || (cd ~/clones/hd2-qemu/src/qemu/build && ninja qemu-system-arm)
```
- Do not build while a game is running (the `GameThread` check above).
- To reach "Boot AD SD" on the iOS setup, run `build-tmp/adsd.sh`. It restarts QEMU with
  `NAND=nand-magldr.img SDCARD=sdcard-ios7.img`, sets AD SD Dir = IOS7 and leaves the cursor on
  item 2. Then press Call: `python3 tools/keys.py images/qmp.sock f2:220`.
- gdb stub: QEMU starts with `-s`. Scripts in `build-tmp/*.gdb` begin with
  `set architecture arm` and `target remote :1234`. The last one is `build-tmp/load.gdb`, which
  traces the loader 0x280232d4.
- See the screen with `tools/magkey.sh` (writes `build-tmp/key.png`). Menu navigation:
  `tools/magnav.py qmp.sock ITEM [select]`. It reads the highlight bar back from the screen,
  because raw key presses drop or repeat.
- MAGLDR keys:
  - VolUp = `pgup`, VolDown = `pgdn`, Call = `f2` (select), Windows = `f1`.
  - In the AD SD Dir picker, VolDown moves and Call selects. **Windows cancels and saves `<.>`.**
  - The AD SD Dir choice is lost after a restart. Pick it again every time (adsd.sh does this).

### ⚠️ Disk pitfall
Never leave QEMU running with `-d unimp` (or a busy trace) while the guest spins on an
unmodelled register. This session `images/qemu.log` reached **19 GB** and filled the disk, and
the full disk truncated `htcleo.c` in the middle of a write. The file was rebuilt from
transcripts. Use `-d guest_errors`, kill QEMU after traced runs with `pkill -x qemu-system-arm`
(never `pkill -f`), and check `df -h /` first. Only about 19 GB is free.

## What was fixed this session (all in src/qemu `htcleo`)
1. **SDCC ↔ ADM CRCI pacing.**
   - MAGLDR's FatFs starts the ADM box command before it sends CMD17.
   - The SDCC FIFO window is now a CRCI window: `msm_adm_add_crci()` and `msm_adm_crci_kick()`
     in `msm_adm.c`, and `msm_sdcc_connect_adm()`.
   - A stalled channel resumes from the waiting descriptor. Pointer-list position is kept in
     `stall_list` / `stall_idx`.
   - The SDCC iomem sets `disable_reentrancy_guard`, because the resume runs inside an SDCC
     register write.
   - Result: the AD SD Dir picker works and the files load.
2. **MAGLDR modem takeover, stage 1 (HLE)** in `msm_modem.c`.
   - The modem places the real AMSS Thumb function (16 bytes) at PA **0x9C1742**. That is where
     AMSS 2.15.50.14 keeps it.
   - On proc_comm **88**, if MAGLDR has patched the function (`movs r0,#0x1f; lsls r0,#16; bx r0`),
     the modem writes **0x50000 to PA 0x1E0000** and sets `taken_over`.
   - After that it ignores all doorbells, because AMSS is no longer running.
3. **Bus-master halt/clock block at 0xA8200000** (`busctl_read_hook` in `htcleo.c`). FSBL uses it
   too.
   - `+0x3300+4n`: bit 0 requests a halt, bit 1 acknowledges it.
   - `+0x100+4n`: bit 5 is the clock enable.
   - `+0x3424`: bit n is set when master n's clock is off.
   - Result: stage 2 passes.
4. SDCC and ADM trace events (`msm_sdcc_*`, `msm_adm_*` in `hw/arm/trace-events`).
5. New tool `tools/magnav.py`. `tools/fat_put.py` (FAT32 writer with LFN entries) is used to
   build `images/sdcard-ios7.img`.

## How MAGLDR's AD boot works (reverse-engineered)
- **Load descriptors:** the Android/AD path is 0x2802292c and the hand-off is 0x28022bfc. Two
  NAND load descriptors are passed in r4:
  - `{1, 0, 0x137, 0xA0}` = DSP1, block 311, 160 blocks. Its predicate allows 0x10000000–0x30000000.
  - `{1, 0, 0x26, 0xC0}` = **AMSS, block 38, 192 blocks**. Its predicate allows 0x200000–0x4000000.
- **Stage 1 (modem takeover):**
  - MAGLDR scans modem RAM (0x500000–0x2500000) for the function and patches it through the
    alias VA = PA + 0x4000000.
  - It copies a stub (MAGLDR 0x2801f6e8..0x2801f860) to **0x1F0000** and sends proc_comm 88
    (doorbell write to 0xAC100418).
  - It waits 300 ms and expects `*(0x1E0000) == 0x50000`.
  - On real hardware the stub then:
    - hooks an OKL4 vector at 0xc65ef800 and makes `svc 6` calls;
    - disables IRQs and writes 1 to modem VA 0xc5c0010c and then to +4 (unknown register);
    - cleans caches, turns the MMU off and jumps to **0x2801f898**.
  - At 0x2801f898 the ARM9 is **parked**. It waits until `*(0x280b7a70) == 0x300d300d`, stores
    `*(0x280b7a74)` to 0x1E0000 and jumps there.
- **Stage 2:** the 0xA8200000 halt/clock handshake (fixed) and clock-control writes at
  0xA8600000, +0x210 and +0x290.
- **Stage 3 (failing now):**
  - MAGLDR calls `0x280232d4(r4+0x20, 0x28022b38)` to ELF-load **AMSS from NAND** into modem
    RAM. The returned entry goes to `[0x280b7a74]`, which is the ARM9's restart address.
  - Stage 4 then loads the DSP the same way and stores its entry to `[0x280b7a78]`.
- **Later stages:**
  - They program 0xA870002c/30/5c (TCSR) and 0xA83C0880.. (SMI/EBI1 memory controller regions,
    which depend on whether the image is at or above 0x2e800000).
  - They then write **0x300d300d to the mailbox**, which releases the ARM9 into the freshly
    loaded AMSS.
  - Finally they return to the caller, which boots the OS.
- **In short: MAGLDR cold-restarts the radio** (fresh AMSS + DSP from NAND) before it boots an
  OS from SD.
- Message codes: "FATAL HIT n" uses string 0x28004490. Stages print "1"…"6".

## Update (later on 2026-10-12): iOS 7 reaches the lock screen
Boot with `build-tmp/iosboot.sh [SECONDS]`. It runs adsd.sh, presses Call, waits, and saves
`build-tmp/ios-screen.png`. About 75 s per run. 3 of the last 4 boots showed the iOS 7 lock
screen; one hit an XNU panic: `RW lock not held (mode 2)`, locks_arm.c:299.

Fixes:
1. NAND (msm_nand.c, committed):
   - A read starting at a column inside a codeword now starts at that offset. MAGLDR's
     bad-block probe is a 2-byte raw read at column 2048.
   - FLASH_STATUS[14:12] = codewords still to come. MAGLDR's flash HAL checks it per codeword
     (expects READY | n<<12). Before this, "FATAL HIT 3".
2. Modem (msm_modem.c, committed): while taken over, poll mailbox 0x280b7a70. On 0x300d300d,
   store [0x280b7a74] to 0x1E0000 and run msm_modem_boot(). XNU's SDCC2 driver needs
   proc_comm 30 (arg 0x17) answered.
3. SD image: the FAT32 BPB hidden-sectors field must equal the partition LBA (2048). The iOS
   kernel's fl32_locate (FAT32 reader for IOS7/ROOTFS.HFS) rejects the card otherwise
   (locator=-3). mksdcard.sh now passes `-h 2048`; sdcard-ios7.img and sdcard.img are patched.
4. Tooling:
   - magnav.py tolerates the bar edge under glyphs.
   - adsd.sh polls for the menu.
   - Kernel analysis lives in build-tmp/ios7k: macho.bin, syms.json (27k symbols) and dis.py,
     a Thumb disassembler that names call and literal targets.

Open:
- Touch, kernel side done. Kernel source: github.com/Garysss123/xnu-hd2, branch
  codex/hd2-source, cloned to build-tmp/xnu-hd2. The touch transport is
  `xnu/iokit/Drivers/KernelBuiltIn/ARM/AppleARMPlatform/leo_touch_transport.c`.
  - The TS model now answers ID 0x55 until the D0 00 01 enable, and reports after. iOS
    polls with the IRQ high.
  - Verified in the kernel: 500+ packets with no errors, correct coordinates, down/move/up
    events enqueued, a userspace client (pid 11) maps the queue and its reader head keeps
    up.
  - Still, UIKit ignores swipes and taps. The lock screen stays and the AssistiveTouch
    button doesn't open. The port's own notes say "not-gesture-proof" and hardware is not
    verified. Next: look at the userspace consumer (pid 11) in rootfs.hfs and the HID
    packet format/timestamps (leo_touch_make_hid_packet, mach_absolute_time).
  - The Android regression run passed after this change (boot, adb, touch).
- Clock: it shows 12:03 Thursday, January 1, so the RTC/time source is not set up for iOS.
- A black screenshot once while the framebuffer held the lock screen. Probably the display
  sleeping on idle; not confirmed.
- The XNU RW-lock panic, sometimes.

## Next steps
1. **Debug FATAL HIT 3.**
   - Run `build-tmp/adsd.sh`, start gdb with `build-tmp/load.gdb` in the background, then press f2.
   - Find whether `open` (0x280219f8) or `elfload` (0x28023044, reader callback 0x280232b0 →
     0x28021b3c) fails.
   - `nand-magldr.img` blocks 38 and 311 do hold the AMSS and DSP ELFs in the controller's raw
     codeword layout, written by `tools/hd2_radio.py --write-nand`. They match the real dump
     logically.
   - Suspects:
     - a NAND read path MAGLDR uses only here (DAL flash driver, ECC, bad-block or partition
       lookup through MIBIB);
     - the OOB/bad-block marker bytes in our raw layout (our block 39 OOB holds payload bytes);
     - the predicate checking VA rather than PA (AMSS VAs include 0xF0000000/0xB0000000).
2. **Decide how to run the restarted modem in HLE mode.** After the mailbox release, a real phone
   runs AMSS from scratch. Options:
   - (a) HLE: when the mailbox gets 0x300d300d, "reboot" the HLE modem: `msm_modem_boot()`
     state, clear `taken_over`, and make SMEM/proc_comm ready again. This is simplest and keeps
     `baseband=hle`.
   - (b) Real: power on the ARM926 (the LLE core) at `[0x280b7a74]`. This needs the LLE path to
     work, and it currently stops at an AMSS FS/NAND fatal (see memory hd2-qemu-baseband-lle).
   - Also check what OSBL normally sets up before AMSS that MAGLDR's restart relies on.
3. Then watch the hand-off to the iOS trampoline:
   - zImage (612 bytes) loads at 0x11800000 and initrd ("UNXF" container with the Mach kernel)
     at 0x12200000.
   - The trampoline checks machtype 2524 and the ATAG initrd, and programs MDP.
   - The XNU kernel has its own SDCC2 driver. It reads `IOS7/ROOTFS.HFS` by its 8.3 name and
     logs to `BOOTLOG.BIN`.
4. Regression-check the Android HLE boot (cLK + NexusHD2 on `images/nand.img`, `tools/start.sh`)
   after the NAND FLASH_STATUS, SDCC/ADM and busctl changes. This has **not been re-tested yet**.
5. Update README.md (MAGLDR/AD SD section, tools table: magnav.py, fat_put.py) and commit.

## Other pending (not requested right now)
- A rebuilt TWRP recovery that continues on read errors is at sha256 25d88dee…. It has not been
  uploaded. The user's real dump stopped at block 504 with EBADMSG. HTC, EFS2, DSP1, FOTA and
  APPSBL read back as 0xFF, probably because of NAND protection.
- LLE baseband: paused at the AMSS FS/NAND fatal. The real dump plus `hd2_radio.py --write-nand`
  could help.

## User constraints
- Never use `/tmp`. Use the session scratchpad or `build-tmp/`.
- No Playwright. Screendumps through QMP are fine for this project.
- Keep waits short and poll for progress.
- On the user's real devices, only install APKs.
- The user prefers accurate hardware emulation over patching guests.
