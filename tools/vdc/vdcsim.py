#!/usr/bin/env python3
"""Reference model of the QSD8250 "Janus" VDC for H.264 command buffers.

Fed a dump from htcleo-q6's vdc-dump, it rebuilds the picture the way the
hardware would.  RE notes are in the comments; this is the prototype for
the C model in hw/hexagon/htcleo_q6_vdc.c.
"""
import struct, sys
import numpy as np
from dumpread import load
from cmdparse import packets

# ---------------------------------------------------------------- tables
LEVEL_SCALE = [[10, 13, 10, 13, 13, 16, 13, 16, 10, 13, 10, 13, 13, 16, 13, 16],
               [11, 14, 11, 14, 14, 18, 14, 18, 11, 14, 11, 14, 14, 18, 14, 18],
               [13, 16, 13, 16, 16, 20, 16, 20, 13, 16, 13, 16, 16, 20, 16, 20],
               [14, 18, 14, 18, 18, 23, 18, 23, 14, 18, 14, 18, 18, 23, 18, 23],
               [16, 20, 16, 20, 20, 25, 20, 25, 16, 20, 16, 20, 20, 25, 20, 25],
               [18, 23, 18, 23, 23, 29, 23, 29, 18, 23, 18, 23, 23, 29, 23, 29]]
# flat scaling lists (baseline): LevelScale4x4 = 16 * normAdjust
LS = [[16 * v for v in row] for row in LEVEL_SCALE]

ALPHA = [0]*16 + [4,4,5,6,7,8,9,10,12,13,15,17,20,22,25,28,32,36,40,45,50,56,63,71,80,90,101,113,127,144,162,182,203,226,255,255]
BETA = [0]*16 + [2,2,2,3,3,3,3,4,4,4,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18]
TC0 = [[0,0,0]]*17 + [[0,0,1],[0,0,1],[0,0,1],[0,0,1],[0,1,1],[0,1,1],[1,1,1],[1,1,1],[1,1,1],[1,1,1],[1,1,2],[1,1,2],[1,1,2],[1,1,2],[1,2,3],[1,2,3],[2,2,3],[2,2,4],[2,3,4],[2,3,4],[3,3,5],[3,4,6],[3,4,6],[4,5,7],[4,5,8],[4,6,9],[5,7,10],[6,8,11],[6,8,13],[7,10,14],[8,11,16],[9,12,18],[10,13,20],[11,15,23],[13,17,25]]
QPC = list(range(30)) + [29,30,31,32,32,33,34,34,35,35,36,36,37,37,37,38,38,38,39,39,39,39]

def clip1(x):
    return 0 if x < 0 else 255 if x > 255 else x

def idct4(b):
    """H.264 4x4 inverse transform of a 4x4 int array, (x+32)>>6."""
    b = b.astype(np.int64)
    t = np.zeros((4, 4), np.int64)
    for i in range(4):
        e = b[i, 0] + b[i, 2]; f = b[i, 0] - b[i, 2]
        g = (b[i, 1] >> 1) - b[i, 3]; h = b[i, 1] + (b[i, 3] >> 1)
        t[i] = [e + h, f + g, f - g, e - h]
    r = np.zeros((4, 4), np.int64)
    for j in range(4):
        e = t[0, j] + t[2, j]; f = t[0, j] - t[2, j]
        g = (t[1, j] >> 1) - t[3, j]; h = t[1, j] + (t[3, j] >> 1)
        r[:, j] = [e + h, f + g, f - g, e - h]
    return (r + 32) >> 6

def hadamard4(c):
    h = np.array([[1, 1, 1, 1], [1, 1, -1, -1], [1, -1, -1, 1], [1, -1, 1, -1]], np.int64)
    return h @ c.astype(np.int64) @ h

# ---------------------------------------------------------------- state
class MB:
    pass

