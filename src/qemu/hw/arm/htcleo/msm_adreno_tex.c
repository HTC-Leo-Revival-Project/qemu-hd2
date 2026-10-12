/*
 * Adreno 200 texture unit: fetch constants, mipmaps and filtering
 *
 * A texture fetch constant is 6 dwords (SQ_FETCH_0 + 6n), as on Xenos:
 *   0: type, per-component sign, clamp x/y/z, pitch/32, tiled
 *   1: format, endian, clamp policy, base address
 *   2: width-1, height-1 (and depth-1)
 *   3: number format, swizzle, exponent adjust, mag/min/mip filters
 *   4: min/max mip level, LOD bias (1/32)
 *   5: border colour, dimension, mip address
 * Level 0 is at the base address with the constant's pitch.  Levels 1+
 * start at the mip address, each in a power-of-two pitch and height of at
 * least 32 blocks, 4 KiB aligned (freedreno's fd2 layout).  Tiled surfaces
 * use the Xenos 2D tiling, in blocks for the compressed formats.
 *
 * Formats: 8/16/32-bit integer and float ones, ETC1, DXT1/3/5, DXT3A,
 * DXT5A, DXN and ATC (Adreno's own: RGB, explicit and interpolated
 * alpha).  The LOD comes from the 2x2 quads' coordinate differences;
 * point/linear filtering, nearest/linear mipmapping.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "msm_adreno.h"

enum { FMT_ETC1_RGBA_INTERP = 46, FMT_ETC1_RGB = 47, FMT_ETC1_RGBA = 48 };

typedef struct TexFmt {
    uint8_t bpb;            /* bytes per block */
    uint8_t bw;             /* block width/height in texels (1 or 4) */
    uint8_t bits[4];        /* uncompressed channel widths from bit 0 */
    uint8_t kind;           /* 0 fixed, 1 float16, 2 float32, 3 block */
} TexFmt;

