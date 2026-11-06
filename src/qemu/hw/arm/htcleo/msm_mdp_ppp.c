/*
 * Qualcomm MDP 3.1 PPP (pixel pipeline / blit engine)
 *
 * Register programming as done by drivers/video/msm/mdp_ppp.c and
 * mdp_ppp31.c (CONFIG_MSM_MDP31).  One write of DISPLAY0_START runs one
 * blit: fetch the source ROI, optionally colour convert, scale, rotate /
 * flip, blend with the background (always the destination) and store.
 *
 *   SRC: ROI 0x10108 (h<<16|w), SRC0 0x1010c, SRC1 0x10110 (chroma),
 *        YSTRIDE 0x1011c (chroma<<16|luma), CFG 0x10124, PACK 0x10128,
 *        XY 0x10200, IMAGE_SIZE 0x10208
 *   OP 0x10138, ALPHA_TRANSP 0x1014c (alpha in 31:24)
 *   DST: CFG 0x10150, PACK 0x10154, ROI 0x10164, DST0 0x10168, DST1 0x1016c,
 *        YSTRIDE 0x10178, OUT_XY 0x1019c
 *   BG:  BG0 0x101c0, BG1 0x101c4, YSTRIDE 0x101cc, CFG 0x101d4,
 *        PACK 0x101d8, XY 0x10204, IMAGE_SIZE 0x1020c,
 *        BLEND_BG_ALPHA_SEL 0x70010
 *
 * Pixel layout comes from CFG (component widths, bytes per pixel, number
 * of interleaved components, pseudo-planar) and the pack pattern, which
 * lists the component ids (G=0, B=1, R=2, A=3) from the LSB up.  Scaling
 * is nearest neighbour; the hardware uses polyphase filters.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "system/physmem.h"
#include "htcleo.h"
#include "msm_mdp.h"
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#define R(off)              (s->regs[(off) >> 2])

#define PPP_SRC_ROI         0x10108
#define PPP_SRC0            0x1010c
#define PPP_SRC1            0x10110
#define PPP_SRC_YSTRIDE     0x1011c
#define PPP_SRC_CFG         0x10124
#define PPP_SRC_PACK        0x10128
#define PPP_OP              0x10138
#define PPP_ALPHA_TRANSP    0x1014c
#define PPP_DST_CFG         0x10150
#define PPP_DST_PACK        0x10154
#define PPP_DST_ROI         0x10164
#define PPP_DST0            0x10168
#define PPP_DST1            0x1016c
#define PPP_DST_YSTRIDE     0x10178
#define PPP_OUT_XY          0x1019c
#define PPP_BG0             0x101c0
#define PPP_BG1             0x101c4
#define PPP_BG_YSTRIDE      0x101cc
#define PPP_BG_CFG          0x101d4
#define PPP_BG_PACK         0x101d8
#define PPP_SRC_XY          0x10200
#define PPP_BG_XY           0x10204
#define PPP_SRC_IMAGE_SIZE  0x10208
#define PPP_BG_IMAGE_SIZE   0x1020c
#define PPP_BG_ALPHA_SEL    0x70010

#define OP_SCALE_X          (1u << 0)
#define OP_SCALE_Y          (1u << 1)
#define OP_CONVERT_ON       (1u << 3)
#define OP_ROT_90           (1u << 9)
#define OP_FLIP_LR          (1u << 10)
#define OP_FLIP_UD          (1u << 11)
#define OP_BLEND_ON         (1u << 12)
#define OP_BLEND_SEL(op)    (((op) >> 13) & 3)
#define OP_BLEND_REVERSE    (1u << 15)
#define OP_SRC_CHROMA_H2    (1u << 18)
#define OP_SRC_CHROMA_V2    (1u << 19)

#define BG_USE_ALPHA_SEL    (1u << 0)
#define BG_ALPHA_REVERSE    (1u << 3)

enum { CLR_G, CLR_B, CLR_R, CLR_A };

/* the image description shared by the src, dst and bg ports */
typedef struct PppImg {
    hwaddr base0, base1;
    uint32_t stride0, stride1;
    int bpp;                    /* bytes per pixel of plane 0 */
    int ncomp;
    int width[4];               /* bits per component, by id */
    int order[4];               /* component id, from the LSB up */
    bool alpha;
    bool pseudoplanar;
    int hsub, vsub;             /* chroma subsampling */
    int fast;                   /* PPP_UNPACK_*: how ppp_fetch_row unpacks */
    int8_t src[4];              /* BYTES: pixel byte of G,B,R,A, -1: 0xFF */
} PppImg;

