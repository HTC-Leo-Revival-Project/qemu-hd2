# Documentation

Everything written down about the emulated HTC HD2, grouped by what you are
trying to do. The [README](../README.md) is the overview — what the project is,
what works, and how to build and run it — and is the place to start.

## Overview and reference

| Document | What is in it |
|---|---|
| [README](../README.md) | What the machine is, build, images, run, what works, real baseband, GPU, credits. |
| [Building](building.md) | Building the emulator on Linux, the Android app, and the web UI, from a clean checkout. |
| [Tools](tools.md) | Every script and helper in `tools/`, what it does and how it is called. |
| [Quirks](../quirks.md) | Open problems and workarounds of the real-firmware (LLE) modes. |

## The operating systems that boot

| Document | What is in it |
|---|---|
| [Android 4.0.4](android.md) | NexusHD2 ICS as a NAND ROM under cLK: images, run, and the read-only-remount rule. |
| [Windows Mobile 6.5](windows-mobile.md) | The stock HTC ROM on the real baseband, straight from the SPL. |
| [iOS 7](ios.md) | The HD2 iOS lab package booting from the microSD through MAGLDR. |

## Subsystems

| Document | What is in it |
|---|---|
| [USB passthrough](usb-passthrough.md) | Putting the emulated HD2 on the phone's own USB port: the bridge, FunctionFS, and every trap. |
| [GPU (Adreno 200)](../README.md#gpu-adreno-200) | The host-GPU, OpenGL ES and software renderers. |

## The Android app and the web UI

| Document | What is in it |
|---|---|
| [web/README.md](../web/README.md) | The browser front end: VNC, H.264, input, the emulator's own lifecycle. |
| [android/README.md](../android/README.md) | The app: build, how it talks to QEMU, the NAND builder, root and USB, the OpenGL ES backend, languages. |
| [android/HEADLESS.md](../android/HEADLESS.md) | Running the emulator on the phone with no app and no screen, over `adb`. Read the three traps first. |
| [android/adreno-divider.md](../android/adreno-divider.md) | The one artifact left in the OpenGL ES backend, and everything ruled out about it. |
