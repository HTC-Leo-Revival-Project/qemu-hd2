#!/bin/sh
# Build hd2bench.apk without Gradle: NDK r23 (armeabi-v7a, loadable on
# Android 4.0), javac, d8 (min API 15), aapt2, zipalign, apksigner (v1+v2).
set -e
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
BT=$SDK/build-tools/36.1.0
JAR=$SDK/platforms/android-34/android.jar
NDK=$SDK/ndk/23.1.7779620/toolchains/llvm/prebuilt/linux-x86_64/bin
rm -rf build && mkdir -p build/lib/armeabi-v7a build/classes build/dex
# ICS's linker wants SysV hash tables
$NDK/armv7a-linux-androideabi16-clang -shared -fPIC -O3 -mfpu=neon \
    -mfloat-abi=softfp -Wall -Wl,--hash-style=sysv -Wl,-soname,libbench.so \
    -o build/lib/armeabi-v7a/libbench.so jni/bench.c
javac -nowarn -Xlint:-options -encoding UTF-8 -source 8 -target 8 \
    -bootclasspath "$JAR" -d build/classes src/com/hd2bench/*.java
$BT/d8 --min-api 15 --release --lib "$JAR" --output build/dex \
    $(find build/classes -name '*.class')
$BT/aapt2 link -I "$JAR" --manifest AndroidManifest.xml \
    --min-sdk-version 15 --target-sdk-version 15 -o build/base.apk
(cd build && cp dex/classes.dex . && zip -q base.apk classes.dex \
    lib/armeabi-v7a/libbench.so)
$BT/zipalign -f -p 4 build/base.apk build/aligned.apk
[ -f bench.keystore ] || keytool -genkeypair -keystore bench.keystore \
    -storepass hd2bench -keypass hd2bench -alias hd2bench -keyalg RSA \
    -keysize 2048 -validity 10000 -dname "CN=HD2 Bench" >/dev/null 2>&1
$BT/apksigner sign --ks bench.keystore --ks-pass pass:hd2bench \
    --min-sdk-version 15 --out hd2bench.apk build/aligned.apk
$BT/apksigner verify --min-sdk-version 15 hd2bench.apk
ls -l hd2bench.apk
