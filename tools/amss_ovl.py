#!/usr/bin/env python3
"""Map AMSS code to the mDSP overlay variables it uses, with its messages.

AMSS reaches the modem DSP's variables through a symbol table that it
switches with the DSP overlay: ldr rX, =0x1350b0c; ldr rX, [rX]; then
ldr rY, [rX, #4*k] (possibly after adds rX, #0x80): slot k.  Message
calls (bl msg_send) load a constant {line, flags, format, file, ...}.
For each Thumb function this prints the slots it uses and its messages.

usage: amss_ovl.py amss.elf [slot ...]     (slots: only functions using them)
       amss_ovl.py amss.elf --grep TEXT    (functions whose messages match)
       amss_ovl.py amss.elf --calls VA     (functions that call VA)
"""
import struct
import sys

SYMTAB = 0x1350B0C
MSG_SEND = (0x526604, 0xC55D88, 0xC57028)

e = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<I', e, 28)
phes, phn = struct.unpack_from('<HH', e, 42)
segs = [struct.unpack_from('<8I', e, phoff + i * phes) for i in range(phn)]


def rd(va, n):
    for t, off, v, pa, fs, ms, fl, al in segs:
        if t == 1 and v <= va < v + fs:
            return e[off + va - v:off + va - v + n]
    return None


def rd32(va):
    b = rd(va, 4)
    return struct.unpack('<I', b)[0] if b and len(b) == 4 else None


def cstr(va):
    b = rd(va, 160)
    if not b or 0 not in b:
        return None
    s = b[:b.index(0)]
    return s.decode('latin1') if s and all(32 <= c < 127 for c in s) else None


def message(m):
    w = rd(m, 16)
    if not w or len(w) < 16:
        return None
    line, flags, fmt, file = struct.unpack('<4I', w)
    f, n = cstr(fmt), cstr(file)
    return '%s:%d: %s' % (n, line, f) if f and n else None


funcs = {}          # start VA -> {'slots': set, 'msgs': list, 'calls': set}
for t, off, v, pa, fs, ms, fl, al in segs:
    if t != 1 or not fl & 1:
        continue
    code = e[off:off + fs]
    start = v
    reg_tab = {}    # register -> state: 'lit' (=SYMTAB), 'tab', 'tab80'
    for i in range(0, len(code) - 4, 2):
        pc = v + i
        h, = struct.unpack_from('<H', code, i)
        if h & 0xff00 == 0xb500 or h & 0xff00 == 0xb400 and h & 0xff:
            if h & 0xff00 == 0xb500:
                start = pc
                reg_tab = {}
        f = funcs.setdefault(start, {'slots': set(), 'msgs': [],
                                     'calls': set()})
        if h & 0xf800 == 0x4800:                      # ldr rd, [pc, #i]
            rd_ = (h >> 8) & 7
            lit = rd32(((pc + 4) & ~3) + (h & 0xff) * 4)
            reg_tab.pop(rd_, None)
            if lit == SYMTAB:
                reg_tab[rd_] = 'lit'
            elif lit is not None:
                reg_tab[('lit', rd_)] = lit
        elif h & 0xf800 == 0x6800:                    # ldr rd, [rn, #i]
            rn, rd_, imm = (h >> 3) & 7, h & 7, ((h >> 6) & 0x1f) * 4
            st = reg_tab.get(rn)
            if st == 'lit' and imm == 0:
                reg_tab[rd_] = 'tab'
            elif st in ('tab', 'tab80'):
                k = (imm + (0x80 if st == 'tab80' else 0)) // 4
                f['slots'].add(k)
                if rd_ != rn:
                    reg_tab.pop(rd_, None)
                else:
                    reg_tab.pop(rd_, None)
            else:
                reg_tab.pop(rd_, None)
        elif h & 0xf800 == 0x3000:                    # adds rd, #i
            rd_ = (h >> 8) & 7
            if reg_tab.get(rd_) == 'tab' and h & 0xff == 0x80:
                reg_tab[rd_] = 'tab80'
        elif h & 0xf800 == 0xf000 and i + 2 < len(code):
            h2, = struct.unpack_from('<H', code, i + 2)
            if h2 & 0xf800 in (0xf800, 0xe800):
                o = ((h & 0x7ff) << 12) | ((h2 & 0x7ff) << 1)
                if o & 0x400000:
                    o -= 0x800000
                tgt = pc + 4 + o
                if h2 & 0xf800 == 0xe800:
                    tgt &= ~3
                f['calls'].add(tgt)
                if tgt in MSG_SEND:
                    lit = reg_tab.get(('lit', 0))
                    msg = message(lit) if lit else None
                    if msg:
                        f['msgs'].append(msg)

want = set()
grep = calls = None
if len(sys.argv) > 2 and sys.argv[2] == '--grep':
    grep = sys.argv[3]
elif len(sys.argv) > 2 and sys.argv[2] == '--calls':
    calls = int(sys.argv[3], 0)
else:
    want = {int(x, 0) for x in sys.argv[2:]}
for s in sorted(funcs):
    f = funcs[s]
    if calls is not None:
        if calls not in f['calls']:
            continue
    elif not f['slots'] and not grep:
        continue
    if want and not f['slots'] & want:
        continue
    if grep and not any(grep in m for m in f['msgs']):
        continue
    print('%07x slots %s' % (s, sorted(f['slots'])))
    for m in dict.fromkeys(f['msgs']):
        print('          ', m)
