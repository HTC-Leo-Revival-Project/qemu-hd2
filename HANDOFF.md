# HD2 QEMU — session handoff (2026-10-12)

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
- Touch: a swipe on "slide to unlock" does nothing. The kernel has
  `ios7leo_register_hid_touch`; check what touch controller and protocol it expects.
- Clock: it shows 12:03 Thursday, January 1, so the RTC/time source is not set up for iOS.
- A black screenshot once while the framebuffer held the lock screen. Probably the display
  sleeping on idle; not confirmed.
- The XNU RW-lock panic, sometimes.
- Android regression test of the NAND/SDCC/ADM changes (still pending, see below).

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
