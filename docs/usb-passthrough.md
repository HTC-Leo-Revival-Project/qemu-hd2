# USB passthrough: the emulated HD2 on the phone's own USB port

Status and findings, for picking this up again. Everything below was measured on
an S25 (SM-S931B, Snapdragon 8 elite, `6.6.127-android15-8`, rooted with
KernelSU), not assumed.

## What this is

With passthrough on, the S25's USB port stops being the S25 and becomes the
emulated HTC HD2: a PC plugged into the phone sees an HD2 and its `adb`,
`fastboot` and flashers talk to the emulated one.

    PC  <--USB-->  S25's UDC (hw)  <--FunctionFS-->  usbffs  <--USB/IP socket-->  QEMU
                                                              (the HD2's gadget)

* **QEMU side (done).** `-M htcleo,usb-host=lle,usb-ip-socket=PATH` speaks the
  same USB/IP stream vhci_hcd would have spoken, on a unix socket. QEMU listens,
  the bridge connects. No root, no `vhci_hcd`.
  Files: `htcleo_usbip.c` (listen/accept, plus re-listening when the peer goes
  away), `msm_hsusb.c` (`usb-ip-socket` property), `htcleo.c` (`usb-host=socket`,
  `usb-ip-socket`).
* **Bridge side (`android/native/usbffs.c`; works, see "Verified end to
  end").** Runs as root, walks the emulated gadget for its descriptors over
  USB/IP, sets up a configfs + FunctionFS gadget, binds the UDC, and proxies
  every transfer.
* **App side (not started).** `Config`/`Emulator`/`RootActivity` still need the
  passthrough switch and the `usb-ip-socket` argument. See "Still to do".

### Running it by hand (what the app will do)

```sh
# on the phone, as root
qemu-system-arm -M htcleo,usb-host=lle,usb-ip-socket=/data/local/tmp/usbip.sock \
    -drive if=mtd,format=raw,file=<nand.img> \
    -display none -serial file:/data/local/tmp/uart1.log \
    -qmp unix:/data/local/tmp/qmp.sock,server=on,wait=off \
    -d unimp,guest_errors &
usbffs --usbip /data/local/tmp/usbip.sock --verbose &
```

`usbffs` takes the UDC when it is up (about 30 s in: QEMU has to boot the
guest first) and gives it back on SIGTERM, restarting `vendor.usb-hal`.
`--selftest` makes it try the descriptor write four ways and print what the
driver says, which is the quickest way to tell a blob problem from a state
problem.

## vhci_hcd: not needed, and not there

The other way round -- attach the emulated gadget to *this phone's* USB host
stack -- is impossible here, and `UsbBridge.Probe.hostPossible()` should say so
plainly rather than just reporting a missing directory:

| | |
|---|---|
| `/sys/bus/platform/drivers/vhci_hcd` | does not exist |
| `/sys/module/vhci_hcd` | does not exist |
| `modprobe vhci_hcd` | fails: there is no `/lib/modules` at all |
| `CONFIG_USB_VHCI_HCD` | not in `/proc/config.gz` (only `BT_HCIVHCI` is mentioned, which is Bluetooth) |

So: passthrough is gadget mode, on this phone's own USB port. `hostPossible()`
has been the wrong question all along; what the feature needs is `gadgetPossible()`
(configfs + functionfs + a free UDC), all of which this phone has.

## The FunctionFS descriptor blob

The header is **host order** (`get_unaligned_le32`), and each per-speed field is
a **descriptor count**, not a byte length:

```c
u32 magic   3        /* FUNCTIONFS_DESCRIPTORS_MAGIC_V2 */
u32 length  93       /* the whole blob, including these 24 bytes */
u32 flags   7        /* HAS_FS_DESC | HAS_HS_DESC | HAS_SS_DESC */
u32 fs_count, hs_count, ss_count
  fs blob | hs blob | ss blob     /* interface + endpoint descriptors ONLY */
```

then the strings as a second write: `magic 2, length, str_count, lang_count,
0x0409, ...NUL-terminated UTF-8`.

Four ways to get a bare `EINVAL` out of the `write()` to `ep0`, all of which look
identical from userspace:

1. **Byte order.** The header written big-endian (the way USB/IP headers go)
   fails `get32(data + 4) != len` and nothing else. The interface and endpoint
   descriptors inside the blobs are little-endian, as on the wire; only the
   six header words are host order. Keep both writers in the bridge: `put32()`
   for USB/IP frames, `put32le()` for these.
2. **Counts are counts.** An adb interface is 3 descriptors (interface, ep IN,
   ep OUT), so `3`, not `23`.
3. **All three speeds must be present**, each with its own blob.
4. **Writes must be at least 16 bytes**, so toybox `base64 -d > ep0` (which
   dribbles the blob out in chunks) always fails.

Interface and endpoint descriptors only: a device or configuration descriptor
in the blob is rejected, because those come from the configfs attributes.
`ffs_do_descs()` leaves `len` to be exactly consumed, so a count that does not
cover the descriptors in its blob fails too.

## High speed, and why not super speed

The emulated HD2 hangs off an EHCI controller. Say so:

```
echo high-speed > /config/usb_gadget/<gadget>/max_speed
```

* the driver only accepts `high-speed`, not `high` (EINVAL for anything else);
* it can only be written while the gadget is unbound;
* the USB HAL puts `super-speed-plus` back on every gadget while it runs, so
  this has to happen *after* `stop vendor.usb-hal`;
* and it matters: at super speed the host insists on SS endpoint companion
  descriptors that FunctionFS will not take, so `SET_CONFIGURATION` fails with
  EPIPE and the interface never comes up at all. The host says so plainly:
  `can't set config #1, error -32`.

## Unbinding a gadget needs a newline

`echo "" > .../UDC` unbinds. `write(fd, "", 0)` does **not** -- the kernel
ignores a zero-length write (and reports EFAULT for it), the gadget stays bound,
and the next bind gets EBUSY.

## The emulated guest has to be told the host configured it

This is the one that is not obvious. The phone's UDC answers the standard
requests itself, out of the descriptor copy we gave it, so the host's
`SET_CONFIGURATION` never reaches the emulated gadget. In cLK that request is
what `handle_setup()` uses to enable the bulk endpoints and signal
`UDC_EVENT_ONLINE`, which is what wakes the fastboot thread. Without it the
emulated phone enumerates and then sits there mute: the guest's queue heads for
the bulk endpoints are never primed, every bulk transfer on the host times out,
and the UART shows `init_ui()` and then nothing at all (no
`fastboot: processing commands`).

So the bridge replays it, on the ffs `ENABLE` event, which is exactly the host
selecting the configuration:

```
ffs: event type 2                        /* FUNCTIONFS_ENABLE */
emu: <- replay SET_CONFIGURATION as seq 8
```

The answer goes nowhere: the UDC has already given the host its status stage and
no SETUP is waiting on ep0 for it.

## Verified end to end

QEMU on the phone with `-M htcleo,usb-host=lle,usb-ip-socket=...`, the guest
booted from the NAND (cLK, straight into fastboot), the bridge on top of
FunctionFS, and the PC on the other end of the phone's own USB port:

```
$ lsusb | grep 18d1
Bus 001 Device 013: ID 18d1:0d02 Google Inc. Celkon A88      (high speed, 480M)

$ fastboot getvar version
version: 0.5
$ fastboot getvar product
product: HTCLEO

$ fastboot flash recovery HD2_TWRP_2.4.0.0-nanddump.img
Sending 'recovery' (4856 KB)
Writing 'recovery'                      OKAY [  0.001s]
```

with the guest's own UART agreeing:

```
fastboot: download:004be000
fastboot: flash:recovery
writing 4972544 bytes to 'recovery'
flash_write_image: success
partition 'recovery' updated
```

and the bytes verifiably in the emulated NAND: page 0x221 (the recovery
partition in cLK's table) starts with `ANDROID!` and the image's cmdline.

So: the phone's UDC serves the host, the host's URBs reach the emulated gadget,
its answers come back, and writes land in the emulated flash.

## Bugs this shook out of the bridge

Each of these was a silent stall, not a crash:

| Symptom | Cause |
|---|---|
| `EINVAL` on the ep0 write, same bytes fine from a shell | big-endian header (above) |
| `can't set config #1, error -32` | super speed negotiated, no companions |
| `EBUSY` on bind | the HAL's gadget was still bound; unbind with `"\n"` |
| guest never serves fastboot | `SET_CONFIGURATION` not replayed (above) |
| no bulk OUT ever moved | the OUT worker waited for `have`, which only a read sets -- deadlock before the first read |
| whole bridge froze after the first wake-up | the wake pipe was blocking and the drain loop read it until EOF, which never comes |
| 7-byte reply lost | `submit_urb()` zeroes the slot, and the submit loop ran before the answer was collected |
| thousands of 0-byte transfers, guest printing empty commands | an OUT *answer* was announced as pending endpoint data, so the loop submitted an empty transfer to the emulator, completing the gadget's outstanding read as an empty command |
| QEMU never listened again after the bridge exited | `usbip_service()` deleted the shared timer, cancelling the re-attach armed by `usbip_link_reset()` |
| emulator read a SETUP on a bulk endpoint | stale stack bytes in the header's setup field; the frame needs zeroing, and the decoder must only believe it on ep0 (it already does) |

Still unexplained: one run stalled deterministically at 4,915,373 of 4,973,312
bytes and needed a reboot of the guest; the next identical run flashed in
0.136s. Not root-caused.

## The `-19` on bind

`usb_gadget_start()` returns `-19` (`ENODEV`) if the ffs function is not
`FFS_ACTIVE`, and `FFS_ACTIVE` only holds while **ep0 is open in the process
that does the bind**. Writing the descriptors in one process and binding in
another always fails this way. The same gadget with `ncm.0` instead of `ffs`
binds fine, which is what identifies it as ffs and not the gadget.

So the order is: gadget up -> ep0 open -> descriptors -> *then* the UDC.

## Constraints on this phone

* **One gadget per boot is not a thing** -- a second gadget (`hd2`) works fine.
  `mkdir` can fail with `ENOMEM`/`EEXIST` when a previous gadget was removed in
  the same boot and left its `android%d` class device behind; a reboot clears
  it. Treat ENOMEM on `mkdir` as "try another name, or reboot", not "unsupported".
* **`ln -s configs/c.1 c.1` returns EPERM.** The gadget-to-configuration
  association is implicit; the phone's own `g1` has no such symlink either. Don't
  write it.
* **A second configuration in one gadget returns EBUSY.** Use the one there is.
* **`max_speed` on the spare gadget only accepts `super-speed-plus`** (high and
  full give EINVAL). Leave it alone; it does not matter, the descriptors carry
  all three speeds.
* **ffs instance names leak.** After `umount -l` a `functionfs` instance stays
  "mounted" forever and refuses a new mount with `EBUSY`. Use a **unique name per
  run** (`usb<pid>`), and unlink/umount on a clean exit.
* **The vendor USB HAL rebinds `g1` within milliseconds** of taking the UDC
  away. `stop vendor.usb-hal` while passthrough runs and `start` it again on exit.
* The configfs function name must be `ffs.<name>` (the type comes first).
* `mount(2)`: the functionfs instance name is the mount **source**, not the data
  string -- anything in the data string is parsed as a mount option and comes back
  EINVAL.

## Verified on the phone

With the guest in fastboot (`-bios lk.bin` so no NAND transfer is needed), the
bridge walks the gadget over USB/IP and gets real answers:

```
emu: <- 80 06 value 0100 length 64        (GET_DESCRIPTOR DEVICE)
emu: -> 18 bytes, status 0
emu: <- 80 06 value 0200 length 9 / 32    (GET_DESCRIPTOR CONFIGURATION)
emu: -> 9 bytes / 32 bytes, status 0
emu: <- 0301 / 0302 / 0303                (the three strings)
emu: -> 14 / 16 / 18 bytes
emu: 18d1:0d02, 32 bytes of configuration, 3 strings
configfs: manufacturer = Google
configfs: product = Android
configfs: serialnumber = fastboot
emu: 3 descriptors, 2 endpoints
```

and a static FunctionFS gadget on the same phone enumerates on the laptop as the
HD2:

```
Bus 001 Device 015: ID 0bb4:0c02 HTC ...
Negotiated speed: High Speed (480Mbps)
  iManufacturer 1 HTC   iProduct 2 HD2 (emulated)   iSerial 3 0123456789
  Interface 0: class 255, bulk 512, ep81 IN / ep01 OUT
```

## Still to do

1. **The one unexplained stall**: a run that stopped at 4,915,373 of 4,973,312
   bytes and needed the guest rebooted, with nothing in the logs to tell it from
   a host that had simply finished sending. Worth a few more runs with
   `--trace enable=msm-usbip*` before calling this done.
2. **App wiring.** `Config`: a passthrough switch . `Emulator.launch()`: add
   `usb-ip-socket=<runDir>/usbip.sock`, set `usb-host=lle`, and drop the adb TCP
   chardev while passthrough is on (it is the same rule `tools/start.sh` follows
   for `USB=lle`). Start/stop `usbffs` as root through `Root`.
3. **Ship the bridge.** Add it to `android/scripts/build-native.sh` so it lands in
   `jniLibs` next to the two QEMU binaries.
4. **`UsbBridge.probe()`** should report vhci as *absent by design* rather than
   as a missing directory, and gain the checks that matter: configfs gadget,
   functionfs in `/proc/filesystems`, a UDC in `/sys/class/udc`, and whether the
   USB HAL can be stopped.
5. **Docs**: `README.md` (USB section) and `android/README.md` both still say
   USB LLE is impossible on the phone.

## Debugging helpers (not part of the APK)

Deliberately outside `android/`: they are for working on the phone, not shipped.

* `tools/androotsh/androotsh.c` -- a root shell over TCP, so adb dropping (which
  happens whenever the USB state changes) does not stop the work. Build with the
  NDK; `-static` does **not** work (bionic wants a 64-byte-aligned TLS segment),
  so it is a dynamic binary.
  Start: `adb shell 'su -c "nohup /data/local/tmp/androotsh --port 7777 &"'`;
  stop: `pkill -x androotsh` (exact name -- `pkill -f` matches your own `su -c`
  command line and kills the shell running it).
* `tools/ph` -- run a shell command as root over that socket:
  `tools/ph 'id'`, or a script on stdin. `PH_ADDR` overrides the phone's address.
* A local `python3 -m http.server` plus the phone's `curl` is the reliable way to
  move binaries over (chunked base64 through a shell loses bytes).

## Rules for working on this phone

* **Never reboot it.** Root here is temporary and its annoying to trigger everytime usb stack can be recovered without reboot
* When the phone's own USB has been taken over, put it back: unbind the UDC, then
  `start vendor.usb-hal`. `adb` over wifi survives all of it.