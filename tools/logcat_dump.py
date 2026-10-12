#!/usr/bin/env python3
"""Recover Android logcat entries from a guest RAM dump (memory order).

Kernel logger_entry (v2): u16 len, u16 hdr_size, pid, tid, sec, nsec, euid,
then the payload: prio(u8) tag\\0 message\\0.
usage: logcat_dump.py dump.bin
"""
import re, struct, sys
d = open(sys.argv[1], 'rb').read()
PR = '??VDIWEFS'
pat = re.compile(rb'[\x02-\x08][A-Za-z][A-Za-z0-9_./\-:]{0,40}\x00')
pos = 0
for m in pat.finditer(d):
    p = m.start()
    # CM9 logger v2 header: len, hdr_size(24), pid, tid, sec, nsec, euid
    h = p - 24
    if h < 0:
        continue
    ln, hsz, pid, tid, sec, nsec = struct.unpack_from('<HHiiii', d, h)
    if hsz != 24 or not (3 < ln < 4100) or not (0 < pid < 65536) or not (0 < tid < 65536):
        continue
    body = d[p:p + ln]
    if body[-1:] != b'\0':
        continue
    tag, _, msg = body[1:].partition(b'\0')
    print('%5d %5d %s/%s: %s' % (pid, tid, PR[body[0]], tag.decode(errors='replace'),
                                msg.rstrip(b'\0').decode(errors='replace')))
