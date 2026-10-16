#!/usr/bin/env python3
"""Send key presses to the emulated phone over QMP, reached through adb.

The phone's QEMU also listens on 127.0.0.1:4444, so `adb forward tcp:4444
tcp:4444` reaches it from here.  That is the only way in: the monitor's unix
socket is inside the app's private data and mode 0700, so adbd (which runs as
the shell user) cannot open it.

F2 is not optional: the cCLK bootloader waits at its menu for a key press
before it boots anything at all.

usage: phone-keys.py [qcode ...]
       phone-keys.py f2            press F2 once
       phone-keys.py --boot        press F2 repeatedly, as startBootKeys() does
"""
import subprocess
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from phone_qmp import Qmp            # noqa: E402

PKG = "org.htcleorevivalproject.hd2emu"


def serial():
    out = subprocess.run(["adb", "devices"], capture_output=True, text=True).stdout
    for line in out.splitlines()[1:]:
        parts = line.split()
        # The phone may be on a wifi serial ("host:port") or on the mDNS name
        # it advertises when it changes networks; both are the one phone.
        if len(parts) >= 2 and parts[1] == "device" and "emulator" not in parts[0]:
            return parts[0]
    raise SystemExit("no phone over adb")


def press(q, key):
    for down in (True, False):
        ev = {"type": "key",
              "data": {"down": down, "key": {"type": "qcode", "data": key}}}
        q.send("input-send-event", events=[ev])
        time.sleep(0.2)
    time.sleep(0.3)


def guest_running(ser):
    """True once the guest kernel is printing to its console.

    The bootloader needs F2; Android does not, and F2 is a real key there, so
    the presses have to stop the moment the guest is up - otherwise they drive
    the UI.  The app's startBootKeys() checks the same thing.
    """
    out = subprocess.run(
        ["adb", "-s", ser, "shell", "run-as", PKG, "wc", "-c",
         f"/data/user/0/{PKG}/files/emu/run/ramcon.log"],
        capture_output=True, text=True).stdout.split()
    try:
        return int(out[0]) > 4096
    except (IndexError, ValueError):
        return False


def main():
    ser = serial()
    subprocess.run(["adb", "-s", ser, "forward", "tcp:4444", "tcp:4444"],
                   capture_output=True)
    args = sys.argv[1:]
    if args == ["--boot"]:
        # What the app does: a few rounds of F2, because the menu may not be
        # up yet when the first one lands - but stop as soon as the guest
        # kernel starts printing, or the presses go into Android instead.
        q = None
        for _ in range(10):
            if guest_running(ser):
                print("guest is up; stopping the F2 presses")
                break
            if q is None:
                try:
                    q = Qmp()
                except OSError:
                    time.sleep(3)
                    continue
            press(q, "f2")
            time.sleep(3.2)
        return
    q = Qmp()
    for k in args or ["f2"]:
        press(q, k)
    print("keys sent:", " ".join(args or ["f2"]))


if __name__ == "__main__":
    main()