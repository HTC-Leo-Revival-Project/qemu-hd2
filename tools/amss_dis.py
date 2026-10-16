#!/usr/bin/env python3
"""Disassemble AMSS around a virtual address.

usage: amss_dis.py amss.elf VA [count [before]]   (odd VA or VA+t: Thumb)
"""
import os
import struct
import subprocess
import sys
import tempfile

OBJDUMP = next((o for o in (
    os.path.join(os.path.dirname(__file__),
                 '../toolchains/arm-eabi-4.8/bin/arm-eabi-objdump'),
    os.path.expanduser('~/toolchain47/bin/arm-eabi-objdump'),
    '/home/j0sh1x/qsd8k/arm-2009q3/bin/arm-none-eabi-objdump',
    'arm-none-eabi-objdump', 'objdump') if os.path.exists(o)), 'objdump')


def segments(e):
    phoff, = struct.unpack_from('<I', e, 28)
    n, = struct.unpack_from('<H', e, 44)
    for i in range(n):
        t, off, va, pa, fs, ms, fl, al = struct.unpack_from(
            '<8I', e, phoff + 32 * i)
        if t == 1 and fs:
            yield va, pa, e[off:off + fs]


def main():
    e = open(sys.argv[1], 'rb').read()
    arg = sys.argv[2]
    thumb = arg.endswith('t') or int(arg.rstrip('t'), 16) & 1
    va = int(arg.rstrip('t'), 16) & ~1
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 32
    before = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    for sva, pa, data in segments(e):
        if sva <= va < sva + len(data):
            step = 2 if thumb else 4
            start = max(sva, (va & ~(step - 1)) - step * before)
            with tempfile.NamedTemporaryFile(suffix='.bin') as f:
                f.write(data)
                f.flush()
                out = subprocess.run(
                    [OBJDUMP, '-b', 'binary', '-m', 'arm', '-D'] +
                    (['-M', 'force-thumb'] if thumb else []) + [
                     '--adjust-vma=%#x' % sva, '--start-address=%#x' % start,
                     '--stop-address=%#x' % (start + step * (count + before)),
                     f.name], capture_output=True, text=True).stdout
            print('\n'.join(out.splitlines()[7:]))
            return
    sys.exit('address not in any segment')


if __name__ == '__main__':
    main()
