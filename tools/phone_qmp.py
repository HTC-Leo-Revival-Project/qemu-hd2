#!/usr/bin/env python3
"""A QMP client for the emulated phone, reached through `adb forward`.

The QEMU started by tools/phone-boot.sh listens on 127.0.0.1:4444 as well as on
a unix socket inside the app's private data, so this forwards that port and
talks to it directly.  Anything the monitor can do works: registers, screendump,
system_reset.

usage:
    phone-qmp.py "info registers"
    phone-qmp.py "xp /8wx 0x28008060"
    phone-qmp.py screendump out.ppm
"""
import json
import socket
import subprocess
import sys
import time


class Qmp:
    def __init__(self, host="127.0.0.1", port=4444, timeout=20, tries=10):
        last = None
        for _ in range(tries):
            try:
                self.s = socket.create_connection((host, port), timeout=timeout)
                self.f = self.s.makefile("rwb")
                self.greeting = json.loads(self.f.readline())
                self.send("qmp_capabilities")
                return
            except OSError as e:       # the monitor may not be listening yet
                last = e
                time.sleep(2)
        raise last

    def send(self, execute, **args):
        cmd = {"execute": execute}
        if args:
            cmd["arguments"] = args
        self.f.write((json.dumps(cmd) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError("QMP closed the connection")
            msg = json.loads(line)
            if "event" in msg:
                continue
            if "error" in msg:
                raise RuntimeError(msg["error"].get("desc", msg["error"]))
            return msg.get("return")

    def hmp(self, command_line):
        """Run a human monitor command; its output comes back as a string."""
        return self.send("human-monitor-command", **{"command-line": command_line})


def serial():
    out = subprocess.run(["adb", "devices"], capture_output=True, text=True).stdout
    for line in out.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device" and "emulator" not in parts[0]:
            return parts[0]
    raise SystemExit("no phone over adb")


def connect():
    ser = serial()
    subprocess.run(["adb", "-s", ser, "forward", "tcp:4444", "tcp:4444"],
                   capture_output=True)
    return Qmp()


def main():
    argv = sys.argv[1:]
    if not argv:
        q = connect()
        print(json.dumps(q.greeting, indent=2))
        return
    q = connect()
    cmd, rest = argv[0], argv[1:]
    if cmd == "screendump":
        # The dump lands on the phone, under the app's data; pull it back.
        name = rest[0] if rest else "shot"
        remote = f"/data/user/0/org.htcleorevivalproject.hd2emu/files/emu/run/{name}.ppm"
        q.send("screendump", filename=remote)
        ser = serial()
        subprocess.run(["adb", "-s", ser, "exec-out", "run-as",
                        "org.htcleorevivalproject.hd2emu", "cat", remote],
                       stdout=open(f"{name}.ppm", "wb"))
        subprocess.run(["adb", "-s", ser, "shell", "rm", "-f", remote],
                       capture_output=True)
        print(f"{name}.ppm")
        return
    out = q.hmp(" ".join(argv))
    print(out if isinstance(out, str) else json.dumps(out, indent=2))


if __name__ == "__main__":
    main()