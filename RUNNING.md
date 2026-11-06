# RUNNING.md — unattended session log (started 2026-11-06)

## Status
- Survey done. Existing: SPL boot (`spl=nand`, HSPL flash via build/claude/spl/ruuflash.sh), USB LLE
  (`usb-host=lle`, usbffs bridge on S25, docs/usb-passthrough.md), Android app (android/app, Java:
  Config/Emulator/NandBuilder/Adb/SensorsActivity), BMA150/AKM8973/microP sensor models in
  src/qemu/hw/arm/htcleo/htcleo_i2cdev.c (static values, only battery/charger/GPS exposed in app).
- **Blocker:** S25 (10.0.0.28) ADB shows `offline`, androotsh :7777 not listening; phone is in MTP mode on USB.
  Not rebooting it / not touching sys.usb.config (user rule). Phone-side tests deferred until reachable.

## Next tasks
1. SPL: verify Config/Emulator "SPL LLE" toggle + fallback to USB when LEOIMG.nbh missing (host QEMU test first).
2. USB LLE reuse of same path (pyusb shell helper).
3. Sensors HLE: live host sensors -> QMP -> i2cdev (BMA150/AKM/light/prox).
4. BYON: minimal NAND + first-run stock RUU picker + HSPL replace option.
5. SD editor, in-app ADB check.

## Log
- 01:45 SPL LLE toggle added to the app (Config.splLle -> `-M htcleo,spl=nand`, Settings row, string). App compiles
  (`JAVA_HOME=/usr/lib/jvm/java-1.21.0-openjdk-amd64 ./gradlew --offline :app:compileDebugJavaWithJavac`).
  Host check: `spl=nand` loads HSPL from NAND block 0x20a on build/claude/spl/nandwmboot.img (screen black, no radio);
  images/nand-cm9-gpu.img has no HSPL, QEMU refuses with a clear error (app should check/steer to BYON/HSPL flash).
- Phone ADB reachable again (10.0.0.28, wifi). Host root: `sudo /bin/sh -c '...'`.
- NOT done yet: USB pyusb path, sensors live mapping, BYON, SD editor, in-app ADB review. Each is a multi-hour item;
  continuing in the order above.
- 02:10 Sensors HLE: QEMU props `accel` (milli-g), `mag` (uT), `light` (lux) on /machine, wired to BMA150 (microP),
  AKM8973, microP light ADC (htcleo_i2cdev.c, htcleo.c). Verified qom-set/get on host QEMU build.
  App: HostSensors.java feeds Android SensorManager accel/mag/light -> qom-set at <=20 Hz while the emulator runs. Compiles.
  UNVERIFIED: guest-side readout (WinMo/Android) and on-phone run — the app's bundled QEMU for the phone must be
  rebuilt with these props (android/native build) before the phone shows live data; not done.
- 03:00 (out of priority order, parked): FatImage.java (SD editor engine, host-tested + fsck.vfat clean) and Adb exec/push/pull/install
  (compiles, untested vs. a guest). Native arm64 QEMU rebuilt with sensor props. NO SD-editor UI yet. Back to SPL -> USB first.
- 03:40 SPL LLE VERIFIED on host: FSBL->OSBL->HSPL->cLK->Android (CM9) boots to the launcher with
  `scratchpad/chain.sh` (cm9chain.img = hd2_mknand --nbh LEOIMG.nbh --spl hspl.bin --rom CM9 --gpu, then
  hd2_radio.py RUU_signed.nbh --write-nand). Needed fix: HSPL-only I2C chips 0x34/0x5C are hidden at hand-off
  (the kernel TS driver took them for the unsupported B8/68 touch types -> BUG). Also `hd2_radio.py --sbl DIR`
  writes fsbl.bin/osbl.bin; app extracts them (Radio.Firmware) and passes them for SPL LLE.
  On the S25 the first try booted a stub kernel NAND (nandosbl.img), not Android: that was my test image, now replaced.
