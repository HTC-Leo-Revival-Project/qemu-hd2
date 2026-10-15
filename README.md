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
  <a href="#what-works"><img src="docs/holo/n-what-works.png" width="101" height="30" alt="What works"></a>
  <a href="#real-baseband-firmware"><img src="docs/holo/n-real-baseband-firmware.png" width="90" height="30" alt="Baseband"></a>
  <a href="#gpu-adreno-200"><img src="docs/holo/n-gpu-adreno-200.png" width="55" height="30" alt="GPU"></a>
  <a href="#tools"><img src="docs/holo/n-tools.png" width="62" height="30" alt="Tools"></a>
</p>

> [!WARNING]
> **Vibecoded.** This project was written FULLY by an AI coding agent (Claude), with little (actually 0) human review. Expect bullshit, don't ask me about this (i really don't know how it works either).

A QEMU machine (`-M htcleo`) that emulates the HTC HD2 (Qualcomm QSD8250) hardware. It boots whatever is in its NAND, unmodified, the way the phone would.

<a name="build"></a>
<h2><img src="docs/holo/h-build.png" width="86" height="40" alt="Build"></h2>

```bash
sudo apt install libslirp-dev libpixman-1-dev   # slirp = -nic user, pixman = VNC
mkdir -p src/qemu/build && cd src/qemu/build
../configure --target-list=arm-softmmu,hexagon-softmmu --enable-sdl \
    --disable-docs --disable-werror --disable-gtk --disable-xen --disable-kvm \
    --disable-tools --disable-guest-agent
ninja qemu-system-arm qemu-system-hexagon
```

The machine is in `src/qemu/hw/arm/htcleo/`. Two more builds, both optional:

```bash
meson configure . -Dvnc=enabled && ninja     # VNC server: only the web UI needs it
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

`LEOIMG.nbh` is the cLK bootloader (`--lk` takes the raw `lk.bin` instead).
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

<a name="run"></a>
<h2><img src="docs/holo/h-run.png" width="76" height="40" alt="Run"></h2>

It is always `tools/start.sh` with some environment variables and extra QEMU
arguments; it leaves QEMU running in the background and sends F2 a moment later
to pick BOOT FROM NAND in the cLK menu.

| What you want | Command | Needs |
|---|---|---|
| **Headless**, no UI (HLE, the default) | `tools/start.sh` | – |
| **An SDL window** | `tools/start.sh -display sdl` | – |
| **The browser UI** | `cd web && npm install && npm run dev` | the `-Dvnc=enabled` build |
| **The real radio** (LLE) | `tools/start-lle.sh` | `images/amss.elf`, `images/nand-lle.img` ([below](#real-baseband-firmware)) |
| **LLE + the real DSP** | `tools/start-q6.sh` | `images/dsp1.elf` ([below](#qdsp6-firmware)) |
| **USB on the real host stack** (LLE) | `tools/start-usb-lle.sh` | root, `vhci_hcd` ([below](#usb-on-the-host-stack)) |

Stop it with `python3 tools/qmon.py images/qmp.sock quit`.

**Headless.** `images/sdcard.img` is attached if present, and USB device traffic (e.g. adb) is bridged to 127.0.0.1:5555 (`ADB_PORT`). Sound is off; for a WAV file `AUDIODEV="-audiodev wav,id=snd0,path=out.wav" tools/start.sh` (`pa,id=snd0` for PulseAudio, `pipewire,id=snd0`).

**An SDL window.** Add `-display sdl` — GTK is not built. It opens on the host's `$DISPLAY`, so without one it fails; with sound: `AUDIODEV="-audiodev pa,id=snd0" tools/start.sh -display sdl`.

**The browser UI** draws the screen inside a picture of a real HD2 and adds touch, keys, typing, sound and switches for HLE/LLE, the DSP firmware, the images, the GPU and the audio backend. It starts and stops QEMU itself over the VNC socket, hence the `-Dvnc=enabled` build; `web/README.md` has the details.

To put it on a server, `sudo tools/deploy-web.sh` copies what it needs into `/opt/htcleo-web`, builds it, adds `htcleo-web.service` and, with `--domain NAME`, an nginx site with a certificate. It is not a static site: the server runs QEMU, so the machine needs the emulator's own files, and the UI has **no login** — it can start the emulator with any extra QEMU arguments — so it stays on `127.0.0.1` behind TLS by default.

Whatever follows the script name goes to QEMU, so `tools/start.sh -vnc :1`, `-S` (do not start the CPUs) and `-snapshot` work as usual. `NAND=` picks another image, `CLK_KEYS=` presses no key at all (leave the bootloader alone) and `CLK_KEYS="pgdn pgdn f2"` picks RECOVERY instead.

<a name="releases"></a>
<h3><img src="docs/holo/n-tools.png" width="62" height="30" alt="Releases"></h3>

`tools/release.sh` builds the three things this project ships — the QEMU machine (with VNC and the plugins), the web UI and the NAND images — packs them into `build/release/` and, with `--upload`, puts them in a GitHub release through the `gh` CLI:

```bash
tools/release.sh --clk lk.bin --rom NexusHD2-ICS-CM9-HWA_V3.0b.zip \
    --recovery HD2_TWRP_2.4.0.0-nanddump.img --upload
