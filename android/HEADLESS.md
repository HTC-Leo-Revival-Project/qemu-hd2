# Debugging the emulator headlessly on the phone

Everything here runs the emulator **without the app and without a screen**:
QEMU is started as a child process on the phone, and the workstation gets at
it over `adb`.  This is how to iterate on the renderer, because a boot is
unattended and every observation comes back as text or a file.

It was not always possible and it was not obvious.  The three things that took
the longest are written up first, because each one cost hours of measurements
that were quietly worthless.

* [Three traps](#three-traps-that-cost-hours)
* [Running a boot](#running-a-boot)
* [Reading what comes back](#reading-what-comes-back)
* [Renderer tracing](#renderer-tracing)
* [Timing](#timing)
* [What is where](#what-is-where)

## Three traps that cost hours

### 1. A headless run will happily test an old build

The app runs `files/emu/bin/qemu-arm`, a copy it makes from the APK.  Nothing
but **starting the app** refreshes that copy - headless runs never start the
app - so for hours every headless run was testing a build from that morning.
The symptom is the worst kind: results that *change* when you are not looking
for them to, and a diagnostic that quietly never fires.  An env-gated trace
line added to fix one thing went unused for several runs purely because the
binary on the phone did not have it.

`tools/phone-deploy.sh` now builds, strips, pushes, and compares **md5 sums**
of what it pushed against what is on the phone, and fails loudly if they
differ.  It runs first in `tools/phone-boot.sh`, so a headless run cannot
silently use the wrong binary.  Compare bytes, not size: stripping leaves the
size nearly unchanged, so a stale copy can pass a size check.

Related: the app's own APK carries a second copy of the same binary, so
`PHONE_DEPLOY_APK=1 tools/phone-deploy.sh` also rebuilds and reinstalls it.

### 2. The bootloader waits for a key press, and nothing tells you

cCLK sits at a menu and boots nothing until it is pressed.  With no key the
guest never starts: `ramcon.log` stays at 0 bytes, the guest's PC parks at
`0x28007b92`, and no amount of waiting helps.  That looks exactly like a hang,
and `ramcon` being empty looks like "the kernel never booted", which sends you
looking at clocks and memory instead of at the bootloader.

The app sends F2 for this from `startBootKeys()`; `tools/phone-keys.py
--boot` does the same, and - like the app - **stops as soon as the guest
kernel is printing**, because F2 is a real key in Android and a `--boot` that
keeps pressing will drive the UI (it once walked the launcher into the
dialer).  A guest that needs this again after a reset does: `system_reset`
puts it back at the menu.

### 3. Only the real baseband boots

Always `baseband=lle`, with `amss.elf` and the radio NAND.  The hle stub does
not bring the guest up.  `tools/glesrun.sh` hardcodes it and starts the QDSP6
as a second QEMU, waiting for `q6link.sock` to appear first - started
together, the DSP has no link and the guest's audio driver faults on it.

The Q6's `-M` line carries `modem-irq=2`.  That number is load-bearing and
easy to "tidy" by accident; getting it wrong crashes the kernel.

## Running a boot

```sh
tools/phone-boot.sh gles            # renderer: gles (default) or soft
tools/phone-boot.sh gles trace      # and log every draw to draw.log
```

It deploys the current build first, kills anything left over, forwards the
monitor ports, and starts QEMU under `run-as` so it has the app's uid.  Then
wait about four minutes.  Nothing needs touching.

Useful afterwards:

```sh
tools/phone_qmp.py "info registers"        # is the guest alive, and where
tools/phone_qmp.py "xp /8wx 0x28008050"    # what is at an address
tools/phone-shot.py name --qmp             # guest framebuffer, native 480x800
tools/phone-keys.py home                   # press a key
```

`phone-qmp.py` is a QMP client over `adb forward`.  The monitor's unix socket
is inside the app's private data and mode 0700, which adbd (running as the
shell user) cannot open - so `glesrun.sh` gives QEMU a **second monitor on
127.0.0.1:4444**, and that is the only way in.  Anything the monitor can do
works: registers, memory, `system_reset`, `screendump`.

### Screenshots

`--qmp` is the one to use: the guest's own framebuffer, native 480x800, exact
pixels.  `--app` taps the screenshot button in the app's title bar, which is
also native and works while the app runs in the GUI.  `--phone` grabs the
phone's screen, which shows the emulator window *scaled* with the title bar
included - useful for seeing what a person sees, useless for pixels.

## Reading what comes back

Two logs, and they are not interchangeable:

| file | what |
|---|---|
| `files/emu/run/ramcon.log` | the guest's kernel console. Boot progress. |
| `files/emu/run/uart1` | the other serial. Usually empty. |
| `files/emu/gemu.out` | QEMU's own log: renderer, warnings, the Adreno stats. |
| `files/emu/run/qemu.log` | device-level trace (`-d unimp,guest_errors`); megabytes. |
| `files/emu/run/q6.log` | the DSP's. |
| `files/emu/run/draw.log` | the draw trace, if `trace` was asked for. |

Read them with `run-as`, since they are in the app's data:

```sh
adb shell "run-as org.htcleorevivalproject.hd2emu tail -c 200 \
    files/emu/run/ramcon.log"
```

**The guest's uptime is the boot clock.**  `ramcon`'s `[ 282.680694]` is
seconds since boot; the `batt:` lines are the guest noticing its battery.  A
large number there is a guest that booted long ago and is idling - not a
year, and not a wrong clock.

Boot order, so "which stage is it at" is answerable:

* nothing at all - still in the bootloader, needs F2
* `SPL hand-off: loaded OS image from NAND` - the bootloader took over
* `yaffs: Attempting MTD mount` - mounting /data; this is where the
  clock-regime livelock used to wedge the boot
* `Release 'boot-time' halt_lock` - userspace starting
* `batt:` lines with a growing uptime - Android is up

### The renderer, from the log

```
msm-adreno: GLES 3 backend on Adreno (TM) 830 (EGL 1.5, OpenGL ES 3.2 ...)
msm-adreno:   smoke test: GMEM storage buffer gave 000000c0 000000c0 ... (want 000000c0 each)
msm-adreno:   smoke test: OK, the host GPU runs the translated shaders and writes GMEM, with atomics
msm-adreno: rendering on this host GPU with OpenGL ES
msm-adreno: the guest's draws are running on this host's GPU through OpenGL ES
```

If `GLES 3 backend` is missing, the guest never asked for the host GPU and
everything is on the CPU; check `-global msm-adreno.backend` really was
`gles`.  If the smoke test does not say `OK`, the host cannot run the backend
and the draws go to the CPU - it says which requirement was missing.

`adreno: <kicks> kicks, draws <n> gpu <n> ...` is **per interval**, not
cumulative: the counters are zeroed after each line.  Identical numbers on
consecutive lines mean the same thing every 5 seconds, not that nothing is
happening.

## Renderer tracing

`ADRENO_TRACE` turns on the existing draw-and-vertex trace; `phone-boot.sh
gles trace` sets it.  It exists as an env var *because on a phone there is no
monitor to reach* - `qom-set /machine/gpu trace FILE` is unreachable over adb.

`ADRENO_TRACE_ALL=1` is the part that matters.  Without it the trace covers
only the **next 2000 draws'** vertices, which on a boot is the boot animation
and never the launcher.  With it, every draw is traced - 20 MB a minute or so,
so pull a slice rather than the lot:

```sh
adb shell "run-as org.htcleorevivalproject.hd2emu head -c 20000000 \
    files/emu/run/draw.log" > draw.log
```

A draw looks like this, and the `vtx` lines under it are its vertices:

```
  draw #2 prim 8 src 0 n 3 mode 4 color 00000005 surf 00008020 ...
    vtx 0.00,0.00,1.000 rhw 1.000  i0 0.000 0.000 0.000 0.000
```

**The trace is blind to the GPU path.**  A draw the host GPU takes returns
before the vertices are traced, so most draws have none - and `prim 8`
(rectlist) mostly does not.  Geometry that only the GPU draws has to be looked
for where it is built, in `gl_emit()`.  This cost a long wrong turn.

## Timing

`ADRENO_STATS=5` (always on here) prints a breakdown every 5 seconds:

```
adreno: 24 kicks, draws 1625 gpu 117 ..., 92 resolves; ms: cp 59.0,
        gmem sync 0.0, resolve 3.8, textures 0.0, vertex upload 0.0, GPU span 0.0
```

`cp` is the command processor, `resolve` the CPU-side copy out of GMEM, `gpu`
time inside the host GPU.  A per-draw breakdown follows, counting what the
draws were (to memory, with a sample grid, blending, no colour write, dropped
for an empty clip) - identical numbers whichever backend ran them, which is
what makes it comparable between `soft` and `gles`.

## What is where

Under `/data/user/0/org.htcleorevivalproject.hd2emu/files`:

```
emu/bin/qemu-arm      the binary a headless run uses (see trap 1)
emu/images/           nand-lle.img, sdcard.img, amss.elf, dsp1.elf
emu/run/              sockets, logs, dram.bin (1 GB, recreated every boot)
emu/run/dram.bin      the guest's RAM, shared with the Q6 through mem-path
gemu.out              QEMU's console, when started by phone-boot.sh
```

The phone is reached over wifi adb, and **its serial changes**: `host:port`
normally, but an mDNS name like `adb-XXXXXX._adb-tls-connect._tcp` after it
changes networks.  Tools here take the first `device` in `adb devices`, so
they do not care; a hand-written `adb -s` line does.