static bool tex_format(unsigned fmt, TexFmt *t)
{
    static const TexFmt tbl[64] = {
        [1]  = { 1, 1, { 8, 0, 0, 0 } },        /* FMT_1 (as 8) */
        [2]  = { 1, 1, { 8, 0, 0, 0 } },        /* FMT_8 */
        [3]  = { 2, 1, { 5, 5, 5, 1 } },        /* FMT_1_5_5_5 */
        [4]  = { 2, 1, { 5, 6, 5, 0 } },        /* FMT_5_6_5 */
        [5]  = { 2, 1, { 6, 5, 5, 0 } },        /* FMT_6_5_5 */
        [6]  = { 4, 1, { 8, 8, 8, 8 } },        /* FMT_8_8_8_8 */
        [7]  = { 4, 1, { 10, 10, 10, 2 } },     /* FMT_2_10_10_10 */
        [8]  = { 1, 1, { 8, 0, 0, 0 } },        /* FMT_8_A */
        [9]  = { 1, 1, { 8, 0, 0, 0 } },        /* FMT_8_B */
        [10] = { 2, 1, { 8, 8, 0, 0 } },        /* FMT_8_8 */
        [13] = { 2, 1, { 1, 5, 5, 5 } },        /* FMT_5_5_5_1 */
        [14] = { 4, 1, { 8, 8, 8, 8 } },        /* FMT_8_8_8_8_A */
        [15] = { 2, 1, { 4, 4, 4, 4 } },        /* FMT_4_4_4_4 */
        [16] = { 4, 1, { 8, 8, 8, 0 } },        /* FMT_8_8_8 (as 32 bpp) */
        [18] = { 8, 4, { 0 }, 3 },              /* FMT_DXT1 */
        [19] = { 16, 4, { 0 }, 3 },             /* FMT_DXT2_3 */
        [20] = { 16, 4, { 0 }, 3 },             /* FMT_DXT4_5 */
        [21] = { 4, 1, { 2, 10, 10, 10 } },     /* FMT_10_10_10_2 */
        [22] = { 4, 1, { 8, 24, 0, 0 } },       /* FMT_24_8 */
        [24] = { 2, 1, { 16, 0, 0, 0 } },       /* FMT_16 */
        [25] = { 4, 1, { 16, 16, 0, 0 } },      /* FMT_16_16 */
        [26] = { 8, 1, { 16, 16, 16, 16 } },    /* FMT_16_16_16_16 */
        [30] = { 2, 1, { 16, 0, 0, 0 }, 1 },    /* FMT_16_FLOAT */
        [31] = { 4, 1, { 16, 16, 0, 0 }, 1 },   /* FMT_16_16_FLOAT */
        [32] = { 8, 1, { 16, 16, 16, 16 }, 1 }, /* FMT_16_16_16_16_FLOAT */
        [33] = { 4, 1, { 32, 0, 0, 0 } },       /* FMT_32 */
        [34] = { 8, 1, { 32, 32, 0, 0 } },      /* FMT_32_32 */
        [36] = { 4, 1, { 32, 0, 0, 0 }, 2 },    /* FMT_32_FLOAT */
        [37] = { 8, 1, { 32, 32, 0, 0 }, 2 },   /* FMT_32_32_FLOAT */
        [38] = { 16, 1, { 32, 32, 32, 32 }, 2 },/* FMT_32_32_32_32_FLOAT */
        [39] = { 8, 4, { 0 }, 3 },              /* FMT_ATI_TC_RGB */
        [40] = { 16, 4, { 0 }, 3 },             /* FMT_ATI_TC_RGBA */
        [41] = { 8, 4, { 0 }, 3 },              /* FMT_ATI_TC_555_565_RGB */
        [42] = { 16, 4, { 0 }, 3 },             /* ..._RGBA (explicit) */
        [43] = { 16, 4, { 0 }, 3 },             /* FMT_ATI_TC_RGBA_INTERP */
        [44] = { 16, 4, { 0 }, 3 },             /* ..._RGBA_INTERP */
        [46] = { 16, 4, { 0 }, 3 },             /* FMT_ETC1_RGBA_INTERP */
        [47] = { 8, 4, { 0 }, 3 },              /* FMT_ETC1_RGB */
        [48] = { 16, 4, { 0 }, 3 },             /* FMT_ETC1_RGBA */
        [49] = { 16, 4, { 0 }, 3 },             /* FMT_DXN */
        [58] = { 8, 4, { 0 }, 3 },              /* FMT_DXT3A */
        [59] = { 8, 4, { 0 }, 3 },              /* FMT_DXT5A */
    };

    *t = tbl[fmt & 63];
    return t->bpb != 0;
}

/* ------------------------------------------------------------------ */
/* Compressed blocks: one texel (x, y in 0..3) as RGBA in 0..1        */
/* ------------------------------------------------------------------ */

static void rgb565(uint16_t c, int *rgb)
{
    rgb[0] = ((c >> 11) & 31) * 255 / 31;
    rgb[1] = ((c >> 5) & 63) * 255 / 63;
    rgb[2] = (c & 31) * 255 / 31;
}

/* DXT1-style colour block; four colours always for DXT3/5 */
static void dxt_color(const uint8_t *b, int i, bool dxt1, float *out)
{
    uint16_t c0 = lduw_le_p(b), c1 = lduw_le_p(b + 2);
    int sel = (ldl_le_p(b + 4) >> (2 * i)) & 3;
    int a[3], c[3];

    rgb565(c0, a);
    rgb565(c1, c);
    out[3] = 1.0f;
    for (int k = 0; k < 3; k++) {
        int v;

        if (!dxt1 || c0 > c1) {
            switch (sel) {
            case 0: v = a[k]; break;
            case 1: v = c[k]; break;
            case 2: v = (2 * a[k] + c[k]) / 3; break;
            default: v = (a[k] + 2 * c[k]) / 3; break;
            }
        } else {
            switch (sel) {
            case 0: v = a[k]; break;
            case 1: v = c[k]; break;
            case 2: v = (a[k] + c[k]) / 2; break;
            default: v = 0; out[3] = 0.0f; break;
            }
        }
        out[k] = v / 255.0f;
    }
}

