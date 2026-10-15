#!/usr/bin/env python3
"""Inject touchscreen taps via QMP.

usage: touch.py SOCKET [x y [holdms]] ...

Coordinates are panel pixels on a 480x800 screen; the emulator scales them into
the controller's 6..576 x 6..952 range (see htcleo_touch_event() in
hw/arm/htcleo/htcleo.c).  A tap is a move to the point, the left button down,
then up; pass several points to make a drag.
"""
import json
import socket
import sys
import time

PANEL_W, PANEL_H = 480, 800
CTRL_X0, CTRL_X1 = 6, 576
CTRL_Y0, CTRL_Y1 = 6, 952


def to_abs(panel, panel_max, ctrl0, ctrl1):
    """Panel pixels -> QEMU absolute axis value (0..INPUT_EVENT_ABS_MAX)."""
    value = ctrl0 + panel * (ctrl1 - ctrl0) / panel_max
    # INPUT_EVENT_ABS_MAX in QEMU is 0x7fff, not 0xffff
    return max(0, min(0x7FFF, round(value * 0x7FFF / ctrl1)))


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile("rw")
        json.loads(self.f.readline())
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            msg = json.loads(self.f.readline())
            if "return" in msg or "error" in msg:
                if "error" in msg:
                    raise SystemExit("QMP error: %s" % msg["error"])
                return msg["return"]

    def event(self, kind, **data):
        self.cmd("input-send-event", events=[{"type": kind, "data": data}])

    def tap(self, x, y, hold_ms=200, drag_to=None):
        ax = to_abs(x, PANEL_W - 1, CTRL_X0, CTRL_X1)
        ay = to_abs(y, PANEL_H - 1, CTRL_Y0, CTRL_Y1)
        self.event("abs", axis="x", value=ax)
        self.event("abs", axis="y", value=ay)
        self.event("btn", button="left", down=True)
        if drag_to is not None:
            steps = 8
            for i in range(1, steps + 1):
                mx = to_abs(x + (drag_to[0] - x) * i / steps,
                            PANEL_W - 1, CTRL_X0, CTRL_X1)
                my = to_abs(y + (drag_to[1] - y) * i / steps,
                            PANEL_H - 1, CTRL_Y0, CTRL_Y1)
                self.event("abs", axis="x", value=mx)
                self.event("abs", axis="y", value=my)
                time.sleep(hold_ms / 1000 / steps)
        else:
            time.sleep(hold_ms / 1000)
        self.event("btn", button="left", down=False)
        time.sleep(0.2)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    qmp = Qmp(sys.argv[1])
    args = sys.argv[2:]
    if not args:
        raise SystemExit("no coordinates given")
    i = 0
    while i + 1 < len(args):
        x, y = int(args[i]), int(args[i + 1])
        hold = int(args[i + 2]) if i + 2 < len(args) and args[i + 2].isdigit() else 200
        print("tap %d,%d (%d ms)" % (x, y, hold))
        qmp.tap(x, y, hold)
        i += 3 if (i + 2 < len(args) and args[i + 2].isdigit()) else 2


if __name__ == "__main__":
    main()
