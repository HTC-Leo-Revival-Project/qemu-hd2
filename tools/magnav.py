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


def row_span(px, w, y):
    """(first, last, solid) white columns of row y, or None"""
    xs = [x for x in range(w) if px[x, y] == (255, 255, 255)]
    if not xs:
        return None
    return xs[0], xs[-1], len(xs) == xs[-1] - xs[0] + 1


def cursor(sock):
    """Menu row under the highlight bar: a block of rows whose white pixels
    all start and end on the same columns, with solid top and bottom."""
    subprocess.run([sys.executable, os.path.join(HERE, 'qmon.py'), sock,
                    'screendump ' + os.path.abspath(SHOT)], check=True,
                   stdout=subprocess.DEVNULL)
    im = Image.open(SHOT).convert('RGB')
    w, h = im.size
    px = im.load()
    run, edges = [], None
    for y in range(80, h + 1):
        span = row_span(px, w, y) if y < h else None
        if span and span[1] - span[0] >= 12 and span[:2] == edges:
            run.append(span)
            continue
        if len(run) >= 10 and sum(1 for r in run if r[2]) >= 2:
            top = y - len(run)
            return round(((top + y - 1) / 2 - FIRST_Y) / STEP) + 1
        run = [span] if span and span[1] - span[0] >= 12 else []
        edges = span[:2] if run else None
    return None


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
        if cur is None:         # caught mid-redraw?
            time.sleep(0.5)
            cur = cursor(sock)
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
