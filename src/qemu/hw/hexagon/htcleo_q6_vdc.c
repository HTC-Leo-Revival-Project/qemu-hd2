/*
 * QSD8250 QDSP6 subsystem: the "Janus" video decoder core (VDC), H.264
 *
 * The DSP1 firmware parses the bitstream itself (headers, CAVLC) and hands
 * the VDC one command buffer per picture describing every macroblock; the
 * VDC does the pixel work: inverse quantisation and transforms, intra and
 * inter prediction, the deblocking filter, and writes the picture out.
 * The format was reverse-engineered from the firmware's command buffers
 * against the JM reference decoder (tools/vdc/vdcsim.py is the model this
 * code follows, and the place the RE notes live).
 *
 * Command buffer: a header word, then packets until a zero word, header
 * byte = opcode:
 *   04 rrrrrr v          write register r
 *   03 rrr sss v w       write registers r, s
 *   02 rr ss tt u v w    write registers r, s, t
 *   08 rrrrrr a n        feed n words (low 16 bits of n) from a to r
 *   0a                   end of a pipeline step
 *   00 xxxxxx            synchronisation, no payload
 *   01                   end of the command buffer (after the last
 *                        picture's steps, which set register 10fc to 1)
 * Word 2 of the header is height << 16 | width.  The hardware is a
 * pipeline: in step s it takes the header of MB s, the coefficients of
 * MB s-1, reconstructs MB s-2 and deblocks MB s-3.
 *
 * Registers (H.264):
 *   7a / 72   reference picture luma / chroma
 *   82        current MB (row << 16 | column), with 8b
 *   101 feed  inter partitions: pairs of (motion vector, geometry); the
 *             vector is y << 16 | x in eighth pels (quarter-pel H.264
 *             vectors doubled); geometry bit 8 width 16 (else 8), bit 12
 *             height 16 (else 8), bit 10 x + 8, bit 14 y + 8
 *   100b/100c feed  coefficient list of the next MB: (int16 level, int16
 *             position), position = block << 4 | raster index, blocks
 *             0-15 luma (raster order), 16-19 Cr, 20-23 Cb; levels are
 *             not dequantised; Intra16x16 and chroma DC levels sit at
 *             the DC position of the block whose index they have
 *   2c feed   output DMA programs; words 3 and 9 are the picture's luma
 *             and chroma addresses
 *   cd        bits 0-15 luma 4x4 blocks coded, 16-23 chroma AC coded,
 *             24/25 chroma DC coded, 26 Intra16x16 DC coded
 *   d1        QP: luma /6, %6, Cb /6, %6, Cr /6, %6 (nibbles)
 *   d2        bits 0-7 MB column, 8-15 type (0 inter, 1 Intra16x16,
 *             3 Intra4x4), 16-17 intra chroma mode, 24-29 neighbours
 *             missing, 31 no residual
 *   d0 / cf   Intra4x4 modes of blocks 0-7 / 8-15 (nibbles, raster), or
 *             the Intra16x16 mode in d0
 *   f5, e9, e8 boundary strengths of the four vertical edges (nibble per
 *             4-pixel segment): f5[15:0] edge 0, e9[15:0] edge 1,
 *             e9[31:16] edge 2, e8[15:0] edge 3
 *   e8, e7, ec horizontal edges: e8[31:16] edge 0, e7[31:16] edge 1,
 *             ec[15:0] edge 2, ec[31:16] edge 3
 *   e5        deblocking line slot ((row & 3) << 8 | column); the qPav
 *             registers (ea-f2) are the standard averages, recomputed here
 *
 * Pictures are stored NV21-like: luma, then interleaved chroma at
 * luma + width * height.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "system/physmem.h"
#include "qemu/bswap.h"
#include "hw/hexagon/htcleo_q6_vdc.h"

#define MAX_MB          (1280 / 16 * 736 / 16)
#define MAX_COEF        (24 * 16)
#define MAX_PART        16          /* words of the inter feed */
#define CMD_MAX         0x40000     /* words */

/* the chroma byte order of the stored pictures: Cr first (NV21) */
#define CHROMA_V_FIRST  1

typedef struct VdcMB {
    uint32_t d2, cd, d1, d0, cf;
    int ncoef;
    uint32_t coef[MAX_COEF];
    int ninter;
    uint32_t inter[MAX_PART];
    bool have_dbk;
    uint32_t e7, e8, e9, ec, f5;
} VdcMB;

typedef struct VdcPic {
    int w, h;
    uint8_t *y, *u, *v;
} VdcPic;

static const int level_scale[6][16] = {
    {10, 13, 10, 13, 13, 16, 13, 16, 10, 13, 10, 13, 13, 16, 13, 16},
    {11, 14, 11, 14, 14, 18, 14, 18, 11, 14, 11, 14, 14, 18, 14, 18},
    {13, 16, 13, 16, 16, 20, 16, 20, 13, 16, 13, 16, 16, 20, 16, 20},
    {14, 18, 14, 18, 18, 23, 18, 23, 14, 18, 14, 18, 18, 23, 18, 23},
    {16, 20, 16, 20, 20, 25, 20, 25, 16, 20, 16, 20, 20, 25, 20, 25},
    {18, 23, 18, 23, 23, 29, 23, 29, 18, 23, 18, 23, 23, 29, 23, 29},
};

