# Android 4.0.4 on the emulated HD2

Tested with NexusHD2-ICS-CM9-HWA v2.8 (Android 4.0.4, tytung's 2.6.32 kernel) as a NAND ROM, booted by cLK 1.4.0.2. Other NAND ROMs for cLK should work the same way.

cLK 1.5 doesn't boot in the emulator yet: it resets right after the SPL starts it. This applies to the `LEOIMG.nbh` on archive.org and to builds of [zeusk/clk](https://github.com/zeusk/clk). Use a cLK 1.4.x `lk.bin` with `--lk`.

## Images

The ROM is on archive.org:

```bash
mkdir -p firmware/dl
curl -L -o firmware/dl/NexusHD2-ICS-CM9-HWA_V2.8.zip \
    https://archive.org/download/HD2_ADSD_ROM_upload_attempt_003/NexusHD2-ICS-CM9-HWA_V2.8.zip
python3 tools/hd2_mknand.py --lk lk.bin \
    --rom firmware/dl/NexusHD2-ICS-CM9-HWA_V2.8.zip -o images/nand.img
tools/mksdcard.sh images/sdcard.img 1024     # optional microSD (FAT32)
```

The README's [Images](../README.md#images) section has `hd2_mknand.py`'s other options (recovery, partition sizes, MAC addresses, `--gpu`).

## Run

```bash
tools/start.sh                              # start: picks BOOT FROM NAND in cLK
python3 tools/qmon.py images/qmp.sock quit  # stop
```

The first boot of a fresh image takes a few minutes, as on the phone, because Android builds its Dalvik cache. Later boots take about a minute.

Before quitting, remount the partitions read-only so YAFFS writes its checkpoint. Otherwise the next boot scans the whole NAND first (see [quirks.md](../quirks.md)):

```bash
adb -s 127.0.0.1:5555 shell 'sync; for m in /data /cache /system; do mount -o remount,ro $m; done'
```

To run the radio's own firmware instead of the modelled baseband (calls, texts, mobile data, the DSP), see [Real baseband firmware](../README.md#real-baseband-firmware). [What works](../README.md#what-works) lists the emulated hardware.