```

- **sources**: `--clk`, `--rom`, `--radio`, `--recovery` take a path or a URL, and default to public downloads: the ROM (`NexusHD2-ICS-CM9-HWA_V3.0b.zip`), the bootloader (`cLK.nbh`) and the radio all live in [HTC-HD7-Internals](https://github.com/HTC-Leo-Revival-Project/HTC-HD7-Internals/tree/main/roms), so `tools/release.sh` with no arguments builds everything; `--recovery` has no default. A `mega.nz` link works too but needs megatools (`megaget`), a plain `https` one just curl. Which of cLK's two partition schemes the bootloader uses is decided by looking inside it: an NBH wrapping a 1.4 LK (a `PTABLE-MB` table compiled in) is unpacked and handed to `hd2_mknand.py` as `--lk`, because `--nbh` would lay it out the 1.5 way, with a `VPTABLE` the 1.4 bootloader never reads. The radio's download is a zip inside a zip; the script digs the NBH out. A ROM that is not a NAND ROM is rejected before the long build, not after.
- **assets**: `nand.img.zst` and `nand-lle.img.zst` (528 MiB each, mostly erased flash, so zstd gets them to ~135 MiB), `htcleo-web-<tag>.tar.zst` (the tree the web server wants, without the images) and `SHA256SUMS`.
- **CI**: `.github/workflows/release.yml` runs exactly this on a `v*` tag or by hand. Needs no secrets and no configuration — the release is written with the automatic `GITHUB_TOKEN`, and the sources come from the defaults above. `--no-download` reuses what is already in `build/sources`.

To install a bundle: extract `htcleo-web-*.tar.zst` to `/opt/htcleo-web`, unpack the NAND image into its `images/`, and `sudo tools/deploy-web.sh --dest /opt/htcleo-web --skip-copy`.

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

The default modem is **HLE**: QEMU models the AMSS services Android talks to
(SMEM, proc_comm, SMD, RPC), so there is no firmware to install and the radio
finds a network.

**LLE** (`baseband=lle`) instead runs the stock radio's own AMSS on an emulated
ARM926 next to the apps core. The apps side boots on it and the SIM works, but the
radio still finds no network: the modem DSP runs no code, so there is no RF. It
needs the radio ROM from an official HD2 firmware update, `RUU_signed.nbh`:

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

Audio and the hardware H.264 decoder / MPEG-4 encoder then run on the DSP (the encoder's motion search is not modelled). `Q6_AUDIO=none` mutes it; `Q6_MACH=vdc-dump=DIR` saves codec command buffers.

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

| Tool | Purpose |
|---|---|
| `hd2_mknand.py`, `hd2_setmac.py`, `mksdcard.sh` | NAND and SD images |
| `start.sh`, `start-lle.sh`, `start-q6.sh` | Launch |
| `deploy-web.sh` | Install the web UI on a server (systemd, nginx, TLS) |
| `release.sh` | Build everything and put it in a GitHub release |
| `qmon.py`, `keys.py`, `touch.py` | Monitor commands, input |
| `capture.py`, `waitscreen.py`, `waitlog.sh` | Screendumps, logs |
| `yaffs_check.py`, `logcat.sh` | Read a NAND image's files, guest logs from RAM |
| `hd2_radio.py`, `amss_*.py` | Radio firmware |
| `gltri.c`, `gltest.c` | GPU tests |
| `hd2bench/` | Benchmark app to compare a real HD2 with the emulator |

<a name="credits"></a>
<h2><img src="docs/holo/h-credits.png" width="104" height="40" alt="Credits"></h2>

The Adreno model builds on [freedreno](https://docs.mesa3d.org/drivers/freedreno.html) (a2xx registers, packets, shader formats), [Xenia](https://xenia.jp) (Xenos shader semantics, texture tiling, and the host-GPU rendering approach) and [texture2ddecoder](https://github.com/UniversalGameExtraction/texture2ddecoder) (ATC).

The phone in `docs/hd2.png` and the web UI is derived from ["HTC HD2 - Front.jpg"](https://commons.wikimedia.org/wiki/File:HTC_HD2_-_Front.jpg) by Beamish4, [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