static const uint8_t alpha_tab[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 5, 6, 7, 8, 9,
    10, 12, 13, 15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71,
    80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255,
};
static const uint8_t beta_tab[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14,
    14, 15, 15, 16, 16, 17, 17, 18, 18,
};
static const uint8_t tc0_tab[52][3] = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 1},
    {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 1}, {1, 1, 1},
    {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 2}, {1, 1, 2}, {1, 1, 2},
    {1, 1, 2}, {1, 2, 3}, {1, 2, 3}, {2, 2, 3}, {2, 2, 4}, {2, 3, 4},
    {2, 3, 4}, {3, 3, 5}, {3, 4, 6}, {3, 4, 6}, {4, 5, 7}, {4, 5, 8},
    {4, 6, 9}, {5, 7, 10}, {6, 8, 11}, {6, 8, 13}, {7, 10, 14},
    {8, 11, 16}, {9, 12, 18}, {10, 13, 20}, {11, 15, 23}, {13, 17, 25},
};

/* Intra4x4 blocks in decoding (8x8 z) order, raster indices */
static const int blk_order[16] = {
    0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15,
};

static inline int clip1(int x)
{
    return x < 0 ? 0 : x > 255 ? 255 : x;
}

static inline int clip3(int lo, int hi, int x)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static inline int s16(uint32_t v)
{
    return (int16_t)(v & 0xffff);
}

/* ---------------------------------------------------------------- */
/* transforms                                                        */
/* ---------------------------------------------------------------- */

static void idct4(int c[16], int r[16])
{
    int t[16];

    for (int i = 0; i < 4; i++) {
        int *b = &c[4 * i];
        int e = b[0] + b[2], f = b[0] - b[2];
        int g = (b[1] >> 1) - b[3], h = b[1] + (b[3] >> 1);

        t[4 * i + 0] = e + h;
        t[4 * i + 1] = f + g;
        t[4 * i + 2] = f - g;
        t[4 * i + 3] = e - h;
    }
    for (int j = 0; j < 4; j++) {
        int e = t[j] + t[8 + j], f = t[j] - t[8 + j];
        int g = (t[4 + j] >> 1) - t[12 + j], h = t[4 + j] + (t[12 + j] >> 1);

        r[j] = (e + h + 32) >> 6;
        r[4 + j] = (f + g + 32) >> 6;
        r[8 + j] = (f - g + 32) >> 6;
        r[12 + j] = (e - h + 32) >> 6;
    }
}

static void hadamard4(const int c[16], int f[16])
{
    static const int h[4][4] = {
        {1, 1, 1, 1}, {1, 1, -1, -1}, {1, -1, -1, 1}, {1, -1, 1, -1},
    };
    int t[16];

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int s = 0;
            for (int k = 0; k < 4; k++) {
                s += h[i][k] * c[4 * k + j];
            }
            t[4 * i + j] = s;
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int s = 0;
            for (int k = 0; k < 4; k++) {
                s += t[4 * i + k] * h[k][j];
            }
            f[4 * i + j] = s;
        }
    }
}

static inline int dequant(int c, int div, int mod, int i)
{
    return (c * 16 * level_scale[mod][i]) << div >> 4;
}

/* add a 4x4 residual to the prediction already in the plane */
static void add_block(uint8_t *p, int stride, const int pred[16],
                      int c[16])
{
    int r[16];

    idct4(c, r);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            p[y * stride + x] = clip1(pred[4 * y + x] + r[4 * y + x]);
        }
    }
}

/* ---------------------------------------------------------------- */
/* intra prediction                                                  */
/* ---------------------------------------------------------------- */