/*
 * Every frame Android composes goes through ppp_fetch_row a pixel at a
 * time, so the common formats get a short cut that gives the same bytes:
 * 16-bit pixels with G, B and R all present through a table of the whole
 * 64 Ki values, 8-bit components by moving bytes.
 */
enum { PPP_UNPACK_ANY, PPP_UNPACK_LUT16, PPP_UNPACK_BYTES };

/*
 * src and bg CFG: comps 7:0, alpha en 8, bpp 10:9, interleave 14:13,
 * pseudo-planar 20.  dst CFG: comps 7:0, alpha en 8, interleave 10:9,
 * bpp 17:16, planar 18, pseudo-planar 19.
 */
static void ppp_img_decode(PppImg *im, uint32_t cfg, uint32_t pack,
                           bool dst)
{
    static const int bits[4] = { 4, 5, 6, 8 };
    int i;

    im->width[CLR_G] = bits[cfg & 3];
    im->width[CLR_B] = bits[(cfg >> 2) & 3];
    im->width[CLR_R] = bits[(cfg >> 4) & 3];
    im->width[CLR_A] = bits[(cfg >> 6) & 3];
    im->alpha = cfg & (1u << 8);
    if (dst) {
        im->ncomp = ((cfg >> 9) & 3) + 1;
        im->bpp = ((cfg >> 16) & 3) + 1;
        im->pseudoplanar = cfg & (1u << 19);
    } else {
        im->bpp = ((cfg >> 9) & 3) + 1;
        im->ncomp = ((cfg >> 13) & 3) + 1;
        im->pseudoplanar = cfg & (1u << 20);
    }
    for (i = 0; i < 4; i++) {
        im->order[i] = (pack >> (8 * i)) & 3;
    }
    if (im->pseudoplanar) {
        /* plane 0 is 8-bit luma, plane 1 two interleaved chroma bytes */
        im->bpp = 1;
    }
    im->fast = PPP_UNPACK_ANY;
    if (!im->pseudoplanar) {
        int seen = 0, bits8 = 1, total = 0;

        for (i = 0; i < im->ncomp; i++) {
            seen |= 1 << im->order[i];
            total += im->width[im->order[i]];
            bits8 &= im->width[im->order[i]] == 8;
        }
        if (im->bpp == 2 && (seen & 7) == 7 && total <= 16) {
            im->fast = PPP_UNPACK_LUT16;
        } else if (bits8 && im->ncomp == im->bpp && (seen & 7) == 7) {
            im->fast = PPP_UNPACK_BYTES;
            memset(im->src, -1, sizeof(im->src));
            /* as ppp_unpack: a later component of the same id wins */
            for (i = 0; i < im->ncomp; i++) {
                if (im->order[i] != CLR_A || im->alpha) {
                    im->src[im->order[i]] = i;
                }
            }
        }
    }
}

static inline uint8_t expand(uint32_t v, int w)
{
    if (w >= 8) {
        return v;
    }
    v <<= 8 - w;
    return v | (v >> w);
}

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* BT.601, video range, as the default MDP CSC matrix */
static void yuv2rgb(int y, int cb, int cr, uint8_t *rgb)
{
    int c = (y - 16) * 298, d = cb - 128, e = cr - 128;

    rgb[CLR_R] = clamp8((c + 409 * e + 128) >> 8);
    rgb[CLR_G] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
    rgb[CLR_B] = clamp8((c + 516 * d + 128) >> 8);
}

