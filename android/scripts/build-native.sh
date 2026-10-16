#!/bin/bash
#
# Cross-compiles QEMU (the htcleo machine) and the few libraries it needs
# for Android, with the NDK, and drops the result into the app's jniLibs.
#
#   android/scripts/build-native.sh                 arm64-v8a (the only ABI)
#   android/scripts/build-native.sh arm64-v8a       the same, explicitly
#   android/scripts/build-native.sh --clean         start over
#   android/scripts/build-native.sh --stage-only    skip building, just install
#
# Everything downloads into android/build/dl and is reused, and every build
# directory is stamped, so this can be killed and re-run as often as you like.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ANDROID_DIR="$ROOT/android"
QEMU_SRC="$ROOT/src/qemu"
BUILD="$ANDROID_DIR/build"
DL="$BUILD/dl"
JLIB="$ANDROID_DIR/app/src/main/jniLibs"
PREFIX_ROOT="$BUILD/prefix"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
NDK_VERSION="27.3.13750724"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/$NDK_VERSION}"
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
# The emulator binary itself targets Android 9.  The app installs and runs on
# Android 4.0.4 (minSdk 15) - the settings, the log viewer and the NAND builder
# all work there - but QEMU needs API 28: that is where bionic starts
# declaring <iconv.h>, which glib insists on, and it is a sane floor anyway.
API=28
JOBS="$(nproc 2>/dev/null || echo 4)"

# QEMU 11 has no 32-bit host backend left - tcg/ ships aarch64, x86_64,
# riscv64, s390x, ppc64, sparc64, loongarch64 and the interpreter, and
# meson.build rejects any host with 4-byte pointers - so an armeabi-v7a build
# of the emulator is not a matter of flags: there is nothing to build it with.
# The app therefore ships arm64-v8a only.
ABIS=("arm64-v8a")

# sources: small, and every one of them a plain tarball
ZLIB_VERSION=1.3.1
PCRE2_VERSION=10.45
GLIB_VERSION=2.80.2
PIXMAN_VERSION=0.46.2
SLIRP_VERSION=4.8.0
DTC_VERSION=1.7.0

CLEAN=0
STAGE_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --stage-only) STAGE_ONLY=1 ;;
        -j*) JOBS="${arg#-j}" ;;
        arm64-v8a) ABIS=("arm64-v8a") ;;
        armeabi-v7a)
            echo "QEMU 11 has no 32-bit host TCG backend (tcg/ has none, and meson.build" >&2
            echo "rejects a 4-byte-pointer host), so the emulator cannot be built for" >&2
            echo "armeabi-v7a.  The app ships arm64-v8a only." >&2
            exit 2 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
note() { printf '   %s\n' "$*"; }

fetch() { # fetch <url> <file>
    local url="$1" file="$DL/$2"
    [ -s "$file" ] && { note "have $2"; return 0; }
    note "downloading $2"
    curl -L -C - --retry 20 --retry-delay 5 --retry-all-errors --connect-timeout 30 -o "$file" "$url"
}

# the meson cross file for one ABI
cross_file() { # cross_file <abi> <prefix>
    local abi="$1" prefix="$2" cc triple cpu family
    case "$abi" in
        arm64-v8a)  triple="aarch64-linux-android$API"; cpu="aarch64"; family="aarch64" ;;
        armeabi-v7a) triple="armv7a-linux-androideabi$API"; cpu="armv7"; family="arm" ;;
        *) echo "unknown abi $abi" >&2; exit 2 ;;
    esac
    cat > "$3" <<EOF
[binaries]
c = '$LLVM/bin/$triple-clang'
cpp = '$LLVM/bin/$triple-clang++'
ar = '$LLVM/bin/llvm-ar'
strip = '$LLVM/bin/llvm-strip'
nm = '$LLVM/bin/llvm-nm'
objcopy = '$LLVM/bin/llvm-objcopy'
readelf = '$LLVM/bin/llvm-readelf'
ld = '$LLVM/bin/ld'
pkg-config = 'pkg-config'

