# SUMMARY — unattended session 2026-11-06

Order worked: SPL → USB LLE → sensors → BYON → SD editor → ADB. Details and evidence in RUNNING.md.

## Done and verified
- **SPL LLE mode** (Settings): real FSBL → OSBL → HSPL → cLK → Android (CM9) and → WinMo, in the app on the S25 and on the host.
  Fixed the kernel BUG in the touch driver (HSPL-only I2C chips 0x34/0x5C now vanish at hand-off). `tools/hd2_radio.py --sbl` and the
  app extract FSBL/OSBL from a stock RUU or a NAND with a radio.
- **SPL USB mode**: no OS + no LEOIMG.nbh on the card → Volume Down is held, the HSPL shows its bootloader screen and a USB gadget.
- **USB LLE on the same path**: the S25's own USB port shows the emulated HD2 to the PC: cLK fastboot (`fastboot getvar`,
  `tools/hd2_usb.py fastboot …` pure pyusb), and after `fastboot continue` Android's adb (`adb shell` = root on NexusHD2 4.0.4,
  push/cat OK). Bridge fixes in `android/native/usbffs.c` (reconnect on guest USB resets, follow gadget switches, FFS workers survive
  ESHUTDOWN, per-speed bulk packet size — the cause of adb "offline").
- **Sensors (HLE)**: QEMU props `accel`/`mag`/`light` on /machine, BMA150/AKM8973/microP; `HostSensors` feeds the S25's accelerometer,
  magnetometer and light sensor live. Android sees them (dumpsys sensorservice). WinMo uses the same chips; not checked in a WinMo app.
- **BYON**: bundled stock SPL + HSPL, first-run stock-RUU picker (`ByonActivity`), minimal SPL-only NAND, Settings "Replace the SPL
  with HSPL" / "Restore the stock SPL". Host-verified: Java-built NAND from the real RUU boots WinMo through the real chain.
- **SD-card editor** (`FatImage` + `SdEditorActivity`): FAT12/16/32 images, LFN, add/export/rename/move/delete/mkdir, create new; fsck.vfat clean.
- **In-app ADB** (`Adb` + `AdbActivity`): shell, logcat, push, pull, install; exec/push/pull verified against the phone's guest.

## Open / polish
- HSPL USB-mode serial protocol not decoded; USB MSC (mass storage) and RNDIS tethering through the passthrough untested.
- `hd2_usb.py adb-shell` fixed (checksum) but not re-tested after the fix; BYON picks `RUU_signed.nbh` (the RUU .exe is not unpacked in the app).
- WinMo sensor readout, the new UI screens (SD editor, ADB, BYON) and the `install` path were compiled and logic-tested, not clicked through.
- Phone: the app's `adb forward` + USB passthrough interplay with Wi-Fi ADB worked; S25 never locked (stay-on while plugged is set).