static uint32_t ld_le(const uint8_t *p, int n)
{
    uint32_t v = 0;

    while (n--) {
        v = (v << 8) | p[n];
    }
    return v;
}

static void st_le(uint8_t *p, int n, uint32_t v)
{
    int i;

    for (i = 0; i < n; i++, v >>= 8) {
        p[i] = v;
    }
}

/* unpack one interleaved pixel into c[G,B,R,A] */
static void ppp_unpack(const PppImg *im, uint32_t v, uint8_t *c)
{
    int i, id, w;

    c[CLR_A] = 0xFF;
    for (i = 0; i < im->ncomp; i++) {
        id = im->order[i];
        w = im->width[id];
        if (id != CLR_A || im->alpha) {
            c[id] = expand(v & ((1u << w) - 1), w);
        }
        v >>= w;
    }
}

static uint32_t ppp_pack(const PppImg *im, const uint8_t *c)
{
    uint32_t v = 0;
    int i, id, w, sh = 0;

    for (i = 0; i < im->ncomp; i++) {
        id = im->order[i];
        w = im->width[id];
        v |= (uint32_t)(c[id] >> (8 - w)) << sh;
        sh += w;
    }
    return v;
}

/* the unpacked G,B,R,A of every 16-bit value, for the last two formats */
typedef struct PppLut16 {
    int key;                    /* -1: empty */
    uint8_t c[65536][4];
} PppLut16;

static int ppp_lut_key(const PppImg *im)
{
    int k = im->ncomp | im->alpha << 2;

    for (int i = 0; i < 4; i++) {
        k |= im->order[i] << (3 + 2 * i) | im->width[i] << (11 + 4 * i);
    }
    return k;
}

static const uint8_t (*ppp_lut16(const PppImg *im))[4]
{
    static PppLut16 *lut[2];
    static int next;
    int key = ppp_lut_key(im);

    for (int i = 0; i < 2; i++) {
        if (lut[i] && lut[i]->key == key) {
            return lut[i]->c;
        }
    }
    if (!lut[next]) {
        lut[next] = g_new(PppLut16, 1);
    }
    for (uint32_t v = 0; v < 65536; v++) {
        ppp_unpack(im, v, lut[next]->c[v]);
    }
    lut[next]->key = key;
    next ^= 1;
    return lut[next ^ 1]->c;
}

/*
 * Four bytes a pixel in, G,B,R,A out: src[id] is the input byte that holds
 * component id, or -1 for 0xFF.  False where there is no vector code.
 */
static bool ppp_swizzle4(const uint8_t *in, uint8_t *out, int w,
                         const int8_t *src)
{
#ifdef __aarch64__
    int idx[4], x = 0;

    for (int k = 0; k < 4; k++) {
        idx[k] = src[k] < 0 ? 4 : src[k];
    }
    for (; x + 8 <= w; x += 8) {
        uint8x8x4_t i4 = vld4_u8(in + 4 * x), o4;
        uint8x8_t ch[5] = { i4.val[0], i4.val[1], i4.val[2], i4.val[3],
                            vdup_n_u8(0xFF) };

        for (int k = 0; k < 4; k++) {
            o4.val[k] = ch[idx[k]];
        }
        vst4_u8(out + 4 * x, o4);
    }
    for (; x < w; x++) {
        for (int k = 0; k < 4; k++) {
            out[4 * x + k] = src[k] < 0 ? 0xFF : in[4 * x + src[k]];
        }
    }
    return true;
#else
    return false;
#endif
}