[properties]
# No sys_root here on purpose: the NDK's clang wrappers already know where
# their own sysroot is, and meson would rewrite every absolute -I that comes
# out of a .pc file to be sysroot-relative.
pkg_config_libdir = '$prefix/lib/pkgconfig'
# bionic's own libc, nothing from the build machine
root = '$prefix'

[host_machine]
system = 'android'
kernel = 'linux'
subtype = ''
cpu_family = '$family'
cpu = '$cpu'
endian = 'little'

[built-in options]
c_args = ['-D_FORTIFY_SOURCE=2', '-fno-stack-protector', '-ffunction-sections', '-fdata-sections', '-Wno-unused-command-line-argument']
# a static QEMU with bionic behind it is one file the app can just exec
b_lto = false
b_pie = true
default_library = 'static'
strip = 'true'
# 16 KB page sizes: every LOAD segment has to line up with a 16 KB boundary or
# the loader refuses the binary on a device whose pages are that big
# (https://developer.android.com/16kb-page-size)
c_link_args = ['-Wl,--gc-sections', '-Wl,--as-needed', '-static-libstdc++',
              '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384']
EOF
}

# one dependency: unpack into dl/<name>, then meson or configure into build/<abi>/<name>
unpack() { # unpack <tarball> <dir name>
    local tarball="$DL/$1" dir="$DL/$2"
    [ -d "$dir" ] && return 0
    note "unpacking $1"
    case "$1" in
        *.tar.bz2|*.tar.bz) tar xjf "$tarball" -C "$DL" ;;
        *.tar.xz) tar xJf "$tarball" -C "$DL" ;;
        *) tar xzf "$tarball" -C "$DL" ;;
    esac
    # some tarballs unpack under their own name (libslirp-v4.8.0-<hash>)
    if [ ! -d "$dir" ]; then
        local newest
        newest="$(ls -dt "$DL"/*/ | head -1)"
        [ -n "$newest" ] && mv "$newest" "$dir"
    fi
}

meson_dep() { # meson_dep <abi> <dir name> <prefix> <src dir> <name> <options...>
    local abi="$1" name="$2" prefix="$3" src="$4"; shift 4
    local bdir="$BUILD/$abi/$name"
    [ -f "$bdir/.stamp" ] && { note "have $name ($abi)"; return 0; }
    mkdir -p "$bdir"
    cross_file "$abi" "$prefix" "$bdir/cross.txt"
    PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" \
    meson setup "$bdir" "$src" --cross-file "$bdir/cross.txt" \
        --prefix "$prefix" --libdir lib --buildtype release "$@" >"$bdir/meson.log" 2>&1 ||
        { echo "--- meson setup failed for $name/$abi ---" >&2; tail -40 "$bdir/meson.log" >&2; exit 1; }
    meson compile -C "$bdir" -j "$JOBS" >"$bdir/build.log" 2>&1 ||
        { echo "--- build failed for $name/$abi ---" >&2; tail -40 "$bdir/build.log" >&2; exit 1; }
    meson install -C "$bdir" >"$bdir/install.log" 2>&1 ||
        { echo "--- install failed for $name/$abi ---" >&2; tail -20 "$bdir/install.log" >&2; exit 1; }
    touch "$bdir/.stamp"
    note "built $name ($abi)"
}

# an autotools dependency: pcre2 still ships configure, and it cross compiles fine
configure_dep() { # configure_dep <abi> <dir name> <prefix> <src dir> <host triplet> <options...>
    local abi="$1" name="$2" prefix="$3" src="$4" host="$5"; shift 5
    local bdir="$BUILD/$abi/$name" log="$BUILD/$abi/$name.log"
    [ -f "$bdir/.stamp" ] && { note "have $name ($abi)"; return 0; }
    mkdir -p "$bdir"
    local cc
    case "$abi" in
        arm64-v8a) cc="aarch64-linux-android$API" ;;
        armeabi-v7a) cc="armv7a-linux-androideabi$API" ;;
    esac
    ( cd "$bdir" && CC="$LLVM/bin/$cc-clang" \
        CXX="$LLVM/bin/$cc-clang++" \
        AR="$LLVM/bin/llvm-ar" RANLIB="$LLVM/bin/llvm-ranlib" \
        STRIP="$LLVM/bin/llvm-strip" NM="$LLVM/bin/llvm-nm" \
        "$src/configure" --host="$host" --prefix="$prefix" \
            --enable-static --disable-shared "$@" ) >"$log" 2>&1 ||
        { echo "--- configure failed for $name/$abi ---" >&2; tail -40 "$log" >&2; exit 1; }
    ( cd "$bdir" && make -j "$JOBS" ) >>"$log" 2>&1 ||
        { echo "--- build failed for $name/$abi ---" >&2; tail -40 "$log" >&2; exit 1; }
    ( cd "$bdir" && make install ) >>"$log" 2>&1 ||
        { echo "--- install failed for $name/$abi ---" >&2; tail -20 "$log" >&2; exit 1; }
    touch "$bdir/.stamp"
    note "built $name ($abi)"
}

