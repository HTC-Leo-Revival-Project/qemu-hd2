# Tools

Every script and helper in `tools/`, what it is for, and how it is called.
The [README](../README.md#tools) has the short version; this is the reference.

Unless a tool says otherwise, run it from the repository root and it finds the
build and the images by itself (`src/qemu/build/qemu-system-arm`, `images/`).

The launchers all start QEMU on the same QMP unix socket, `images/qmp.sock`, so
one set of tools talks to whichever launcher is running:

    python3 tools/qmon.py images/qmp.sock "info registers"
    python3 tools/keys.py images/qmp.sock f2
    python3 tools/touch.py images/qmp.sock 240 400

## Building

| Tool | What it does |
|---|---|
| `build-linux.sh` | Configure and build the host QEMU — `arm-softmmu`, `hexagon-softmmu` and the `amsslog` plugin, with VNC. `-j N`, `--clean`. See [building.md](building.md). |

## Images — building a NAND or SD card

| Tool | What it does |
|---|---|
| `hd2_mknand.py` | Build a raw 512 MiB NAND image with cLK and a **NAND Android ROM**: the bootloader in the OS area, cLK's partition table, `boot.img` built from the ROM's `kernel/`, and yaffs2 `/system` + `/data`. |
| `hd2_wmnand.py` | Build a NAND image from a stock **Windows Mobile** `RUU_signed.nbh`: the radio partitions and the OS area (ULDR, XIP, IMGFS) with WinCE `SectorInfo`. |
| `hd2_mkmagldr.py` | Build a NAND image with **MAGLDR 1.13** in the OS area and the factory MACs. Add the radio with `hd2_radio.py --write-nand`. |
| `hd2_radio.py` | Extract the modem firmware from a radio RUU: the AMSS ELF (`-o amss.elf`), the DSP1 image (`--dsp dsp1.elf`), and/or the radio NAND partitions (`--write-nand nand.img`). Prints the MIBIB table. |
| `hd2_setmac.py` | Rewrite the factory WiFi/Bluetooth MAC addresses in an existing NAND image (block 505) and nothing else. |
| `nand_put.py` | Write an image into a partition of an existing NAND image, erased first, page by page in the controller's raw layout — like `flash_image`. `nand_put.py nand.img 0x221 40 recovery.img`. |
| `mksdcard.sh` | Create a microSD image with one FAT32 partition, as Android's `vold` expects. `mksdcard.sh sdcard.img [MiB]`. |
| `fat_put.py` | Copy files into a FAT32 image made by `mksdcard.sh`, without mounting it. Writes both an 8.3 name and a long-name entry, which is what HD2 kernels and MAGLDR look up. |
| `yaffs_check.py` | Read files back out of a raw NAND image (ECC-mode view) and compare them with the ROM zip. Used to prove a built image matches its source. |
| `mknand.py` | **Legacy duplicate** of `hd2_mknand.py` without the `--kernel`/`--cmdline` options. Unused; use `hd2_mknand.py`. |

`hd2_mknand.py` options include `--nbh`/`--lk`, `--rom`, `--recovery`,
`--system-mb`, `--cache-mb`, `--wifi-mac`, `--bt-mac`, `--gpu`, `--kernel`,
`--cmdline`. The [README](../README.md#images) and
[docs/android.md](android.md) walk through the common cases.

## Launching the emulator

All of these are thin wrappers over `tools/start.sh`; each one sets the machine
properties and images for its mode, then passes any extra arguments to QEMU.

| Tool | What it starts |
|---|---|
| `start.sh` | The **base machine**. HLE baseband by default; boots the image in NAND, presses cLK's keys, and leaves QEMU in the background. |
| `start-lle.sh` | `start.sh` with the **real baseband**: `BASEBAND=lle`, `nand-lle.img`, `images/amss.elf`. |
| `start-q6.sh` | `start-lle.sh` **plus the real DSP**: a second `qemu-system-hexagon -M htcleo-q6` process sharing `dram.bin` over a socket. |
| `start-wm.sh` | Stock **Windows Mobile**: `nand-wm.img` on the real baseband, with no cLK menu key. |
| `start-usb-lle.sh` | Any of the above with the USB gadget handed to **this machine's own USB stack** (`usb-host=lle`), through `vhci_hcd`. Needs **root**; re-executes itself through `sudo`. |
| `ios-boot.sh` | The **iOS 7 SD package**: MAGLDR from `nand-magldr.img`, the card `sdcard-ios7.img`, drives MAGLDR's menus and boots AD SD. |

Environment variables understood by the launchers (see `start.sh` for the full
list): `NAND=`, `SDCARD=`, `BASEBAND=lle`, `AMSS=`, `TRACE=on`, `AMSSLOG=1`,
`AUDIODEV=`, `ADB_PORT=`, `CLK_KEYS=`, `USB=lle`, `QEMU_D=`.

## Driving a running emulator (QMP)

| Tool | What it does |
|---|---|
| `qmon.py` | Run HMP monitor commands through the QMP socket: `qmon.py SOCK "info registers"`. `QMON_CPU=n` selects a CPU (1 = the modem core under LLE). |
| `keys.py` | Key presses: `keys.py SOCK f2`, `key:80` to hold, `sleep:500`. |
| `touch.py` | Touchscreen taps and swipes: `touch.py SOCK x y [holdms] ...`; several points are a drag. |
| `capture.py` | Take periodic screendumps over QMP: `capture.py SOCK PREFIX INTERVAL COUNT`. |
| `waitscreen.py` | Poll screendumps until the panel shows something: `waitscreen.py SOCK MAX_S`. |
| `pcsample.py` | Sample the guest's PC over QMP and histogram it by kernel symbol. Needs a symbol file — see the note below. |

`pcsample.py` has a hardcoded symbol path from the author's machine
(`/home/davi/clones/...`). Point it at your own `build-tmp/syms.txt` (three
columns: address, type, name) before using it.

## Booting unattended, and reading the logs

| Tool | What it does |
|---|---|
| `boot.sh` | Boot headless, press F2, and poll `ramcon.log` until a regex, a panic, or a timeout. `boot.sh MAX [UNTIL]`. |
| `boot-q6.sh` | Boot LLE + DSP and wait for Android, failing fast. On a stall it prints where the apps core spins, the pending `proc_comm` command, the modem VIC and every DSP thread. `BOOT_MAX=`, `BOOT_STALL=`. |
| `run.sh` | Boot for N seconds, capture a screenshot and the logs, quit. `run.sh SECONDS`. |
| `waitlog.sh` | Poll `images/ramcon.log` for a regex: `waitlog.sh MAX REGEX`. |
| `logcat.sh` | Dump guest RAM over QMP and recover `logcat.txt` from it: `logcat.sh [GREP]`. |
| `logcat_dump.py` | The decoder behind `logcat.sh`: recover log entries from a raw guest RAM dump. |
| `test-usb-lle.sh` | USB-LLE regression: start the emulator, one `getvar` and one `flash`, reset, repeat. `RUNS=`, `IMG=`, `PRISTINE=`, `BLOB=`. |

The two guest logs are `images/ramcon.log` (kernel console — boot progress) and
`images/uart1.log` (the other serial); QEMU's own log is `images/qemu.log`.

## MAGLDR menus and the iOS package

| Tool | What it does |
|---|---|
| `magnav.py` | Move MAGLDR's cursor to an item by reading the screen back: `magnav.py SOCK ITEM [select]`. Works for the menus and the AD SD Dir picker. |
| `magkey.sh` | Press keys one at a time with a screenshot after each — the manual fallback for the menus. |
| `ios7_initrd.py` | Put a different XNU kernel into the iOS 7 package's `initrd.gz`, keeping the loader and the device tree and updating the sizes, CRCs and SHA-256s the loader checks. |

## Radio firmware and reverse engineering (AMSS)

| Tool | What it does |
|---|---|
| `amss_dis.py` | Disassemble AMSS around a virtual address. `amss_dis.py amss.elf VA [count [before]]`; an odd VA or `VA+t` is Thumb. |
| `amss_fatal.py` | Find the `ERR_FATAL` call sites for a `FILE:LINE`. `amss_fatal.py amss.elf sleepctl.c 2490`. |
| `amss_xref.py` | Find code that loads a literal-pool word: `amss_xref.py amss.elf LIT_VA`. |
| `amss_ovl.py` | Map AMSS Thumb functions to the mDSP overlay variables and messages they use; `--grep TEXT`, `--calls VA`. |
| `smem_dump.py` | Dump the SMEM heap table of contents from a raw SMEM image (`images/smem.bin`). |

## The web UI

| Tool | What it does |
|---|---|
| `deploy-web.sh` | Install the web UI on a machine: copy what it needs, build it, and (with `sudo`) add a systemd service and, with `--domain NAME`, an nginx site with TLS. |

`web/README.md` has the UI itself; [building.md](building.md) covers building
the emulator, the app and the UI.

## Working on a phone (headless, over adb)

These run the emulator **on a rooted phone**, without the app and without a
screen. `android/HEADLESS.md` is the guide; these are the tools.

| Tool | What it does |
|---|---|
| `phone-deploy.sh` | Build QEMU for Android, strip it, push it to the phone's `files/emu/bin/qemu-arm`, and compare md5s so a headless run cannot test a stale build. |
| `phone-boot.sh` | Headless boot on the phone: `phone-boot.sh [soft\|gles] [trace]`. Deploys first, then starts QEMU under `run-as`. |
| `phone_qmp.py` | QMP client reached over `adb forward`: `phone_qmp.py "info registers"`, `screendump out.ppm`. |
| `phone-keys.py` | Key presses over the forwarded QMP: `phone-keys.py f2`, `--boot` to hold F2 through the bootloader. |
| `phone-shot.py` | Screenshot: `--qmp` (exact guest framebuffer), `--app` (the app's button), `--phone` (the phone's own screen). |
| `phone-frame-rate.sh` | Measure the guest's redraw rate from the Adreno stats lines. |
| `glesrun.sh` | The QEMU command line the headless runner uses, pushed to the phone and run under `run-as`. |

## Working on a phone (root shell over TCP)

| Tool | What it does |
|---|---|
| `androotsh/androotsh.c` | A tiny persistent **root shell server** over TCP, so work survives adb dropping (which it does whenever the USB state changes). Build with the NDK (dynamic, not `-static`). |
| `ph` | Run a shell command as root through `androotsh`: `ph 'id'`, or a script on stdin. `PH_ADDR=`/`PH_PORT=` override the phone. |

## USB passthrough

| Tool | What it does |
|---|---|
| `tools/androotsh/` | Root shell (above), used while the phone's USB is taken over. |
| `test-usb-lle.sh` | The host-side USB-LLE regression (above). |

The gadget → FunctionFS bridge is `android/native/usbffs.c`, built into the APK.
`docs/usb-passthrough.md` is the long write-up.

## GPU tests

| Tool | What it does |
|---|---|
| `gltri.c` | Draw a colour-interpolated triangle with GL ES 2.0 through the ROM's Adreno driver. |
| `gltest.c` | GL ES 2.0 feature checks: `mip`, `comp` (ETC1/ATC/DXT1), `pts`, `cube`. |

Both build with the NDK against an API 16 sysroot and take the framebuffer, so
stop SurfaceFlinger first.

## Recovery and benchmarks (subdirectories)

| Path | What it is |
|---|---|
| `recovery/mkbootimg.py` | Minimal Android boot image (v0) writer, as AOSP's `mkbootimg` does it. |
| `recovery/hd2nanddump.c` | Read a NAND MTD device page by page and write the data and spare bytes plus a report — the phone-side NAND dumper. |
| `recovery/hd2-nand-dump.sh` | The recovery-side wrapper: back the radio area up to the SD card, read-only. |
| `hd2bench/` | Benchmark app (Java + NDK) to compare a real HD2 with the emulator. Build with `hd2bench/build.sh`. |
| `vdc/` | Reverse-engineering helpers for the QSD8250's "Janus" video decoder: `vdcsim.py` (reference model), `cmdparse.py`, `dumpread.py`, `jmmb.py`, `venc_psnr.py`. |

## Conventions

* **Launchers leave QEMU running in the background.** Stop one with
  `python3 tools/qmon.py images/qmp.sock quit`.
* **Everything logs into `images/`**: `ramcon.log`, `uart1.log`, `qemu.log`,
  `qemu.out`, `logcat.txt`, `scr.png`.
* **A fresh image's first boot is slow** (Dalvik), and in LLE mode every NAND
  command is a `proc_comm` round trip — see [quirks.md](../quirks.md).
* **Remount read-only before quitting** (or use `boot-q6.sh`, which does) so
  YAFFS writes its checkpoint; otherwise the next boot scans the whole NAND.
