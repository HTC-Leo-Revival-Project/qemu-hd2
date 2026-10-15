# Windows Mobile 6.5 on the emulated HD2

The stock Windows Mobile ROM boots on the real baseband (`baseband=lle`, see the [README](../README.md#real-baseband-firmware)). The HTC SPL in the NAND loads the OS straight away, with no cLK.

## Images

HTC's RUU 3.14.405.2 (WWE, radio 2.15.50.14) is on archive.org. Its NBH holds both the OS and the radio, so it is the only download (`7z` is from p7zip):

```bash
mkdir -p firmware/ruu && cd firmware/ruu
curl -LO https://archive.org/download/htc-hd2-stock-ruu/RUU_Leo_S_HTC_WWE_3.14.405.2_Radio_15.42.50.11U_2.15.50.14_LEO_S_Ship.exe
7z e -y RUU_Leo_S_HTC_WWE_3.14.405.2_Radio_15.42.50.11U_2.15.50.14_LEO_S_Ship.exe   # RUU_signed.nbh and HTC's flasher
cd ../..
python3 tools/hd2_radio.py firmware/ruu/RUU_signed.nbh -o images/amss.elf
python3 tools/hd2_wmnand.py --ruu firmware/ruu/RUU_signed.nbh -o images/nand-wm.img
```

`hd2_wmnand.py` writes what flashing the RUU leaves on a phone:
- the radio partitions;
- then the OS area: ULDR, the XIP kernel and IMGFS, each page with its sector info in the spare bytes;
- the user store is left erased for Windows to format.

`--radio NBH` takes the radio from another radio RUU instead.

## Run

```bash
tools/start-wm.sh                           # start
python3 tools/qmon.py images/qmp.sock quit  # stop
```

The web UI recognises a Windows Mobile image by its partition table and boots it on the real baseband.

The first boot runs HTC's setup (SIM, carrier settings, then the Windows setup wizard) and reaches it in a little over a minute. The image is the phone's NAND and keeps what Windows writes, so rebuild it to start over.

## What works

The phone registers on the emulated GSM network. The Sense home screen and the Start menu render on the emulated GPU. WiFi, Bluetooth and touch work.

Internet Explorer can't open most HTTPS sites because its TLS is too old, but plain HTTP works.

## Debugging

`WCELOG=1 tools/start-wm.sh` logs the kernel's debug output to `images/qemu.log` as `wce:` lines. The hook is at its address in this RUU's kernel.