/* DXT5/BC4-style interpolated 8-bit channel */
static float dxt5_alpha(const uint8_t *b, int i)
{
    int a0 = b[0], a1 = b[1];
    uint64_t bits = ldq_le_p(b) >> 16;
    int sel = (bits >> (3 * i)) & 7;

    if (sel == 0) {
        return a0 / 255.0f;
    }
    if (sel == 1) {
        return a1 / 255.0f;
    }
    if (a0 > a1) {
        return ((8 - sel) * a0 + (sel - 1) * a1) / 7 / 255.0f;
    }
    if (sel == 6) {
        return 0.0f;
    }
    if (sel == 7) {
        return 1.0f;
    }
    return ((6 - sel) * a0 + (sel - 1) * a1) / 5 / 255.0f;
}

/* explicit 4-bit alpha */
static float explicit_alpha(const uint8_t *b, int i)
{
    return ((ldq_le_p(b) >> (4 * i)) & 15) / 15.0f;
}

/* ATC (Adreno) colour block: colour0 RGB555 with a mode bit, colour1 565 */
static void atc_color(const uint8_t *b, int i, float *out)
{
    uint16_t c0 = lduw_le_p(b), c1 = lduw_le_p(b + 2);
    int sel = (ldl_le_p(b + 4) >> (2 * i)) & 3;
    int a[3], c[3];

    a[0] = ((c0 >> 10) & 31) * 255 / 31;
    a[1] = ((c0 >> 5) & 31) * 255 / 31;
    a[2] = (c0 & 31) * 255 / 31;
    rgb565(c1, c);
    for (int k = 0; k < 3; k++) {
        int v;

        if (!(c0 & 0x8000)) {
            switch (sel) {
            case 0: v = a[k]; break;
            case 1: v = (5 * a[k] + 3 * c[k]) / 8; break;
            case 2: v = (3 * a[k] + 5 * c[k]) / 8; break;
            default: v = c[k]; break;
            }
        } else {
            switch (sel) {
            case 0: v = 0; break;
            case 1: v = MAX(a[k] - c[k] / 4, 0); break;
            case 2: v = a[k]; break;
            default: v = c[k]; break;
            }
        }
        out[k] = v / 255.0f;
    }
    out[3] = 1.0f;
}

static void etc1_texel(const uint8_t *b, int x, int y, float out[4])
{
    static const int tbl[8][4] = {
        { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 },
        { 13, 42, -13, -42 }, { 18, 60, -18, -60 }, { 24, 80, -24, -80 },
        { 33, 106, -33, -106 }, { 47, 183, -47, -183 },
    };
    uint32_t hi = ldl_be_p(b), lo = ldl_be_p(b + 4);
    bool diff = hi & 2, flip = hi & 1;
    int sub = flip ? (y >= 2) : (x >= 2);
    int base[3], cw;
    int idx = x * 4 + y;
    int msb = (lo >> (16 + idx)) & 1, lsb = (lo >> idx) & 1;
    int mod;

    if (diff) {
        for (int k = 0; k < 3; k++) {
            int c5 = (hi >> (27 - 8 * k)) & 0x1f;
            int d = ((int)((hi >> (24 - 8 * k)) & 7) << 29) >> 29;

            if (sub) {
                c5 += d;
            }
            c5 &= 0x1f;
            base[k] = (c5 << 3) | (c5 >> 2);
        }
    } else {
        for (int k = 0; k < 3; k++) {
            base[k] = ((hi >> (28 - 8 * k - (sub ? 4 : 0))) & 0xf) * 17;
        }
    }
    cw = (hi >> (sub ? 2 : 5)) & 7;
    mod = tbl[cw][(msb << 1) | lsb];
    for (int k = 0; k < 3; k++) {
        out[k] = MIN(MAX(base[k] + mod, 0), 255) / 255.0f;
    }
    out[3] = 1.0f;
}

