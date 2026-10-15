# iOS 7 on the emulated HD2

The "HD2 iOS lab" package is an iOS 7 port for the HD2: an XNU 13 kernel built for the QSD8250 (source: [Garysss123/xnu-hd2](https://github.com/Garysss123/xnu-hd2)) and a minimal iOS 7 root filesystem. It boots from the microSD card through MAGLDR 1.13's "Boot AD SD", as on the phone:

1. MAGLDR, in the NAND's OS area, loads `ios7/zImage` and `ios7/initrd.gz` from the card.
2. Before it jumps to them, MAGLDR takes over the modem core and restarts the radio from the NAND.
3. `initrd.gz` holds a small loader and the Mach-O kernel. The kernel mounts `ios7/rootfs.hfs` from the card, read-only.

## Images

You need MAGLDR 1.13 (in the same archive.org bootloader pack as cLK), radio 2.15.50.14 and the iOS package (`7z` is from p7zip):

```bash
mkdir -p firmware/dl && cd firmware/dl
curl -LO https://archive.org/download/htc-hd2-leo-t8585_202304/Bootloader.zip
unzip -o Bootloader.zip Bootloader/MAGLDR113.zip && unzip -o Bootloader/MAGLDR113.zip magldr.nbh
curl -L -o radio.zip 'https://github.com/HTC-Leo-Revival-Project/HTC-HD7-Internals/raw/main/roms/Leo/1.%20Radio_Leo_2.15.50.14.zip'
unzip -o radio.zip RUU_signed.nbh
curl -L -o ios7_v2.rar https://pixeldrain.com/api/file/jnBDwb5c   # https://pixeldrain.com/u/jnBDwb5c
7z x -y ios7_v2.rar                                                # -> ios7/
cd ../..
python3 tools/hd2_mkmagldr.py firmware/dl/magldr.nbh -o images/nand-magldr.img
python3 tools/hd2_radio.py firmware/dl/RUU_signed.nbh -o images/amss.elf --write-nand images/nand-magldr.img
tools/mksdcard.sh images/sdcard-ios7.img 4096
python3 tools/fat_put.py images/sdcard-ios7.img ios7 firmware/dl/ios7/zImage firmware/dl/ios7/initrd.gz \
    firmware/dl/ios7/startup.txt firmware/dl/ios7/VERSION.txt firmware/dl/ios7/BOOTLOG.BIN firmware/dl/ios7/rootfs.hfs
```

`hd2_mkmagldr.py` writes what flashing `magldr.nbh` leaves on a phone: MAGLDR in the OS area and the factory WiFi/BT MAC addresses. MAGLDR restarts the radio from the NAND, so `hd2_radio.py --write-nand` adds the radio partitions.

`fat_put.py` gives each file an 8.3 name and a long name. The kernel looks for `IOS7/ROOTFS.HFS`, and MAGLDR matches the long names. The card's FAT32 hidden-sectors field must be the partition start, and `mksdcard.sh` sets it.

The emulator also needs `images/smem.bin`: the shared memory a real HD2's boot loaders leave for the OS. Without it MAGLDR stops at "FAILED". The repository has one, dumped from a real HD2 with its IMEI zeroed.

## Run

```bash
tools/ios-boot.sh                           # modelled baseband
BASEBAND=lle tools/ios-boot.sh              # the radio's own AMSS (images/amss.elf)
python3 tools/qmon.py images/qmp.sock quit  # stop
```

`ios-boot.sh` starts QEMU and drives MAGLDR's menus with `tools/magnav.py`: Services, Boot Settings, AD SD Dir = `ios7`, then Boot AD SD. MAGLDR forgets the folder at every start, so the script sets it every time. `NAND=`, `SDCARD=` and `IMAGES=` (a directory other than `images/`) pick other images.

MAGLDR's menu is up in about 30 s. The iOS lock screen follows about 4 to 5 minutes after Boot AD SD, with either baseband. The screen is black for the last minute or two, while SpringBoard starts.

With the radio's own AMSS, MAGLDR's takeover runs on the emulated modem core and AMSS cold-starts again before the kernel runs. The kernel's SD driver then turns the card's power on with proc_comm calls to that restarted AMSS.

## What works

- MAGLDR's AD SD boot, the kernel and the 480x800 display.
- The root filesystem on the card, read-only. The kernel writes its log to `ios7/BOOTLOG.BIN`.
- The iOS 7 lock screen, on both basebands.
- Touch reports reach backboardd.

## What doesn't

- Unlocking: the slide to unlock doesn't follow the touch yet.
- The clock always starts at 12:03, Thursday January 1. There is no RTC driver.
- No sound, no telephony, no WiFi. The root filesystem is a minimal one, without mediaserverd or powerd.
- The package's kernel has no driver for the hardware buttons or the battery. The package turns on AssistiveTouch as a software home button instead.
- Now and then the kernel panics with an RW-lock error (`lck_rw_*`).

## Another kernel

`tools/ios7_initrd.py` puts a different XNU build into the package's `initrd.gz`. It keeps the package's loader and device tree, and updates the sizes, CRCs and SHA-256s the loader checks. Rebuild the card with the new file:

```bash
python3 tools/ios7_initrd.py firmware/dl/ios7/initrd.gz mach_kernel initrd.gz
```

`ios7_initrd.py` given the package's own kernel reproduces the original `initrd.gz` byte for byte.
