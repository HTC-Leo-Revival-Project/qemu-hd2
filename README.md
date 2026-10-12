# HTC HD2 (htcleo) on QEMU

A QEMU machine (`-M htcleo`) that boots the phone's real software stack:
cLK bootloader → Linux 2.6.32 (tytung r3.6) → Android 4.0 (NexusHD2 ICS CM9),
all from an emulated NAND, with no changes to the kernel or ROM.

## Build

```bash
cd src/qemu/build && ninja qemu-system-arm
```

The machine lives in `src/qemu/hw/arm/htcleo/`.

## Create the images

```bash
cd images
python3 ../tools/hd2_mknand.py --lk ../firmware/lk-user.bin --rom ../firmware/dl/NexusHD2-ICS-CM9-HWA_V2.8.zip -o nand.img
../tools/mksdcard.sh sdcard.img 1024             # optional microSD (FAT32)
```

`hd2_mknand.py` lays out the NAND the way cLK expects:
- the ROM area at block 0x212 and the partition table;
- boot.img;
- yaffs2 /system and /data, installed the same way the ROM's updater-script would;
- the factory WiFi/BT MAC addresses in block 505.

## Run

```bash
tools/start.sh            # headless; picks "BOOT FROM NAND" in cLK
```

- `start.sh` attaches `images/sdcard.img` when it exists.
- It opens the adb bridge on 127.0.0.1:5555 (`ADB_PORT` changes it).
- Audio goes nowhere by default. Set `AUDIODEV` to hear it or record it:
  ```bash
  AUDIODEV="-audiodev pa,id=snd0" tools/start.sh
  AUDIODEV="-audiodev wav,id=snd0,path=out.wav" tools/start.sh
  ```
- To get a window, drop `-display none` from the script or pass `-display gtk`.
- Stop QEMU with `python3 tools/qmon.py images/qmp.sock quit`.

Boot to the Android UI takes about 30 s. The first boot runs dexopt and takes longer.

```bash
adb connect 127.0.0.1:5555 && adb shell    # root shell
```

### Controls

| Key | HD2 button |
|---|---|
| PgUp / PgDn | Volume up / down |
| Home | Home |
| Esc | Back |
| F1 | Windows (Menu) |
| F2 / F3 | Call / End |
| F12 or End | Power |

The mouse acts as the touchscreen.

Headless helpers:
- `tools/keys.py SOCK key[:ms]...` sends key presses (QEMU qcode names).
- `tools/touch.py SOCK x,y | swipe:x0,y0,x1,y1` sends taps and swipes in screen pixels.
- `tools/logcat.sh` dumps logcat from guest RAM, for use when adb isn't up yet.

### Battery and USB cable

The DS2746 gauge models a 1230 mAh battery. It charges while a cable is plugged in and Linux enables the charger (GPIO 22, fast rate on GPIO 16), and it drains otherwise. The machine starts with the adb cable plugged in. To unplug it, plug in a wall charger or set the charge level, like the AOSP emulator's `power` commands:
```bash
python3 tools/qmon.py images/qmp.sock "qom-set /machine cable none"   # usb | ac | none
python3 tools/qmon.py images/qmp.sock "qom-set /machine battery-capacity 15"
```
With `baseband=lle` the cable state goes to the PM7540, and the real AMSS updates HTC's VBUS word and notifies Linux. With the modelled baseband, the modem model does the same.

### GPS