static void block_texel(unsigned fmt, const uint8_t *b, int x, int y,
                        float out[4])
{
    int i = y * 4 + x;

    switch (fmt) {
    case 18:    /* DXT1 */
        dxt_color(b, i, true, out);
        break;
    case 19:    /* DXT2/3 */
        dxt_color(b + 8, i, false, out);
        out[3] = explicit_alpha(b, i);
        break;
    case 20:    /* DXT4/5 */
        dxt_color(b + 8, i, false, out);
        out[3] = dxt5_alpha(b, i);
        break;
    case 49:    /* DXN: two interpolated channels */
        out[0] = dxt5_alpha(b, i);
        out[1] = dxt5_alpha(b + 8, i);
        out[2] = 0.0f;
        out[3] = 1.0f;
        break;
    case 58:    /* DXT3A */
        out[0] = out[1] = out[2] = out[3] = explicit_alpha(b, i);
        break;
    case 59:    /* DXT5A */
        out[0] = out[1] = out[2] = out[3] = dxt5_alpha(b, i);
        break;
    case 39:
    case 41:    /* ATC RGB */
        atc_color(b, i, out);
        break;
    case 40:
    case 42:    /* ATC explicit alpha */
        atc_color(b + 8, i, out);
        out[3] = explicit_alpha(b, i);
        break;
    case 43:
    case 44:    /* ATC interpolated alpha */
        atc_color(b + 8, i, out);
        out[3] = dxt5_alpha(b, i);
        break;
    case FMT_ETC1_RGB:
        etc1_texel(b, x, y, out);
        break;
    case FMT_ETC1_RGBA:
        etc1_texel(b + 8, x, y, out);
        out[3] = explicit_alpha(b, i);
        break;
    case FMT_ETC1_RGBA_INTERP:
        etc1_texel(b + 8, x, y, out);
        out[3] = dxt5_alpha(b, i);
        break;
    default:
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Levels and texels                                                   */
/* ------------------------------------------------------------------ */

typedef struct TexLevel {
    uint32_t base;          /* GPU address */
    uint32_t pitch;         /* in blocks */
    uint32_t w, h;          /* in texels */
    uint32_t ox, oy;        /* block offset inside a packed mip tail */
} TexLevel;

typedef struct TexDesc {
    const uint32_t *fc;
    TexFmt tf;
    unsigned fmt;
    bool tiled;
    unsigned bpb_log2;
    unsigned dim;
    uint32_t w0, h0;
    int nlevels;
    int packed_level;       /* first level of the packed tail, or 99 */
    unsigned clamp[2];
    unsigned sign[4];
    bool integer;
    int exp_adj;
    uint32_t swz;
    float border[4];
    uint32_t face_size;     /* cube maps: bytes per face of level 0 */
} TexDesc;

static unsigned log2_ceil(uint32_t v)
{
    return v <= 1 ? 0 : 32 - clz32(v - 1);
}

/*
 * Block offset of a level inside the 32x32 tile its packed mip tail
 * shares (Xenos: Xenia's texture_util.cc GetPackedMipOffset, 2D).
 */
static void packed_offset(uint32_t w, uint32_t h, int level, uint32_t *x,
                          uint32_t *y)
{
    unsigned lw = log2_ceil(w), lh = log2_ceil(h);
    unsigned ls = MIN(lw, lh);
    unsigned base = ls > 4 ? ls - 4 : 0;
    unsigned pm = level - base;

    *x = *y = 0;
    if (pm < 3) {
        if (lw > lh) {
            *y = 16 >> pm;
        } else {
            *x = 16 >> pm;
        }
    } else if (lw > lh) {
        *x = (1u << (lw - base)) >> (pm - 2);
    } else {
        *y = (1u << (lh - base)) >> (pm - 2);
    }
}

/*
 * Level 0 is at the base address with the constant's pitch.  Mips start at
 * the mip address; each is a level of the texture rounded up to a power of
 * two, its pitch and height aligned to 32 blocks, 4 KiB aligned.  With
 * SQ_TEX_5 PACKED_MIPS the levels whose smaller side is 16 or less share
 * one 32x32-block tile (as on Xenos).
 */
static void tex_level(const TexDesc *t, int level, TexLevel *lv)
{
    const uint32_t *fc = t->fc;
    uint32_t bw = t->tf.bw;
    int stored = MIN(level, t->packed_level);

    lv->w = MAX(t->w0 >> level, 1u);
    lv->h = MAX(t->h0 >> level, 1u);
    lv->ox = lv->oy = 0;
    if (level >= t->packed_level) {
        /* in blocks, also for the compressed formats */
        packed_offset(t->w0, t->h0, level, &lv->ox, &lv->oy);
    }
    if (stored == 0) {
        lv->base = fc[1] & 0xfffff000;
        lv->pitch = ((fc[0] >> 22) & 0x1ff) * 32;
        lv->pitch = lv->pitch ? lv->pitch / bw : DIV_ROUND_UP(lv->w, bw);
        if (t->packed_level == 0) {
            lv->pitch = ROUND_UP(lv->pitch, 32);
        }
        return;
    }
    lv->base = fc[5] & 0xfffff000;
    for (int k = 1; k <= stored; k++) {
        uint32_t wt = MAX(pow2ceil(t->w0) >> k, 1u);
        uint32_t ht = MAX(pow2ceil(t->h0) >> k, 1u);
        uint32_t pitch = ROUND_UP(DIV_ROUND_UP(wt, bw), 32);
        uint32_t rows = ROUND_UP(DIV_ROUND_UP(ht, bw), 32);
        uint32_t bytes = pitch << t->bpb_log2;

        if (!t->tiled) {
            bytes = ROUND_UP(bytes, 256);
            pitch = bytes >> t->bpb_log2;
        }
        if (k == stored) {
            lv->pitch = pitch;
            break;
        }
        lv->base += ROUND_UP(bytes * rows, 4096);
    }
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* texel (x, y) of a level, wrapped/clamped already; raw XYZW */
static void tex_texel(AdrenoLanes *L, const TexDesc *t, const TexLevel *lv,
                      int x, int y, uint32_t face, float out[4])
{
    uint32_t bw = t->tf.bw;
    uint32_t bx = x / bw + lv->ox, by = y / bw + lv->oy;
    uint32_t off;
    uint8_t blk[16];
    const uint8_t *p;
    uint32_t addr;

    off = t->tiled ? adreno_tiled_offset(bx, by, ROUND_UP(lv->pitch, 32),
                                         t->bpb_log2)
                   : (by * lv->pitch + bx) << t->bpb_log2;
    addr = lv->base + face * t->face_size + off;
    p = adreno_host_ptr(L->s, L->tlb, addr);
    if (!p || ((addr & 0xfff) + t->tf.bpb > 0x1000)) {
        adreno_tlb_read(L->s, L->tlb, addr, blk, t->tf.bpb);
        p = blk;
    }
    if (t->tf.kind == 3) {
        block_texel(t->fmt, p, x & 3, y & 3, out);
        return;
    }
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (t->tf.kind == 1) {
        for (int k = 0; k < 4 && t->tf.bits[k]; k++) {
            out[k] = adreno_half_to_float(lduw_le_p(p + 2 * k));
        }
        return;
    }
    if (t->tf.kind == 2) {
        for (int k = 0; k < 4 && t->tf.bits[k]; k++) {
            out[k] = adreno_f(ldl_le_p(p + 4 * k));
        }
        return;
    }
    {
        uint64_t v = 0;
        int pos = 0;

        memcpy(&v, p, MIN(t->tf.bpb, 8));
        v = le64_to_cpu(v);
        for (int k = 0; k < 4 && t->tf.bits[k]; k++) {
            uint64_t mask = (1ull << t->tf.bits[k]) - 1;
            uint64_t c = (v >> pos) & mask;
            float f;

            switch (t->sign[k]) {
            case 1: {   /* signed */
                int64_t sc = (int64_t)(c << (64 - t->tf.bits[k])) >>
                             (64 - t->tf.bits[k]);
                f = t->integer ? (float)sc : MAX(-1.0f, sc / (float)(mask >> 1));
                break;
            }
            case 2:     /* unsigned, biased */
                f = c / (float)mask * 2.0f - 1.0f;
                break;
            case 3:     /* gamma */
                f = srgb_to_linear(c / (float)mask);
                break;
            default:
                f = t->integer ? (float)c : c / (float)mask;
                break;
            }
            out[k] = f;
            pos += t->tf.bits[k];
        }
    }
}

static int wrap(int i, int size, unsigned mode, bool *border)
{
    switch (mode) {
    case 0:     /* repeat */
        i %= size;
        return i < 0 ? i + size : i;
    case 1:     /* mirrored repeat */
    case 3:     /* mirror once to edge */
    case 5:
    case 7: {
        int p = size * 2;

        if (mode != 1) {
            i = i < 0 ? -1 - i : i;
            return MIN(i, size - 1);
        }
        i %= p;
        if (i < 0) {
            i += p;
        }
        return i < size ? i : p - 1 - i;
    }
    case 6:     /* border */
        if (i < 0 || i >= size) {
            *border = true;
        }
        return MIN(MAX(i, 0), size - 1);
    default:    /* clamp to edge / half way */
        return MIN(MAX(i, 0), size - 1);
    }
}

static void sample_level(AdrenoLanes *L, const TexDesc *t, int level,
                         float u, float v, uint32_t face, bool linear,
                         float out[4])
{
    TexLevel lv;
    float x, y;

    tex_level(t, level, &lv);
    x = u * lv.w;
    y = v * lv.h;
    if (linear) {
        float fx = x - 0.5f, fy = y - 0.5f;
        int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
        float ax = fx - x0, ay = fy - y0;
        float c[4][4];

        for (int i = 0; i < 4; i++) {
            bool b = false;
            int xi = wrap(x0 + (i & 1), lv.w, t->clamp[0], &b);
            int yi = wrap(y0 + (i >> 1), lv.h, t->clamp[1], &b);

            if (b) {
                memcpy(c[i], t->border, sizeof(c[i]));
            } else {
                tex_texel(L, t, &lv, xi, yi, face, c[i]);
            }
        }
        for (int k = 0; k < 4; k++) {
            float top = c[0][k] + (c[1][k] - c[0][k]) * ax;
            float bot = c[2][k] + (c[3][k] - c[2][k]) * ax;

            out[k] = top + (bot - top) * ay;
        }
    } else {
        bool b = false;
        int xi = wrap((int)floorf(x), lv.w, t->clamp[0], &b);
        int yi = wrap((int)floorf(y), lv.h, t->clamp[1], &b);

        if (b) {
            memcpy(out, t->border, sizeof(float) * 4);
        } else {
            tex_texel(L, t, &lv, xi, yi, face, out);
        }
    }
}

static bool tex_desc(MsmAdrenoState *s, unsigned idx, TexDesc *t)
{
    const uint32_t *fc = &s->regs[A2_SQ_FETCH_0 + idx * 6];
    int maxl, minl;

    memset(t, 0, sizeof(*t));
    t->fc = fc;
    t->fmt = fc[1] & 0x3f;
    if (!tex_format(t->fmt, &t->tf)) {
        return false;
    }
    t->tiled = fc[0] >> 31;
    t->bpb_log2 = ctz32(t->tf.bpb);
    if (!is_power_of_2(t->tf.bpb)) {
        t->tiled = false;
    }
    t->dim = (fc[5] >> 9) & 3;
    t->w0 = (fc[2] & 0x1fff) + 1;
    t->h0 = t->dim == 0 ? 1 : ((fc[2] >> 13) & 0x1fff) + 1;
    t->clamp[0] = (fc[0] >> 10) & 7;
    t->clamp[1] = (fc[0] >> 13) & 7;
    for (int k = 0; k < 4; k++) {
        t->sign[k] = (fc[0] >> (2 + 2 * k)) & 3;
    }
    t->integer = fc[3] & 1;
    t->exp_adj = ((int32_t)(fc[3] << 13)) >> 26;
    t->swz = (fc[3] >> 1) & 0xfff;
    switch (fc[5] & 3) {
    case 1:
        t->border[0] = t->border[1] = t->border[2] = t->border[3] = 1.0f;
        break;
    default:
        t->border[3] = (fc[5] & 3) ? 1.0f : 0.0f;
        break;
    }
    /* levels: up to log2(max size), within the constant's min/max */
    maxl = 31 - clz32(MAX(t->w0, t->h0));
    minl = (fc[4] >> 2) & 0xf;
    maxl = MIN(maxl, (int)((fc[4] >> 6) & 0xf));
    if (!(fc[5] & 0xfffff000)) {
        maxl = 0;
    }
    t->nlevels = MAX(maxl, minl) + 1;
    t->packed_level = 99;
    if (fc[5] & (1 << 11)) {
        unsigned ls = log2_ceil(MIN(t->w0, t->h0));

        t->packed_level = ls > 4 ? ls - 4 : 0;
    }
    if (t->dim == 3) {
        TexLevel lv;

        tex_level(t, 0, &lv);
        t->face_size = ROUND_UP(lv.pitch *
                                ROUND_UP(DIV_ROUND_UP(t->h0, t->tf.bw), 32)
                                << t->bpb_log2, 4096);
    }
    return true;
}

void adreno_sample(AdrenoLanes *L, const AdrenoTexArgs *a,
                   float coord[3][AL], uint32_t mask, float out[4][AL])
{
    MsmAdrenoState *s = L->s;
    TexDesc t;
    unsigned mag = a->mag, min = a->min, mip = a->mip;
    float lod[AL];
    float fc_bias;
    int minl, maxl;
    const uint32_t *fc;

    if (!tex_desc(s, a->const_idx, &t)) {
        for (int k = 0; k < 4; k++) {
            for (int l = 0; l < AL; l++) {
                out[k][l] = k == 3 ? 1.0f : 0.0f;
            }
        }
        return;
    }
    fc = t.fc;
    /* filters 3 = from the fetch constant */
    if (mag == 3) {
        mag = (fc[3] >> 19) & 3;
    }
    if (min == 3) {
        min = (fc[3] >> 21) & 3;
    }
    if (mip == 3) {
        mip = (fc[3] >> 23) & 3;
    }
    fc_bias = (((int32_t)(fc[4] << 10)) >> 22) / 32.0f;
    minl = (fc[4] >> 2) & 0xf;
    maxl = t.nlevels - 1;

    /* the LOD of each quad, from its coordinate differences */
    for (int q = 0; q < AL; q += 4) {
        float l = 0.0f;

        if (L->quads && mip != 2 && maxl > 0) {
            float w = t.w0, h = t.h0;
            float dux = (coord[0][q + 1] - coord[0][q]) * w;
            float dvx = (coord[1][q + 1] - coord[1][q]) * h;
            float duy = (coord[0][q + 2] - coord[0][q]) * w;
            float dvy = (coord[1][q + 2] - coord[1][q]) * h;
            float rho2 = MAX(dux * dux + dvx * dvx, duy * duy + dvy * dvy);

            if (a->unnormalized) {
                rho2 /= w * h;
            }
            l = rho2 > 0 ? 0.5f * log2f(rho2) : -100.0f;
        }
        l += fc_bias + a->lod_bias;
        lod[q] = lod[q + 1] = lod[q + 2] = lod[q + 3] = l;
    }

    for (int l = 0; l < AL; l++) {
        float u, v, raw[4];
        uint32_t face = 0;

        if (!(mask & (1u << l))) {
            continue;
        }
        u = coord[0][l];
        v = coord[1][l];
        if (t.dim == 3) {
            /* cube: (s, t) in 1..2 for the face in z (the CUBE op's) */
            face = MIN(MAX((int)coord[2][l], 0), 5);
            u -= 1.0f;
            v -= 1.0f;
        } else if (a->unnormalized) {
            u /= t.w0;
            v /= t.h0;
        }
        u += a->offset[0] / t.w0;
        v += a->offset[1] / t.h0;

        if (mip == 2 || maxl == 0 || lod[l] <= 0.0f) {
            /* magnification or the base map only */
            sample_level(L, &t, minl, u, v, face,
                         (lod[l] <= 0.0f ? mag : min) == 1, raw);
        } else {
            float lf = MIN(lod[l] + minl, (float)maxl);
            bool lin = min == 1;

            if (mip == 1 && lf < maxl) {
                int l0 = (int)floorf(lf);
                float f = lf - l0;
                float a0[4], a1[4];

                sample_level(L, &t, l0, u, v, face, lin, a0);
                sample_level(L, &t, l0 + 1, u, v, face, lin, a1);
                for (int k = 0; k < 4; k++) {
                    raw[k] = a0[k] + (a1[k] - a0[k]) * f;
                }
            } else {
                sample_level(L, &t, (int)lrintf(lf), u, v, face, lin, raw);
            }
        }
        if (t.exp_adj) {
            for (int k = 0; k < 4; k++) {
                raw[k] = ldexpf(raw[k], t.exp_adj);
            }
        }
        for (int k = 0; k < 4; k++) {
            unsigned sel = (t.swz >> (3 * k)) & 7;

            out[k][l] = sel < 4 ? raw[sel] : (sel == 5 ? 1.0f : 0.0f);
        }
    }
}
