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
- 02:20 (clock) SPL LLE VERIFIED IN THE APP ON THE S25: nand-spl.img (CM9 + HSPL + radio chain) -> real FSBL/OSBL/HSPL -> cLK -> CM9 wallpaper, 13 fps.
  Live sensors reach QEMU (qom-get accel/mag/light show real values). Next: USB LLE passthrough on top of the SPL chain.
- 04:55 USB LLE on top of SPL LLE VERIFIED on the S25's own USB port, PC = this host:
  * real chain boots -> cLK fastboot: `fastboot getvar product` = HTCLEO; `tools/hd2_usb.py fastboot getvar product` (pure pyusb, no ttyUSB/iPAC) works.
  * `fastboot continue` -> Android: PC re-enumerates as 0bb4:0c02 and `adb -s fastboot shell` gives uid=0 on NexusHD2 4.0.4; adb push/cat work.
  Bridge fixes (android/native/usbffs.c): (1) reconnect+re-probe when QEMU drops the link while the HSPL/cLK reset the USB
  controller (bridge used to die on SIGPIPE, which killed the whole emulator start); (2) re-exec to follow the guest's gadget switch
  fastboot->adb (QEMU closes the link on gadget reset); (3) FFS workers survive ESHUTDOWN between ENABLE events;
  (4) bulk wMaxPacketSize rewritten per speed (the Android gadget advertises 64 at FS, the port is pinned HS: transfers >64 B hung, adb "offline").
  Found with usbmon (`sudo modprobe usbmon; cat /sys/kernel/debug/usb/usbmon/2u`).
  Known gap: hd2_usb.py adb-shell times out waiting for CNXN right after a real adb server session (real adb works); MSC/tethering untested.
  Also: host QEMU needs `-global msm-modem.clk-regime-fake=on` for the YAFFS mount to not wedge (the app sets it).
- 05:20 BYON implemented (Java): assets/spl/{spl_stock,hspl}.bin bundled; Nand.flashSpl/writeHtcConfig/edit; NandBuilder flashes SPL +
  HTC config (WinMo build uses the RUU's own stock SPL, section 0x200); Spl.createMinimal/replace; ByonActivity (first launch once, also
  Settings -> Bootloader): pick stock RUU (.nbh) -> nand.img, or minimal SPL-only NAND; Settings rows "Replace the SPL with HSPL" /
  "Restore the stock SPL". Emulator uses the full FSBL/OSBL chain only if the NAND has a radio, else plain spl=nand (minimal NAND,
  verified on host: HSPL runs from the minimal image). Host-verified: Java-built NAND from the real RUU boots WinMo (HTC splash) through
  the real chain. RUU .exe is not unpacked in the app yet (pick the RUU_signed.nbh).
- 05:50 SD-card editor (FatImage + SdEditorActivity: create/open image, browse, add/export/rename/move/delete/mkdir; FatImage host-tested, fsck.vfat clean)
  and in-app ADB (Adb.exec/push/pull/install + AdbActivity: shell, logcat, push, pull, install). Adb verified against the phone's guest
  through `adb forward`: exec, 300 KB push, pull round trip OK. Bug fixed: the packet check field is a byte SUM, not CRC32 (adbd dropped
  every packet with a payload; same bug was in tools/hd2_usb.py adb-shell). Menu: SD card editor, ADB in the main menu.
- 05:30 SPL USB mode: with no OS in the NAND and no LEOIMG.nbh on the card the app holds Volume Down (keys=...voldown:4000) so the real
  HSPL shows its bootloader screen ("SPL-2.08.HSPL", "Security Unlocked") and a USB gadget; through the S25's port the PC sees
  0bb4:00ce "Qualcomm CDMA Technologies MSM" (class ff/ff/ff bulk pair); hd2_usb.py list/serial can open it. The HSPL's serial
  protocol itself was not decoded (no reply to `info 2` / `help`). Phone restored: passthrough off, nand-spl.img selected, USB HAL running.

## Session 2 (2026-11-06, unattended) — next tasks
1. USB MSC + RNDIS from the Android guest, on the host via vhci_hcd (start-usb-lle.sh): guest `setprop sys.usb.config mass_storage,adb` / `rndis,adb`.
2. Re-test `tools/hd2_usb.py adb-shell` (byte-sum checksum fix) against the guest.
3. usbffs.c: check multi-interface / interrupt-EP / IAD support needed for RNDIS through the S25 passthrough.
4. WinMo sensors: boot WinMo on host, confirm the guest polls BMA150/AKM/light (I2C trace) and reacts to qom-set (rotation).
5. In-app `Adb.install` against a guest (host harness around Adb.java).
- Phone state at start: Wi-Fi ADB only (port changes, found with nmap 30000-49999), USB powered but in `sec_charging`
  and NOT on this host's USB (lsusb shows nothing) -> passthrough-to-PC tests need the cable in this host.
- 10:20 USER RULE: verify in the app on the S25 only (adb screencap + input), no headless host QEMU, no scrcpy.
  (A host vhci_hcd run had already shown guest UMS sharing a 512 MB disk with an 8 MB md5 round trip and RNDIS
  enumerating as 0bb4:0ffc with no host netdev; host-only, does NOT count as verification.)
- 10:21 In-app ADB install FIXED + VERIFIED in the app (S25, CM9 guest via SPL chain): install failed with "CLSE" because
  Adb.java did not demultiplex streams: the guest's CLSE for the finished push sync stream was taken as the answer to
  the `pm install` OPEN. Now every stream skips OKAY/WRTE/CLSE not addressed to it (arg1); shell() acked with our own
  id instead of the guest's (arg0); QUIT may be answered by CLSE. Clicked through: Install an APK -> hd2bench.apk ->
  "Success", `pm path com.hd2bench` in Run a shell command -> /data/app/com.hd2bench-2.apk; Logcat shows 200 lines.
- Phone USB cable is not in this PC (lsusb: no S25), so MSC/RNDIS through the passthrough cannot be tested now.
- 10:28 WinMo sensors: in the app (nand-ruu-osbl.img, real chain, Sense up in 58 s) WinMo never used the Linux G-sensor path
  (microP 0x76 + 0x77..0x7A). It talks to the BMA150 registers through the microP: 0x74 [reg] request, 0x75 read
  [reg,value], 0x73 [reg,value] write. The model answered 0x75 with zeros. Added a BMA150 register file (0x00..0x15:
  chip id 2, live acc in 0x02..0x07 from the same `accel` prop, 2 g clamp) and a trace line per 0x75 read
  ("htcleo-microp: bma150 reg"). Phone build installed; WinMo boot shows its driver init: reads 0x0a/0x14, writes
  0x14=0x08, 0x0a=0x08/0x09. Waiting for the user's Teeter test; Teeter's polling should show reg 0x02..0x07 reads.