# glib and pixman need their own bits of the NDK's include tree that pkg-config
# does not know about, and the 32 bit ABI has to say so on every compile
# Two pkg-configs: QEMU's build machine still needs the host's libraries for
# the few tools it builds to run at build time (hexagon's idef-parser), while
# everything it cross compiles must only see the prefix we just built.
pkgconfig_wrappers() { # pkgconfig_wrappers <prefix>
    local prefix="$1"
    mkdir -p "$BUILD/hostbin" "$BUILD/targetbin"
    cat > "$BUILD/hostbin/pkg-config" <<EOF
#!/bin/sh
PKG_CONFIG_LIBDIR=\${PKG_CONFIG_LIBDIR_HOST:-/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:/usr/lib/pkgconfig} \\
exec /usr/bin/pkg-config "\$@"
EOF
    cat > "$BUILD/targetbin/pkg-config" <<EOF
#!/bin/sh
PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig exec /usr/bin/pkg-config "\$@"
EOF
    chmod +x "$BUILD/hostbin/pkg-config" "$BUILD/targetbin/pkg-config"
}

# QEMU's configure does not take a meson cross file: it builds its own out of
# the tools it finds, so the NDK's clang goes in through the environment and
# --cpu tells it which machine this is.  --cross-prefix (empty) is what turns
# on the cross build.
qemu_configure() { # qemu_configure <abi> <prefix>
    local abi="$1" prefix="$2"
    local bdir="$BUILD/$abi/qemu"
    [ -f "$bdir/.stamp" ] && { note "have qemu ($abi)"; return 0; }
    local triple cpu
    case "$abi" in
        arm64-v8a)   triple="aarch64-linux-android$API"; cpu="aarch64" ;;
        armeabi-v7a) triple="armv7a-linux-androideabi$API"; cpu="arm" ;;
    esac
    mkdir -p "$bdir"
    pkgconfig_wrappers "$prefix"
    # Note: downloads are left enabled.  A fresh checkout has no
    # subprojects/keycodemapdb, dtc or berkeley-softfloat-3 (they are
    # git-ignored), so QEMU fetches them as meson wraps here; a dev tree that
    # already built the host emulator has them and nothing is downloaded.
    ( cd "$bdir" && \
      PATH="$BUILD/hostbin:$PATH" \
      CC="$LLVM/bin/$triple-clang" \
      CXX="$LLVM/bin/$triple-clang++" \
      AR="$LLVM/bin/llvm-ar" \
      RANLIB="$LLVM/bin/llvm-ranlib" \
      STRIP="$LLVM/bin/llvm-strip" \
      NM="$LLVM/bin/llvm-nm" \
      OBJCOPY="$LLVM/bin/llvm-objcopy" \
      READELF="$LLVM/bin/llvm-readelf" \
      LD="$LLVM/bin/ld.lld" \
      PKG_CONFIG="$BUILD/targetbin/pkg-config" \
      PKG_CONFIG_PATH= \
      "$QEMU_SRC/configure" \
        --cross-prefix= \
        --cpu="$cpu" \
        --host-cc="${HOST_CC:-gcc}" \
        --prefix="$prefix" \
        --target-list=arm-softmmu,hexagon-softmmu \
        --extra-cflags="-I$prefix/include" \
        --extra-ldflags="-L$prefix/lib -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384" \
        --extra-cxxflags="-I$prefix/include" \
        --enable-vnc --enable-slirp --enable-pixman \
        --disable-opengl --disable-virglrenderer --disable-spice \
        --disable-sdl --disable-gtk --disable-curses \
        --disable-xen --disable-kvm --disable-hvf --disable-nvmm --disable-whpx \
        --disable-tools --disable-guest-agent --disable-docs \
        --disable-cap-ng --disable-libudev --disable-curl --disable-bzip2 \
        --disable-snappy --disable-lzo --disable-zstd --disable-gcrypt \
        --disable-vde --disable-brlapi --disable-rbd --disable-linux-aio \
        --disable-vhost-user-blk-server \
        --disable-vhost-user --disable-vhost-kernel --disable-vhost-net --disable-virtfs \
        --disable-install-blobs --disable-modules \
        --disable-plugins \
        -Dwerror=false -Db_lto=false -Drust=disabled -Ddbus_display=disabled ) >"$bdir/configure.log" 2>&1 ||
        { echo "--- configure failed ---" >&2; tail -60 "$bdir/configure.log" >&2; exit 1; }
    ( cd "$bdir" && ninja -j "$JOBS" ) >"$bdir/build.log" 2>&1 ||
        { echo "--- ninja failed ---" >&2; tail -60 "$bdir/build.log" >&2; exit 1; }
    touch "$bdir/.stamp"
    note "built qemu ($abi)"
}