def parse(path):
    """Group the command stream into per-MB records (intra pictures)."""
    addr, w, mem = load(path)
    hdr = w[:4]
    height, width = hdr[2] >> 16, hdr[2] & 0xffff
    regs = {}; coef_pending = []; coef_prev = []; mbs = []
    out = {}; inter = {}; cur_mb = None; cur_x = -1; row = 0; dbk = []
    for i, k, r, v in packets(w, 4):
        if k == 'END':
            if 0xe5 in regs:
                dbk.append({x: regs.get(x, 0) for x in (0xe6, 0xe7, 0xe8, 0xe9, 0xec, 0xf5)})
            if 0xd2 in regs:
                m = MB()
                m.d2, m.cd, m.d1 = regs[0xd2], regs[0xcd], regs[0xd1]
                m.d0, m.cf = regs.get(0xd0, 0), regs.get(0xcf, 0)
                m.coef = coef_prev
                mbs.append(m)
            coef_prev = coef_pending; coef_pending = []
            regs = {}
            continue
        if k == 'W2' and r[0] == 0x8b:
            # 0x82: MB row << 16 | MB column
            cur_mb = (v[1] >> 16) * (width // 16) + (v[1] & 0xffff)
        if k == 'MEM' and r[0] == 0x101:
            inter[cur_mb] = mem.get(v[0], [])
        if k == 'MEM' and r[0] in (0x100b, 0x100c):
            blk = mem.get(v[0], [])
            coef_pending = [struct.unpack('<hh', struct.pack('<I', x)) for x in blk]
            continue
        for a, b in zip(r, v):
            regs[a] = b
            if a in (0x7a, 0x72):
                out[a] = b
        if k == 'MEM' and r[0] == 0x2c and 'luma' not in out:
            blk = mem.get(v[0], [])
            out['luma'], out['chroma'] = blk[3], blk[9]
    for n, m in enumerate(mbs):
        m.inter = inter.get(n, [])
        m.dbk = dbk[n] if n < len(dbk) else None
    return width, height, mbs, out

# ---------------------------------------------------------------- intra
def pred4x4(Y, x0, y0, mode, avail_top, avail_left, avail_tr, avail_tl):
    p = lambda x, y: int(Y[y0 + y, x0 + x])
    top = [p(i, -1) for i in range(4)] if avail_top else None
    tr = [p(4 + i, -1) for i in range(4)] if avail_tr else (top[3:4] * 4 if top else None)
    left = [p(-1, i) for i in range(4)] if avail_left else None
    tl = p(-1, -1) if avail_tl else None
    o = np.zeros((4, 4), np.int64)
    if mode == 0:
        for y in range(4): o[y] = top
    elif mode == 1:
        for y in range(4): o[y] = left[y]
    elif mode == 2:
        if top and left: dc = (sum(top) + sum(left) + 4) >> 3
        elif left: dc = (sum(left) + 2) >> 2
        elif top: dc = (sum(top) + 2) >> 2
        else: dc = 128
        o[:] = dc
    else:
        T = top + tr if top else None
        if mode == 3:
            for y in range(4):
                for x in range(4):
                    if x == 3 and y == 3: o[y, x] = (T[6] + 3 * T[7] + 2) >> 2
                    else: o[y, x] = (T[x + y] + 2 * T[x + y + 1] + T[x + y + 2] + 2) >> 2
        else:
            L = left
            P = lambda i: tl if i == -1 else (T[i] if i >= 0 else L[-2 - i])  # P(i): -1 tl, >=0 top, -2.. left
            def px(x, y):
                if y == -1: return tl if x == -1 else T[x]
                return L[y]
            if mode == 4:
                for y in range(4):
                    for x in range(4):
                        if x > y: o[y, x] = (px(x - y - 2, -1) + 2 * px(x - y - 1, -1) + px(x - y, -1) + 2) >> 2
                        elif x < y: o[y, x] = (px(-1, y - x - 2) + 2 * px(-1, y - x - 1) + px(-1, y - x) + 2) >> 2
                        else: o[y, x] = (px(0, -1) + 2 * px(-1, -1) + px(-1, 0) + 2) >> 2
            elif mode == 5:
                for y in range(4):
                    for x in range(4):
                        z = 2 * x - y
                        if z >= 0 and z % 2 == 0: o[y, x] = (px(x - (y >> 1) - 1, -1) + px(x - (y >> 1), -1) + 1) >> 1
                        elif z >= 0: o[y, x] = (px(x - (y >> 1) - 2, -1) + 2 * px(x - (y >> 1) - 1, -1) + px(x - (y >> 1), -1) + 2) >> 2
                        elif z == -1: o[y, x] = (px(-1, 0) + 2 * px(-1, -1) + px(0, -1) + 2) >> 2
                        else: o[y, x] = (px(-1, y - 1) + 2 * px(-1, y - 2) + px(-1, y - 3) + 2) >> 2
            elif mode == 6:
                for y in range(4):
                    for x in range(4):
                        z = 2 * y - x
                        if z >= 0 and z % 2 == 0: o[y, x] = (px(-1, y - (x >> 1) - 1) + px(-1, y - (x >> 1)) + 1) >> 1
                        elif z >= 0: o[y, x] = (px(-1, y - (x >> 1) - 2) + 2 * px(-1, y - (x >> 1) - 1) + px(-1, y - (x >> 1)) + 2) >> 2
                        elif z == -1: o[y, x] = (px(-1, 0) + 2 * px(-1, -1) + px(0, -1) + 2) >> 2
                        else: o[y, x] = (px(x - 1, -1) + 2 * px(x - 2, -1) + px(x - 3, -1) + 2) >> 2
            elif mode == 7:
                for y in range(4):
                    for x in range(4):
                        i = x + (y >> 1)
                        if y % 2 == 0: o[y, x] = (T[i] + T[i + 1] + 1) >> 1
                        else: o[y, x] = (T[i] + 2 * T[i + 1] + T[i + 2] + 2) >> 2
            elif mode == 8:
                for y in range(4):
                    for x in range(4):
                        z = x + 2 * y
                        if z > 5: o[y, x] = L[3]
                        elif z == 5: o[y, x] = (L[2] + 3 * L[3] + 2) >> 2
                        elif z % 2 == 0: o[y, x] = (L[y + (x >> 1)] + L[y + (x >> 1) + 1] + 1) >> 1
                        else: o[y, x] = (L[y + (x >> 1)] + 2 * L[y + (x >> 1) + 1] + L[y + (x >> 1) + 2] + 2) >> 2
    return o

def pred_plane16(P, x0, y0, n, mode, at, al, atl, chroma=False):
    """16x16 luma or 8x8 chroma prediction.  Chroma modes: 0 DC 1 H 2 V 3
    plane; luma modes: 0 V 1 H 2 DC 3 plane."""
    p = lambda x, y: int(P[y0 + y, x0 + x])
    top = [p(i, -1) for i in range(n)] if at else None
    left = [p(-1, i) for i in range(n)] if al else None
    o = np.zeros((n, n), np.int64)
    if chroma:
        mode = {0: 2, 1: 1, 2: 0, 3: 3}[mode]
    if mode == 0:
        for y in range(n): o[y] = top
    elif mode == 1:
        for y in range(n): o[y] = left[y]
    elif mode == 2:
        if not chroma:
            if top and left: dc = (sum(top) + sum(left) + 16) >> 5
            elif left: dc = (sum(left) + 8) >> 4
            elif top: dc = (sum(top) + 8) >> 4
            else: dc = 128
            o[:] = dc
        else:
            for by in range(2):
                for bx in range(2):
                    t = top[4 * bx:4 * bx + 4] if top else None
                    l = left[4 * by:4 * by + 4] if left else None
                    if (bx, by) in ((0, 0), (1, 1)):
                        if t and l: dc = (sum(t) + sum(l) + 4) >> 3
                        elif l: dc = (sum(l) + 2) >> 2
                        elif t: dc = (sum(t) + 2) >> 2
                        else: dc = 128
                    elif (bx, by) == (1, 0):
                        if t: dc = (sum(t) + 2) >> 2
                        elif l: dc = (sum(l) + 2) >> 2
                        else: dc = 128
                    else:
                        if l: dc = (sum(l) + 2) >> 2
                        elif t: dc = (sum(t) + 2) >> 2
                        else: dc = 128
                    o[4 * by:4 * by + 4, 4 * bx:4 * bx + 4] = dc
    else:
        tl = p(-1, -1)
        if not chroma:
            H = sum((x + 1) * (p(8 + x, -1) - p(6 - x, -1)) for x in range(8))
            V = sum((y + 1) * (p(-1, 8 + y) - p(-1, 6 - y)) for y in range(8))
            a = 16 * (p(-1, 15) + p(15, -1)); b = (5 * H + 32) >> 6; c = (5 * V + 32) >> 6
            for y in range(16):
                for x in range(16):
                    o[y, x] = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5)
        else:
            H = sum((x + 1) * (p(4 + x, -1) - p(2 - x, -1)) for x in range(4))
            V = sum((y + 1) * (p(-1, 4 + y) - p(-1, 2 - y)) for y in range(4))
            a = 16 * (p(-1, 7) + p(7, -1)); b = (34 * H + 32) >> 6; c = (34 * V + 32) >> 6
            for y in range(8):
                for x in range(8):
                    o[y, x] = clip1((a + b * (x - 3) + c * (y - 3) + 16) >> 5)
    return o


