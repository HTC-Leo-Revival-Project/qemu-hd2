#!/usr/bin/env python3
"""Screenshot the emulated phone's framebuffer and save it as a PNG.

Two ways in, and the difference matters:

  --qmp   QMP screendump: the guest's own framebuffer, native 480x800, exact.
          Needs phone-boot.sh running (and so a QMP port forwarded).

  --app   the app's screenshot button: also native, and works while the app
          runs in the GUI.  Taps the button in the emulator's title bar, which
          lands the file in /sdcard/Pictures/HTC HD2/.

  --phone the phone's own screen: the emulator window as the user sees it,
          scaled.  Blurry, and includes the app's title bar - last resort.

usage: phone-shot.py NAME [--qmp|--app|--phone]
"""
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phone_qmp import Qmp, serial           # noqa: E402

PKG = "org.htcleorevivalproject.hd2emu"
GUEST_W, GUEST_H = 480, 800


def adb(ser, *args, **kw):
    return subprocess.run(["adb", "-s", ser] + list(args), capture_output=True,
                          text=True, **kw)


def ppm_to_png(name):
    from PIL import Image
    Image.open(f"{name}.ppm").save(f"{name}.png")
    os.unlink(f"{name}.ppm")
    with open(f"{name}.png", "rb") as f:
        im = Image.open(f)
        w, h = im.size
    return w, h


def via_qmp(ser, name):
    remote = f"/data/user/0/{PKG}/files/emu/run/{name}.ppm"
    Qmp().send("screendump", filename=remote)
    time.sleep(1.5)
    with open(f"{name}.ppm", "wb") as o:
        subprocess.run(["adb", "-s", ser, "exec-out", "run-as", PKG, "cat",
                        remote], stdout=o)
    adb(ser, "shell", "rm", "-f", remote)
    return ppm_to_png(name)


def via_app(ser, name):
    # The camera button in the emulator's title bar.
    adb(ser, "shell", "input", "tap", "830", "152")
    time.sleep(4)
    out = adb(ser, "shell", "su", "-c",
              "ls -t '/sdcard/Pictures/HTC HD2/' | head -1").stdout.strip()
    if not out:
        raise SystemExit("no screenshot in /sdcard/Pictures/HTC HD2/")
    subprocess.run(["adb", "-s", ser, "pull",
                    f"/sdcard/Pictures/HTC HD2/{out}", f"{name}.png"],
                   capture_output=True)
    from PIL import Image
    im = Image.open(f"{name}.png")
    return im.size


def via_phone(ser, name):
    subprocess.run(["adb", "-s", ser, "shell", "screencap", "-p",
                    f"/sdcard/{name}.png"])
    time.sleep(1)
    subprocess.run(["adb", "-s", ser, "pull", f"/sdcard/{name}.png",
                    f"{name}.png"], capture_output=True)
    adb(ser, "shell", "rm", "-f", f"/sdcard/{name}.png")
    from PIL import Image
    return Image.open(f"{name}.png").size


def main():
    argv = sys.argv[1:]
    name = argv[0] if argv and not argv[0].startswith("--") else "shot"
    how = argv[1] if len(argv) > 1 else "--qmp"
    ser = serial()
    if how == "--qmp":
        size = via_qmp(ser, name)
    elif how == "--app":
        size = via_app(ser, name)
    else:
        size = via_phone(ser, name)
    exact = " (guest native)" if size == (GUEST_W, GUEST_H) else " (not native)"
    print(f"{name}.png {size[0]}x{size[1]}{exact}")


if __name__ == "__main__":
    main()