# glib insists on an iconv dependency.  bionic has had <iconv.h> and
# iconv_open() since API 28 (they live in libc), so an empty .pc is all it
# needs; glib only calls it for charset conversions QEMU never makes.
shim_iconv() {
    mkdir -p "$1/lib/pkgconfig"
    cat > "$1/lib/pkgconfig/iconv.pc" <<EOF
prefix=$1
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: iconv
Description: iconv comes from bionic's libc
Version: 1.0
Libs:
Cflags:
EOF
}

stage() { # stage <abi>
    local abi="$1" prefix="$PREFIX_ROOT/$abi" qdir="$BUILD/$abi/qemu"
    local out="$JLIB/$abi"
    mkdir -p "$out"
    rm -f "$out/qemu-system-arm" "$out/qemu-system-hexagon"
    local installed
    installed="$(find "$prefix/lib" -maxdepth 1 \( -name '*.so' -o -name '*.so.[0-9]*' \) -type f 2>/dev/null | head -20)"
    if [ -n "$installed" ]; then
        # dynamically linked after all: keep the libraries next to the binary
        while IFS= read -r so; do cp -f "$so" "$out/"; done <<< "$installed"
    fi
    # Only lib/<abi>/lib*.so survives into the APK: the installer unpacks
    # nothing else, so the executables go in under those names and
    # Paths.qemu() finds them again.  See Paths.java.
    for pair in "qemu-system-arm:libhtcleo-qemu.so" "qemu-system-hexagon:libhtcleo-q6.so"; do
        local bin="${pair%%:*}" lib="${pair##*:}"
        cp -f "$qdir/$bin" "$out/$lib"
        "$LLVM/bin/llvm-strip" --strip-all "$out/$lib" 2>/dev/null || true
        chmod 755 "$out/$lib"
    done
    # the FunctionFS bridge (android/native/usbffs.c), the same trick: it is not
    # a library, it just has to arrive executable, and Paths.usbffs() looks for
    # exactly this name
    build_bridge "$abi" "$out/libhtcleo-usbffs.so"
    note "staged $(du -sh "$out" | cut -f1) into app/src/main/jniLibs/$abi"
}

# usbffs, for usb-ip-socket: small, plain C, and nothing from the NDK but libc
# and pthreads, so it does not need the QEMU build tree at all.
build_bridge() { # build_bridge <abi> <output>
    local abi="$1" out="$2" dir="$ANDROID_DIR/native"
    local cc
    case "$abi" in
        arm64-v8a)   cc="aarch64-linux-android$API" ;;
        armeabi-v7a) cc="armv7a-linux-androideabi$API" ;;
    esac
    # The same 16 KB page alignment the QEMU build gets (see cross_file() and
    # qemu_configure): every LOAD segment has to line up with a 16 KB boundary
    # or the loader refuses the file on a device whose pages are that big
    # (https://developer.android.com/16kb-page-size).  usbffs is exec'd rather
    # than dlopen'd, but it is unpacked from the APK by the installer into the
    # same directory as the two QEMU binaries, so it holds itself to the same
    # rule rather than being the one odd file out.
    "$LLVM/bin/$cc-clang" -O2 -Wall -Wextra -Wno-unused-parameter -pthread \
        -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
        -o "$out" "$dir/usbffs.c" || {
            echo "--- usbffs failed ---" >&2; exit 1; }
    "$LLVM/bin/llvm-strip" --strip-all "$out" 2>/dev/null || true
    chmod 755 "$out"
    note "built usbffs ($abi)"
}

