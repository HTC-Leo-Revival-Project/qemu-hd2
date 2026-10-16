#!/bin/sh
# Build the host QEMU (the -M htcleo machine) on Linux, the way the README
# describes it: arm-softmmu (the apps core), hexagon-softmmu (the QDSP6) and
# the amsslog plugin, with the VNC server the web UI needs.
#
# usage: tools/build-linux.sh [options]
#   -j N        parallel jobs (default: nproc)
#   --clean     remove the build directory first and configure from scratch
#   --jobs...   any other argument is passed to QEMU's configure
#
# The build lands in src/qemu/build/ (git-ignored).  Configure only runs the
# first time, so re-running this is an incremental `ninja`.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="$ROOT/src/qemu"
BUILD="$QEMU/build"
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
EXTRA=""

while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        -h|--help) sed -n '2,16p' "$0" | cut -c3-; exit 0 ;;
        *) EXTRA="$EXTRA $1"; shift ;;
    esac
done

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }

if [ "$CLEAN" = 1 ]; then
    say "cleaning $BUILD"
    rm -rf "$BUILD"
fi

mkdir -p "$BUILD"
cd "$BUILD"

if [ ! -f build.ninja ]; then
    say "configure"
    # --enable-slirp is the user-mode network, --enable-sdl the optional
    # window; --enable-vnc is the web UI's screen and sound.  The rest keeps
    # the build small: no GTK, no docs, no tools, no guest agent.
    # shellcheck disable=SC2086
    "$QEMU/configure" \
        --target-list=arm-softmmu,hexagon-softmmu \
        --enable-sdl --enable-slirp --enable-vnc \
        --disable-docs --disable-werror --disable-gtk \
        --disable-xen --disable-kvm --disable-tools --disable-guest-agent \
        $EXTRA
else
    say "configure (already done; --clean to redo)"
fi

say "build"
ninja -j "$JOBS" qemu-system-arm qemu-system-hexagon

# The amsslog plugin is what AMSSLOG=1 loads; it is not part of the two
# emulators, so build it separately and tolerate a build that cannot.
say "amsslog plugin"
ninja -j "$JOBS" contrib/plugins/libamsslog.so || \
    echo "  (plugin not built; AMSSLOG=1 will not work)"

say "done"
ls -la qemu-system-arm qemu-system-hexagon
echo
echo "Run one with: tools/start.sh   (see the README's Run section)"
