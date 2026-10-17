# Building

Three things get built from this tree: the **emulator** (QEMU, on Linux), the
**Android app** (QEMU cross-compiled plus the Java UI), and the **web UI**
(Node). They are independent; build only what you need.

Everything here expects to be run from the repository root.

## The emulator, on Linux

QEMU 11 needs a C toolchain, a `meson`/`ninja` recent enough for it
(`meson >= 1.5`, which Ubuntu's package is not — install it with
`pip install --user meson`), and:

```bash
sudo apt install libslirp-dev libpixman-1-dev flex bison
#                 slirp = -nic user, pixman = VNC, flex/bison = the Hexagon idef-parser
```

Then one script configures and builds it:

```bash
tools/build-linux.sh                 # ~10 min the first time
tools/build-linux.sh --clean         # configure from scratch and rebuild
tools/build-linux.sh -j8             # pick the parallelism
```

It builds the two targets the machine needs — `arm-softmmu` (the apps core) and
`hexagon-softmmu` (the QDSP6) — plus the `amsslog` plugin, with the VNC server
the [web UI](../web/README.md) and audio use. The result is
`src/qemu/build/qemu-system-arm` and `src/qemu/build/qemu-system-hexagon`.

On a fresh checkout the first `configure` also fetches QEMU's subprojects —
`keycodemapdb`, `dtc`, `berkeley-softfloat-3` and friends. They are git-ignored
and not committed, and QEMU pulls them in as meson wraps.

That is exactly the README's command, so doing it by hand works too:

```bash
mkdir -p src/qemu/build && cd src/qemu/build
../configure --target-list=arm-softmmu,hexagon-softmmu --enable-sdl --enable-vnc \
    --disable-docs --disable-werror --disable-gtk --disable-xen --disable-kvm \
    --disable-tools --disable-guest-agent
ninja qemu-system-arm qemu-system-hexagon
ninja contrib/plugins/libamsslog.so      # only for AMSSLOG=1
```

QEMU is **never `ninja install`ed** here. The launchers pass
`-L ../src/qemu/pc-bios` so the VNC server finds its keymaps; if you run
`qemu-system-arm` by hand, pass it too, or VNC dies with
`could not find keymap file for language 'en-us'`.

**VNC is always built** (`--enable-vnc`): the web UI's picture and sound, and
the Android app's, both come through it. There is no build without it.

## The Android app

Three steps, all resumable. The first fetches a JDK, Gradle, the Android SDK
and the NDK (~7 GB); the second cross-compiles QEMU with the NDK (~15 min); the
third makes the APK.

```bash
# 1. the toolchain
nohup android/scripts/fetch-toolchain.sh > /tmp/fetch.log 2>&1 &

# 2. QEMU for Android, into app/src/main/jniLibs/arm64-v8a/
android/scripts/build-native.sh                 # arm64-v8a (the only ABI)
android/scripts/build-native.sh --clean         # start over
android/scripts/build-native.sh --stage-only    # skip building, just install

# 3. the APK
cd android && JAVA_HOME=~/toolchain/jdk ./gradlew assembleDebug
# -> android/app/build/outputs/apk/debug/app-debug.apk
```

`build-native.sh` downloads and builds zlib, pcre2, glib, pixman, libslirp and
libfdt into `android/build/prefix/<abi>` (all static), then QEMU itself, then
stages the stripped `qemu-system-arm`, `qemu-system-hexagon` and the `usbffs`
bridge into `jniLibs` under the names the app looks for. It needs
`ANDROID_SDK_ROOT`/`ANDROID_HOME` (default `~/Android/Sdk`) and the NDK; see the
script's header for the exact versions.

The cross-compile runs on the host, so it needs the same build tools as the
Linux build above, plus `meson` for glib, pixman, libslirp and dtc and
`python3` with the `packaging` module (glib's meson refuses without it):

```bash
sudo apt install meson ninja-build pkg-config flex bison \
    libglib2.0-dev python3-packaging
```

`libglib2.0-dev` is the *build machine's* glib, which is separate from the glib
this script cross-compiles into the prefix: QEMU's Hexagon target builds a
native `idef-parser` against it (`dependency('glib-2.0', native: true)`), and
without it QEMU's configure fails with `Dependency "glib-2.0" not found`.

**arm64-v8a only.** QEMU 11 has no 32-bit host TCG backend left, so there is no
`armeabi-v7a` emulator to package. `android/README.md` explains why; the app's
`minSdk` is 15 but the emulator binary itself needs API 28 (bionic's
`<iconv.h>`, which glib requires).

## The web UI

It needs **Node >= 22.18** (the server runs its TypeScript with Node's own
stripping, with no compile step):

```bash
cd web
npm install
npm run dev                  # http://127.0.0.1:3000
npm run build && npm start   # production
npm run typecheck && npm run lint   # what CI checks before the build
```

At runtime it drives QEMU (which is always built with VNC, above).
[web/README.md](../web/README.md) is the full story; `tools/deploy-web.sh`
installs it on a server, and CI builds and packages it as the `hd2-web-ui`
artifact (see below).

## Continuous integration

`.github/workflows/` builds the project on GitHub:

| Workflow | What it does |
|---|---|
| [build.yml](../.github/workflows/build.yml) | On every push and pull request: `lint` (shell/Python/YAML), `linux` (the emulator), `web` (the web UI) and `android` (the APK). |

Each job uploads what it made, unzipped (actions/upload-artifact v7 with
`archive: false`): `htcleo-linux-x86_64.tar.gz` (the two emulator binaries),
`hd2-web-ui.tar.gz` (the web UI, built) and `htcleo-debug.apk` (the app).
Uploading a tarball also keeps the executable bit, which a zipped artifact
does not (everything comes back 644).  `act`'s artifact server predates that
API, so a local run skips the three uploads (and checks what was packaged
instead); on GitHub they are attached to the run.

It can be run locally with [nektos/act](https://github.com/nektos/act)
instead of pushing to GitHub, which is how the workflow file itself is checked:

```bash
sudo apt install docker.io                     # act runs each job in a container
sudo usermod -aG docker "$USER"                # so act can reach the daemon
# act itself: https://github.com/nektos/act#installation

# act copies the workspace into its container by default and honours
# .gitignore, so the ignored images/, build directories and node_modules stay
# out and your checkout is left untouched.  The runner image is a fuller one
# than act's default, and the artifact server is what makes
# actions/upload-artifact work locally.
ACT="act -P ubuntu-latest=catthehacker/ubuntu:act-latest --artifact-server-path /tmp/artifacts"

$ACT --list                                    # what jobs exist
$ACT -j lint                                   # shell/python/YAML checks (seconds)
$ACT -j linux                                  # the Linux emulator
$ACT -j web                                    # the web UI
$ACT -j android                                # the APK
```

`--bind` is the alternative to the copy: it mounts the tree into the container
instead. It is faster on repeat runs, but the build output (QEMU's
`src/qemu/build/`, `node_modules/`, the APK) lands back in your checkout.

The Android job downloads the NDK and the SDK, so a local `act -j android` is
several gigabytes; `act -j linux` is the quick one.