/* read a row of an RGB or YCbCr image as G,B,R,A */
static void ppp_fetch_row(const PppImg *im, int x0, int y, int w,
                          uint8_t *line, uint8_t *out)
{
    int x;

    if (!im->pseudoplanar) {
        physical_memory_read(im->base0 + (hwaddr)y * im->stride0 +
                             (hwaddr)x0 * im->bpp, line, w * im->bpp);
        if (im->fast == PPP_UNPACK_LUT16) {
            const uint8_t (*c)[4] = ppp_lut16(im);

            for (x = 0; x < w; x++) {
                memcpy(out + x * 4, c[lduw_le_p(line + x * 2)], 4);
            }
        } else if (im->fast == PPP_UNPACK_BYTES) {
            const int bpp = im->bpp;
            const int sg = im->src[CLR_G], sb = im->src[CLR_B];
            const int sr = im->src[CLR_R], sa = im->src[CLR_A];

            if (bpp == 4 && sg == 0 && sb == 1 && sr == 2 &&
                (sa == 3 || sa < 0)) {
                memcpy(out, line, w * 4);
                if (sa < 0) {
                    for (x = 0; x < w; x++) {
                        out[x * 4 + CLR_A] = 0xFF;
                    }
                }
            } else if (bpp == 4 && ppp_swizzle4(line, out, w, im->src)) {
                /* done */
            } else {
                for (x = 0; x < w; x++) {
                    const uint8_t *p = line + x * bpp;
                    uint8_t *c = out + x * 4;

                    c[CLR_G] = p[sg];
                    c[CLR_B] = p[sb];
                    c[CLR_R] = p[sr];
                    c[CLR_A] = sa < 0 ? 0xFF : p[sa];
                }
            }
        } else {
            for (x = 0; x < w; x++) {
                ppp_unpack(im, ld_le(line + x * im->bpp, im->bpp),
                           out + x * 4);
            }
        }
    } else {
        int cx0 = (x0 / im->hsub) * 2;
        int cw = ((x0 + w - 1) / im->hsub) * 2 + 2 - cx0;
        uint8_t *chroma = line + w;

        physical_memory_read(im->base0 + (hwaddr)y * im->stride0 + x0,
                             line, w);
        physical_memory_read(im->base1 + (hwaddr)(y / im->vsub) *
                             im->stride1 + cx0, chroma, cw);
        for (x = 0; x < w; x++) {
            const uint8_t *cc = chroma + ((x0 + x) / im->hsub) * 2 - cx0;
            uint8_t uv[4] = { 0, 0, 0, 0 };

            /* the two chroma bytes follow the pack pattern (Cb=B, Cr=R) */
            uv[im->order[0]] = cc[0];
            uv[im->order[1]] = cc[1];
            yuv2rgb(line[x], uv[CLR_B], uv[CLR_R], out + x * 4);
            out[x * 4 + CLR_A] = 0xFF;
        }
    }
}

typedef struct PppBlit {
    PppImg dst, bg;
    uint32_t op, const_alpha, bgsel;
    int dx, dy, bx, by, dw;
    uint8_t *bline, *brgb, *dline;
    /* how the blend and the pack run, worked out once per blit */
    int fsel, bsel;             /* where the alphas come from (bsel < 0: 1 - fa) */
    bool frev, brev;
    uint8_t kf, kb;
    bool pack8;                 /* 4 bytes a pixel, every present component 8 bits */
    int8_t pidx[4];             /* byte i of the pixel holds channel pidx[i], -1: 0 */
    bool verify;                /* check the fast path against the plain one */
} PppBlit;

static void ppp_blit_prepare(PppBlit *b)
{
    const PppImg *d = &b->dst;
    int seen = 0;

    b->fsel = OP_BLEND_SEL(b->op);
    b->frev = b->op & OP_BLEND_REVERSE;
    b->kf = b->const_alpha;
    if (b->bgsel & BG_USE_ALPHA_SEL) {
        b->bsel = (b->bgsel >> 1) & 3;
        b->brev = b->bgsel & BG_ALPHA_REVERSE;
        b->kb = b->bgsel >> 24;
    } else {
        b->bsel = -1;
        b->brev = false;
        b->kb = 0;
    }
    b->pack8 = d->bpp == 4 && d->ncomp >= 1 && d->ncomp <= 4 &&
               !d->pseudoplanar;
    for (int i = 0; b->pack8 && i < d->ncomp; i++) {
        int id = d->order[i];

        if (d->width[id] != 8 || (seen & (1 << id))) {
            b->pack8 = false;
        }
        seen |= 1 << id;
    }
    for (int i = 0; i < 4; i++) {
        b->pidx[i] = i < d->ncomp ? d->order[i] : -1;
    }
}