static void pred4x4(const uint8_t *s, int stride, int mode, bool at,
                    bool al, bool atr, bool atl, int o[16])
{
#define P(x, y) ((int)s[(y) * stride + (x)])
    int T[8], L[4], tl = 0;

    for (int i = 0; i < 4; i++) {
        T[i] = at ? P(i, -1) : 0;
        L[i] = al ? P(-1, i) : 0;
    }
    for (int i = 4; i < 8; i++) {
        T[i] = atr ? P(i, -1) : T[3];
    }
    if (atl) {
        tl = P(-1, -1);
    }
#undef P
    /* px(x, -1): top row with x = -1 the corner; px(-1, y): left column */
#define TOP(x) ((x) < 0 ? tl : T[x])
#define LEFT(y) ((y) < 0 ? tl : L[y])
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int v = 128, z;

            switch (mode) {
            case 0:
                v = T[x];
                break;
            case 1:
                v = L[y];
                break;
            case 2:
                if (at && al) {
                    v = (T[0] + T[1] + T[2] + T[3] + L[0] + L[1] + L[2] +
                         L[3] + 4) >> 3;
                } else if (al) {
                    v = (L[0] + L[1] + L[2] + L[3] + 2) >> 2;
                } else if (at) {
                    v = (T[0] + T[1] + T[2] + T[3] + 2) >> 2;
                }
                break;
            case 3:
                v = (x == 3 && y == 3) ? (T[6] + 3 * T[7] + 2) >> 2 :
                    (T[x + y] + 2 * T[x + y + 1] + T[x + y + 2] + 2) >> 2;
                break;
            case 4:
                if (x > y) {
                    v = (TOP(x - y - 2) + 2 * TOP(x - y - 1) + TOP(x - y) +
                         2) >> 2;
                } else if (x < y) {
                    v = (LEFT(y - x - 2) + 2 * LEFT(y - x - 1) +
                         LEFT(y - x) + 2) >> 2;
                } else {
                    v = (T[0] + 2 * tl + L[0] + 2) >> 2;
                }
                break;
            case 5:
                z = 2 * x - y;
                if (z >= 0 && !(z & 1)) {
                    v = (TOP(x - (y >> 1) - 1) + TOP(x - (y >> 1)) + 1) >> 1;
                } else if (z >= 0) {
                    v = (TOP(x - (y >> 1) - 2) + 2 * TOP(x - (y >> 1) - 1) +
                         TOP(x - (y >> 1)) + 2) >> 2;
                } else if (z == -1) {
                    v = (L[0] + 2 * tl + T[0] + 2) >> 2;
                } else {
                    v = (LEFT(y - 1) + 2 * LEFT(y - 2) + LEFT(y - 3) + 2) >> 2;
                }
                break;
            case 6:
                z = 2 * y - x;
                if (z >= 0 && !(z & 1)) {
                    v = (LEFT(y - (x >> 1) - 1) + LEFT(y - (x >> 1)) + 1) >> 1;
                } else if (z >= 0) {
                    v = (LEFT(y - (x >> 1) - 2) + 2 * LEFT(y - (x >> 1) - 1) +
                         LEFT(y - (x >> 1)) + 2) >> 2;
                } else if (z == -1) {
                    v = (L[0] + 2 * tl + T[0] + 2) >> 2;
                } else {
                    v = (TOP(x - 1) + 2 * TOP(x - 2) + TOP(x - 3) + 2) >> 2;
                }
                break;
            case 7:
                if (!(y & 1)) {
                    v = (T[x + (y >> 1)] + T[x + (y >> 1) + 1] + 1) >> 1;
                } else {
                    v = (T[x + (y >> 1)] + 2 * T[x + (y >> 1) + 1] +
                         T[x + (y >> 1) + 2] + 2) >> 2;
                }
                break;
            case 8:
                z = x + 2 * y;
                if (z > 5) {
                    v = L[3];
                } else if (z == 5) {
                    v = (L[2] + 3 * L[3] + 2) >> 2;
                } else if (!(z & 1)) {
                    v = (L[y + (x >> 1)] + L[y + (x >> 1) + 1] + 1) >> 1;
                } else {
                    v = (L[y + (x >> 1)] + 2 * L[y + (x >> 1) + 1] +
                         L[y + (x >> 1) + 2] + 2) >> 2;
                }
                break;
            }
            o[4 * y + x] = v;
        }
    }
#undef TOP
#undef LEFT
}

/*
 * 16x16 luma (modes 0 V, 1 H, 2 DC, 3 plane) or 8x8 chroma (0 DC, 1 H,
 * 2 V, 3 plane) prediction into o[n * n]
 */
