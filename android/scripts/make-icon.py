#!/usr/bin/env python3
"""Draws the launcher icon: the phone's face, black and lit up Holo blue.

No PIL in the build, so the PNGs are written by hand (zlib is all a PNG
really needs).  Run it from anywhere:

    python3 android/scripts/make-icon.py
"""
import math
import struct
import zlib
from pathlib import Path

RES = Path(__file__).resolve().parent.parent / "app/src/main/res"
DENSITIES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}

BG = (13, 16, 18)
BODY = (24, 26, 28)
BODY_EDGE = (58, 64, 70)
SCREEN = (28, 44, 52)
SCREEN_TOP = (51, 181, 229)
SCREEN_BOTTOM = (0, 102, 153)
KEY = (72, 76, 80)
KEY_ON = (51, 181, 229)


def blend(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def sample(n, x, y):
    """x, y are in pixels; everything inside is 0..1 of the icon."""
    fx, fy = x / n, y / n
    body_w, body_h = 0.62, 0.92
    body_x, body_y = 0.19, 0.04
    # rounded rect mask for the body
    if not (body_x <= fx <= body_x + body_w and body_y <= fy <= body_y + body_h):
        return (0, 0, 0), 0
    r = 0.06
    dx = max(body_x + r - fx, fx - (body_x + body_w - r), 0)
    dy = max(body_y + r - fy, fy - (body_y + body_h - r), 0)
    if math.hypot(dx, dy) > r:
        return (0, 0, 0), 0

    colour = BODY
    # the screen
    sx, sy, sw, sh = body_x + 0.035, body_y + 0.045, body_w - 0.07, body_h - 0.17
    if sx <= fx <= sx + sw and sy <= fy <= sy + sh:
        t = (fy - sy) / sh
        colour = blend(SCREEN_TOP, SCREEN_BOTTOM, t)
        # a couple of list rows, so it reads as a screen
        row = (fy - sy) * (1.0 / sh)
        if 0.16 < row < 0.205 or 0.30 < row < 0.345 or 0.44 < row < 0.485:
            colour = blend(colour, (235, 235, 235), 0.75)
        if 0.16 < row < 0.205 and sx + 0.03 <= fx <= sx + 0.12:
            colour = SCREEN_TOP
    # the keys under it
    elif sy + sh < fy < sy + sh + 0.085:
        kx = body_x + 0.05
        kw = (body_w - 0.10) / 4
        for i in range(4):
            if kx + i * kw + 0.012 <= fx <= kx + (i + 1) * kw - 0.012:
                colour = KEY_ON if i in (0, 2) else KEY
    # a bevel around the body
    edge = min(fx - body_x, body_x + body_w - fx, fy - body_y, body_y + body_h - fy)
    if edge < 0.012:
        colour = blend(colour, BODY_EDGE, (0.012 - edge) / 0.012)
    return colour, 255


def render(n):
    px = bytearray()
    for y in range(n):
        px.append(0)                                   # filter byte
        for x in range(n):
            colour, alpha = sample(n, x + 0.5, y + 0.5)
            bg = BG
            out = tuple(int(round(c * alpha / 255 + b * (255 - alpha) / 255))
                        for c, b in zip(colour, BG))
            px += bytes(out) + bytes((alpha,))
    return bytes(px)


def png(raw, n):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def main():
    for name, size in DENSITIES.items():
        out = RES / ("mipmap-" + name)
        out.mkdir(parents=True, exist_ok=True)
        for icon in ("ic_launcher.png", "ic_launcher_round.png"):
            (out / icon).write_bytes(png(render(size), size))
        print("%s: %d px" % (out.name, size))


if __name__ == "__main__":
    main()
