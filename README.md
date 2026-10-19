<h1 align="center">HTC HD2 on QEMU</h1>

<p align="center">
  <img alt="SoC: QSD8250" src="https://img.shields.io/badge/SoC-QSD8250-33B5E5?style=flat-square&labelColor=222222&logo=qualcomm&logoColor=white">
  <img alt="QEMU: 11.1" src="https://img.shields.io/badge/QEMU-11.1-0099CC?style=flat-square&labelColor=222222&logo=qemu&logoColor=white">
  <img alt="status: experimental" src="https://img.shields.io/badge/status-experimental-FFBB33?style=flat-square&labelColor=222222">
  <img alt="vibecoded: 100%" src="https://img.shields.io/badge/vibecoded-100%25-FF4444?style=flat-square&labelColor=222222">
</p>

<p align="center">
  <img src="docs/hd2.png" width="720" alt="The emulated HD2: setup wizard, Settings and a 3D test on the emulated GPU">
</p>

<p align="center">
  <a href="#build"><img src="docs/holo/n-build.png" width="59" height="30" alt="Build"></a>
  <a href="#run"><img src="docs/holo/n-run.png" width="52" height="30" alt="Run"></a>
  <a href="#known-booting"><img src="docs/holo/n-known-booting.png" width="121" height="30" alt="Known booting"></a>
  <a href="#what-works"><img src="docs/holo/n-what-works.png" width="101" height="30" alt="What works"></a>
  <a href="#real-baseband-firmware"><img src="docs/holo/n-real-baseband-firmware.png" width="90" height="30" alt="Baseband"></a>
  <a href="#gpu-adreno-200"><img src="docs/holo/n-gpu-adreno-200.png" width="55" height="30" alt="GPU"></a>
  <a href="#tools"><img src="docs/holo/n-tools.png" width="62" height="30" alt="Tools"></a>
</p>

> [!WARNING]
> **Vibecoded.** This project was written FULLY by an AI coding agent (Claude), with little (actually 0) human review. Expect bullshit, don't ask me about this (i really don't know how it works either).

A QEMU machine (`-M htcleo`) that emulates the HTC HD2 (Qualcomm QSD8250) hardware. It boots whatever is in its NAND, unmodified, the way the phone would.

Everything written down lives under [docs/](docs/README.md): the [build guide](docs/building.md), the [tools reference](docs/tools.md), the per-OS guides and the subsystem write-ups.

<a name="build"></a>
<h2><img src="docs/holo/h-build.png" width="86" height="40" alt="Build"></h2>

One script does it all (see [docs/building.md](docs/building.md) for the
options); by hand it is:

```bash
sudo apt install libslirp-dev libpixman-1-dev flex bison   # flex/bison: the Hexagon idef-parser
mkdir -p src/qemu/build && cd src/qemu/build
../configure --target-list=arm-softmmu,hexagon-softmmu --enable-sdl --enable-vnc \
    --disable-docs --disable-werror --disable-gtk --disable-xen --disable-kvm \
    --disable-tools --disable-guest-agent
ninja qemu-system-arm qemu-system-hexagon
```

`tools/build-linux.sh` is that, plus the `amsslog` plugin, and it configures
only the first time. It needs `meson >= 1.5`, which Ubuntu's package is not:
`pip install --user meson`.

The machine is in `src/qemu/hw/arm/htcleo/`. VNC is always built — the web UI
and the Android app both draw the screen and play sound through it. One
optional extra:

```bash
ninja contrib/plugins/libamsslog.so          # only for AMSSLOG=1
```

QEMU is never `ninja install`ed here, so VNC cannot find its keymaps and dies with
`could not find keymap file for language 'en-us'`. The start scripts pass
`-L ../src/qemu/pc-bios` for that; if you run QEMU by hand, pass it too.

<a name="images"></a>
<h2><img src="docs/holo/h-images.png" width="106" height="40" alt="Images"></h2>