static uint32_t ppp_alpha(int sel, const uint8_t *f, const uint8_t *b,
                          uint32_t konst)
{
    switch (sel) {
    case 0:
        return f[CLR_A];
    case 1:
        return b[CLR_A];
    case 2:
        return konst;
    }
    /* transparency keying: never enabled by the driver */
    return 0xFF;
}


/* the plain blend of pixels [from, to): what everything else must equal */
static void ppp_blend_range(const PppBlit *b, uint8_t *srgb,
                            const uint8_t *brgb, int from, int to)
{
    for (int x = from; x < to; x++) {
        uint8_t *f = srgb + x * 4;
        const uint8_t *g = brgb + x * 4;
        uint32_t fa, ba;

        fa = ppp_alpha(OP_BLEND_SEL(b->op), f, g, b->const_alpha);
        if (b->op & OP_BLEND_REVERSE) {
            fa = 0xFF - fa;
        }
        if (b->bgsel & BG_USE_ALPHA_SEL) {
            ba = ppp_alpha((b->bgsel >> 1) & 3, f, g, b->bgsel >> 24);
            if (b->bgsel & BG_ALPHA_REVERSE) {
                ba = 0xFF - ba;
            }
        } else {
            ba = 0xFF - fa;
        }
        for (int i = 0; i < 4; i++) {
            f[i] = clamp8((f[i] * fa + g[i] * ba + 127) / 255);
        }
    }
}

static void ppp_pack_range(const PppBlit *b, const uint8_t *srgb,
                           uint8_t *dline, int from, int to)
{
    for (int x = from; x < to; x++) {
        st_le(dline + x * b->dst.bpp, b->dst.bpp,
              ppp_pack(&b->dst, srgb + x * 4));
    }
}

#ifdef __aarch64__
static inline uint8x8_t ppp_alpha8(int sel, uint8x8x4_t f, uint8x8x4_t g,
                                   uint8_t konst)
{
    switch (sel) {
    case 0:
        return f.val[CLR_A];
    case 1:
        return g.val[CLR_A];
    case 2:
        return vdup_n_u8(konst);
    }
    return vdup_n_u8(0xFF);
}

/*
 * floor(x / 255) for x up to 130177 (the most a blend sums to), in single
 * precision: (x + 0.5) / 255 never falls within 1e-3 of the next integer
 * from below, and the rounding of the multiply is 1e-5 -- checked for
 * every x in range, against the integer division.
 */
static inline uint32x4_t ppp_div255(uint32x4_t x)
{
    float32x4_t v = vaddq_f32(vcvtq_f32_u32(x), vdupq_n_f32(0.5f));

    return vcvtq_u32_f32(vmulq_n_f32(v, 1.0f / 255.0f));
}

static inline uint8x8_t ppp_blend_ch(uint8x8_t f, uint8x8_t fa, uint8x8_t g,
                                     uint8x8_t ba)
{
    const uint32x4_t k127 = vdupq_n_u32(127), k255 = vdupq_n_u32(255);
    uint16x8_t p = vmull_u8(f, fa), q = vmull_u8(g, ba);
    uint32x4_t lo = vaddq_u32(vaddl_u16(vget_low_u16(p), vget_low_u16(q)),
                              k127);
    uint32x4_t hi = vaddq_u32(vaddl_high_u16(p, q), k127);

    lo = vminq_u32(ppp_div255(lo), k255);
    hi = vminq_u32(ppp_div255(hi), k255);
    return vmovn_u16(vcombine_u16(vmovn_u32(lo), vmovn_u32(hi)));
}
#endif

