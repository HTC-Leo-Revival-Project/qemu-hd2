#!/usr/bin/env python3
"""PSNR of decoded encoder output (yuv420p 176x144) against the venc-test-pattern picture."""
import sys, math
w, h = 176, 144
OFF = int(sys.argv[2]) if len(sys.argv) > 2 else 0
d = open(sys.argv[1], 'rb').read()
fs = w * h * 3 // 2
n = len(d) // fs
def pat(f):
    bx, by = (f * 3) % (w - 32), (f * 2) % (h - 32)
    y = bytearray(w * h)
    for j in range(h):
        for i in range(w):
            box = bx <= i < bx + 32 and by <= j < by + 32
            y[j * w + i] = 235 if box else 16 + ((i + j + f * 4) & 0x7f)
    return y
def psnr(a, b):
    m = sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)
    return 99 if m == 0 else 10 * math.log10(255 * 255 / m)
for k in range(n):
    Y = d[k * fs:k * fs + w * h]
    best = max(range(0, 400), key=lambda f: psnr(Y, pat(f)) if abs(f - k - OFF) < 6 else -1)
    print(k, 'best pattern frame', best, 'PSNR %.1f' % psnr(Y, pat(best)))