Nothing boots without a NAND image at `images/nand.img` (`NAND=` picks another
one). It is the phone's raw 512 MiB NAND: a dump of your own, or one built from a
bootloader and a NAND ROM zip:

```bash
cd images
python3 ../tools/hd2_mknand.py --nbh LEOIMG.nbh --rom rom.zip -o nand.img   # or --lk lk.bin
../tools/mksdcard.sh sdcard.img 1024                                        # optional microSD (FAT32)
```

`LEOIMG.nbh` is the cLK bootloader (`--lk` takes the raw `lk.bin` instead);
`--spl hspl.bin` additionally flashes the HSPL, the phone's own bootloader, so
that `SPL=nand tools/start.sh` can boot the real thing (see
[HSPL](#hspl-the-phones-own-bootloader) below).
`hd2_mknand.py` lays the NAND out as a flashed phone would have it: the
bootloader in the OS area, the partition table, the ROM installed (`boot.img`,
which it builds itself from `kernel/zImage` + `kernel/boot_NAND/initrd.gz`, and
yaffs2 `/system` and `/data`) and the factory WiFi/BT MAC addresses. The zip has
to be a **NAND** ROM — one with `kernel/`, `system/`, `data/` and
`mods/clk-ppp-patch/`; an SD install zip does not work. Other options:
- `--recovery recovery.img` adds a recovery image;
- `--system-mb`, `--cache-mb` set the partition sizes;
- `--wifi-mac`, `--bt-mac` set the MACs (`tools/hd2_setmac.py` changes them in an existing image);
- `--gpu` renders with the emulated Adreno GPU instead of in software (see [GPU](#gpu-adreno-200)).

<a name="hspl"></a>
<h2><img src="docs/holo/h-run.png" width="76" height="40" alt="HSPL"></h2>

By default QEMU does what the HSPL would do - read the cLK OS area out of the
NAND, drop it at 0x11800000, seed a few board globals and jump - which is enough
for cLK but skips everything between the SoC coming out of reset and the OS
starting. `SPL=nand` runs the real HSPL instead, so the whole chain is the
phone's:

```bash
cd images
python3 ../tools/hd2_mknand.py --spl hspl.bin --nbh LEOIMG.nbh --rom rom.zip -o nand.img
cd .. && SPL=nand tools/start.sh
```

`spl=nand` makes the boot ROM read the HSPL out of the NAND (block 0x20a, one
2048-byte sector per page, the layout `hd2_mknand.py --spl` writes and
`tools/hd2_osarea.py` can refresh in an existing image); `spl=<file>` boots it
from a loose image instead, unwrapping HTC's `R000FF` container on the way in.
From there nothing is emulated on the bootloader's behalf: the HSPL brings the
clocks up, identifies the NAND and builds its bad block table, reads the panel
and the EEPROM over I2C, DMOVs the OS image out of the NAND into RAM, and jumps
to physical 0x11800000 with the MMU off. cLK then relocates itself to its
`MEMBASE` of 0x28000000 and runs as usual. The log on `uart1.log` is the HSPL's
own, from `Scorpion Core eFuse Setting` through `IPLMSG:0x9:INFO: Jumping to
image...` to `Jump to Physical Address 11800000`.

Three things about the SoC have to be modelled for this to work, and all three
are in `hw/arm/htcleo/htcleo.c`:

- **the HSPL is entered at physical 0 with the MMU off**, the way the boot ROM
  does. Once it has built its page tables it installs its own window with
  `mov r1, pc ; lsr #20 ; lsl #20 ; orr #0x40e` (hspl.bin 0x9501deb0), which
  only comes out right if the core was executing at physical 0 when it ran.
  Enter it at VA 0x95000000 with the MMU on instead and the HSPL maps itself
  onto physical 0x95000000, which is not RAM.
- **CP15 c9 is implementation-defined**, so all of it (CRm 0..15, CRn 9) reads
  as zero and swallows writes, as on a Scorpion. Varying CRn instead would
  shadow c0..c15 - c1, the SCTLR, among them - and the HSPL's MMU-off
  trampoline (`mcr p15,0,r1,c1,c0,0`) would silently do nothing.
- **the DACR is RAZ/WI from ARMv6**, every domain a client. The HSPL still
  zeroes it while it sets the MMU up (hspl.bin 0x9500117c); in a model that
  honours it, the very next instruction fetch aborts.

<a name="run"></a>
<h2><img src="docs/holo/h-run.png" width="76" height="40" alt="Run"></h2>

It is always `tools/start.sh` with some environment variables and extra QEMU
arguments; it leaves QEMU running in the background and sends F2 a moment later
to pick BOOT FROM NAND in the cLK menu.

| What you want | Command | Needs |
|---|---|---|
| **Headless**, no UI (HLE, the default) | `tools/start.sh` | – |
| **An SDL window** | `tools/start.sh -display sdl` | – |
| **The browser UI** | `cd web && npm install && npm run dev` | – |
| **The real radio** (LLE) | `tools/start-lle.sh` | `images/amss.elf`, `images/nand-lle.img` ([below](#real-baseband-firmware)) |
| **LLE + the real DSP** | `tools/start-q6.sh` | `images/dsp1.elf` ([below](#qdsp6-firmware)) |
| **USB on the real host stack** (LLE) | `tools/start-usb-lle.sh` | root, `vhci_hcd` ([below](#usb-on-the-host-stack)) |

Stop it with `python3 tools/qmon.py images/qmp.sock quit`.

**Headless.** `images/sdcard.img` is attached if present, and USB device traffic (e.g. adb) is bridged to 127.0.0.1:5555 (`ADB_PORT`). Sound is off; for a WAV file `AUDIODEV="-audiodev wav,id=snd0,path=out.wav" tools/start.sh` (`pa,id=snd0` for PulseAudio, `pipewire,id=snd0`).

**An SDL window.** Add `-display sdl` — GTK is not built. It opens on the host's `$DISPLAY`, so without one it fails; with sound: `AUDIODEV="-audiodev pa,id=snd0" tools/start.sh -display sdl`.

**The browser UI** draws the screen inside a picture of a real HD2 and adds touch, keys, typing, sound and switches for HLE/LLE, the DSP firmware, the images, the GPU and the audio backend. It starts and stops QEMU itself over the VNC socket (which every build has); `web/README.md` has the details.

To put it on a server, `sudo tools/deploy-web.sh` copies what it needs into `/opt/htcleo-web`, builds it, adds `htcleo-web.service` and, with `--domain NAME`, an nginx site with a certificate. It is not a static site: the server runs QEMU, so the machine needs the emulator's own files, and the UI has **no login** — it can start the emulator with any extra QEMU arguments — so it stays on `127.0.0.1` behind TLS by default.

Whatever follows the script name goes to QEMU, so `tools/start.sh -vnc :1`, `-S` (do not start the CPUs) and `-snapshot` work as usual. `NAND=` picks another image, `CLK_KEYS=` presses no key at all (leave the bootloader alone) and `CLK_KEYS="pgdn pgdn f2"` picks RECOVERY instead.

The emulator, the Android app and the web UI are built with the scripts in
[docs/building.md](docs/building.md); the NAND and SD images with the tools in
[docs/tools.md](docs/tools.md).

| Key | HD2 button |
|---|---|
| PgUp / PgDn | Volume up / down |
| Home / Esc | Home / Back |
| F1 | Windows (Menu) |
| F2 / F3 | Call / End |
| F12 or End | Power |

F2 is also what picks BOOT FROM NAND in the cLK menu, which is why `start.sh`
presses it for you; in Android it is the Call key as usual.

The mouse is the touchscreen. Headless: `tools/keys.py` and `tools/touch.py` send keys, taps and swipes over QMP.

<a name="known-booting"></a>
<h2><img src="docs/holo/h-known-booting.png" width="170" height="40" alt="Known booting"></h2>

| OS | Boots with | Guide |
|---|---|---|
| Android 4.0.4 (NexusHD2-ICS-CM9-HWA v2.8) | cLK 1.4 from NAND | [docs/android.md](docs/android.md) |
| Windows Mobile 6.5 (HTC stock ROM 3.14.405.2) | the stock SPL, on the real baseband | [docs/windows-mobile.md](docs/windows-mobile.md) |
| iOS 7 (HD2 port) | MAGLDR from the microSD | [docs/ios.md](docs/ios.md) |

<a name="what-works"></a>
<h2><img src="docs/holo/h-what-works.png" width="143" height="40" alt="What works"></h2>

| Block | Emulation |
|---|---|
| CPU | Scorpion as a Cortex-A8 with Qualcomm's MIDR and CP15 registers |
| Core SoC | VIC/SIRC/GPIO interrupts, timers, clocks, NAND + ADM DMA, SDCC microSD |
| Display | MDP 3.1 scanout and PPP blitter |
| GPU | Adreno 200 on the host GPU or the CPU ([below](#gpu-adreno-200)) |
| Modem | HLE: modelled AMSS services (SMEM, proc_comm, SMD, RPC) by default; or LLE: the real AMSS firmware ([below](#real-baseband-firmware)) |
| Cellular | T-Mobile US SIM, UMTS registration, data through QEMU's network |
| WiFi | BCM4329; connect to the open network **HD2-QEMU** for internet |
| Bluetooth / FM | BCM4329 HCI (adapter works, no radio links); FM tuner with test tones or `-global bcm4329-bt.fm-stations="MHZ=SOURCE;…"` |
| GPS | 1 Hz fixes; `qom-set /machine/modem gps-fix LAT,LON,ALT` moves the phone |
| Audio | QDSP6 playback to the QEMU audio backend |
| Battery / USB | 1230 mAh gauge that charges and drains; `qom-set /machine cable usb\|ac\|none`, `qom-set /machine battery-capacity N` |
| Input | Touchscreen, key matrix, power key, G-sensor, light sensor, compass |
| USB | HSUSB device controller; adb is bridged to TCP, or handed to the host's own stack ([below](#usb-on-the-host-stack)) |

Not emulated: camera, audio capture, Bluetooth pairing/connections.

The `qom-set` commands go through `python3 tools/qmon.py images/qmp.sock "..."`.

<a name="real-baseband-firmware"></a>
<h2><img src="docs/holo/h-real-baseband-firmware.png" width="247" height="40" alt="Real baseband firmware"></h2>

`baseband=lle` runs the radio ROM's AMSS on an emulated ARM926 next to the apps core. The apps side boots on it and the SIM works. The modem DSP (mDSP) is modelled at its command interface, with a GSM cell and network behind it (MCC 310, MNC 260, ARFCN 133; `hw/arm/htcleo/gsm_net.c`): AMSS scans, camps and registers, calls and texts work both ways, and mobile data runs over GPRS (Android's data connection, to the host's user-mode network as with `-nic user`). Calls have no speech yet. GPRS uses CS-2 on 4 downlink timeslots by default, about 40 kbit/s down as on a typical network; `-global msm-mdsp.gprs-cs=4` (CS-1 to CS-4) and `gprs-slots=1` to `4` change it, also at run time with `qom-set`. The network's other side is driven over QMP: `qom-set /machine/modem-lle/mdsp sms "+15551234:text"`, `... call +15551234` (reads back the call's state), `... hangup on`, and `qom-get ... events` lists what the phone sent:

```bash
cd images
python3 ../tools/hd2_radio.py RUU_signed.nbh -o amss.elf          # AMSS for the modem core
cp nand.img nand-lle.img                                         # its own NAND: + radio partitions
python3 ../tools/hd2_radio.py RUU_signed.nbh --write-nand nand-lle.img
cd .. && tools/start-lle.sh
```

That is `tools/start.sh` with `BASEBAND=lle` and `NAND=nand-lle.img`, so
`BASEBAND=lle NAND=nand-lle.img tools/start.sh` is the same thing. Both files
have to be in `images/` — `hd2_radio.py` writes into the current directory, so run
it from `images/`, or `amss.elf` lands in the repo root and QEMU is left without
its firmware.

The secure boot stages are skipped and their hand-off to AMSS is reproduced.
Debugging: `AMSSLOG=1` puts AMSS's debug log in `images/qemu.log` (it needs
`contrib/plugins/libamsslog.so`, see [Build](#build)), `TRACE=on` logs the modem's
device accesses, fatal errors go to `images/uart1.log`, and `tools/amss_*.py` help
with disassembly.

<a name="qdsp6-firmware"></a>
<h3><img src="docs/holo/h-qdsp6-firmware.png" width="182" height="40" alt="QDSP6 firmware"></h3>

On top of LLE, `tools/start-q6.sh` also runs the radio's DSP1 image on the Hexagon
QDSP6 in a second process (`qemu-system-hexagon -M htcleo-q6`, sharing the DRAM
through a file and talking over a socket). It is `start-lle.sh` plus that process,
so it needs `amss.elf`, `nand-lle.img` **and** `dsp1.elf`, all in `images/`:

```bash
cd images
python3 ../tools/hd2_radio.py RUU_signed.nbh -o amss.elf --dsp dsp1.elf
cd .. && tools/start-q6.sh
```

Audio and the hardware H.264 decoder / MPEG-4 encoder then run on the DSP (the encoder codes P pictures without motion vectors by default). `Q6_AUDIO=none` mutes it; `Q6_MACH=vdc-dump=DIR` saves codec command buffers. `tools/boot-q6.sh` boots this mode for scripts and reports hangs.

Known problems of the real-firmware modes (boot hangs and their fix, slow boots after an unclean stop, DSP speed, the experimental motion search, the GSM network's limits) are in [quirks.md](quirks.md).

<a name="usb-on-the-host-stack"></a>
<h3>USB on the host's own stack</h3>

By default the USB gadget is joined to a **built-in** host inside the emulator: it enumerates the phone itself and pipes adb to `127.0.0.1:5555`. With **LLE** the host is this machine instead — the gadget is plugged into `vhci_hcd`, the kernel's virtual host controller, and the host's real USB stack enumerates it, so the emulated HD2 turns up in `lsusb` and `dmesg` and the host's own `adb` and `fastboot` talk to it with nothing in between:

```bash
sudo modprobe vhci_hcd
tools/start-usb-lle.sh          # comes back through sudo itself
fastboot devices                # ????????????  fastboot
fastboot -s usb:5-2 getvar product
fastboot -s usb:5-2 flash recovery recovery.img
```

It needs **root**: `vhci_hcd`'s `attach` attribute is root-only *and* the kernel resolves the socket fd against the process that writes it, so QEMU has to own the socket and write the file itself — `sudo sh -c 'echo ... > attach'` cannot work, which is why the wrapper re-executes itself instead. Only one emulated phone can hold a port; if a killed run still owns it, `sudo rmmod vhci_hcd && sudo modprobe vhci_hcd` releases it.

From QEMU it is the same switch as everything else, `usb-host=lle`, and `USB=lle tools/start.sh` is what the wrapper runs. `QEMU_D=unimp,guest_errors,trace tools/start.sh` logs the USB/IP traffic to `images/qemu.log` under `msm-usbip:` — every setup packet in, every URB out, which is the quickest way to see what the host and the gadget each think is going on.

Without `vhci_hcd` — on a phone, where it is not in the kernel — the same
USB/IP stream can go to a bridge over a unix socket instead, with
`usb-ip-socket=PATH`: QEMU listens there and the far end owns a host
controller of its own. That is how the Android app puts the emulated HD2 on the
phone's own USB port, so a PC plugged into the phone sees an HD2 and its
`fastboot` and `adb` talk to it (`android/README.md`, **Root and USB**;
`docs/usb-passthrough.md` for how it works). Nothing else changes: the gadget
side is the same `htcleo_usbip.c` either way.

<a name="gpu-adreno-200"></a>
<h2><img src="docs/holo/h-gpu-adreno-200.png" width="196" height="40" alt="GPU (Adreno 200)"></h2>

The guest's own graphics driver runs on an emulated Adreno 200: command processor, GPU MMU, a2xx shaders, binning into GMEM, and textures including ETC1, ATC, DXT and mipmaps.

Three renderers (`-global msm-adreno.backend=gl|gles|soft`):
- **`gl`** (default) uses the host GPU through OpenGL 4.5, Xenia-style: shaders are translated to GLSL, and GMEM is a GPU buffer the shaders update under fragment shader interlock. Needs `GL_ARB_fragment_shader_interlock` (NVIDIA, recent Intel); otherwise QEMU falls back to `soft`.
- **`gles`** is for a host with no desktop OpenGL - a phone, which gives us GLES 3.2 and nothing more. It brings up an EGL context from QEMU's own process and checks the driver for image atomics (GMEM's read-modify-write) and framebuffer fetch (in place of interlock); the log says what it found. The ES renderer itself is not written yet, so `gles` draws in software after probing - see [android/README.md](android/README.md#opengl-es-on-the-phones-gpu).
- **`soft`** interprets everything on the CPU (`-global msm-adreno.threads=N`).

On an RTX 3080, `gl` draws 8 full-screen blended layers in ~5 ms (`soft`: ~150 ms) and an 18K-triangle mesh in ~3 ms (`soft`: ~49 ms).

`tools/gltri.c` and `tools/gltest.c` are GLES2 test programs. For debugging: `qom-set /machine/gpu trace FILE`, `ADRENO_STATS=SECONDS`, `ADRENO_GL_DEBUG=1`.

<a name="tools"></a>
<h2><img src="docs/holo/h-tools.png" width="90" height="40" alt="Tools"></h2>

Every script and helper has its own entry in the [tools reference](docs/tools.md).

| Tool | Purpose |
|---|---|
| `hd2_mknand.py`, `hd2_wmnand.py`, `hd2_mkmagldr.py`, `hd2_setmac.py`, `mksdcard.sh`, `fat_put.py` | NAND and SD images |
| `start.sh`, `start-lle.sh`, `start-q6.sh`, `start-wm.sh`, `ios-boot.sh` | Launch |
| `qmon.py`, `keys.py`, `touch.py`, `magnav.py` | Monitor commands, input, MAGLDR's menus |
| `deploy-web.sh` | Install the web UI on a server (systemd, nginx, TLS) |
| `build-linux.sh` | Configure and build the host QEMU (the emulator) |
| `capture.py`, `waitscreen.py`, `waitlog.sh` | Screendumps, logs |
| `yaffs_check.py`, `logcat.sh` | Read a NAND image's files, guest logs from RAM |
| `hd2_radio.py`, `amss_*.py` | Radio firmware |
| `ios7_initrd.py` | Put an XNU kernel into the iOS 7 package |
| `gltri.c`, `gltest.c` | GPU tests |
| `hd2bench/` | Benchmark app to compare a real HD2 with the emulator |

<a name="credits"></a>
<h2><img src="docs/holo/h-credits.png" width="104" height="40" alt="Credits"></h2>

The Adreno model builds on [freedreno](https://docs.mesa3d.org/drivers/freedreno.html) (a2xx registers, packets, shader formats), [Xenia](https://xenia.jp) (Xenos shader semantics, texture tiling, and the host-GPU rendering approach) and [texture2ddecoder](https://github.com/UniversalGameExtraction/texture2ddecoder) (ATC).

The phone in `docs/hd2.png` and the web UI is derived from ["HTC HD2 - Front.jpg"](https://commons.wikimedia.org/wiki/File:HTC_HD2_-_Front.jpg) by Beamish4, [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
