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

### Real baseband firmware (work in progress)

`baseband=lle` runs the radio ROM's own AMSS on an emulated ARM926 modem core, next to the Scorpion, instead of the modelled baseband services:
```bash
python3 tools/hd2_radio.py RUU_signed.nbh -o images/amss.elf   # e.g. radio 2.15.50.14
tools/start-lle.sh                                             # TRACE=on logs modem I/O
```
- `hd2_radio.py` unwraps the signed NBH, prints the radio's MIBIB partition table and extracts the AMSS ELF.
- The modem core (cluster 1) sees the shared DRAM and peripherals. It also has its own devices:
  - the modem VIC and timers at 0xB8000000;
  - internal SRAM at 0xFFFC0000 and RAM at 0x9C000000;
  - a register store for modem hardware that has no model yet.
- The secure boot stages (DBL/FSBL/OSBL) are skipped. Their hand-off to AMSS is reproduced instead: the SMEM heap, the fixed items, and the boot segment info (item 98).
- AMSS prints fatal errors on UART1 (`images/uart1.log`).
- The apps core is held in reset until AMSS starts it.

Current state: the OKL4 kernel and AMSS's tasks start. Shared memory gets SMSM, the SMEM logs and the version info. The sleep controller and DRAM self-refresh cycle run, and the chip identifies as a QSD8250. The File System task then stops in AMSS's NAND driver (`flash_nand_ctlr_d.c:518`): the radio partitions (MIBIB, EFS2) are not on the emulated NAND yet. The apps core is not released yet, so Android does not boot in this mode.

Helpers: `tools/amss_dis.py` (disassembles by AMSS virtual address, ARM or Thumb), `tools/amss_xref.py` (finds the code that loads a literal), `tools/amss_fatal.py` (finds ERR_FATAL sites). `QMON_CPU=1 tools/qmon.py` runs monitor commands on the modem core. `-global msm-modem-lle.dump-on-write=0xA9A0000C` dumps the modem stack at the first fatal message.

### WiFi

Turn WiFi on in Settings and connect to the open network **HD2-QEMU**. The phone gets 10.0.2.15 by DHCP and real internet through QEMU's user-mode network.

The connection comes from the default `-nic`. To pick your own backend:
```bash
tools/start.sh -nic user,model=bcm4329
```

## Processors

The QSD8250 (Snapdragon S1) has four processors. Here is how each one is emulated:

| Processor | Role on the phone | Emulation |
|---|---|---|
| Scorpion (ARMv7, Qualcomm's own core) | Apps processor: cLK, Linux, Android | **Low level.** The instructions run on a Cortex-A8 model with Qualcomm's MIDR and the Scorpion CP15 registers (PLL, L2). |
| ARM926EJ-S (ARMv5) | Boots first from the boot ROM (PBL, then DBL/FSBL/OSBL from NAND), sets up DRAM and clocks, releases the apps core, then runs AMSS: the radio stack, SMEM, proc_comm, RPC, PMIC and battery services. | **High level by default** (`baseband=hle`). `msm_modem.c` and its helpers implement the services AMSS gives the apps core: SMEM, proc_comm, SMSM/SMD, the RPC router, the AT modem with SIM and data, and GPS. **Low level with `baseband=lle`.** The real AMSS runs instruction by instruction on an ARM926 core (cluster 1). The secure boot stages are skipped and their hand-off is reproduced. In progress, see below. |
| QDSP6 (Hexagon, 4-issue VLIW with vector instructions) | Multimedia DSP: audio decoding and mixing, voice, Android's "aDSP" | **High level.** `msm_qdsp6.c` answers the apps core's DAL audio calls and plays PCM to the QEMU audio backend. No Hexagon instructions run, so the DSP1 firmware image is never executed. |
| QDSP4 (modem DSP) | Baseband signal processing for GSM/WCDMA, driven by AMSS | **Not emulated.** There is no radio. In HLE mode, registration, calls and data are modelled above it. In LLE mode, AMSS's accesses to the modem DSP's registers fall into an unmodelled register store. |

## Hardware status

| Block | Model |
|---|---|
| CPU | Scorpion as Cortex-A8 (Qualcomm MIDR, Scorpion CP15 registers) |
| Interrupts | VIC, SIRC, GPIO interrupts (edge and level) |
| Timers / clocks | CSR GPT/DGT timers, clock control, PLLs, Scorpion PLL |
| NAND | Controller (16-bit ECC layout) + ADM data mover; yaffs2 read/write |
| microSD | SDCC (SDC2) on QEMU's SD card model; mounted by vold |
| WiFi | BCM4329 on SDC1: SDIO card, backplane, firmware download. The firmware is emulated at protocol level (SDPCM/CDC/BDC, scan, join, events) and bridged to a QEMU NIC. |
| Modem | SMEM, proc_comm, DEX, SMSM, SMD, RPC router (RTC, PMIC, battery…); or the real AMSS on an ARM926 core (`baseband=lle`, in progress) |
| Cellular | AT modem on SMD with a T-Mobile US (310/260) SIM, UMTS registration, PPP data call to QEMU user networking |
| GPS | gpsOne PDAPI over ONC RPC: position callbacks to the HAL's client endpoint, 1 Hz fixes, runtime `gps-fix` |
| Bluetooth | UART1DM (msm_serial_hs) with paced ADM DMA, host-wake LPM; BCM4329 HCI controller (patchram, BT 2.1 + EDR) |
| FM radio | BCM4329 FM core via HCI `0xFC15` (tune, seek, RSSI, flags/interrupts, volume); audio through the codec aux loopback (DEX `0x51`) |
| Audio | QDSP6 DAL playback → QEMU audio backend (verified bit-exact) |
| Display | MDP 3.1 DMA_P scanout + PPP blitter; backlight via microP |
| GPU | ICS software fallback (PixelFlinger); Adreno is not emulated |
| I2C | Touchscreen, microP (backlight, G-sensor, light sensor), battery gauge, PMIC, compass |
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