/* blend src row (G,B,R,A) with the background row, in place */
static void ppp_blend_row(const PppBlit *b, uint8_t *srgb,
                          const uint8_t *brgb, int n)
{
    int x = 0;

#ifdef __aarch64__
    for (; x + 8 <= n; x += 8) {
        uint8x8x4_t f = vld4_u8(srgb + 4 * x), g = vld4_u8(brgb + 4 * x);
        uint8x8_t fa = ppp_alpha8(b->fsel, f, g, b->kf), ba;

        if (b->frev) {
            fa = vmvn_u8(fa);
        }
        if (b->bsel >= 0) {
            ba = ppp_alpha8(b->bsel, f, g, b->kb);
            if (b->brev) {
                ba = vmvn_u8(ba);
            }
        } else {
            ba = vmvn_u8(fa);
        }
        for (int c = 0; c < 4; c++) {
            f.val[c] = ppp_blend_ch(f.val[c], fa, g.val[c], ba);
        }
        vst4_u8(srgb + 4 * x, f);
    }
#endif
    ppp_blend_range(b, srgb, brgb, x, n);
}

static void ppp_pack_row(const PppBlit *b, const uint8_t *srgb,
                         uint8_t *dline, int n)
{
    int x = 0;

#ifdef __aarch64__
    if (b->pack8) {
        for (; x + 8 <= n; x += 8) {
            uint8x8x4_t f = vld4_u8(srgb + 4 * x), o;
            uint8x8_t ch[5] = { f.val[0], f.val[1], f.val[2], f.val[3],
                                vdup_n_u8(0) };

            for (int i = 0; i < 4; i++) {
                o.val[i] = ch[b->pidx[i] < 0 ? 4 : b->pidx[i]];
            }
            vst4_u8(dline + 4 * x, o);
        }
    }
#endif
    ppp_pack_range(b, srgb, dline, x, n);
}

static void ppp_check_row(const PppBlit *b, int y, const uint8_t *src0,
                          const uint8_t *bg0)
{
    static unsigned bad;
    int n = b->dw, bpp = b->dst.bpp;
    g_autofree uint8_t *f = g_memdup2(src0, n * 4);
    g_autofree uint8_t *ref = g_malloc(n * bpp);

    if (b->op & OP_BLEND_ON) {
        ppp_blend_range(b, f, bg0, 0, n);
    }
    ppp_pack_range(b, f, ref, 0, n);
    for (int i = 0; i < n * bpp; i++) {
        if (ref[i] != b->dline[i]) {
            if (bad++ < 20) {
                error_report("msm-mdp: blit verify: row %d byte %d is %02x, "
                             "should be %02x (op %08x bgsel %08x src %02x "
                             "%02x %02x %02x bg %02x %02x %02x %02x)", y, i,
                             b->dline[i], ref[i], b->op, b->bgsel,
                             src0[(i / bpp) * 4], src0[(i / bpp) * 4 + 1],
                             src0[(i / bpp) * 4 + 2], src0[(i / bpp) * 4 + 3],
                             bg0 ? bg0[(i / bpp) * 4] : 0,
                             bg0 ? bg0[(i / bpp) * 4 + 1] : 0,
                             bg0 ? bg0[(i / bpp) * 4 + 2] : 0,
                             bg0 ? bg0[(i / bpp) * 4 + 3] : 0);
            }
            return;
        }
    }
}

static void msm_mdp_ppp_run1(MsmMdpState *s);