def luma_mc(R, x, y, mvx, mvy, w, h):
    """quarter-pel luma prediction of a w x h block at (x, y)"""
    H, W = R.shape
    def px(xx, yy):
        return int(R[min(max(yy, 0), H - 1), min(max(xx, 0), W - 1)])
    ix, iy = x + (mvx >> 2), y + (mvy >> 2); fx, fy = mvx & 3, mvy & 3
    tap = lambda a, b, c, d, e, f: a - 5 * b + 20 * c + 20 * d - 5 * e + f
    o = np.zeros((h, w), np.int64)
    for j in range(h):
        for i in range(w):
            X, Y = ix + i, iy + j
            G = px(X, Y)
            def bh(xx, yy):  # horizontal half sample b at (xx+1/2, yy), unclipped
                return tap(px(xx-2,yy), px(xx-1,yy), px(xx,yy), px(xx+1,yy), px(xx+2,yy), px(xx+3,yy))
            def hv(xx, yy):  # vertical half sample h at (xx, yy+1/2), unclipped
                return tap(px(xx,yy-2), px(xx,yy-1), px(xx,yy), px(xx,yy+1), px(xx,yy+2), px(xx,yy+3))
            c1 = lambda v: clip1((v + 16) >> 5)
            b = c1(bh(X, Y)); hh = c1(hv(X, Y))
            if fx == 0 and fy == 0: v = G
            elif fx == 2 and fy == 0: v = b
            elif fx == 0 and fy == 2: v = hh
            else:
                j1 = tap(*[bh(X, Y + k) for k in (-2, -1, 0, 1, 2, 3)])
                jj = clip1((j1 + 512) >> 10)
                s_ = c1(bh(X, Y + 1)); m_ = c1(hv(X + 1, Y))
                if fx == 2 and fy == 2: v = jj
                elif fx == 1 and fy == 0: v = (G + b + 1) >> 1
                elif fx == 3 and fy == 0: v = (b + px(X + 1, Y) + 1) >> 1
                elif fx == 0 and fy == 1: v = (G + hh + 1) >> 1
                elif fx == 0 and fy == 3: v = (hh + px(X, Y + 1) + 1) >> 1
                elif fx == 2 and fy == 1: v = (b + jj + 1) >> 1
                elif fx == 2 and fy == 3: v = (jj + s_ + 1) >> 1
                elif fx == 1 and fy == 2: v = (hh + jj + 1) >> 1
                elif fx == 3 and fy == 2: v = (jj + m_ + 1) >> 1
                elif fx == 1 and fy == 1: v = (b + hh + 1) >> 1
                elif fx == 3 and fy == 1: v = (b + m_ + 1) >> 1
                elif fx == 1 and fy == 3: v = (hh + s_ + 1) >> 1
                else: v = (s_ + m_ + 1) >> 1
            o[j, i] = v
    return o

