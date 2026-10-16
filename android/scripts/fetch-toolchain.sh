#!/bin/bash
#
# Installs everything the native build and the APK need, into ~/Android/Sdk
# and ~/toolchain, without root and without a package manager.  Every download
# is resumable (curl -C -) and every step is skipped when it is already done,
# so this is safe to run again on a metered connection as often as you like.
#
#     nohup android/scripts/fetch-toolchain.sh > /tmp/fetch.log 2>&1 &
#     tail -f /tmp/fetch.log
#
set -u
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
DL="$HOME/toolchain/dl"
NDK_VERSION="27.3.13750724"
mkdir -p "$DL" "$SDK" "$HOME/toolchain"

log() { printf '\n=== %s ===\n' "$*"; }

get() { # get <url> <file>
    [ -s "$DL/$2" ] && { echo "have $2 ($(du -h "$DL/$2" | cut -f1))"; return 0; }
    echo "downloading $2"
    curl -L -C - --retry 20 --retry-delay 5 --retry-all-errors --connect-timeout 30 \
         -o "$DL/$2" "$1"
}

log "JDK 17 (only Gradle needs it)"
if [ ! -x "$HOME/toolchain/jdk/bin/java" ]; then
    get "https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse" jdk17.tar.gz &&
        mkdir -p "$HOME/toolchain/jdk" &&
        tar xzf "$DL/jdk17.tar.gz" -C "$HOME/toolchain/jdk" --strip-components=1
fi
"$HOME/toolchain/jdk/bin/java" -version

log "Gradle 8.11.1 (the wrapper can find this one itself, but it saves a download)"
if [ ! -x "$HOME/toolchain/gradle-8.11.1/bin/gradle" ]; then
    get "https://services.gradle.org/distributions/gradle-8.11.1-bin.zip" gradle.zip &&
        ( cd "$HOME/toolchain" && unzip -q -o "$DL/gradle.zip" )
fi

log "Android command line tools"
if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
    get "https://dl.google.com/android/repository/commandlinetools-linux-16111833_latest.zip" cmdline-tools.zip &&
        rm -rf "$DL/cmdx" && mkdir -p "$DL/cmdx" &&
        unzip -q "$DL/cmdline-tools.zip" -d "$DL/cmdx" &&
        mkdir -p "$SDK/cmdline-tools" &&
        rm -rf "$SDK/cmdline-tools/latest" &&
        mv "$DL/cmdx/cmdline-tools" "$SDK/cmdline-tools/latest"
fi

export ANDROID_HOME="$SDK"
export ANDROID_SDK_ROOT="$SDK"
export JAVA_HOME="$HOME/toolchain/jdk"
SDKMANAGER="$SDK/cmdline-tools/latest/bin/sdkmanager"

log "platform 36, build-tools 36, platform-tools"
yes | "$SDKMANAGER" --licenses > /dev/null 2>&1
"$SDKMANAGER" "platform-tools" "platforms;android-36" "build-tools;36.0.0" ||
    "$SDKMANAGER" "platform-tools" "platforms;android-36" "build-tools;36.0.0"

log "NDK r27c (664 MB, the long one)"
if [ ! -d "$SDK/ndk/$NDK_VERSION" ]; then
    get "https://dl.google.com/android/repository/android-ndk-r27c-linux.zip" ndk-r27c.zip &&
        rm -rf "$DL/ndkx" && mkdir -p "$DL/ndkx" &&
        unzip -q "$DL/ndk-r27c.zip" -d "$DL/ndkx" &&
        mkdir -p "$SDK/ndk" &&
        rm -rf "$SDK/ndk/$NDK_VERSION" &&
        mv "$DL/ndkx/android-ndk-r27c" "$SDK/ndk/$NDK_VERSION"
fi

log "done"
du -sh "$SDK" "$HOME/toolchain" 2>/dev/null
echo
echo "Next:"
echo "  android/scripts/build-native.sh      # QEMU for arm64-v8a"
echo "  cd android && ./gradlew assembleDebug"