/* blend src row (G,B,R,A, dw pixels) with the background and store row y */
static void ppp_store_row(PppBlit *b, int y, uint8_t *srgb)
{
    g_autofree uint8_t *src0 = b->verify ? g_memdup2(srgb, b->dw * 4) : NULL;
    g_autofree uint8_t *bg0 = NULL;

    if (b->op & OP_BLEND_ON) {
        ppp_fetch_row(&b->bg, b->bx, b->by + y, b->dw, b->bline, b->brgb);
        if (b->verify) {
            bg0 = g_memdup2(b->brgb, b->dw * 4);
        }
        ppp_blend_row(b, srgb, b->brgb, b->dw);
    }
    ppp_pack_row(b, srgb, b->dline, b->dw);
    if (b->verify) {
        ppp_check_row(b, y, src0, bg0);
    }
    physical_memory_write(b->dst.base0 + (hwaddr)(b->dy + y) * b->dst.stride0
                          + (hwaddr)b->dx * b->dst.bpp, b->dline,
                          b->dw * b->dst.bpp);
}

/*
 * Which blits the guest asks for and what they cost, to see what is worth
 * making fast: the registers that decide how a blit runs, with a count, the
 * pixels and the nanoseconds.  msm_mdp_ppp_dump() prints the dearest and
 * clears them.
 */
typedef struct PppCombo {
    uint32_t key[7];
    uint64_t n, px, ns;
} PppCombo;
static PppCombo ppp_combos[64];

static void ppp_account(const uint32_t *key, uint64_t px, uint64_t ns)
{
    PppCombo *c = NULL;

    for (int i = 0; i < 64; i++) {
        if (ppp_combos[i].n && !memcmp(ppp_combos[i].key, key, 28)) {
            c = &ppp_combos[i];
            break;
        }
        if (!ppp_combos[i].n && !c) {
            c = &ppp_combos[i];
        }
    }
    if (c) {
        memcpy(c->key, key, 28);
        c->n++;
        c->px += px;
        c->ns += ns;
    }
}

void msm_mdp_ppp_dump(void)
{
    for (int rank = 0; rank < 4; rank++) {
        PppCombo *best = NULL;

        for (int i = 0; i < 64; i++) {
            if (ppp_combos[i].n && (!best || ppp_combos[i].ns > best->ns)) {
                best = &ppp_combos[i];
            }
        }
        if (!best) {
            break;
        }
        info_report("msm-mdp:   blit op %08x src %08x/%08x dst %08x/%08x bg "
                    "%08x bgsel %08x: %" PRIu64 "x, %" PRIu64 " px, %.1f ms",
                    best->key[0], best->key[1], best->key[2], best->key[3],
                    best->key[4], best->key[5], best->key[6], best->n,
                    best->px, best->ns / 1e6);
        best->n = 0;
    }
    memset(ppp_combos, 0, sizeof(ppp_combos));
}

void msm_mdp_ppp_run(MsmMdpState *s)
{
    int64_t t_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint32_t dbg_key[7] = { R(PPP_OP), R(PPP_SRC_CFG), R(PPP_SRC_PACK),
                            R(PPP_DST_CFG), R(PPP_DST_PACK), R(PPP_BG_CFG),
                            R(PPP_BG_ALPHA_SEL) };
    uint64_t dbg_px = (uint64_t)(R(PPP_DST_ROI) & 0xfff) *
                      ((R(PPP_DST_ROI) >> 16) & 0xfff);

    msm_mdp_ppp_run1(s);
    ppp_account(dbg_key, dbg_px,
                qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t_start);
}