def chroma_mc(R, x, y, mvx, mvy, w, h):
    H, W = R.shape
    def px(xx, yy):
        return int(R[min(max(yy, 0), H - 1), min(max(xx, 0), W - 1)])
    ix, iy = x + (mvx >> 3), y + (mvy >> 3); fx, fy = mvx & 7, mvy & 7
    o = np.zeros((h, w), np.int64)
    for j in range(h):
        for i in range(w):
            A, B = px(ix + i, iy + j), px(ix + i + 1, iy + j)
            C, D = px(ix + i, iy + j + 1), px(ix + i + 1, iy + j + 1)
            o[j, i] = ((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B + (8 - fx) * fy * C + fx * fy * D + 32) >> 6
    return o

def s16(v):
    return v - 0x10000 if v & 0x8000 else v

# ---------------------------------------------------------------- recon
def blk_xy(b):
    return (b & 3) * 4, (b >> 2) * 4

def reconstruct(width, height, mbs, ref=None):
    W, H = width, height
    Y = np.zeros((H, W), np.int64); U = np.zeros((H // 2, W // 2), np.int64); V = np.zeros_like(U)
    mbw = W // 16
    info = []
    for n, m in enumerate(mbs):
        mx, my = n % mbw, n // mbw
        x0, y0 = 16 * mx, 16 * my
        cls = (m.d2 >> 8) & 0xff
        na = m.d2 >> 24          # neighbour unavailable flags
        chroma_mode = (m.d2 >> 16) & 3
        qy_div, qy_mod = m.d1 & 15, (m.d1 >> 4) & 15
        qcb_div, qcb_mod = (m.d1 >> 8) & 15, (m.d1 >> 12) & 15
        qcr_div, qcr_mod = (m.d1 >> 16) & 15, (m.d1 >> 20) & 15
        at, al = mx > 0 or True, True
        at = my > 0; al = mx > 0
        atl = at and al; atr = at and mx < mbw - 1
        coef = np.zeros((24, 16), np.int64)
        for lev, pos in m.coef:
            coef[pos >> 4, pos & 15] = lev
        # ---- luma
        def dq(c, div, mod, i):
            return (c * LS[mod][i]) << div >> 4
        if cls == 1:     # intra 16x16
            pm = m.d0 & 3
            pred = pred_plane16(Y, x0, y0, 16, pm, at, al, atl)
            dc = np.array([coef[b, 0] for b in range(16)]).reshape(4, 4)
            f = hadamard4(dc)
            qp = 6 * qy_div + qy_mod
            if qp >= 36:
                dcy = (f * LS[qy_mod][0]) << (qy_div - 6)
            else:
                dcy = (f * LS[qy_mod][0] + (1 << (5 - qy_div))) >> (6 - qy_div)
            for b in range(16):
                bx, by = blk_xy(b)
                c = np.array([dq(coef[b, i], qy_div, qy_mod, i) for i in range(16)]).reshape(4, 4)
                c[0, 0] = dcy[b >> 2, b & 3]
                r = idct4(c)
                for yy in range(4):
                    for xx in range(4):
                        Y[y0 + by + yy, x0 + bx + xx] = clip1(pred[by + yy, bx + xx] + r[yy, xx])
        elif cls == 3:   # intra 4x4: decode order of the 16 blocks
            modes = [((m.d0 >> (4 * i)) & 15) for i in range(8)] + [((m.cf >> (4 * i)) & 15) for i in range(8)]
            order = [0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15]
            for b in order:
                bx, by = blk_xy(b)
                t = at or by > 0; l = al or bx > 0
                tl = (at or by > 0) and (al or bx > 0)
                # top-right: from the MB above (row 0), never from the right
                # MB, else only if already decoded (8x8 z-order)
                cx, cy = bx >> 2, by >> 2
                if cy == 0:
                    tr = at if cx < 3 else atr
                elif cx == 3:
                    tr = False
                else:
                    tr = order.index((cy - 1) * 4 + cx + 1) < order.index(b)
                pred = pred4x4(Y, x0 + bx, y0 + by, modes[b], t, l, tr, tl)
                c = np.array([dq(coef[b, i], qy_div, qy_mod, i) for i in range(16)]).reshape(4, 4)
                r = idct4(c)
                for yy in range(4):
                    for xx in range(4):
                        Y[y0 + by + yy, x0 + bx + xx] = clip1(pred[yy, xx] + r[yy, xx])
        if cls == 0:     # inter: partitions from the 0x101 feed
            RY, RU, RV = ref
            predY = np.zeros((16, 16), np.int64); predU = np.zeros((8, 8), np.int64); predV = np.zeros((8, 8), np.int64)
            for pi in range(0, len(m.inter), 2):
                mvw, geo = m.inter[pi], m.inter[pi + 1]
                # eighth-pel in the hardware word, quarter-pel for H.264
                mvx, mvy = s16(mvw & 0xffff) >> 1, s16(mvw >> 16) >> 1
                pw = 16 if geo & 0x100 else 8
                ph = 16 if geo & 0x1000 else 8
                px0 = 8 if geo & 0x400 else 0
                py0 = 8 if geo & 0x4000 else 0
                predY[py0:py0 + ph, px0:px0 + pw] = luma_mc(RY, x0 + px0, y0 + py0, mvx, mvy, pw, ph)
                predU[py0 // 2:(py0 + ph) // 2, px0 // 2:(px0 + pw) // 2] = chroma_mc(RU, (x0 + px0) // 2, (y0 + py0) // 2, mvx, mvy, pw // 2, ph // 2)
                predV[py0 // 2:(py0 + ph) // 2, px0 // 2:(px0 + pw) // 2] = chroma_mc(RV, (x0 + px0) // 2, (y0 + py0) // 2, mvx, mvy, pw // 2, ph // 2)
            for b in range(16):
                bx, by = blk_xy(b)
                c = np.array([dq(coef[b, i], qy_div, qy_mod, i) for i in range(16)]).reshape(4, 4)
                r = idct4(c)
                for yy in range(4):
                    for xx in range(4):
                        Y[y0 + by + yy, x0 + bx + xx] = clip1(predY[by + yy, bx + xx] + r[yy, xx])
            m.cpred = (predU, predV)
        else:
            m.cpred = None
        # ---- chroma
        for ci, (P, div, mod) in enumerate(((U, qcb_div, qcb_mod), (V, qcr_div, qcr_mod))):
            cx0, cy0 = 8 * mx, 8 * my
            pred = m.cpred[ci] if m.cpred else pred_plane16(P, cx0, cy0, 8, chroma_mode, at, al, atl, chroma=True)
            base = 20 - 4 * ci      # blocks 16-19 Cr, 20-23 Cb
            c0 = np.array([[coef[base + 0, 0], coef[base + 1, 0]], [coef[base + 2, 0], coef[base + 3, 0]]], np.int64)
            f = np.array([[1, 1], [1, -1]]) @ c0 @ np.array([[1, 1], [1, -1]])
            dcc = ((f * LS[mod][0]) << div) >> 5
            for b in range(4):
                bx, by = (b & 1) * 4, (b >> 1) * 4
                c = np.array([dq(coef[base + b, i], div, mod, i) for i in range(16)]).reshape(4, 4)
                c[0, 0] = dcc[b >> 1, b & 1]
                r = idct4(c)
                for yy in range(4):
                    for xx in range(4):
                        P[cy0 + by + yy, cx0 + bx + xx] = clip1(pred[by + yy, bx + xx] + r[yy, xx])
        info.append((cls, 6 * qy_div + qy_mod))
    return Y, U, V, info


def bs_of(g):
    """boundary strengths [dir][edge][segment] from a deblock register group"""
    nib = lambda v, i: (v >> (4 * i)) & 15
    v = [[nib(g[0xf5], i) for i in range(4)], [nib(g[0xe9], i) for i in range(4)],
         [nib(g[0xe9], 4 + i) for i in range(4)], [nib(g[0xe8], i) for i in range(4)]]
    h = [[nib(g[0xe8], 4 + i) for i in range(4)], [nib(g[0xe7], 4 + i) for i in range(4)],
         [nib(g[0xec], i) for i in range(4)], [nib(g[0xec], 4 + i) for i in range(4)]]
    return v, h

def filt_line(p, q, bs, qpav, chroma, offA=0, offB=0):
    """filter one line across an edge; p[0..2], q[0..2] nearest first"""
    iA = min(max(qpav + offA, 0), 51); iB = min(max(qpav + offB, 0), 51)
    a, b = ALPHA[iA], BETA[iB]
    p0, p1, q0, q1 = p[0], p[1], q[0], q[1]
    if not (abs(p0 - q0) < a and abs(p1 - p0) < b and abs(q1 - q0) < b):
        return p, q
    p = list(p); q = list(q)
    if bs < 4:
        tc0 = TC0[iA][bs - 1]
        if chroma:
            tc = tc0 + 1
            d = min(max((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tc), tc)
            p[0] = clip1(p0 + d); q[0] = clip1(q0 - d)
        else:
            p2, q2 = p[2], q[2]
            ap, aq = abs(p2 - p0), abs(q2 - q0)
            tc = tc0 + (ap < b) + (aq < b)
            d = min(max((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tc), tc)
            p[0] = clip1(p0 + d); q[0] = clip1(q0 - d)
            if ap < b: p[1] = p1 + min(max((p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1, -tc0), tc0)
            if aq < b: q[1] = q1 + min(max((q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1, -tc0), tc0)
    else:
        if chroma:
            p[0] = (2 * p1 + p0 + q1 + 2) >> 2; q[0] = (2 * q1 + q0 + p1 + 2) >> 2
        else:
            p2, q2, p3, q3 = p[2], q[2], p[3], q[3]
            small = abs(p0 - q0) < ((a >> 2) + 2)
            if abs(p2 - p0) < b and small:
                p[0] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3
                p[1] = (p2 + p1 + p0 + q0 + 2) >> 2
                p[2] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3
            else:
                p[0] = (2 * p1 + p0 + q1 + 2) >> 2
            if abs(q2 - q0) < b and small:
                q[0] = (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3
                q[1] = (p0 + q0 + q1 + q2 + 2) >> 2
                q[2] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3
            else:
                q[0] = (2 * q1 + q0 + p1 + 2) >> 2
    return p, q

def deblock(Y, U, V, mbs, mbw):
    qp = lambda m: (6 * (m.d1 & 15) + ((m.d1 >> 4) & 15), 6 * ((m.d1 >> 8) & 15) + ((m.d1 >> 12) & 15), 6 * ((m.d1 >> 16) & 15) + ((m.d1 >> 20) & 15))
    for n, m in enumerate(mbs):
        if m.dbk is None: continue
        mx, my = n % mbw, n // mbw
        x0, y0 = 16 * mx, 16 * my
        bv, bh = bs_of(m.dbk)
        q = qp(m)
        ql = qp(mbs[n - 1]) if mx else q
        qt = qp(mbs[n - mbw]) if my else q
        for dirn in (0, 1):
            for e in range(4):
                nb = (ql if dirn == 0 else qt) if e == 0 else q
                qav = [(q[i] + nb[i] + 1) >> 1 for i in range(3)]
                for k in range(16):
                    bs = (bv if dirn == 0 else bh)[e][k >> 2]
                    if not bs: continue
                    if dirn == 0:
                        xx, yy = x0 + 4 * e, y0 + k
                        P = [int(Y[yy, xx - 1 - i]) for i in range(4)]; Q = [int(Y[yy, xx + i]) for i in range(4)]
                        P, Q = filt_line(P, Q, bs, qav[0], False)
                        for i in range(3): Y[yy, xx - 1 - i] = P[i]; Y[yy, xx + i] = Q[i]
                    else:
                        xx, yy = x0 + k, y0 + 4 * e
                        P = [int(Y[yy - 1 - i, xx]) for i in range(4)]; Q = [int(Y[yy + i, xx]) for i in range(4)]
                        P, Q = filt_line(P, Q, bs, qav[0], False)
                        for i in range(3): Y[yy - 1 - i, xx] = P[i]; Y[yy + i, xx] = Q[i]
                # chroma: luma edges 0 and 2 map onto chroma edges 0 and 4
                if e in (0, 2):
                    ce = e * 2
                    for C, qi in ((U, 1), (V, 2)):
                        for k in range(8):
                            bs = (bv if dirn == 0 else bh)[e][k >> 1]
                            if not bs: continue
                            if dirn == 0:
                                xx, yy = x0 // 2 + ce, y0 // 2 + k
                                P = [int(C[yy, xx - 1 - i]) for i in range(2)] + [0, 0]; Q = [int(C[yy, xx + i]) for i in range(2)] + [0, 0]
                                P, Q = filt_line(P, Q, bs, qav[qi], True)
                                C[yy, xx - 1] = P[0]; C[yy, xx] = Q[0]
                            else:
                                xx, yy = x0 // 2 + k, y0 // 2 + ce
                                P = [int(C[yy - 1 - i, xx]) for i in range(2)] + [0, 0]; Q = [int(C[yy + i, xx]) for i in range(2)] + [0, 0]
                                P, Q = filt_line(P, Q, bs, qav[qi], True)
                                C[yy - 1, xx] = P[0]; C[yy, xx] = Q[0]

# ---------------------------------------------------------------- compare
def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)

if __name__ == '__main__':
    width, height, mbs, out = parse(sys.argv[1])
    Y, U, V, info = reconstruct(width, height, mbs)
    if len(sys.argv) > 2:
        ref = np.fromfile(sys.argv[2], np.uint8)
        fi = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        fs = width * height * 3 // 2
        r = ref[fi * fs:(fi + 1) * fs]
        RY = r[:width * height].reshape(height, width)
        print('luma PSNR %.2f dB, exact %d/%d' % (psnr(Y, RY), (Y == RY).sum(), Y.size))
        mbw = width // 16
        bad = [n for n in range(len(mbs)) if not (Y[16*(n//mbw):16*(n//mbw)+16, 16*(n%mbw):16*(n%mbw)+16] == RY[16*(n//mbw):16*(n//mbw)+16, 16*(n%mbw):16*(n%mbw)+16]).all()]
        print('first mismatching MBs', bad[:20])
