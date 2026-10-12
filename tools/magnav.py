#!/usr/bin/env python3
"""Move MAGLDR's menu cursor to an item, reading the screen back.

usage: magnav.py SOCK ITEM [select]   (ITEM counts from 1)

MAGLDR polls its keypad and drops or repeats presses now and then, so
this presses VolUp/VolDown one at a time, finds the highlighted row (the
white bar) on a screendump and stops at ITEM; "select" then presses Call.
Works for the menus and the AD SD Dir picker (rows 33 px apart from y 98).
"""
import os
import subprocess
import sys
import time

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = os.path.join(HERE, '../build-tmp/magnav.ppm')
FIRST_Y, STEP = 98, 33


def longest_white(px, w, y):
    best = n = 0
    for x in range(w):
        n = n + 1 if px[x, y] == (255, 255, 255) else 0
        best = max(best, n)
    return best


def cursor(sock):
    subprocess.run([sys.executable, os.path.join(HERE, 'qmon.py'), sock,
                    'screendump ' + os.path.abspath(SHOT)], check=True,
                   stdout=subprocess.DEVNULL)
    im = Image.open(SHOT).convert('RGB')
    w, h = im.size
    px = im.load()
    run = []
    for y in range(80, h):
        if longest_white(px, w, y) >= 12:
            run.append(y)
        elif len(run) >= 10:
            break
        else:
            run = []
    if len(run) < 10:
        return None
    return round(((run[0] + run[-1]) / 2 - FIRST_Y) / STEP) + 1


def press(sock, key):
    subprocess.run([sys.executable, os.path.join(HERE, 'keys.py'), sock,
                    key + ':220'], check=True)
    time.sleep(1.0)


def main():
    sock, target = sys.argv[1], int(sys.argv[2])
    for _ in range(40):
        cur = cursor(sock)
        while True:             # settle: presses can land late
            time.sleep(0.4)
            again = cursor(sock)
            if again == cur:
                break
            cur = again
        if cur is None:
            sys.exit('no menu cursor on screen')
        if cur == target:
            break
        press(sock, 'pgdn' if cur < target else 'pgup')
    else:
        sys.exit('cursor stuck at %s' % cur)
    if len(sys.argv) > 3 and sys.argv[3] == 'select':
        press(sock, 'f2')


if __name__ == '__main__':
    main()