static void pred_block(const uint8_t *s, int stride, int n, int mode,
                       bool at, bool al, bool chroma, int *o)
{
#define P(x, y) ((int)s[(y) * stride + (x)])
    if (chroma) {
        static const int map[4] = { 2, 1, 0, 3 };
        mode = map[mode];
    }
    if (mode == 0) {
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                o[y * n + x] = P(x, -1);
            }
        }
    } else if (mode == 1) {
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                o[y * n + x] = P(-1, y);
            }
        }
    } else if (mode == 2 && !chroma) {
        int st = 0, sl = 0, dc = 128;

        for (int i = 0; i < 16; i++) {
            st += at ? P(i, -1) : 0;
            sl += al ? P(-1, i) : 0;
        }
        if (at && al) {
            dc = (st + sl + 16) >> 5;
        } else if (al) {
            dc = (sl + 8) >> 4;
        } else if (at) {
            dc = (st + 8) >> 4;
        }
        for (int i = 0; i < 256; i++) {
            o[i] = dc;
        }
    } else if (mode == 2) {
        for (int by = 0; by < 2; by++) {
            for (int bx = 0; bx < 2; bx++) {
                int st = 0, sl = 0, dc = 128;

                for (int i = 0; i < 4; i++) {
                    st += at ? P(4 * bx + i, -1) : 0;
                    sl += al ? P(-1, 4 * by + i) : 0;
                }
                if (bx == by) {
                    if (at && al) {
                        dc = (st + sl + 4) >> 3;
                    } else if (al) {
                        dc = (sl + 2) >> 2;
                    } else if (at) {
                        dc = (st + 2) >> 2;
                    }
                } else if (bx == 1) {
                    if (at) {
                        dc = (st + 2) >> 2;
                    } else if (al) {
                        dc = (sl + 2) >> 2;
                    }
                } else {
                    if (al) {
                        dc = (sl + 2) >> 2;
                    } else if (at) {
                        dc = (st + 2) >> 2;
                    }
                }
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) {
                        o[(4 * by + y) * 8 + 4 * bx + x] = dc;
                    }
                }
            }
        }
    } else {
        int h = 0, v = 0, a, b, c, half = n / 2, k = chroma ? 34 : 5;

        for (int i = 0; i < half; i++) {
            h += (i + 1) * (P(half + i, -1) - P(half - 2 - i, -1));
            v += (i + 1) * (P(-1, half + i) - P(-1, half - 2 - i));
        }
        a = 16 * (P(-1, n - 1) + P(n - 1, -1));
        b = (k * h + 32) >> 6;
        c = (k * v + 32) >> 6;
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                o[y * n + x] = clip1((a + b * (x - half + 1) +
                                      c * (y - half + 1) + 16) >> 5);
            }
        }
    }
#undef P
}

/* ---------------------------------------------------------------- */
/* inter prediction                                                  */
/* ---------------------------------------------------------------- */

static inline int refpx(const VdcPic *r, const uint8_t *pl, int w, int h,
                        int x, int y)
{
    return pl[clip3(0, h - 1, y) * w + clip3(0, w - 1, x)];
}

static inline int tap6(int a, int b, int c, int d, int e, int f)
{
    return a - 5 * b + 20 * c + 20 * d - 5 * e + f;
}

static int luma_sample(const VdcPic *r, int X, int Y, int fx, int fy)
{
    const uint8_t *pl = r->y;
    int w = r->w, h = r->h;
#define G(x, y) refpx(r, pl, w, h, (x), (y))
#define BH(x, y) tap6(G((x) - 2, y), G((x) - 1, y), G(x, y), G((x) + 1, y), \
                      G((x) + 2, y), G((x) + 3, y))
#define HV(x, y) tap6(G(x, (y) - 2), G(x, (y) - 1), G(x, y), G(x, (y) + 1), \
                      G(x, (y) + 2), G(x, (y) + 3))
#define C1(v) clip1(((v) + 16) >> 5)
    int g = G(X, Y), b, hh, j, s, m;

    if (!fx && !fy) {
        return g;
    }
    b = C1(BH(X, Y));
    hh = C1(HV(X, Y));
    if (fx == 2 && !fy) {
        return b;
    }
    if (!fx && fy == 2) {
        return hh;
    }
    if (fx == 1 && !fy) {
        return (g + b + 1) >> 1;
    }
    if (fx == 3 && !fy) {
        return (b + G(X + 1, Y) + 1) >> 1;
    }
    if (!fx && fy == 1) {
        return (g + hh + 1) >> 1;
    }
    if (!fx && fy == 3) {
        return (hh + G(X, Y + 1) + 1) >> 1;
    }
    j = clip1((tap6(BH(X, Y - 2), BH(X, Y - 1), BH(X, Y), BH(X, Y + 1),
                    BH(X, Y + 2), BH(X, Y + 3)) + 512) >> 10);
    s = C1(BH(X, Y + 1));
    m = C1(HV(X + 1, Y));
    switch (fy * 4 + fx) {
    case 2 * 4 + 2: return j;
    case 1 * 4 + 2: return (b + j + 1) >> 1;
    case 3 * 4 + 2: return (j + s + 1) >> 1;
    case 2 * 4 + 1: return (hh + j + 1) >> 1;
    case 2 * 4 + 3: return (j + m + 1) >> 1;
    case 1 * 4 + 1: return (b + hh + 1) >> 1;
    case 1 * 4 + 3: return (b + m + 1) >> 1;
    case 3 * 4 + 1: return (hh + s + 1) >> 1;
    default:        return (s + m + 1) >> 1;
    }
#undef G
#undef BH
#undef HV
#undef C1
}