Location requests get a 1 Hz fix with eight satellites in view. The default position is 37.4220 N, 122.0841 W at 10 m; `-global msm-modem.gps-latitude=…` (and `gps-longitude`, `gps-altitude`) set a different one at startup. To move the phone while it runs (like the AOSP emulator's `geo fix`):
```bash
python3 tools/qmon.py images/qmp.sock "qom-set /machine/modem gps-fix -23.5505,-46.6333,760"
```

### Bluetooth and FM radio

Turn Bluetooth on in Settings. The ROM's `brcm_patchram_plus` loads `bcm4329.hcd` into the emulated chip, then bluetoothd brings the adapter up with the factory address from NAND. Scans complete without finding devices, and connections fail with a page timeout, since there is no radio.

The FM receiver sits behind Broadcom's HCI vendor command `0xFC15` (register access), with Bluetooth on. For example, as root:
```bash
hcitool cmd 0x3f 0x15 0x00 0x00 0x01            # FM on
hcitool cmd 0x3f 0x15 0x0a 0x00 0x7c 0x92       # FREQ = 101500 - 64000 kHz
hcitool cmd 0x3f 0x15 0x09 0x00 0x01            # tune (0x02 = seek, see 0x07)
hcitool cmd 0x3f 0x15 0x12 0x01                 # read FLAG0: 0x01 done, 0x20 stereo
```
`AUDIO_START_FM` on `/dev/msm_audio_ctl` turns on the codec's aux loopback, and the tuned station plays through the audio backend. Stations are set with `-global bcm4329-bt.fm-stations="MHZ=SOURCE;…"`, where SOURCE is `tone:HZ` or any file or stream URL that ffmpeg can decode. Without it, test tones play on 88.1, 94.7, 101.5 and 105.9 MHz, with static between stations. The ROM itself has no FM app.

### Real baseband firmware

`baseband=lle` runs the radio ROM's own AMSS on an emulated ARM926 modem core, next to the Scorpion, instead of the modelled baseband services. cLK, Linux and Android boot on top of it to the lock screen, and adb works:
```bash
cd images
python3 ../tools/hd2_radio.py RUU_signed.nbh -o amss.elf          # e.g. radio 2.15.50.14
cp nand.img nand-lle.img
python3 ../tools/hd2_radio.py RUU_signed.nbh --write-nand nand-lle.img   # MIBIB, AMSS, DSP1 ...
cd .. && tools/start-lle.sh          # start.sh with BASEBAND=lle and nand-lle.img
```
- `hd2_radio.py` unwraps the signed NBH, prints the radio's MIBIB partition table and extracts the AMSS ELF. `--write-nand` puts the radio partitions into a NAND image. AMSS then finds its MIBIB at block 2 and formats EFS2 on first boot.
- The secure boot stages (DBL/FSBL/OSBL) are skipped. Their hand-off is reproduced instead:
  - the SMEM heap, the fixed items and the boot segment info (item 98);
  - HTC's configuration block at SMEM + 0xEF000 (board ID, radio version, CID, NAND ID, USB cable present);
  - the apps core and the QDSP6, which OSBL starts before it jumps to AMSS, run from reset.
- The modem core (cluster 1) sees the shared DRAM and peripherals and has its own devices:

| Address | Device |
|---|---|
| 0xB8000000 | timers, watchdog; writing 1 to 0x594 + 4n interrupts the apps core (INT_A9_M2A_n) |
| 0xB8001000 | modem VIC: the L4 tick on line 32, the ADM on 52, the watchdog bark (FIQ) on 55, apps doorbells A2M 0-6 on 56-62 |
| 0xB8000300, 0xB8000400, 0xA8100000, 0xB0007580 | SSBI controllers; the PM7540 PMIC behind the first (`msm_ssbi.c`) |
| 0xA8E00000, 0xA8F00400 | the modem's own shadow of the GPIO registers; its group interrupts are lines 44 and 45 |
| 0xB1000000 | modem DSP (QDSP4) memories and control, high level: image download, the boot handshake, master controller commands, the GSM/WCDMA ready flags, WCDMA synchronous and search commands, the receive AGC (`msm_mdsp.c`) |
| 0xB0007B00 | GSM frame timer: quarter-symbol counter, frame number, the frame tick every 120/26 ms on line 18 (`msm_gstmr.c`) |
| 0xB0005D00 | WCDMA system timer: chip counter, 10 ms frames, three event interrupts on lines 12-14 (`msm_wstmr.c`) |
| 0xA9B00000 | UIM: UART2 in SIM mode with a GSM SIM card behind it, interrupt through the second-level controller 7 on line 37 (`msm_uim.c`, `sim_card.c`) |
| 0xA9400000 | ADM security domain 0, shared with the apps data mover |
| 0xFFFC0000, 0x9C000000 | internal SRAM, modem RAM |

- The PM7540 PMIC is modelled with:
  - its RTC (the clock comes from QEMU's);
  - its interrupt controller, wired to MSM GPIO 24 and the modem's GPIO interrupt shadow;
  - the power key, USB power and charger sources.
- The SIM is the same T-Mobile US card as in the default mode (IMSI 310260000000000), here a real ISO 7816 card: AMSS's UIM driver powers it through the PMIC, gets its ATR, falls back from USIM to GSM commands and reads it over T=0. AMSS reports it present and ready, and Android's RIL reads it with AT+CRSM.
- The modem DSP runs no code. The model answers what AMSS's GSM and WCDMA layers wait for: firmware ready, master controller and synchronous commands, and the searcher. With no RF signal the receiver sits at its noise floor (−106 dBm) and searches find no peaks.
- So the radio stack behaves like a phone with no network around: WCDMA frequency scans and acquisition, then GSM power scans, then NO SERVICE and power save, retried periodically. Android shows the SIM READY and "out of service".
- GPS requests reach AMSS's gpsOne engine. The receiver turns on, finds no satellites and times out every 2 s, like a phone indoors.
- Out of reach in this mode: registering on a network and getting a GPS fix. Both need the DSP's signal processing and the RF front end, and the DSP firmware cannot be run. Use the default mode for those.
- Known gaps: GSM power measurements read −70 dBm rather than the noise floor (the DSP's per-frame GSM interface is not modelled), so GSM tries to acquire a few channels before it gives up.
- AMSS prints fatal errors on UART1 (`images/uart1.log`).

Debugging:
- `AMSSLOG=1 tools/start-lle.sh` writes AMSS's own debug log ("F3" messages) to `images/qemu.log`. It uses the TCG plugin `contrib/plugins/amsslog.c`, which hooks the msg_send functions. `AMSSLOG="1 hook=0xVA"` also reports each call of a function.
- `TRACE=on` logs the modem core's peripheral accesses.
- `tools/amss_dis.py` disassembles by AMSS virtual address (ARM or Thumb). `tools/amss_xref.py` finds the code that loads a literal, and `tools/amss_fatal.py` finds ERR_FATAL sites.
- `QMON_CPU=1 tools/qmon.py` runs monitor commands on the modem core. gdb (multiprocess stub, inferior 2) works, but it slows AMSS enough to trip its watchdog.

### WiFi

Turn WiFi on in Settings and connect to the open network **HD2-QEMU**. The phone gets 10.0.2.15 by DHCP and real internet through QEMU's user-mode network.

The connection comes from the default `-nic`. To pick your own backend:
```bash
tools/start.sh -nic user,model=bcm4329
```

### GPU (Adreno 200)

The Adreno 200 is emulated (QEMU branch `adreno`, `src/qemu-gpu`), so Android can render with the ROM's own Qualcomm libraries instead of the PixelFlinger software renderer. Build a NAND image without the emulator's GPU properties:
```bash
python3 ../tools/hd2_mknand.py --lk ../firmware/lk-user.bin --rom ../firmware/dl/NexusHD2-ICS-CM9-HWA_V2.8.zip --gpu -o nand-gpu.img
```
Then SurfaceFlinger, the boot animation and apps (hwui) all render on the emulated GPU. For an image built without `--gpu`, the GPU is idle and nothing changes.

What is modelled:
- the KGSL-facing hardware: interrupts, the GPU MMU, and the command processor running the ring buffer and indirect buffers (PM4 packets);
- the a2xx shader core: an interpreter for the driver's compiled shader microcode, run on 16 lanes (batches of vertices, or 4x4 pixel blocks);
- the 3D pipe: vertex fetch, triangles, strips, fans, rectangles, points and lines, clipping, scissors, perspective-correct interpolation, depth/stencil, alpha test and blending, rendered in bins into GMEM and resolved to memory;
- textures: 8/16/32-bit and float formats, ETC1, ATC, DXT and 3DC, mipmaps with the Xenos layout (packed mip tails), bilinear/trilinear filtering, and tiled surfaces.

It runs on the host CPU, with 6 rasterizer threads by default (`-global msm-adreno.threads=N`). Measured with the test programs below:

| Workload | Time per frame |
|---|---|
| Full-screen clear + one triangle | ~15 ms |
| Spinning depth-tested cube | ~16 ms |
| 8 full-screen blended, trilinear-textured layers | ~150 ms |

Not modelled:
- hardware video decode/encode (the QDSP6 video DAL), so video uses Android's software codecs;
- occlusion queries and the Z180 2D core.

Test programs (NDK r23, non-PIE; see the headers for build lines):
- `tools/gltri.c` draws an RGB triangle with EGL/GLES2 on the framebuffer, reads pixels back and times the frames;
- `tools/gltest.c` checks mipmaps, compressed textures, points and lines, depth, and a fill-rate scene.

Run them with SurfaceFlinger stopped (`adb shell stop`).

Debugging:
- `qom-set /machine/gpu trace FILE` logs every packet and draw, with screen-space vertices (`""` stops it);
- `qom-set /machine/gpu dump GPUADDR,SIZE,FILE` saves GPU memory (`0xffffffff` saves GMEM).

## Processors

The QSD8250 (Snapdragon S1) has four processors. Here is how each one is emulated:

| Processor | Role on the phone | Emulation |
|---|---|---|
| Scorpion (ARMv7, Qualcomm's own core) | Apps processor: cLK, Linux, Android | **Low level.** The instructions run on a Cortex-A8 model with Qualcomm's MIDR and the Scorpion CP15 registers (PLL, L2). |
| ARM926EJ-S (ARMv5) | Boots first from the boot ROM (PBL, then DBL/FSBL/OSBL from NAND), sets up DRAM and clocks, releases the apps core, then runs AMSS: the radio stack, SMEM, proc_comm, RPC, PMIC and battery services. | **High level by default** (`baseband=hle`). `msm_modem.c` and its helpers implement the services AMSS gives the apps core: SMEM, proc_comm, SMSM/SMD, the RPC router, the AT modem with SIM and data, and GPS. **Low level with `baseband=lle`.** The real AMSS runs instruction by instruction on an ARM926 core (cluster 1) and Android boots on it. The secure boot stages are skipped and their hand-off is reproduced. The SIM works; the radio stack runs but finds no network, see below. |
| QDSP6 (Hexagon, 4-issue VLIW with vector instructions) | Multimedia DSP: audio decoding and mixing, voice, Android's "aDSP" | **High level.** `msm_qdsp6.c` answers the apps core's DAL audio calls and plays PCM to the QEMU audio backend. No Hexagon instructions run, so the DSP1 firmware image is never executed. With `baseband=lle` it acts as its own processor: it creates its SMD channels in AMSS's shared memory like the DSP firmware would. |
| QDSP4 (modem DSP) | Baseband signal processing for GSM/WCDMA, driven by AMSS | **Not emulated** in HLE mode, where registration, calls and data are modelled above it. **High level in LLE mode** (`msm_mdsp.c`): AMSS downloads the mDSP image and its overlays into modelled memories; the model answers the boot handshake, master controller commands, WCDMA synchronous and search commands, and reports a receiver with no signal. No DSP instructions run. |

## Hardware status

| Block | Model |
|---|---|
| CPU | Scorpion as Cortex-A8 (Qualcomm MIDR, Scorpion CP15 registers) |
| Interrupts | VIC, SIRC, GPIO interrupts (edge and level) |
| Timers / clocks | CSR GPT/DGT timers, clock control, PLLs, Scorpion PLL |
| NAND | Controller (16-bit ECC layout) + ADM data mover; yaffs2 read/write |
| microSD | SDCC (SDC2) on QEMU's SD card model; mounted by vold |
| WiFi | BCM4329 on SDC1: SDIO card, backplane, firmware download. The firmware is emulated at protocol level (SDPCM/CDC/BDC, scan, join, events) and bridged to a QEMU NIC. |
| Modem | SMEM, proc_comm, DEX, SMSM, SMD, RPC router (RTC, PMIC, battery…); or the real AMSS on an ARM926 core (`baseband=lle`, boots Android with the SIM; no network) |
| Cellular | AT modem on SMD with a T-Mobile US (310/260) SIM, UMTS registration, PPP data call to QEMU user networking |
| GPS | gpsOne PDAPI over ONC RPC: position callbacks to the HAL's client endpoint, 1 Hz fixes, runtime `gps-fix` |
| Bluetooth | UART1DM (msm_serial_hs) with paced ADM DMA, host-wake LPM; BCM4329 HCI controller (patchram, BT 2.1 + EDR) |
| FM radio | BCM4329 FM core via HCI `0xFC15` (tune, seek, RSSI, flags/interrupts, volume); audio through the codec aux loopback (DEX `0x51`) |
| Audio | QDSP6 DAL playback → QEMU audio backend (verified bit-exact) |
| Display | MDP 3.1 DMA_P scanout + PPP blitter; backlight via microP |
| GPU | Adreno 200: command processor, MMU, a2xx shader interpreter, threaded rasterizer, GMEM bins and resolves, textures incl. ETC1/ATC (branch `adreno`; images built with `--gpu`). Otherwise the ICS software fallback (PixelFlinger). |
| I2C | Touchscreen, microP (backlight, G-sensor, light sensor), battery gauge (charging and draining), PMIC, compass |
| Buttons | GPIO key matrix + power key |
| USB | HSUSB (ChipIdea) device controller with a built-in host. It enumerates the gadget and bridges adb to TCP. |

Not emulated:
- camera;
- Bluetooth radio links (pairing and connections);
- audio capture.

## Tools

| Tool | Purpose |
|---|---|
| `hd2_mknand.py` | NAND image builder |
| `hd2_setmac.py` | Rewrite the factory MACs in an existing image |
| `mksdcard.sh` | FAT32 SD card image |
| `start.sh` / `boot.sh` / `run.sh` | Launch variants |
| `qmon.py` | HMP commands over QMP |
| `capture.py` / `waitscreen.py` | Screendumps |
| `waitlog.sh` | Poll the kernel log |
| `yaffs_check.py` | Read files back from a NAND image offline |
| `logcat_dump.py` / `logcat.sh` | Recover logcat from a RAM dump |
| `pcsample.py` | PC histogram by kernel symbol |
| `hd2_radio.py` | Radio NBH unpacker: partition table and AMSS ELF |
| `start-lle.sh` | Launch with the real baseband firmware |
| `amss_dis.py` / `amss_xref.py` / `amss_fatal.py` | AMSS reverse-engineering helpers |
| `gltri.c` / `gltest.c` | GLES2 test programs for the emulated GPU |

## Credits

The Adreno 200 model is written from scratch for QEMU, but it stands on the work of these projects:
- **[freedreno](https://docs.mesa3d.org/drivers/freedreno.html)** (Mesa; Rob Clark and contributors): the a2xx register and PM4 packet definitions (`registers/adreno/a2xx.xml`, `adreno_pm4.xml`), the a2xx shader instruction formats and disassembler (`ir2/instr-a2xx.h`, `disasm-a2xx.c`), and the a2xx driver (`gallium/drivers/freedreno/a2xx`). The driver shows how binning, resolves, fast clears and surface layouts work.
- **[Xenia](https://xenia.jp)** (the Xbox 360 emulator, whose Xenos GPU shares the Adreno 200's design): the shader microcode definitions and interpreter semantics (`src/xenia/gpu/ucode.h`, `shader_interpreter.cc`), the command processor's packet handling, the tiled texture addressing (`texture_address.h`) and the packed mip tail layout (`texture_util.cc`).
- **[texture2ddecoder](https://github.com/UniversalGameExtraction/texture2ddecoder)**: the reference for decoding ATC (Adreno texture compression) blocks.
- The kernel's KGSL driver (`drivers/gpu/msm` in the htcleo kernel) defines the register interface the model answers.
