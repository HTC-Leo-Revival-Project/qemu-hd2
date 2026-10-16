#!/bin/sh
# Build the QEMU binary, strip it, and put it where the app and the headless
# runner will both use it.
#
# The phone runs files/emu/bin/qemu-arm, the app's own copy of the binary from
# the APK's jniLibs.  Nothing but starting the app refreshes that, so a
# headless run will happily test an old build - which it did, for hours.  So
# this also pushes the build there, and checks the result really is this build
# rather than trusting the copy.
#
# usage: phone-deploy.sh
set -e
cd "$(dirname "$0")/.."
REPO=$(pwd)
LLVM=${LLVM:-/home/j0sh1x/Android/Sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin}
SO=android/app/src/main/jniLibs/arm64-v8a/libhtcleo-qemu.so
PKG=org.htcleorevivalproject.hd2emu

ninja -C android/build/arm64-v8a/qemu -j"$(nproc)" qemu-system-arm 2>&1 | tail -1
cp -f android/build/arm64-v8a/qemu/qemu-system-arm "$SO"
"$LLVM/llvm-strip" --strip-all "$SO"
chmod 755 "$SO"
echo "built and stripped $(stat -c %s "$SO") bytes, $(date +%T)"

# Compare the bytes, not the size: stripping leaves the size nearly
# unchanged, so a stale copy can look current.
S=$(adb devices | awk '$2 == "device" { print $1; exit }' 2>/dev/null || true)
[ -n "$S" ] || { echo "no phone over adb: built only"; exit 0; }

# The copy cannot be replaced while it is running.
adb -s "$S" shell "su -c 'for p in \$(ps -A | grep -E \"qemu-arm|qemu-q6\" | tr -s \" \" | cut -d\" \" -f2); do kill -9 \$p 2>/dev/null; done'"
sleep 2
adb -s "$S" push "$SO" /data/local/tmp/qemu-arm.new >/dev/null
adb -s "$S" shell "su -c 'cp /data/local/tmp/qemu-arm.new /data/user/0/$PKG/files/emu/bin/qemu-arm; chmod 755 /data/user/0/$PKG/files/emu/bin/qemu-arm'"

want=$(md5sum "$SO" | cut -d' ' -f1)
got=$(adb -s "$S" shell "su -c 'md5sum /data/user/0/$PKG/files/emu/bin/qemu-arm'" | tr -d '\r' | cut -d' ' -f1)
if [ "$want" = "$got" ]; then
    echo "on-device qemu-arm: current ($want)"
else
    echo "on-device qemu-arm: STILL STALE (want $want, got $got)"
    echo "headless runs would test an old build"
    exit 1
fi

# The app APK carries its own copy of the same binary; rebuild it if asked.
if [ "$PHONE_DEPLOY_APK" = 1 ]; then
    (cd android && JAVA_HOME=${JAVA_HOME:-/home/j0sh1x/toolchain/jdk} ./gradlew assembleDebug) | tail -1
    adb -s "$S" install -r android/app/build/outputs/apk/debug/app-debug.apk | tail -1
fi