static void inter_pred(const VdcPic *ref, int x0, int y0, const VdcMB *m,
                       int py[256], int pu[64], int pv[64])
{
    for (int i = 0; i + 1 < m->ninter; i += 2) {
        uint32_t mv = m->inter[i], geo = m->inter[i + 1];
        /* eighth pels in the hardware word, quarter pels in H.264 */
        int mvx = s16(mv) >> 1, mvy = s16(mv >> 16) >> 1;
        int pw = geo & 0x100 ? 16 : 8, ph = geo & 0x1000 ? 16 : 8;
        int px = geo & 0x400 ? 8 : 0, pyo = geo & 0x4000 ? 8 : 0;

        for (int y = 0; y < ph; y++) {
            for (int x = 0; x < pw; x++) {
                int X = x0 + px + x, Y = y0 + pyo + y;

                py[(pyo + y) * 16 + px + x] =
                    luma_sample(ref, X + (mvx >> 2), Y + (mvy >> 2),
                                mvx & 3, mvy & 3);
            }
        }
        for (int y = 0; y < ph / 2; y++) {
            for (int x = 0; x < pw / 2; x++) {
                int cx = (x0 + px) / 2 + x + (mvx >> 3);
                int cy = (y0 + pyo) / 2 + y + (mvy >> 3);
                int fx = mvx & 7, fy = mvy & 7;
                int w = ref->w / 2, h = ref->h / 2;
                int o = ((pyo / 2) + y) * 8 + px / 2 + x;

                for (int c = 0; c < 2; c++) {
                    const uint8_t *pl = c ? ref->v : ref->u;
                    int a = refpx(ref, pl, w, h, cx, cy);
                    int b = refpx(ref, pl, w, h, cx + 1, cy);
                    int cc = refpx(ref, pl, w, h, cx, cy + 1);
                    int d = refpx(ref, pl, w, h, cx + 1, cy + 1);
                    int v = ((8 - fx) * (8 - fy) * a + fx * (8 - fy) * b +
                             (8 - fx) * fy * cc + fx * fy * d + 32) >> 6;

                    (c ? pv : pu)[o] = v;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* macroblock reconstruction                                         */
/* ---------------------------------------------------------------- */

static void recon_mb(VdcPic *p, const VdcPic *ref, int n, const VdcMB *m)
{
    int mbw = p->w / 16, mx = n % mbw, my = n / mbw;
    int x0 = 16 * mx, y0 = 16 * my, ws = p->w, cs = p->w / 2;
    int cls = (m->d2 >> 8) & 0xff, cmode = (m->d2 >> 16) & 3;
    int qd = m->d1 & 15, qm = (m->d1 >> 4) & 15;
    int qcd[2] = { (m->d1 >> 8) & 15, (m->d1 >> 16) & 15 };
    int qcm[2] = { (m->d1 >> 12) & 15, (m->d1 >> 20) & 15 };
    bool at = my > 0, al = mx > 0, atr = at && mx < mbw - 1;
    static int coef[24][16];
    int py[256], pu[64], pv[64], *pc[2] = { pu, pv };
    bool have_cpred = false;

    memset(coef, 0, sizeof(coef));
    for (int i = 0; i < m->ncoef; i++) {
        int pos = (m->coef[i] >> 16) & 0xffff;

        if (pos < MAX_COEF) {
            coef[pos >> 4][pos & 15] = s16(m->coef[i]);
        }
    }

    if (cls == 1) {
        int dc[16], f[16], qp = 6 * qd + qm;

        pred_block(p->y + y0 * ws + x0, ws, 16, m->d0 & 3, at, al, false, py);
        for (int b = 0; b < 16; b++) {
            dc[b] = coef[b][0];
        }
        hadamard4(dc, f);
        for (int b = 0; b < 16; b++) {
            int bx = (b & 3) * 4, by = (b >> 2) * 4, c[16], pr[16];

            for (int i = 0; i < 16; i++) {
                c[i] = dequant(coef[b][i], qd, qm, i);
            }
            if (qp >= 36) {
                c[0] = (f[b] * 16 * level_scale[qm][0]) << (qd - 6);
            } else {
                c[0] = (f[b] * 16 * level_scale[qm][0] +
                        (1 << (5 - qd))) >> (6 - qd);
            }
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    pr[4 * y + x] = py[(by + y) * 16 + bx + x];
                }
            }
            add_block(p->y + (y0 + by) * ws + x0 + bx, ws, pr, c);
        }
    } else if (cls == 3) {
        for (int k = 0; k < 16; k++) {
            int b = blk_order[k], bx = (b & 3), by = (b >> 2);
            int mode = b < 8 ? (m->d0 >> (4 * b)) & 15 :
                               (m->cf >> (4 * (b - 8))) & 15;
            bool t = at || by > 0, l = al || bx > 0, tl = t && l, tr;
            int c[16], pr[16];
            uint8_t *dst = p->y + (y0 + 4 * by) * ws + x0 + 4 * bx;

            if (by == 0) {
                tr = bx < 3 ? at : atr;
            } else if (bx == 3) {
                tr = false;
            } else {
                int nb = (by - 1) * 4 + bx + 1, kn = 0;
                while (blk_order[kn] != nb) {
                    kn++;
                }
                tr = kn < k;
            }
            pred4x4(dst, ws, mode, t, l, tr, tl, pr);
            for (int i = 0; i < 16; i++) {
                c[i] = dequant(coef[b][i], qd, qm, i);
            }
            add_block(dst, ws, pr, c);
        }
    } else {
        inter_pred(ref, x0, y0, m, py, pu, pv);
        have_cpred = true;
        for (int b = 0; b < 16; b++) {
            int bx = (b & 3) * 4, by = (b >> 2) * 4, c[16], pr[16];

            for (int i = 0; i < 16; i++) {
                c[i] = dequant(coef[b][i], qd, qm, i);
            }
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    pr[4 * y + x] = py[(by + y) * 16 + bx + x];
                }
            }
            add_block(p->y + (y0 + by) * ws + x0 + bx, ws, pr, c);
        }
    }

    /* chroma: coefficient blocks 16-19 are Cr, 20-23 Cb */
    for (int ci = 0; ci < 2; ci++) {
        uint8_t *pl = ci ? p->v : p->u;
        int base = ci ? 16 : 20, div = qcd[ci], mod = qcm[ci];
        int cx0 = 8 * mx, cy0 = 8 * my, f[4];
        int c0 = coef[base][0], c1 = coef[base + 1][0];
        int c2 = coef[base + 2][0], c3 = coef[base + 3][0];

        if (!have_cpred) {
            pred_block(pl + cy0 * cs + cx0, cs, 8, cmode, at, al, true,
                       pc[ci]);
        }
        f[0] = c0 + c1 + c2 + c3;
        f[1] = c0 - c1 + c2 - c3;
        f[2] = c0 + c1 - c2 - c3;
        f[3] = c0 - c1 - c2 + c3;
        for (int b = 0; b < 4; b++) {
            int bx = (b & 1) * 4, by = (b >> 1) * 4, c[16], pr[16];

            for (int i = 0; i < 16; i++) {
                c[i] = dequant(coef[base + b][i], div, mod, i);
            }
            c[0] = ((f[b] * 16 * level_scale[mod][0]) << div) >> 5;
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    pr[4 * y + x] = pc[ci][(by + y) * 8 + bx + x];
                }
            }
            add_block(pl + (cy0 + by) * cs + cx0 + bx, cs, pr, c);
        }
    }
}