static void msm_mdp_ppp_run1(MsmMdpState *s)
{
    uint32_t op = R(PPP_OP);
    uint32_t sroi = R(PPP_SRC_ROI), droi = R(PPP_DST_ROI);
    int sw = sroi & 0x1fff, sh = (sroi >> 16) & 0x1fff;
    int sx = R(PPP_SRC_XY) & 0x1fff, sy = (R(PPP_SRC_XY) >> 16) & 0x1fff;
    int dw = droi & 0xfff, dh = (droi >> 16) & 0xfff;
    bool rot = op & OP_ROT_90;
    PppImg src;
    PppBlit b = { .op = op };
    int rw, rh, x, y, n;
    uint8_t *sline, *srow, *out, *all = NULL;

    s->ppp_ops++;
    if (!sw || !sh || !dw || !dh) {
        return;
    }

    ppp_img_decode(&src, R(PPP_SRC_CFG), R(PPP_SRC_PACK), false);
    src.base0 = R(PPP_SRC0);
    src.base1 = R(PPP_SRC1);
    src.stride0 = R(PPP_SRC_YSTRIDE) & 0x3fff;
    src.stride1 = R(PPP_SRC_YSTRIDE) >> 16 & 0x3fff;
    src.hsub = (op & OP_SRC_CHROMA_H2) ? 2 : 1;
    src.vsub = (op & OP_SRC_CHROMA_V2) ? 2 : 1;

    ppp_img_decode(&b.dst, R(PPP_DST_CFG), R(PPP_DST_PACK), true);
    b.dst.base0 = R(PPP_DST0);
    b.dst.stride0 = R(PPP_DST_YSTRIDE) & 0x3fff;
    if (b.dst.pseudoplanar) {
        qemu_log_mask(LOG_UNIMP, "msm-mdp: PPP YCbCr output\n");
        return;
    }
    b.dx = R(PPP_OUT_XY) & 0x1fff;
    b.dy = (R(PPP_OUT_XY) >> 16) & 0x1fff;

    ppp_img_decode(&b.bg, R(PPP_BG_CFG), R(PPP_BG_PACK), false);
    b.bg.base0 = R(PPP_BG0);
    b.bg.base1 = R(PPP_BG1);
    b.bg.stride0 = R(PPP_BG_YSTRIDE) & 0x3fff;
    b.bg.stride1 = R(PPP_BG_YSTRIDE) >> 16 & 0x3fff;
    b.bg.hsub = b.bg.vsub = 1;
    b.bx = R(PPP_BG_XY) & 0x1fff;
    b.by = (R(PPP_BG_XY) >> 16) & 0x1fff;
    b.const_alpha = R(PPP_ALPHA_TRANSP) >> 24;
    b.bgsel = R(PPP_BG_ALPHA_SEL);

    /* the source after rotation is what gets scaled onto the dst ROI */
    rw = rot ? sh : sw;
    rh = rot ? sw : sh;
    if (!(op & (OP_SCALE_X | OP_SCALE_Y))) {
        dw = MIN(dw, rw);
        dh = MIN(dh, rh);
    }
    b.dw = dw;
    b.verify = s->verify;
    ppp_blit_prepare(&b);

    n = MAX(sw, dw);
    sline = g_malloc(n * 4 * 2);
    srow = g_malloc(n * 4);
    out = g_malloc(dw * 4);
    b.bline = g_malloc(dw * 4 * 2);
    b.brgb = g_malloc(dw * 4);
    b.dline = g_malloc(dw * 4);

    if (rot) {
        /*
         * 90 degrees clockwise: rotated pixel (rx, ry) is source pixel
         * (ry, sh - 1 - rx).  Fetch the whole source ROI once.
         */
        all = g_malloc((size_t)sw * sh * 4);
        for (y = 0; y < sh; y++) {
            ppp_fetch_row(&src, sx, sy + y, sw, sline,
                          all + (size_t)y * sw * 4);
        }
    }
    for (y = 0; y < dh; y++) {
        int ry = (int)((int64_t)y * rh / dh);

        if (op & OP_FLIP_UD) {
            ry = rh - 1 - ry;
        }
        if (!rot) {
            ppp_fetch_row(&src, sx, sy + ry, sw, sline, srow);
        }
        for (x = 0; x < dw; x++) {
            int rx = (int)((int64_t)x * rw / dw);
            const uint8_t *p;

            if (op & OP_FLIP_LR) {
                rx = rw - 1 - rx;
            }
            p = rot ? all + ((size_t)(sh - 1 - rx) * sw + ry) * 4
                    : srow + rx * 4;
            memcpy(out + x * 4, p, 4);
        }
        ppp_store_row(&b, y, out);
    }

    g_free(all);
    g_free(sline);
    g_free(srow);
    g_free(out);
    g_free(b.bline);
    g_free(b.brgb);
    g_free(b.dline);
}
