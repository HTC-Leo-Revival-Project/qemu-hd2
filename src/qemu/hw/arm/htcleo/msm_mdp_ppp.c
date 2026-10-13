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
#include "system/physmem.h"
#include "htcleo.h"
#include "msm_mdp.h"

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
} PppImg;

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

/* read a row of an RGB or YCbCr image as G,B,R,A */
static void ppp_fetch_row(const PppImg *im, int x0, int y, int w,
                          uint8_t *line, uint8_t *out)
{
    int x;

    if (!im->pseudoplanar) {
        physical_memory_read(im->base0 + (hwaddr)y * im->stride0 +
                             (hwaddr)x0 * im->bpp, line, w * im->bpp);
        for (x = 0; x < w; x++) {
            ppp_unpack(im, ld_le(line + x * im->bpp, im->bpp), out + x * 4);
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
} PppBlit;

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

/* blend src row (G,B,R,A, dw pixels) with the background and store row y */
static void ppp_store_row(PppBlit *b, int y, uint8_t *srgb)
{
    int x, i;

    if (b->op & OP_BLEND_ON) {
        ppp_fetch_row(&b->bg, b->bx, b->by + y, b->dw, b->bline, b->brgb);
        for (x = 0; x < b->dw; x++) {
            uint8_t *f = srgb + x * 4, *g = b->brgb + x * 4;
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
            for (i = 0; i < 4; i++) {
                f[i] = clamp8((f[i] * fa + g[i] * ba + 127) / 255);
            }
        }
    }
    for (x = 0; x < b->dw; x++) {
        st_le(b->dline + x * b->dst.bpp, b->dst.bpp,
              ppp_pack(&b->dst, srgb + x * 4));
    }
    physical_memory_write(b->dst.base0 + (hwaddr)(b->dy + y) * b->dst.stride0
                          + (hwaddr)b->dx * b->dst.bpp, b->dline,
                          b->dw * b->dst.bpp);
}

void msm_mdp_ppp_run(MsmMdpState *s)
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