/* ---------------------------------------------------------------- */
/* deblocking                                                        */
/* ---------------------------------------------------------------- */

static void filter_line(uint8_t *s, int step, int bs, int qpav, bool chroma)
{
    int a = alpha_tab[qpav], b = beta_tab[qpav];
    int p0 = s[-step], p1 = s[-2 * step], q0 = s[0], q1 = s[step];

    if (!(abs(p0 - q0) < a && abs(p1 - p0) < b && abs(q1 - q0) < b)) {
        return;
    }
    if (bs < 4) {
        int tc0 = tc0_tab[qpav][bs - 1], tc, d;

        if (chroma) {
            tc = tc0 + 1;
            d = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            s[-step] = clip1(p0 + d);
            s[0] = clip1(q0 - d);
        } else {
            int p2 = s[-3 * step], q2 = s[2 * step];
            int ap = abs(p2 - p0), aq = abs(q2 - q0);

            tc = tc0 + (ap < b) + (aq < b);
            d = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            s[-step] = clip1(p0 + d);
            s[0] = clip1(q0 - d);
            if (ap < b) {
                s[-2 * step] = p1 + clip3(-tc0, tc0,
                    (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1);
            }
            if (aq < b) {
                s[step] = q1 + clip3(-tc0, tc0,
                    (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1);
            }
        }
    } else if (chroma) {
        s[-step] = (2 * p1 + p0 + q1 + 2) >> 2;
        s[0] = (2 * q1 + q0 + p1 + 2) >> 2;
    } else {
        int p2 = s[-3 * step], q2 = s[2 * step];
        int p3 = s[-4 * step], q3 = s[3 * step];
        bool small = abs(p0 - q0) < ((a >> 2) + 2);

        if (abs(p2 - p0) < b && small) {
            s[-step] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
            s[-2 * step] = (p2 + p1 + p0 + q0 + 2) >> 2;
            s[-3 * step] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
        } else {
            s[-step] = (2 * p1 + p0 + q1 + 2) >> 2;
        }
        if (abs(q2 - q0) < b && small) {
            s[0] = (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3;
            s[step] = (p0 + q0 + q1 + q2 + 2) >> 2;
            s[2 * step] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
        } else {
            s[0] = (2 * q1 + q0 + p1 + 2) >> 2;
        }
    }
}

static void mb_qp(const VdcMB *m, int q[3])
{
    q[0] = 6 * (m->d1 & 15) + ((m->d1 >> 4) & 15);
    q[1] = 6 * ((m->d1 >> 8) & 15) + ((m->d1 >> 12) & 15);
    q[2] = 6 * ((m->d1 >> 16) & 15) + ((m->d1 >> 20) & 15);
}

static void deblock_mb(VdcPic *p, const VdcMB *mbs, int n)
{
    const VdcMB *m = &mbs[n];
    int mbw = p->w / 16, mx = n % mbw, my = n / mbw;
    int x0 = 16 * mx, y0 = 16 * my, ws = p->w, cs = p->w / 2;
    int bs[2][4][4], q[3], ql[3], qt[3];

#define NIB(v, i) (((v) >> (4 * (i))) & 15)
    for (int i = 0; i < 4; i++) {
        bs[0][0][i] = NIB(m->f5, i);
        bs[0][1][i] = NIB(m->e9, i);
        bs[0][2][i] = NIB(m->e9, 4 + i);
        bs[0][3][i] = NIB(m->e8, i);
        bs[1][0][i] = NIB(m->e8, 4 + i);
        bs[1][1][i] = NIB(m->e7, 4 + i);
        bs[1][2][i] = NIB(m->ec, i);
        bs[1][3][i] = NIB(m->ec, 4 + i);
    }
#undef NIB
    mb_qp(m, q);
    if (mx) {
        mb_qp(&mbs[n - 1], ql);
    } else {
        memcpy(ql, q, sizeof(ql));
    }
    if (my) {
        mb_qp(&mbs[n - mbw], qt);
    } else {
        memcpy(qt, q, sizeof(qt));
    }
    for (int dir = 0; dir < 2; dir++) {
        for (int e = 0; e < 4; e++) {
            const int *nb = e ? q : dir ? qt : ql;
            int qav[3];

            for (int i = 0; i < 3; i++) {
                qav[i] = (q[i] + nb[i] + 1) >> 1;
            }
            for (int k = 0; k < 16; k++) {
                int b = bs[dir][e][k >> 2];

                if (!b) {
                    continue;
                }
                if (!dir) {
                    filter_line(p->y + (y0 + k) * ws + x0 + 4 * e, 1, b,
                                qav[0], false);
                } else {
                    filter_line(p->y + (y0 + 4 * e) * ws + x0 + k, ws, b,
                                qav[0], false);
                }
            }
            if (e & 1) {
                continue;
            }
            for (int c = 0; c < 2; c++) {
                uint8_t *pl = c ? p->v : p->u;

                for (int k = 0; k < 8; k++) {
                    int b = bs[dir][e][k >> 1];

                    if (!b) {
                        continue;
                    }
                    if (!dir) {
                        filter_line(pl + (y0 / 2 + k) * cs + x0 / 2 + 2 * e,
                                    1, b, qav[1 + c], true);
                    } else {
                        filter_line(pl + (y0 / 2 + 2 * e) * cs + x0 / 2 + k,
                                    cs, b, qav[1 + c], true);
                    }
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* picture I/O                                                       */
/* ---------------------------------------------------------------- */

static void pic_alloc(VdcPic *p, int w, int h)
{
    p->w = w;
    p->h = h;
    p->y = g_malloc0(w * h);
    p->u = g_malloc0(w * h / 4);
    p->v = g_malloc0(w * h / 4);
}

static void pic_free(VdcPic *p)
{
    g_free(p->y);
    g_free(p->u);
    g_free(p->v);
}

static void pic_read(VdcPic *p, uint32_t luma, uint32_t chroma)
{
    int n = p->w * p->h / 4;
    g_autofree uint8_t *c = g_malloc(2 * n);

    physical_memory_read(luma, p->y, p->w * p->h);
    physical_memory_read(chroma, c, 2 * n);
    for (int i = 0; i < n; i++) {
        p->v[i] = c[2 * i + !CHROMA_V_FIRST];
        p->u[i] = c[2 * i + CHROMA_V_FIRST];
    }
}

static void pic_write(const VdcPic *p, uint32_t luma, uint32_t chroma)
{
    int n = p->w * p->h / 4;
    g_autofree uint8_t *c = g_malloc(2 * n);

    for (int i = 0; i < n; i++) {
        c[2 * i + !CHROMA_V_FIRST] = p->v[i];
        c[2 * i + CHROMA_V_FIRST] = p->u[i];
    }
    physical_memory_write(luma, p->y, p->w * p->h);
    physical_memory_write(chroma, c, 2 * n);
}

/* ---------------------------------------------------------------- */
/* command buffer                                                    */
/* ---------------------------------------------------------------- */

static int packet_len(uint32_t h)
{
    switch (h >> 24) {
    case 0x04: return 2;
    case 0x03: return 3;
    case 0x02: return 4;
    case 0x08: return 3;
    default: return 1;
    }
}

int htcleo_vdc_decode(uint32_t addr, bool trace, const char *dump_yuv)
{
    g_autofree uint32_t *w = g_new0(uint32_t, CMD_MAX);
    g_autofree VdcMB *mbs = g_new0(VdcMB, MAX_MB);
    g_autofree uint32_t *pend = g_new0(uint32_t, MAX_COEF);
    g_autofree uint32_t *prev = g_new0(uint32_t, MAX_COEF);
    uint32_t regs[0x200] = { 0 };
    uint32_t ref_y = 0, ref_c = 0, out_y = 0, out_c = 0;
    int npend = 0, nprev = 0, nmb = 0, ndbk = 0, cur = -1, width, height;
    bool written[0x200] = { false };
    VdcPic pic, ref;

    physical_memory_read(addr, w, CMD_MAX * 4);
    height = le32_to_cpu(w[2]) >> 16;
    width = le32_to_cpu(w[2]) & 0xffff;
    if (!width || !height || width > 1280 || height > 736 ||
        (width | height) & 15) {
        qemu_log_mask(LOG_UNIMP, "q6-vdc: unsupported picture %dx%d\n",
                      width, height);
        return -1;
    }

    for (int i = 1; i < CMD_MAX && w[i]; ) {
        uint32_t h = le32_to_cpu(w[i]), op = h >> 24;
        int len = packet_len(h);
        uint32_t r[3] = { 0 }, v[3] = { 0 };
        int nr = 0;

        if (op == 0x01) {
            break;
        }

        if (i + len > CMD_MAX) {
            break;
        }
        switch (op) {
        case 0x04:
            r[0] = h & 0xffffff;
            nr = 1;
            break;
        case 0x03:
            r[0] = (h >> 12) & 0xfff;
            r[1] = h & 0xfff;
            nr = 2;
            break;
        case 0x02:
            r[0] = (h >> 16) & 0xff;
            r[1] = (h >> 8) & 0xff;
            r[2] = h & 0xff;
            nr = 3;
            break;
        case 0x08: {
            uint32_t reg = h & 0xffffff, a = le32_to_cpu(w[i + 1]);
            uint32_t cnt = le32_to_cpu(w[i + 2]) & 0xffff;

            if (reg == 0x100b || reg == 0x100c) {
                npend = MIN(cnt, MAX_COEF);
                physical_memory_read(a, pend, npend * 4);
            } else if (reg == 0x101 && cur >= 0 && cur < MAX_MB) {
                mbs[cur].ninter = MIN(cnt, MAX_PART);
                physical_memory_read(a, mbs[cur].inter, mbs[cur].ninter * 4);
            } else if (reg == 0x2c && !out_y && cnt >= 10) {
                uint32_t d[10];

                physical_memory_read(a, d, sizeof(d));
                out_y = le32_to_cpu(d[3]);
                out_c = le32_to_cpu(d[9]);
            }
            break;
        }
        case 0x0a:
            /* end of a step: commit what the pipeline stages received */
            if (written[0xe5] && ndbk < MAX_MB) {
                VdcMB *m = &mbs[ndbk++];

                m->have_dbk = true;
                m->e7 = regs[0xe7];
                m->e8 = regs[0xe8];
                m->e9 = regs[0xe9];
                m->ec = regs[0xec];
                m->f5 = regs[0xf5];
            }
            if (written[0xd2] && nmb < MAX_MB) {
                VdcMB *m = &mbs[nmb++];

                m->d2 = regs[0xd2];
                m->cd = regs[0xcd];
                m->d1 = regs[0xd1];
                m->d0 = regs[0xd0];
                m->cf = regs[0xcf];
                m->ncoef = nprev;
                memcpy(m->coef, prev, nprev * 4);
            }
            memcpy(prev, pend, npend * 4);
            nprev = npend;
            npend = 0;
            memset(written, 0, sizeof(written));
            break;
        }
        for (int k = 0; k < nr; k++) {
            v[k] = le32_to_cpu(w[i + 1 + k]);
            if (r[k] < ARRAY_SIZE(regs)) {
                regs[r[k]] = v[k];
                written[r[k]] = true;
            }
            if (r[k] == 0x82) {
                cur = (v[k] >> 16) * (width / 16) + (v[k] & 0xffff);
            } else if (r[k] == 0x7a) {
                ref_y = v[k];
            } else if (r[k] == 0x72) {
                ref_c = v[k];
            }
        }
        i += len;
    }

    if (trace) {
        qemu_log("q6-vdc: %dx%d, %d MBs, ref %08x out %08x/%08x\n", width,
                 height, nmb, ref_y, out_y, out_c);
    }
    if (!out_y || nmb != (width / 16) * (height / 16)) {
        qemu_log_mask(LOG_UNIMP, "q6-vdc: incomplete command buffer "
                      "(%d MBs, out %08x)\n", nmb, out_y);
        return -1;
    }

    pic_alloc(&pic, width, height);
    pic_alloc(&ref, width, height);
    if (ref_y) {
        pic_read(&ref, ref_y, ref_c);
    }
    for (int n = 0; n < nmb; n++) {
        recon_mb(&pic, &ref, n, &mbs[n]);
    }
    for (int n = 0; n < nmb; n++) {
        if (mbs[n].have_dbk) {
            deblock_mb(&pic, mbs, n);
        }
    }
    pic_write(&pic, out_y, out_c);
    if (dump_yuv) {
        FILE *f = fopen(dump_yuv, "wb");

        if (f) {
            fwrite(pic.y, 1, width * height, f);
            fwrite(pic.u, 1, width * height / 4, f);
            fwrite(pic.v, 1, width * height / 4, f);
            fclose(f);
        }
    }
    pic_free(&pic);
    pic_free(&ref);
    return 0;
}
