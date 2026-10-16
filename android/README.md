# Android app

The HTC HD2 (`htcleo` machine) as an Android app: QEMU cross-compiled for
Android, the screen drawn from QEMU's VNC server, keys and taps sent back over
QMP, and the sound played by `AudioTrack`.  The UI is **Holo**, not Material -
`@android:style/Theme.Holo`, the platform widgets, and *no dependencies at
all*: no AndroidX, no Material, not even Kotlin.

    minSdk 15 (Android 4.0.4)   targetSdk 36 (Android 16; runs unchanged on 17)
    ABI       arm64-v8a
    QEMU      built for API 28 (Android 9) - see "why 28" below

## Build

Three steps, all of them resumable:

```bash
# 1. the toolchain: JDK 17, Gradle, the Android SDK and the NDK (~7 GB)
nohup android/scripts/fetch-toolchain.sh > /tmp/fetch.log 2>&1 &

# 2. QEMU, cross-compiled with the NDK (~15 min, downloads ~15 MB of sources)
android/scripts/build-native.sh                 # both ABIs
android/scripts/build-native.sh arm64-v8a       # or just one

# 3. the APK
cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

`build-native.sh` builds zlib, pcre2, glib, pixman, libslirp and libfdt into
`android/build/prefix/<abi>` (all static, so the app ships two binaries and no
shared libraries), then QEMU itself with `--target-list=arm-softmmu,
hexagon-softmmu`, and copies the stripped `qemu-system-arm` and
`qemu-system-hexagon` into `app/src/main/jniLibs/<abi>/`.  Everything is
stamped, so an interrupted run continues where it stopped; `--clean` starts
over.

**arm64-v8a only.**  QEMU 11 has no 32-bit host backend left - `tcg/` ships
aarch64, x86_64, riscv64, s390x, ppc64, sparc64, loongarch64 and the
interpreter, and its `meson.build` refuses any host with 4-byte pointers - so
there is no armeabi-v7a emulator to package.  The old `arm` TCG backend would
have to be ported first.

The VNC server wants its keymaps, so `pc-bios/keymaps` is copied into
`app/src/main/assets` and unpacked into the app's storage on first use.

## How the app talks to QEMU

QEMU runs as a child process of the app, started out of `nativeLibraryDir`
(or unpacked from the APK if that is not there), with `-display none` and:

| | |
|---|---|
| picture | `-vnc unix:<app>/emu/run/vnc.sock` - an RFB 3.8 client in Java takes raw RGBX frames, which are byte for byte what an ARGB_8888 `Bitmap` holds, and QEMU's audio extension delivers S16 stereo PCM to `AudioTrack` |
| keys, taps | QMP `input-send-event`: abs axes 0..0x7fff, qcodes for the eight hardware keys |
| settings | QMP `qom-get`/`qom-set`, `system_reset`, `quit` |

Both sockets are unix sockets **inside the app's own directory**, on purpose:
every app on a device shares the loopback interface, so a QMP on 127.0.0.1
would be open to any other app that cared to connect.

Commands that do not wait for their reply still have their answer looked at,
and log it when QEMU refuses: an input event QEMU dropped is otherwise
invisible and looks like a guest that ignores the keys.

The log screen has two tabs that are not files:

* **monitor** - QEMU's own monitor (`human-monitor-command`): `info cpus`,
  `info registers`, `screendump`, `stop`, `cont`.  The fastest way into a
  guest that is not answering.
* **adb** - a shell into the Android *inside* the emulator, over the adb
  bridge QEMU exposes on this device's loopback (`Adb.java`).  It speaks the
  adb message protocol itself rather than shipping an `adb` binary: the
  arm64 builds that exist are linked against another app's shared libraries,
  and the protocol is small (CNWN, OPEN a service, read WRTE until CLSE).
  `logcat -v time` is the one worth having - the guest's kernel log stops at
  whatever `console=null` hides, and init's own messages only ever appear
  here.  Until the guest's `adbd` starts, minutes into the boot, the tab says
  so instead of hanging.

Where the desktop version runs the adb bridge on TCP, the app keeps the same
`-chardev socket,...,port=5555` bridge; nothing in the app connects to it yet.

## What is missing, and why

* **USB host LLE** (`usb=lle`).  It hands the emulated phone to the host's own
  USB stack through `vhci_hcd`, whose `attach` attribute is root-only and has
  to be written by the process that owns the socket.  An app can never do it,
  so the phone always uses the built-in host, which enumerates it and bridges
  adb to a TCP port.
* **The host GPU renderer** (`msm-adreno.backend=gl`).  It needs OpenGL 4.5 and
  `GL_ARB_fragment_shader_interlock`; a phone's GLES has neither.  The
  software renderer is what draws today - see "OpenGL ES" below.
* **Why API 28 for the binary.**  The app installs and its settings, logs and
  NAND builder work from Android 4.0.4 up, but the emulator itself needs API
  28: that is where bionic starts declaring `<iconv.h>`, which glib requires.
  QEMU also runs on far slower hardware than a 2017 phone.

## OpenGL ES on the phone's GPU

The desktop backend (`gl`) needs desktop OpenGL; an Adreno 8xx gives us GLES
3.2 and nothing more, so `hw/arm/htcleo/msm_adreno_gles.c` is a second
backend for exactly that case, chosen with `-global msm-adreno.backend=gles`
(the app's **Adreno 200 renderer** setting).  `gl` and `soft` are untouched.

What is in place:

* `libEGL`/`libGLESv3` loaded with `dlopen`, so nothing is linked in and the
  same code builds wherever the app does - and it works from QEMU's *child*
  process on Android, which is not obvious and was worth checking.
* A surfaceless (or pbuffer, `ADRENO_GLES_PBUFFER=1`) EGL context, asked for
  OpenGL ES 3, with the extension list checked for what a renderer needs.
* A smoke test at device init that compiles a GLSL ES pixel shader, draws with
  a vertex array, writes GMEM through an `imageStore` and an
  `imageAtomicOr`, and reads it back.  All of it in `qemu.out`:

      msm-adreno: GLES 3 backend on Adreno (TM) 830 (EGL 1.5, OpenGL ES 3.2 ...)
      msm-adreno:   smoke test: OK, the host GPU runs the translated shaders
                    and writes GMEM

  On this phone it passes, so the Adreno 830 has everything the desktop
  backend's design needs: image atomics for GMEM's read-modify-write and
  framebuffer fetch in place of fragment shader interlock.

* The **render backend compiles as GLSL ES**: `depth_stencil()`, the colour
  write, blending, the surface addressing - the desktop backend's own GLSL
  with GMEM read and written through a `uimage2D` instead of an SSBO, and
  without the interlock.  The check runs on whatever host has a GLES 3
  context, and `ADRENO_GLES_DEBUG=1` writes the assembled shader out.

The renderer itself is written: the program cache, the texture cache, the
guest's vertex and pixel shaders as GLSL ES, GMEM as an image with read-back
for the resolves, and the draws.  What it needs from the host it asks for
before it takes a draw, and a host that cannot is a software renderer:

| needs | for | this phone |
|---|---|---|
| image atomics | GMEM's read-modify-write, where neighbours share a word | yes |
| `GL_EXT_clip_cull_distance` | the guest's vertex shader's clip planes | yes |
| GL ES 3 | everything | 3.2 |

It **runs**: the log says `the guest's draws are running on this host's GPU
through OpenGL ES` once the hundredth draw has gone through it, and a boot
to the lock screen comes out right - clock, wallpaper, icons, carrier and
all, the same picture the software renderer draws.

Three things it needs that the desktop backend does not, all of them the
reason it drew nothing at all before:

* **GMEM's texture unit.**  The pixel shader takes units 0..15 and the
  vertex shader 16..31, so GMEM's image gets a unit of its own - 32 on a
  phone with 96 units; a host with fewer shares the two stages' units, which
  only costs a vertex shader that samples something of its own.  Its *image*
  unit stays 0: an image's `layout(binding=)` names an image unit, of which
  there are far fewer than texture units, and it is `glUniform1i` that points
  the sampler at the texture.
* **A framebuffer big enough, and not GMEM.**  A fragment outside the
  framebuffer's attachment is dropped before its shader runs, so the draws go
  to a dummy attachment the size of the window space the scissor can name -
  with GMEM itself as the attachment it would also be a feedback loop, the
  texture being bound for `imageLoad`/`imageStore` while it is being drawn
  to.  A second framebuffer holds GMEM, for the read-back.
* **`gl_ClipDistance`** needs `GL_EXT_clip_cull_distance` even in GLSL ES
  3.20, so the guest's clip planes only go into the vertex shader when the
  host has it; the primitives are clipped on the CPU either way.

A host with neither image atomics nor framebuffer fetch draws in software,
and says which it was in the log.

## Booting is not reliable yet

The guest reaches Android ICS most of the time, but sometimes it stops: the
vCPU halts with its PC unchanged and almost no CPU to speak of, in kernel
mode, just after `yaffs: Attempting MTD mount of 31.4,"mtdblock4"` - the
mount of `/system`.  It is a lost wake-up rather than a slow mount (runs that
get through take the same path and carry on), so the next job is which
interrupt never arrives.  What it is *not*: the app - the same image boots
from `tools/start-q6.sh` - and not the renderer, which never sees a draw
either way.

The **adb** tab above is what settles it: the guest's own logcat says how far
init got.  For QEMU itself, `-monitor tcp:127.0.0.1:4444,server,nowait` in
**extra QEMU arguments** (with `adb forward tcp:4444 tcp:4444`) puts the
monitor where a machine can reach it.  Neither is on by default: every app on
a device shares the loopback interface, so a monitor listening there is open
to all of them.

## Two changes to QEMU

Both are in the tree in `src/qemu` and only fire when building for Android:

* `util/oslib-posix.c`, `qemu_shm_alloc()`: Android has no POSIX shared
  memory (`shm_open`) and does not let an app call `memfd_create`, so there it
  opens a file in `$TMPDIR` and unlinks it immediately - an anonymous file the
  app owns, which is all that function is ever asked for.
* `meson.build`, the `rt` lookup: the same, so the build stops asking for a
  `librt` that does not exist for apps.

Everything else Android needed was already in the NDK or in configure.

## NAND builder

The menu has **Build a NAND image**, which builds any of the three kinds of
image the tools in `tools/` make; **Kind** at the top picks one:

* **Android (cLK)** - `tools/hd2_mknand.py`, below.
* **MAGLDR** - `tools/hd2_mkmagldr.py`: `magldr.nbh`'s OS section in the OS
  area and the factory MACs.  An optional radio NBH is written into the first
  blocks the way `hd2_radio.py --write-nand` adds it afterwards, since MAGLDR
  restarts the radio from NAND.
* **Windows Mobile 6.5** - `tools/hd2_wmnand.py`: a stock `RUU_signed.nbh`'s
  radio (or another radio NBH's) and its OS section, page by page with the
  WinCE sector info, from block 0x212.  The RUU is read in place
  (`Radio.Nbh`), not loaded whole: it is bigger than an app's heap.

`Radio.java` is `hd2_radio.py`'s NAND half.  The Java builders write the same
bytes as the Python tools.

The Android kind is `tools/hd2_mknand.py` in Java
(`NandBuilder.java`, `Nand.java`, `Yaffs2.java`): pick `LEOIMG.nbh` (or a raw
`lk.bin`), a *NAND* Android ROM zip and, optionally, a recovery image, and it
writes the same 512 MiB image the Python tool does - OS area at block 0x212,
VPTABLE (or cLK 1.4's compiled table), `boot.img`, then yaffs2 `/system` and
`/userdata`, including the `updater-script`'s symlinks and permissions.

Files come in through the system file picker (`ACTION_OPEN_DOCUMENT`) and are
copied into the app's own storage, so no storage permission is needed and
scoped storage does not matter.

## Images and the radio firmware

**Images** (in the menu, and Settings -> Manage images) lists the images
folder: tap a file to use it as the NAND flash or the microSD card, or to
delete it.  Settings also picks the microSD card directly.

The real baseband (LLE) runs `amss.elf` and the real DSP `dsp1.elf`.  A NAND
image with a radio in it - a stock ROM's, or MAGLDR's built with a radio -
already holds both, so when one is missing at start the emulator takes it out
of the NAND image's radio partitions by itself (`Radio.fromNand`, the MIBIB
table in the boot area says where).  Images -> Radio firmware does the same on
demand, from the NAND image or from a radio NBH (`hd2_radio.py -o amss.elf
--dsp dsp1.elf`).  Both give the same bytes as `hd2_radio.py`.

## Root and USB

The emulator never needs root.  **Root & USB** is the start of the features
that do: `Root.java` keeps one `su` shell (Magisk, KernelSU, APatch ask once)
and frames each command with a marker line carrying its exit status;
`UsbBridge.java` probes, as root, what this phone's kernel offers for putting
the emulated HD2's USB on real USB:

* **to a PC** through this phone's USB port (configfs `usb_gadget`, FunctionFS
  and a free UDC), so a PC's flash tools, adb and fastboot see a real HD2;
* **to this phone** through `vhci_hcd`, what `usb-host=lle` does on a PC.

Neither is switched on yet.  The gadget bridge needs QEMU to hand the USB/IP
stream `htcleo_usbip.c` speaks to `vhci_hcd` to the app over a unix socket
instead; the app then serves the HD2's descriptors and endpoints through
FunctionFS, with the phone's own gadget unbound from the UDC meanwhile.

## Languages

English, German, French, Spanish, Italian, Brazilian Portuguese, Russian,
Polish and Simplified Chinese, in `res/values-*/strings.xml`; Android 13 and
later list them under the app's own language setting (`res/xml/locales_config.xml`).
Every string the screens show is a resource.  What stays English is the
output of the ported tools (the NAND builder's steps, the NBH and radio
parsers' errors) and protocol errors from QEMU, adb and su, which match what
the Python tools and the desktop print.

## Layout

    android/scripts/build-native.sh    deps + QEMU, for both ABIs
    android/scripts/fetch-toolchain.sh  JDK, Gradle, SDK, NDK
    android/scripts/make-icon.py        the launcher icon, no PIL needed
    app/src/main/java/org/qemu/htcleo/
        MainActivity      the screen, the key bar, the machine in the action bar
        Emulator          the QEMU processes, the state machine, QMP
        EmuService        the foreground service that keeps it alive
        Qmp VncClient     the two protocols, in Java
        PcmPlayer         AudioTrack
        ScreenView        the screen: frames drawn in onDraw, touches
        Holo              the hand-drawn Holo kit: switch, radio, field, dialogs,
                          rows and the scrolling preference Page; insets
        HoloIcons         the action bar and key glyphs, as Paths (no PNGs)
        HoloActivity      the sub-screens' base: up caret, black bars
        ImagesActivity    the images folder, and the radio firmware out of an image
        Root UsbBridge    the root shell, and the USB passthrough probe
        RootActivity      Root & USB
        Config Images     the settings, and the files in the images folder
        LogsActivity      the four logs, and QEMU's monitor
        Nand NandBuilder Yaffs2 Radio   the NAND builder (Android, MAGLDR, WinMo)

## Notes

* The guest's RAM comes up through `emu/run/dram.bin` and is **deleted** on
  every start, as `tools/start-q6.sh` does.  Keeping it hands the next boot
  the last one's leftovers - the ram_console ring among them - and a guest
  still sitting in the bootloader then looks like one that booted.
* Screenshots land in `getExternalFilesDir(null)`, the one place an app may
  write on shared storage without asking.
* The service keeps the emulator alive in the background with a Holo
  notification; Android 13 and later need `POST_NOTIFICATIONS`, which the app
  asks for once on first start (it is only the notification that is affected).
* `targetSdk 36` means edge-to-edge is enforced on Android 15 and later, so
  every screen pads itself from the window insets (`Holo.apply`).