downloads() {
    mkdir -p "$DL"
    fetch "https://zlib.net/fossils/zlib-$ZLIB_VERSION.tar.gz" "zlib-$ZLIB_VERSION.tar.gz"
    fetch "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$PCRE2_VERSION/pcre2-$PCRE2_VERSION.tar.bz2" "pcre2-$PCRE2_VERSION.tar.bz2"
    fetch "https://download.gnome.org/sources/glib/$(echo $GLIB_VERSION | cut -d. -f1,2)/glib-$GLIB_VERSION.tar.xz" "glib-$GLIB_VERSION.tar.xz"
    fetch "https://www.cairographics.org/releases/pixman-$PIXMAN_VERSION.tar.gz" "pixman-$PIXMAN_VERSION.tar.gz"
    fetch "https://distfiles.macports.org/libslirp/libslirp-$SLIRP_VERSION.tar.bz2" "libslirp-$SLIRP_VERSION.tar.bz2"
    # libfdt, which arm-softmmu needs and bionic of course does not have
    fetch "https://mirrors.edge.kernel.org/pub/software/utils/dtc/dtc-$DTC_VERSION.tar.xz" "dtc-$DTC_VERSION.tar.xz"
}

build_abi() { # build_abi <abi>
    local abi="$1"
    local prefix="$PREFIX_ROOT/$abi"
    local b="$BUILD/$abi"
    say "zlib $ZLIB_VERSION ($abi)"
    if [ ! -f "$b/zlib/.stamp" ]; then
        unpack "zlib-$ZLIB_VERSION.tar.gz" "zlib-$ZLIB_VERSION"
        mkdir -p "$b/zlib" && cross_file "$abi" "$prefix" "$b/zlib/cross.txt"
        ( cd "$b/zlib" && CHOST="$abi" CC="$LLVM/bin/$( [ "$abi" = arm64-v8a ] && echo aarch64-linux-android$API || echo armv7a-linux-androideabi$API)-clang" \
            AR="$LLVM/bin/llvm-ar" RANLIB="$LLVM/bin/llvm-ranlib" \
            "$DL/zlib-$ZLIB_VERSION/configure" --prefix "$prefix" --static ) >"$b/zlib/build.log" 2>&1 &&
        { make -C "$b/zlib" -j "$JOBS" >>"$b/zlib/build.log" 2>&1 && make -C "$b/zlib" install >>"$b/zlib/build.log" 2>&1; } ||
        { echo "--- zlib failed ---" >&2; tail -30 "$b/zlib/build.log" >&2; exit 1; }
        touch "$b/zlib/.stamp"
    else note "have zlib ($abi)"; fi

    say "pcre2 $PCRE2_VERSION ($abi)"
    unpack "pcre2-$PCRE2_VERSION.tar.bz2" "pcre2-$PCRE2_VERSION"
    configure_dep "$abi" pcre2 "$prefix" "$DL/pcre2-$PCRE2_VERSION" \
        "$([ "$abi" = arm64-v8a ] && echo aarch64-linux-android || echo arm-linux-androideabi)" \
        --enable-pcre2-8 --disable-pcre2-16 --disable-pcre2-32 \
        --disable-pcre2grep --disable-pcre2test --enable-jit

    say "glib $GLIB_VERSION ($abi)"
    shim_iconv "$prefix"
    unpack "glib-$GLIB_VERSION.tar.xz" "glib-$GLIB_VERSION"
    meson_dep "$abi" glib "$prefix" "$DL/glib-$GLIB_VERSION" \
        -Ddefault_library=static -Db_staticpic=true -Dnls=disabled \
        -Dselinux=disabled -Dlibmount=disabled -Dman-pages=disabled \
        -Ddocumentation=false -Dtests=false -Dglib_debug=disabled \
        -Dinstalled_tests=false -Dintrospection=disabled \
        -Dglib_assert=false -Dsysprof=disabled -Ddtrace=false

    say "pixman $PIXMAN_VERSION ($abi)"
    unpack "pixman-$PIXMAN_VERSION.tar.gz" "pixman-$PIXMAN_VERSION"
    meson_dep "$abi" pixman "$prefix" "$DL/pixman-$PIXMAN_VERSION" \
        -Ddefault_library=static -Dgtk=disabled -Dlibpng=disabled \
        -Dtests=disabled -Ddemos=disabled -Dtimers=false -Dopenmp=disabled \
        -Dloongson-mmi=disabled -Dmmx=disabled -Dsse2=disabled -Dssse3=disabled \
        -Dvmx=disabled -Dneon=disabled -Darm-simd=disabled -Drvv=disabled \
        -Dtls=enabled

    say "libslirp $SLIRP_VERSION ($abi)"
    unpack "libslirp-$SLIRP_VERSION.tar.bz2" "libslirp-$SLIRP_VERSION"
    meson_dep "$abi" libslirp "$prefix" "$DL/libslirp-$SLIRP_VERSION" \
        -Ddefault_library=static

    say "libfdt ($abi)"
    unpack "dtc-$DTC_VERSION.tar.xz" "dtc-$DTC_VERSION"
    if [ ! -f "$BUILD/$abi/libfdt/.stamp" ]; then
        local cc
        case "$abi" in
            arm64-v8a)   cc="aarch64-linux-android$API" ;;
            armeabi-v7a) cc="armv7a-linux-androideabi$API" ;;
        esac
        mkdir -p "$prefix/lib" "$prefix/include"
        ( cd "$DL/dtc-$DTC_VERSION" && make -j "$JOBS" CC="$LLVM/bin/$cc-clang" \
              AR="$LLVM/bin/llvm-ar" libfdt/libfdt.a ) >"$BUILD/$abi/libfdt.build.log" 2>&1 ||
            { echo "--- libfdt failed ---" >&2; tail -20 "$BUILD/$abi/libfdt.build.log" >&2; exit 1; }
        cp -f "$DL/dtc-$DTC_VERSION/libfdt/libfdt.a" "$prefix/lib/"
        cp -f "$DL/dtc-$DTC_VERSION/libfdt/libfdt.h" \
              "$DL/dtc-$DTC_VERSION/libfdt/libfdt_env.h" \
              "$DL/dtc-$DTC_VERSION/libfdt/fdt.h" "$prefix/include/"
        mkdir -p "$BUILD/$abi/libfdt"
        touch "$BUILD/$abi/libfdt/.stamp"
        note "built libfdt ($abi)"
    else note "have libfdt ($abi)"; fi

    say "qemu ($abi)"
    qemu_configure "$abi" "$prefix"
    stage "$abi"
}

if [ "$CLEAN" = 1 ]; then
    say "cleaning"
    rm -rf "$BUILD" "$JLIB"
    mkdir -p "$JLIB"
fi

say "toolchain"
[ -d "$LLVM" ] || { echo "No NDK at $NDK - run android/scripts/fetch-toolchain.sh first." >&2; exit 1; }
note "NDK $NDK_VERSION"
note "meson $(meson --version), ninja $(ninja --version), $JOBS jobs"

for abi in "${ABIS[@]}"; do
    if [ "$STAGE_ONLY" = 1 ]; then stage "$abi"; else downloads; build_abi "$abi"; fi
done

say "the VNC server wants its keymaps"
PC="$ANDROID_DIR/app/src/main/assets/pc-bios"
mkdir -p "$PC"
if [ ! -d "$PC/keymaps" ]; then
    cp -r "$QEMU_SRC/pc-bios/keymaps" "$PC/keymaps"
fi
note "$(du -sh "$PC" | cut -f1) of keymaps in app/src/main/assets"

say "done"
du -sh "$JLIB"/* 2>/dev/null || true
