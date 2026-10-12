/*
 * Adreno 200 3D pipe: vertex fetch and shading, primitive assembly,
 * clipping, rasterization, render backend and resolves
 *
 * A draw either renders (RB_MODECONTROL COLOR_DEPTH or DEPTH_ONLY) or,
 * in EDRAM_COPY mode, resolves: the rectangle the vertices cover is
 * copied from GMEM to RB_COPY_DEST_* in memory and optionally cleared.
 *
 * Render targets live in GMEM when their base falls in RB_EDRAM_INFO's
 * range, else in memory (GPU addresses).  Surfaces are linear with
 * RB_SURFACE_INFO's pitch: GMEM is invisible to the CPU, so its layout
 * only has to agree with itself.
 *
 * Colour formats pack channels from bit 0 up (R, G, B, A); SWAP exchanges
 * R and B.  Texture formats likewise pack X, Y, Z, W from bit 0 and the
 * fetch constant swizzles them.
 *
 * Coverage uses 1/16-pixel fixed point and the top-left rule; attributes
 * are interpolated perspective-correctly unless flat shaded.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "msm_adreno.h"

#define SUBPIX      16
#define MAX_CLIPPED 12

/* ------------------------------------------------------------------ */
/* Colour formats                                                      */
/* ------------------------------------------------------------------ */

typedef struct ColorFmt {
    uint8_t bpp;        /* bytes per pixel */
    uint8_t bits[4];    /* channel widths from bit 0 */
    uint8_t kind;       /* 0 unorm, 1 snorm, 2 float16, 3 float32 */
} ColorFmt;

static const ColorFmt color_fmts[16] = {
    [0]  = { 2, { 4, 4, 4, 4 }, 0 },     /* COLORX_4_4_4_4 */
    [1]  = { 2, { 5, 5, 5, 1 }, 0 },     /* COLORX_1_5_5_5 */
    [2]  = { 2, { 5, 6, 5, 0 }, 0 },     /* COLORX_5_6_5 */
    [3]  = { 1, { 8, 0, 0, 0 }, 0 },     /* COLORX_8 */
    [4]  = { 2, { 8, 8, 0, 0 }, 0 },     /* COLORX_8_8 */
    [5]  = { 4, { 8, 8, 8, 8 }, 0 },     /* COLORX_8_8_8_8 */
    [6]  = { 4, { 8, 8, 8, 8 }, 1 },     /* COLORX_S8_8_8_8 */
    [7]  = { 2, { 16, 0, 0, 0 }, 2 },    /* COLORX_16_FLOAT */
    [8]  = { 4, { 16, 16, 0, 0 }, 2 },   /* COLORX_16_16_FLOAT */
    [9]  = { 8, { 16, 16, 16, 16 }, 2 }, /* COLORX_16_16_16_16_FLOAT */
    [10] = { 4, { 32, 0, 0, 0 }, 3 },    /* COLORX_32_FLOAT */
    [11] = { 8, { 32, 32, 0, 0 }, 3 },   /* COLORX_32_32_FLOAT */
    [12] = { 16, { 32, 32, 32, 32 }, 3 },/* COLORX_32_32_32_32_FLOAT */
    [13] = { 1, { 3, 3, 2, 0 }, 0 },     /* COLORX_2_3_3 */
    [14] = { 3, { 8, 8, 8, 0 }, 0 },     /* COLORX_8_8_8 */
    [15] = { 4, { 8, 8, 8, 8 }, 0 },
};

static uint16_t float_to_half(float f)
{
    uint32_t u = adreno_u(f);
    uint32_t sign = (u >> 16) & 0x8000;
    int exp = ((u >> 23) & 0xff) - 127 + 15;
    uint32_t man = u & 0x7fffff;

    if (((u >> 23) & 0xff) == 0xff) {
        return sign | 0x7c00 | (man ? 0x200 : 0);
    }
    if (exp >= 31) {
        return sign | 0x7c00;
    }
    if (exp <= 0) {
        if (exp < -10) {
            return sign;
        }
        man |= 0x800000;
        return sign | (man >> (14 - exp));
    }
    return sign | (exp << 10) | (man >> 13);
}

static float half_to_float(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    float f;

    if (exp == 0) {
        f = ldexpf(man, -24);
    } else if (exp == 31) {
        f = man ? NAN : INFINITY;
    } else {
        f = ldexpf(man | 0x400, (int)exp - 25);
    }
    return sign ? -f : f;
}

static inline float clamp01(float f)
{
    return f > 1.0f ? 1.0f : (f >= 0.0f ? f : 0.0f);
}

/* pixel bytes -> channels c0..c3 (before SWAP) */
static void color_unpack(unsigned fmt, const uint8_t *p, float c[4])
{
    const ColorFmt *f = &color_fmts[fmt & 15];

    c[0] = c[1] = c[2] = 0.0f;
    c[3] = 1.0f;
    if (f->kind == 2) {
        for (int k = 0; k < 4 && f->bits[k]; k++) {
            c[k] = half_to_float(lduw_le_p(p + 2 * k));
        }
        return;
    }
    if (f->kind == 3) {
        for (int k = 0; k < 4 && f->bits[k]; k++) {
            c[k] = adreno_f(ldl_le_p(p + 4 * k));
        }
        return;
    }
    {
        uint32_t v = 0;
        int pos = 0;

        for (int i = 0; i < f->bpp; i++) {
            v |= (uint32_t)p[i] << (8 * i);
        }
        for (int k = 0; k < 4 && f->bits[k]; k++) {
            uint32_t mask = (1u << f->bits[k]) - 1;
            uint32_t x = (v >> pos) & mask;

            if (f->kind == 1) {
                int32_t sx = (int32_t)(x << (32 - f->bits[k])) >>
                             (32 - f->bits[k]);
                c[k] = MAX(-1.0f, sx / (float)(mask >> 1));
            } else {
                c[k] = x / (float)mask;
            }
            pos += f->bits[k];
        }
    }
}

static void color_pack(unsigned fmt, const float c[4], uint8_t *p)
{
    const ColorFmt *f = &color_fmts[fmt & 15];

    if (f->kind == 2) {
        for (int k = 0; k < 4 && f->bits[k]; k++) {
            stw_le_p(p + 2 * k, float_to_half(c[k]));
        }
        return;
    }
    if (f->kind == 3) {
        for (int k = 0; k < 4 && f->bits[k]; k++) {
            stl_le_p(p + 4 * k, adreno_u(c[k]));
        }
        return;
    }
    {
        uint32_t v = 0;
        int pos = 0;

        for (int k = 0; k < 4 && f->bits[k]; k++) {
            uint32_t mask = (1u << f->bits[k]) - 1;
            uint32_t x;

            if (f->kind == 1) {
                float s = MIN(MAX(c[k], -1.0f), 1.0f) * (mask >> 1);
                x = (uint32_t)(int32_t)lrintf(s) & mask;
            } else {
                x = (uint32_t)lrintf(clamp01(c[k]) * mask);
            }
            v |= x << pos;
            pos += f->bits[k];
        }
        for (int i = 0; i < f->bpp; i++) {
            p[i] = v >> (8 * i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

typedef struct Surface {
    uint8_t *host;      /* GMEM, or NULL for memory */
    uint32_t gpuaddr;   /* memory surfaces */
    uint32_t offset;    /* GMEM byte offset */
    uint32_t pitch;     /* pixels */
    uint32_t bpp;
    uint32_t limit;     /* bytes addressable from the base */
} Surface;

static void surface_setup(MsmAdrenoState *s, Surface *sf, uint32_t base,
                          uint32_t pitch, uint32_t bpp)
{
    uint32_t info = s->regs[A2_RB_EDRAM_INFO];
    uint32_t ebase = info & 0xffffc000;
    uint32_t esize = (16 * 1024) << (info & 0xf);

    esize = MIN(esize, ADRENO_GMEM_SIZE);
    sf->pitch = pitch;
    sf->bpp = bpp;
    if (base - ebase < esize) {
        sf->host = s->gmem;
        sf->offset = base - ebase;
        sf->limit = ADRENO_GMEM_SIZE - sf->offset;
    } else {
        sf->host = NULL;
        sf->gpuaddr = base;
        sf->offset = 0;
        sf->limit = UINT32_MAX;
    }
}

static bool surface_rw(MsmAdrenoState *s, Surface *sf, int x, int y,
                       uint8_t *buf, bool write)
{
    uint64_t off;

    if (x < 0 || y < 0) {
        return false;
    }
    off = ((uint64_t)y * sf->pitch + x) * sf->bpp;
    if (off + sf->bpp > sf->limit) {
        return false;
    }
    if (sf->host) {
        uint8_t *p = sf->host + sf->offset + off;

        if (write) {
            memcpy(p, buf, sf->bpp);
        } else {
            memcpy(buf, p, sf->bpp);
        }
        return true;
    }
    if (write) {
        adreno_write(s, sf->gpuaddr + off, buf, sf->bpp);
    } else {
        adreno_read(s, sf->gpuaddr + off, buf, sf->bpp);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */

typedef struct TexFmt {
    uint8_t bpp;
    uint8_t bits[4];
} TexFmt;

static bool tex_format(unsigned fmt, TexFmt *t)
{
    static const TexFmt tbl[64] = {
        [2]  = { 1, { 8, 0, 0, 0 } },        /* FMT_8 */
        [3]  = { 2, { 5, 5, 5, 1 } },        /* FMT_1_5_5_5 */
        [4]  = { 2, { 5, 6, 5, 0 } },        /* FMT_5_6_5 */
        [5]  = { 2, { 6, 5, 5, 0 } },        /* FMT_6_5_5 */
        [6]  = { 4, { 8, 8, 8, 8 } },        /* FMT_8_8_8_8 */
        [7]  = { 4, { 10, 10, 10, 2 } },     /* FMT_2_10_10_10 */
        [8]  = { 1, { 8, 0, 0, 0 } },        /* FMT_8_A */
        [9]  = { 1, { 8, 0, 0, 0 } },        /* FMT_8_B */
        [10] = { 2, { 8, 8, 0, 0 } },        /* FMT_8_8 */
        [13] = { 2, { 1, 5, 5, 5 } },        /* FMT_5_5_5_1 */
        [14] = { 4, { 8, 8, 8, 8 } },        /* FMT_8_8_8_8_A */
        [15] = { 2, { 4, 4, 4, 4 } },        /* FMT_4_4_4_4 */
        [16] = { 3, { 8, 8, 8, 0 } },        /* FMT_8_8_8 */
        [21] = { 4, { 2, 10, 10, 10 } },     /* FMT_10_10_10_2 */
        [22] = { 4, { 8, 24, 0, 0 } },       /* FMT_24_8 */
        [24] = { 2, { 16, 0, 0, 0 } },       /* FMT_16 */
        [25] = { 4, { 16, 16, 0, 0 } },      /* FMT_16_16 */
        [26] = { 8, { 16, 16, 16, 16 } },    /* FMT_16_16_16_16 */
    };

    *t = tbl[fmt & 63];
    return t->bpp != 0;
}

/* ETC1 4x4 block decode of one texel */
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
            int c4 = (hi >> (28 - 8 * k - (sub ? 4 : 0))) & 0xf;

            base[k] = c4 * 17;
        }
    }
    cw = (hi >> (sub ? 2 : 5)) & 7;
    mod = tbl[cw][(msb << 1) | lsb];
    for (int k = 0; k < 3; k++) {
        out[k] = MIN(MAX(base[k] + mod, 0), 255) / 255.0f;
    }
    out[3] = 1.0f;
}

static int tex_wrap(int i, int size, unsigned mode)
{
    switch (mode) {
    case 0:     /* wrap */
        i %= size;
        return i < 0 ? i + size : i;
    case 1: {   /* mirror */
        int p = size * 2;

        i %= p;
        if (i < 0) {
            i += p;
        }
        return i < size ? i : p - 1 - i;
    }
    default:    /* clamp variants */
        return MIN(MAX(i, 0), size - 1);
    }
}

static void tex_texel(MsmAdrenoState *s, const uint32_t *fc, int x, int y,
                      float out[4])
{
    unsigned fmt = fc[1] & 0x3f;
    uint32_t base = fc[1] & 0xfffff000;
    uint32_t pitch = ((fc[0] >> 22) & 0x1ff) * 32;
    uint32_t width = (fc[2] & 0x1fff) + 1;
    TexFmt tf;
    uint8_t px[8] = {0};

    if (!pitch) {
        pitch = width;
    }
    if (fmt == 47 || fmt == 46 || fmt == 48) {     /* ETC1 */
        uint32_t bw = (pitch + 3) / 4;
        uint8_t blk[8];

        adreno_read(s, base + ((y / 4) * bw + x / 4) * 8, blk, 8);
        etc1_texel(blk, x & 3, y & 3, out);
        return;
    }
    if (!tex_format(fmt, &tf)) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    adreno_read(s, base + (y * pitch + x) * tf.bpp, px, tf.bpp);
    {
        uint64_t v = ldq_le_p(px);
        bool sign = (fc[0] >> 2) & 3;   /* sign of X; all alike in practice */
        int pos = 0;

        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        for (int k = 0; k < 4 && tf.bits[k]; k++) {
            uint64_t mask = (1ull << tf.bits[k]) - 1;
            uint64_t c = (v >> pos) & mask;

            if (sign == 1) {
                int64_t sc = (int64_t)(c << (64 - tf.bits[k])) >>
                             (64 - tf.bits[k]);
                out[k] = MAX(-1.0f, sc / (float)(mask >> 1));
            } else {
                out[k] = c / (float)mask;
            }
            pos += tf.bits[k];
        }
    }
}

void adreno_sample_texture(MsmAdrenoState *s, unsigned idx,
                           const float coord[4], uint32_t insn_dword1,
                           uint32_t insn_dword2, float out[4])
{
    const uint32_t *fc = &s->regs[A2_SQ_FETCH_0 + idx * 6];
    uint32_t width = (fc[2] & 0x1fff) + 1;
    uint32_t height = ((fc[2] >> 13) & 0x1fff) + 1;
    unsigned cx = (fc[0] >> 10) & 7, cy = (fc[0] >> 13) & 7;
    unsigned mag = (insn_dword1 >> 12) & 3, min = (insn_dword1 >> 14) & 3;
    unsigned filt;
    float u = coord[0], v = coord[1];
    float raw[4];
    uint32_t swz = (fc[3] >> 1) & 0xfff;

    /* filters 3 = from the fetch constant */
    if (mag == 3) {
        mag = (fc[3] >> 19) & 3;
    }
    if (min == 3) {
        min = (fc[3] >> 21) & 3;
    }
    filt = mag;     /* no LOD: magnification filter for everything */
    (void)min;
    (void)insn_dword2;

    u *= width;
    v *= height;
    if (filt == 1) {
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
        float ax = fu - x0, ay = fv - y0;
        float t[4][4];

        tex_texel(s, fc, tex_wrap(x0, width, cx), tex_wrap(y0, height, cy),
                  t[0]);
        tex_texel(s, fc, tex_wrap(x0 + 1, width, cx),
                  tex_wrap(y0, height, cy), t[1]);
        tex_texel(s, fc, tex_wrap(x0, width, cx),
                  tex_wrap(y0 + 1, height, cy), t[2]);
        tex_texel(s, fc, tex_wrap(x0 + 1, width, cx),
                  tex_wrap(y0 + 1, height, cy), t[3]);
        for (int k = 0; k < 4; k++) {
            float top = t[0][k] + (t[1][k] - t[0][k]) * ax;
            float bot = t[2][k] + (t[3][k] - t[2][k]) * ax;

            raw[k] = top + (bot - top) * ay;
        }
    } else {
        tex_texel(s, fc, tex_wrap((int)floorf(u), width, cx),
                  tex_wrap((int)floorf(v), height, cy), raw);
    }
    for (int k = 0; k < 4; k++) {
        unsigned sel = (swz >> (3 * k)) & 7;

        out[k] = sel < 4 ? raw[sel] : (sel == 5 ? 1.0f : 0.0f);
    }
}

/* ------------------------------------------------------------------ */
/* Vertices                                                            */
/* ------------------------------------------------------------------ */

#define VCACHE 64

typedef struct DrawCtx {
    MsmAdrenoState *s;
    uint32_t prog_cntl;
    int ninterp;
    /* vertex cache */
    uint32_t vc_tag[VCACHE];
    bool vc_valid[VCACHE];
    AdrenoVertex vc[VCACHE];
    /* raster state */
    int clip_x0, clip_y0, clip_x1, clip_y1;    /* inclusive-exclusive */
    float pix_center;
    int win_x, win_y;
    bool win_offset_vtx;
    Surface color, depth;
    unsigned color_fmt, color_swap;
    bool depth_24;
    bool color_write, depth_enabled;
    uint32_t flat_mask;
    bool provoking_last;
} DrawCtx;

static void run_vs(DrawCtx *d, uint32_t index, AdrenoVertex *out)
{
    AdrenoShaderCtx c = {
        .s = d->s, .type = A2_SHADER_VS,
        .code = d->s->imem[A2_SHADER_VS],
        .code_size = d->s->imem_size[A2_SHADER_VS],
        .vout = out,
    };

    memset(out, 0, sizeof(*out));
    out->pos[3] = 1.0f;
    memset(c.r, 0, sizeof(c.r));
    c.r[0][0] = index;
    adreno_shader_run(&c);
}

static AdrenoVertex *get_vertex(DrawCtx *d, uint32_t index)
{
    unsigned slot = index % VCACHE;

    if (!d->vc_valid[slot] || d->vc_tag[slot] != index) {
        run_vs(d, index, &d->vc[slot]);
        d->vc_tag[slot] = index;
        d->vc_valid[slot] = true;
    }
    return &d->vc[slot];
}

/* ------------------------------------------------------------------ */
/* Clipping and the viewport                                           */
/* ------------------------------------------------------------------ */

static void lerp_vertex(const AdrenoVertex *a, const AdrenoVertex *b,
                        float t, int ninterp, AdrenoVertex *o)
{
    for (int k = 0; k < 4; k++) {
        o->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t;
    }
    o->psize = a->psize;
    for (int i = 0; i < ninterp; i++) {
        for (int k = 0; k < 4; k++) {
            o->interp[i][k] = a->interp[i][k] +
                              (b->interp[i][k] - a->interp[i][k]) * t;
        }
    }
}

/* clip a polygon against plane dot(p, pos) >= 0 */
static int clip_plane(AdrenoVertex *in, int n, AdrenoVertex *out,
                      const float pl[4], int ninterp)
{
    int m = 0;

    for (int i = 0; i < n; i++) {
        AdrenoVertex *a = &in[i], *b = &in[(i + 1) % n];
        float da = pl[0] * a->pos[0] + pl[1] * a->pos[1] +
                   pl[2] * a->pos[2] + pl[3] * a->pos[3];
        float db = pl[0] * b->pos[0] + pl[1] * b->pos[1] +
                   pl[2] * b->pos[2] + pl[3] * b->pos[3];

        if (da >= 0) {
            out[m++] = *a;
        }
        if ((da >= 0) != (db >= 0) && m < MAX_CLIPPED) {
            lerp_vertex(a, b, da / (da - db), ninterp, &out[m++]);
        }
        if (m >= MAX_CLIPPED) {
            break;
        }
    }
    return m;
}

/* screen-space vertex: x, y (with window offset), z, 1/w */
typedef struct SVert {
    float x, y, z, rhw;
    const AdrenoVertex *v;
} SVert;

static void to_screen(DrawCtx *d, const AdrenoVertex *v, SVert *o)
{
    uint32_t *r = d->s->regs;
    uint32_t vte = r[A2_PA_CL_VTE_CNTL];
    float x = v->pos[0], y = v->pos[1], z = v->pos[2], w = v->pos[3];
    float rhw = (vte & (1 << 10)) ? 1.0f / w : w;

    if (!(vte & (1 << 8))) {
        x *= rhw;
        y *= rhw;
    }
    if (!(vte & (1 << 9))) {
        z *= rhw;
    }
    if (vte & (1 << 0)) {
        x *= adreno_f(r[A2_PA_CL_VPORT_XSCALE]);
    }
    if (vte & (1 << 1)) {
        x += adreno_f(r[A2_PA_CL_VPORT_XOFFSET]);
    }
    if (vte & (1 << 2)) {
        y *= adreno_f(r[A2_PA_CL_VPORT_YSCALE]);
    }
    if (vte & (1 << 3)) {
        y += adreno_f(r[A2_PA_CL_VPORT_YOFFSET]);
    }
    if (vte & (1 << 4)) {
        z *= adreno_f(r[A2_PA_CL_VPORT_ZSCALE]);
    }
    if (vte & (1 << 5)) {
        z += adreno_f(r[A2_PA_CL_VPORT_ZOFFSET]);
    }
    if (d->win_offset_vtx) {
        x += d->win_x;
        y += d->win_y;
    }
    o->x = x;
    o->y = y;
    o->z = z;
    o->rhw = rhw;
    o->v = v;
}

/* ------------------------------------------------------------------ */
/* Render backend                                                      */
/* ------------------------------------------------------------------ */

static bool compare(unsigned func, float a, float b)
{
    switch (func & 7) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    }
    return true;
}

static uint32_t stencil_op(unsigned op, uint32_t v, uint32_t ref)
{
    switch (op & 7) {
    case 1: return 0;
    case 2: return ref;
    case 3: return v < 255 ? v + 1 : 255;
    case 4: return v > 0 ? v - 1 : 0;
    case 5: return ~v & 0xff;
    case 6: return (v + 1) & 0xff;
    case 7: return (v - 1) & 0xff;
    }
    return v;
}

static void blend_factor(unsigned f, const float src[4], const float dst[4],
                         const float cst[4], int k, float *out)
{
    switch (f) {
    case 0: *out = 0; break;
    case 1: *out = 1; break;
    case 4: *out = src[k]; break;
    case 5: *out = 1 - src[k]; break;
    case 6: *out = src[3]; break;
    case 7: *out = 1 - src[3]; break;
    case 8: *out = dst[k]; break;
    case 9: *out = 1 - dst[k]; break;
    case 10: *out = dst[3]; break;
    case 11: *out = 1 - dst[3]; break;
    case 12: *out = cst[k]; break;
    case 13: *out = 1 - cst[k]; break;
    case 14: *out = cst[3]; break;
    case 15: *out = 1 - cst[3]; break;
    case 16: *out = k == 3 ? 1 : MIN(src[3], 1 - dst[3]); break;
    default: *out = 1; break;
    }
}

static float blend_op(unsigned op, float s, float d, float sf, float df)
{
    switch (op & 7) {
    case 0: return s * sf + d * df;
    case 1: return s * sf - d * df;
    case 2: return MIN(s, d);
    case 3: return MAX(s, d);
    case 4: return d * df - s * sf;
    }
    return s * sf + d * df;
}

/* depth/stencil test and update; returns false when the pixel fails */
static bool depth_stencil(DrawCtx *d, int x, int y, float z, bool backface,
                          bool write_ok, bool late)
{
    uint32_t *r = d->s->regs;
    uint32_t dc = r[A2_RB_DEPTHCONTROL];
    bool stencil_en = dc & 1, z_en = dc & 2, z_write = dc & 4;
    uint8_t buf[4];
    uint32_t raw, stored_z, sten;
    float dz;
    bool pass = true, zpass = true;
    uint32_t refmask = r[backface && (dc & (1 << 7)) ? A2_RB_STENCILREFMASK_BF
                                                     : A2_RB_STENCILREFMASK];
    uint32_t ref = refmask & 0xff, mask = (refmask >> 8) & 0xff;
    uint32_t wmask = (refmask >> 16) & 0xff;
    unsigned sfunc, sfail, szpass, szfail;

    (void)late;
    if (!d->depth_enabled || (!stencil_en && !z_en)) {
        return true;
    }
    if (!surface_rw(d->s, &d->depth, x, y, buf, false)) {
        return true;
    }
    if (d->depth_24) {
        raw = ldl_le_p(buf);
        stored_z = raw >> 8;
        sten = raw & 0xff;
        dz = stored_z / 16777215.0f;
    } else {
        raw = lduw_le_p(buf);
        stored_z = raw;
        sten = 0;
        dz = stored_z / 65535.0f;
    }
    if (backface && (dc & (1 << 7))) {
        sfunc = (dc >> 20) & 7;
        sfail = (dc >> 23) & 7;
        szpass = (dc >> 26) & 7;
        szfail = (dc >> 29) & 7;
    } else {
        sfunc = (dc >> 8) & 7;
        sfail = (dc >> 11) & 7;
        szpass = (dc >> 14) & 7;
        szfail = (dc >> 17) & 7;
    }
    if (stencil_en) {
        pass = compare(sfunc, ref & mask, sten & mask);
    }
    if (pass && z_en) {
        zpass = compare((dc >> 4) & 7, clamp01(z), dz);
    }
    if (stencil_en && d->depth_24) {
        uint32_t ns;

        if (!pass) {
            ns = stencil_op(sfail, sten, ref);
        } else if (!zpass) {
            ns = stencil_op(szfail, sten, ref);
        } else {
            ns = stencil_op(szpass, sten, ref);
        }
        sten = (sten & ~wmask) | (ns & wmask);
    }
    if (pass && zpass && z_en && z_write && write_ok) {
        stored_z = d->depth_24 ? (uint32_t)lrintf(clamp01(z) * 16777215.0f)
                               : (uint32_t)lrintf(clamp01(z) * 65535.0f);
    }
    if (d->depth_24) {
        stl_le_p(buf, (stored_z << 8) | sten);
    } else {
        stw_le_p(buf, stored_z);
    }
    if ((stencil_en || (z_en && z_write)) && write_ok) {
        surface_rw(d->s, &d->depth, x, y, buf, true);
    }
    return pass && zpass;
}

static void write_color(DrawCtx *d, int x, int y, const float in[4])
{
    uint32_t *r = d->s->regs;
    uint32_t cc = r[A2_RB_COLORCONTROL];
    uint32_t bc = r[A2_RB_BLEND_CONTROL];
    uint32_t cmask = r[A2_RB_COLOR_MASK] & 0xf;
    const ColorFmt *f = &color_fmts[d->color_fmt];
    uint8_t buf[16];
    float src[4], dst[4], out[4];
    bool blend;

    if (!d->color_write || !cmask) {
        return;
    }
    memcpy(src, in, sizeof(src));
    if (f->kind == 0) {
        for (int k = 0; k < 4; k++) {
            src[k] = clamp01(src[k]);
        }
    }
    blend = !(cc & (1 << 5)) && bc != 0x00010001;
    if (blend || cmask != 0xf) {
        if (!surface_rw(d->s, &d->color, x, y, buf, false)) {
            return;
        }
        color_unpack(d->color_fmt, buf, dst);
        if (d->color_swap == 1) {
            float t = dst[0];

            dst[0] = dst[2];
            dst[2] = t;
        }
    } else {
        memset(dst, 0, sizeof(dst));
    }
    if (blend) {
        float cst[4] = {
            (r[A2_RB_BLEND_RED] & 0xff) / 255.0f,
            (r[A2_RB_BLEND_GREEN] & 0xff) / 255.0f,
            (r[A2_RB_BLEND_BLUE] & 0xff) / 255.0f,
            (r[A2_RB_BLEND_ALPHA] & 0xff) / 255.0f,
        };

        for (int k = 0; k < 4; k++) {
            unsigned sfac = k < 3 ? bc & 0x1f : (bc >> 16) & 0x1f;
            unsigned dfac = k < 3 ? (bc >> 8) & 0x1f : (bc >> 24) & 0x1f;
            unsigned op = k < 3 ? (bc >> 5) & 7 : (bc >> 21) & 7;
            float sf, df;

            blend_factor(sfac, src, dst, cst, k, &sf);
            blend_factor(dfac, src, dst, cst, k, &df);
            out[k] = blend_op(op, src[k], dst[k], sf, df);
        }
    } else {
        memcpy(out, src, sizeof(out));
    }
    for (int k = 0; k < 4; k++) {
        if (!(cmask & (1 << k))) {
            out[k] = dst[k];
        }
    }
    if (d->color_swap == 1) {
        float t = out[0];

        out[0] = out[2];
        out[2] = t;
    }
    color_pack(d->color_fmt, out, buf);
    surface_rw(d->s, &d->color, x, y, buf, true);
}

/* ------------------------------------------------------------------ */
/* Rasterizer                                                          */
/* ------------------------------------------------------------------ */

static void shade_pixel(DrawCtx *d, int x, int y, const SVert *v[3],
                        const float bary[3], bool backface)
{
    MsmAdrenoState *s = d->s;
    uint32_t *r = s->regs;
    AdrenoPixelOut po = { 0 };
    AdrenoShaderCtx c = {
        .s = s, .type = A2_SHADER_PS,
        .code = s->imem[A2_SHADER_PS],
        .code_size = s->imem_size[A2_SHADER_PS],
        .pout = &po,
    };
    uint32_t ps_cntl = d->prog_cntl;
    float z = bary[0] * v[0]->z + bary[1] * v[1]->z + bary[2] * v[2]->z;
    float rhw = bary[0] * v[0]->rhw + bary[1] * v[1]->rhw +
                bary[2] * v[2]->rhw;
    float pw[3];
    bool early_z = false;
    uint32_t cc = r[A2_RB_COLORCONTROL];

    if (rhw != 0.0f && !(r[A2_PA_SU_SC_MODE_CNTL] & (1 << 20))) {
        for (int i = 0; i < 3; i++) {
            pw[i] = bary[i] * v[i]->rhw / rhw;
        }
    } else {
        memcpy(pw, bary, sizeof(pw));
    }

    /* the pixel shader's inputs: interpolators in r0.. */
    for (int i = 0; i < d->ninterp; i++) {
        const AdrenoVertex *pv = v[d->provoking_last ? 2 : 0]->v;

        for (int k = 0; k < 4; k++) {
            if (d->flat_mask & (1u << i)) {
                c.r[i][k] = pv->interp[i][k];
            } else {
                c.r[i][k] = pw[0] * v[0]->v->interp[i][k] +
                            pw[1] * v[1]->v->interp[i][k] +
                            pw[2] * v[2]->v->interp[i][k];
            }
        }
    }
    if (ps_cntl & (1 << 18)) {
        /* PARAM_GEN: screen position (sign of x = back facing) */
        unsigned pg = (r[A2_SQ_CONTEXT_MISC] >> 8) & 0xff;

        if (pg < 64) {
            c.r[pg][0] = (x - d->win_x) + 0.5f;
            c.r[pg][1] = (y - d->win_y) + 0.5f;
            if (backface) {
                c.r[pg][0] = -c.r[pg][0];
            }
            c.r[pg][2] = 0.0f;
            c.r[pg][3] = 0.0f;
        }
    }
    (void)early_z;

    adreno_shader_run(&c);
    if (po.killed) {
        return;
    }
    s->pixels++;

    /* alpha test */
    if (cc & (1 << 3)) {
        if (!compare(cc & 7, po.color[0][3],
                     adreno_f(r[A2_RB_ALPHA_REF]))) {
            return;
        }
    }
    if (!depth_stencil(d, x, y, po.depth_written ? po.depth : z, backface,
                       true, false)) {
        return;
    }
    write_color(d, x, y, po.color[0]);
}

static void raster_triangle(DrawCtx *d, const SVert *a, const SVert *b,
                            const SVert *c)
{
    uint32_t sc = d->s->regs[A2_PA_SU_SC_MODE_CNTL];
    int64_t x0 = llrintf(a->x * SUBPIX), y0 = llrintf(a->y * SUBPIX);
    int64_t x1 = llrintf(b->x * SUBPIX), y1 = llrintf(b->y * SUBPIX);
    int64_t x2 = llrintf(c->x * SUBPIX), y2 = llrintf(c->y * SUBPIX);
    int64_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    const SVert *v[3] = { a, b, c };
    bool cw, backface;
    int minx, maxx, miny, maxy;
    int64_t cofs = llrintf(d->pix_center * SUBPIX);

    if (area == 0 || !isfinite(a->x + b->x + c->x + a->y + b->y + c->y)) {
        return;
    }
    /* positive area: clockwise on screen (y down) */
    cw = area > 0;
    backface = (sc & (1 << 2)) ? !cw : cw;
    if ((backface && (sc & 2)) || (!backface && (sc & 1))) {
        return;
    }
    if (area < 0) {
        /* make the edge functions positive inside */
        const SVert *t = v[1];
        int64_t tx = x1, ty = y1;

        v[1] = v[2];
        v[2] = t;
        x1 = x2;
        y1 = y2;
        x2 = tx;
        y2 = ty;
        area = -area;
    }

    minx = MAX(d->clip_x0, (int)((MIN(MIN(x0, x1), x2) - cofs) / SUBPIX) - 1);
    maxx = MIN(d->clip_x1 - 1,
               (int)((MAX(MAX(x0, x1), x2) - cofs) / SUBPIX) + 1);
    miny = MAX(d->clip_y0, (int)((MIN(MIN(y0, y1), y2) - cofs) / SUBPIX) - 1);
    maxy = MIN(d->clip_y1 - 1,
               (int)((MAX(MAX(y0, y1), y2) - cofs) / SUBPIX) + 1);
    if (minx > maxx || miny > maxy) {
        return;
    }

    {
        /* edge i is opposite vertex i: e = A x + B y + C, >= 0 inside */
        int64_t ex[3][2] = { { x1, y1 }, { x2, y2 }, { x0, y0 } };
        int64_t ey[3][2] = { { x2, y2 }, { x0, y0 }, { x1, y1 } };
        int64_t A[3], B[3], C[3];
        bool tl[3];
        float inv_area = 1.0f / (float)area;

        for (int i = 0; i < 3; i++) {
            A[i] = ex[i][1] - ey[i][1];
            B[i] = ey[i][0] - ex[i][0];
            C[i] = ex[i][0] * ey[i][1] - ey[i][0] * ex[i][1];
            /* top edge (horizontal, inside below) or left edge */
            tl[i] = (A[i] == 0 && B[i] > 0) || A[i] > 0;
        }
        for (int py = miny; py <= maxy; py++) {
            int64_t sy = (int64_t)py * SUBPIX + cofs;

            for (int px = minx; px <= maxx; px++) {
                int64_t sx = (int64_t)px * SUBPIX + cofs;
                int64_t e[3];
                bool in = true;

                for (int i = 0; i < 3; i++) {
                    e[i] = A[i] * sx + B[i] * sy + C[i];
                    if (e[i] < 0 || (e[i] == 0 && !tl[i])) {
                        in = false;
                        break;
                    }
                }
                if (in) {
                    float bary[3] = {
                        e[0] * inv_area, e[1] * inv_area, e[2] * inv_area,
                    };

                    shade_pixel(d, px, py, v, bary, backface);
                }
            }
        }
    }
}

static void draw_triangle(DrawCtx *d, const AdrenoVertex *a,
                          const AdrenoVertex *b, const AdrenoVertex *c)
{
    uint32_t clip = d->s->regs[A2_PA_CL_CLIP_CNTL];
    AdrenoVertex buf[2][MAX_CLIPPED];
    SVert sv[MAX_CLIPPED];
    int n = 3, cur = 0;

    buf[0][0] = *a;
    buf[0][1] = *b;
    buf[0][2] = *c;
    if (!(clip & (1 << 16))) {
        bool dx = clip & (1 << 19);
        float planes[][4] = {
            { 0, 0, 0, 1 },                     /* w > 0 (approximately) */
            { 1, 0, 0, 1 }, { -1, 0, 0, 1 },    /* -w <= x <= w */
            { 0, 1, 0, 1 }, { 0, -1, 0, 1 },
            { 0, 0, -1, 1 },                    /* z <= w */
            { 0, 0, 1, dx ? 0 : 1 },            /* z >= 0 or -w */
        };
        bool inside = true;

        for (int i = 0; i < 3 && inside; i++) {
            const float *p = buf[0][i].pos;

            inside = p[3] > 0 && fabsf(p[0]) <= p[3] &&
                     fabsf(p[1]) <= p[3] && p[2] <= p[3] &&
                     (dx ? p[2] >= 0 : p[2] >= -p[3]);
        }
        if (!inside) {
            for (size_t i = 0; i < ARRAY_SIZE(planes) && n >= 3; i++) {
                n = clip_plane(buf[cur], n, buf[cur ^ 1], planes[i],
                               d->ninterp);
                cur ^= 1;
            }
            if (n < 3) {
                return;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        to_screen(d, &buf[cur][i], &sv[i]);
    }
    for (int i = 1; i + 1 < n; i++) {
        raster_triangle(d, &sv[0], &sv[i], &sv[i + 1]);
    }
}

static void draw_rect(DrawCtx *d, const AdrenoVertex *a,
                      const AdrenoVertex *b, const AdrenoVertex *c)
{
    const AdrenoVertex *v[3] = { a, b, c };
    AdrenoVertex fourth;
    int corner = 0;
    float best = -1;

    /* the corner is the vertex opposite the longest edge */
    for (int i = 0; i < 3; i++) {
        const AdrenoVertex *p = v[(i + 1) % 3], *q = v[(i + 2) % 3];
        float dx = p->pos[0] - q->pos[0], dy = p->pos[1] - q->pos[1];

        if (dx * dx + dy * dy > best) {
            best = dx * dx + dy * dy;
            corner = i;
        }
    }
    {
        const AdrenoVertex *k = v[corner];
        const AdrenoVertex *p = v[(corner + 1) % 3];
        const AdrenoVertex *q = v[(corner + 2) % 3];

        for (int j = 0; j < 4; j++) {
            fourth.pos[j] = p->pos[j] + q->pos[j] - k->pos[j];
        }
        fourth.psize = k->psize;
        for (int i = 0; i < d->ninterp; i++) {
            for (int j = 0; j < 4; j++) {
                fourth.interp[i][j] = p->interp[i][j] + q->interp[i][j] -
                                      k->interp[i][j];
            }
        }
        draw_triangle(d, k, p, q);
        draw_triangle(d, p, &fourth, q);
    }
}

/* ------------------------------------------------------------------ */
/* Resolve (EDRAM_COPY)                                                */
/* ------------------------------------------------------------------ */

static void resolve(DrawCtx *d, const AdrenoVertex **vs, int nv)
{
    MsmAdrenoState *s = d->s;
    uint32_t *r = s->regs;
    uint32_t ci = r[A2_RB_COLOR_INFO];
    uint32_t dinfo = r[A2_RB_COPY_DEST_INFO];
    uint32_t copyc = r[A2_RB_COPY_CONTROL];
    unsigned sfmt = ci & 0xf, dfmt = (dinfo >> 4) & 0xf;
    unsigned sswap = (ci >> 9) & 3, dswap = (dinfo >> 8) & 3;
    unsigned dmask = (dinfo >> 14) & 0xf;
    uint32_t dpitch = (r[A2_RB_COPY_DEST_PITCH] & 0x1ff) * 32;
    uint32_t dbase = r[A2_RB_COPY_DEST_BASE];
    int offx = r[A2_RB_COPY_DEST_OFFSET] & 0x1fff;
    int offy = (r[A2_RB_COPY_DEST_OFFSET] >> 13) & 0x1fff;
    float xmin = INFINITY, ymin = INFINITY, xmax = -INFINITY, ymax = -INFINITY;
    int x0, y0, x1, y1, rx0, ry0;
    Surface src;
    const ColorFmt *sf = &color_fmts[sfmt], *df = &color_fmts[dfmt];
    bool raw = sfmt == dfmt && sswap == dswap && dmask == 0xf &&
               !(dinfo & 7);
    uint8_t *row = NULL;

    for (int i = 0; i < nv; i++) {
        SVert sv;

        to_screen(d, vs[i], &sv);
        xmin = MIN(xmin, sv.x);
        xmax = MAX(xmax, sv.x);
        ymin = MIN(ymin, sv.y);
        ymax = MAX(ymax, sv.y);
    }
    if (!isfinite(xmin + xmax + ymin + ymax)) {
        return;
    }
    /*
     * Pixels whose centre is inside, clipped to the scissors.  The
     * rectangle's corner goes to RB_COPY_DEST_OFFSET in the destination.
     */
    rx0 = (int)ceilf(xmin - d->pix_center);
    ry0 = (int)ceilf(ymin - d->pix_center);
    x0 = MAX(d->clip_x0, rx0);
    y0 = MAX(d->clip_y0, ry0);
    x1 = MIN(d->clip_x1, (int)ceilf(xmax - d->pix_center));
    y1 = MIN(d->clip_y1, (int)ceilf(ymax - d->pix_center));
    adreno_trace(s, "  resolve %d,%d-%d,%d fmt %u->%u to %08x pitch %u "
                 "off %d,%d info %08x ctl %08x\n", x0, y0, x1, y1, sfmt, dfmt,
                 dbase, dpitch, offx, offy, dinfo, copyc);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    surface_setup(s, &src, ci & 0xfffff000,
                  r[A2_RB_SURFACE_INFO] & 0x3fff, sf->bpp);
    if (s->trace && src.host) {
        hwaddr pa = 0;
        uint32_t nz = 0;

        adreno_translate(s, dbase, &pa);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                uint64_t off = src.offset + ((uint64_t)y * src.pitch + x) *
                               sf->bpp;

                if (off + 4 <= ADRENO_GMEM_SIZE && ldl_le_p(s->gmem + off)) {
                    nz++;
                }
            }
        }
        adreno_trace(s, "  resolve: dest pa %08" HWADDR_PRIx ", %u nonzero "
                     "source pixels, %" PRIu64 " shaded so far\n", pa, nz,
                     s->pixels);
    }
    if (!dpitch) {
        dpitch = src.pitch;
    }
    if (!(dinfo & (1 << 3))) {
        static bool warned;

        if (!warned) {
            warned = true;
            adreno_trace(s, "resolve: tiled destination treated as linear\n");
        }
    }
    row = g_malloc((x1 - x0) * MAX(sf->bpp, df->bpp));
    for (int y = y0; y < y1; y++) {
        uint32_t daddr = dbase + ((y - ry0 + offy) * dpitch +
                                  (x0 - rx0 + offx)) * df->bpp;

        if (raw && src.host) {
            uint64_t off = src.offset + ((uint64_t)y * src.pitch + x0) *
                           sf->bpp;

            if (off + (x1 - x0) * sf->bpp > ADRENO_GMEM_SIZE) {
                break;
            }
            adreno_write(s, daddr, s->gmem + off, (x1 - x0) * sf->bpp);
            continue;
        }
        for (int x = x0; x < x1; x++) {
            uint8_t px[16] = {0};
            float c[4];

            surface_rw(s, &src, x, y, px, false);
            if (raw) {
                memcpy(row + (x - x0) * df->bpp, px, df->bpp);
                continue;
            }
            color_unpack(sfmt, px, c);
            if (sswap == 1) {
                float t = c[0];

                c[0] = c[2];
                c[2] = t;
            }
            if (dmask != 0xf) {
                float old[4];

                adreno_read(s, daddr + (x - x0) * df->bpp, px, df->bpp);
                color_unpack(dfmt, px, old);
                if (dswap == 1) {
                    float t = old[0];

                    old[0] = old[2];
                    old[2] = t;
                }
                for (int k = 0; k < 4; k++) {
                    if (!(dmask & (1 << k))) {
                        c[k] = old[k];
                    }
                }
            }
            if (dswap == 1) {
                float t = c[0];

                c[0] = c[2];
                c[2] = t;
            }
            color_pack(dfmt, c, row + (x - x0) * df->bpp);
        }
        if (!(raw && src.host)) {
            adreno_write(s, daddr, row, (x1 - x0) * df->bpp);
        }
    }

    /* clear the source after the copy */
    if ((copyc >> 4) & 0xf) {
        uint32_t cv = r[A2_CLEAR_COLOR];
        float c[4] = {
            (cv & 0xff) / 255.0f, ((cv >> 8) & 0xff) / 255.0f,
            ((cv >> 16) & 0xff) / 255.0f, ((cv >> 24) & 0xff) / 255.0f,
        };
        uint8_t px[16];

        if (sswap == 1) {
            float t = c[0];

            c[0] = c[2];
            c[2] = t;
        }
        color_pack(sfmt, c, px);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                surface_rw(s, &src, x, y, px, true);
            }
        }
    }
    if (copyc & (1 << 3)) {
        uint32_t di = r[A2_RB_DEPTH_INFO];
        Surface ds;
        uint8_t px[4];

        surface_setup(s, &ds, di & 0xfffff000, src.pitch,
                      (di & 1) ? 4 : 2);
        stl_le_p(px, r[A2_RB_DEPTH_CLEAR]);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                surface_rw(s, &ds, x, y, px, true);
            }
        }
    }
    g_free(row);
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static uint32_t fetch_index(DrawCtx *d, uint32_t i, unsigned src,
                            uint32_t init, uint32_t index_base,
                            const uint32_t *imm, uint32_t imm_dwords)
{
    MsmAdrenoState *s = d->s;
    bool i32 = init & (1 << 11), i8 = init & (1 << 13);
    uint32_t idx = i;

    if (src == 0) {
        if (i32) {
            idx = adreno_ldl(s, index_base + i * 4);
        } else if (i8) {
            uint8_t b = 0;

            adreno_read(s, index_base + i, &b, 1);
            idx = b;
        } else {
            uint16_t h = 0;

            adreno_read(s, index_base + i * 2, &h, 2);
            idx = le16_to_cpu(h);
        }
    } else if (src == 1) {
        if (i32) {
            idx = i < imm_dwords ? imm[i] : 0;
        } else {
            idx = (i / 2) < imm_dwords ? (imm[i / 2] >> (16 * (i & 1))) &
                                         0xffff : 0;
        }
    }
    idx += s->regs[A2_VGT_INDX_OFFSET];
    idx = MIN(MAX(idx, s->regs[A2_VGT_MIN_VTX_INDX]),
              s->regs[A2_VGT_MAX_VTX_INDX]);
    return idx;
}

void adreno_draw(MsmAdrenoState *s, uint32_t init, uint32_t num_indices,
                 uint32_t index_base, uint32_t index_size,
                 const uint32_t *imm, uint32_t imm_dwords)
{
    uint32_t *r = s->regs;
    unsigned prim = init & 0x3f, src = (init >> 6) & 3;
    unsigned mode = r[A2_RB_MODECONTROL] & 7;
    g_autofree DrawCtx *d = g_new0(DrawCtx, 1);
    uint32_t tl, br;
    const AdrenoVertex *rv[3];

    s->draws++;
    adreno_trace(s, "  draw #%" PRIu64 " prim %u src %u n %u mode %u "
                 "color %08x surf %08x depth %08x/%08x vte %08x sc %08x\n",
                 s->draws, prim, src, num_indices, mode, r[A2_RB_COLOR_INFO],
                 r[A2_RB_SURFACE_INFO], r[A2_RB_DEPTH_INFO],
                 r[A2_RB_DEPTHCONTROL], r[A2_PA_CL_VTE_CNTL],
                 r[A2_PA_SU_SC_MODE_CNTL]);
    if (!num_indices || mode == A2_EDRAM_NOP) {
        return;
    }

    d->s = s;
    d->prog_cntl = r[A2_SQ_PROGRAM_CNTL];
    d->ninterp = MIN(((d->prog_cntl >> 20) & 0xf) + 1, A2_MAX_INTERP);
    d->flat_mask = r[A2_SQ_INTERPOLATOR_CNTL] & 0xffff;
    d->provoking_last = r[A2_PA_SU_SC_MODE_CNTL] & (1 << 19);
    d->pix_center = (r[A2_PA_SU_VTX_CNTL] & 1) ? 0.5f : 0.0f;
    d->win_x = sextract32(r[A2_PA_SC_WINDOW_OFFSET], 0, 15);
    d->win_y = sextract32(r[A2_PA_SC_WINDOW_OFFSET], 16, 15);
    d->win_offset_vtx = r[A2_PA_SU_SC_MODE_CNTL] & (1 << 16);

    /* scissors: window (offset unless disabled) and screen */
    tl = r[A2_PA_SC_WINDOW_SCISSOR_TL];
    br = r[A2_PA_SC_WINDOW_SCISSOR_BR];
    d->clip_x0 = tl & 0x7fff;
    d->clip_y0 = (tl >> 16) & 0x7fff;
    d->clip_x1 = br & 0x7fff;
    d->clip_y1 = (br >> 16) & 0x7fff;
    if (!(tl & (1u << 31))) {
        d->clip_x0 += d->win_x;
        d->clip_x1 += d->win_x;
        d->clip_y0 += d->win_y;
        d->clip_y1 += d->win_y;
    }
    tl = r[A2_PA_SC_SCREEN_SCISSOR_TL];
    br = r[A2_PA_SC_SCREEN_SCISSOR_BR];
    d->clip_x0 = MAX(d->clip_x0, (int)(tl & 0x7fff));
    d->clip_y0 = MAX(d->clip_y0, (int)((tl >> 16) & 0x7fff));
    d->clip_x1 = MIN(d->clip_x1, (int)(br & 0x7fff));
    d->clip_y1 = MIN(d->clip_y1, (int)((br >> 16) & 0x7fff));
    d->clip_x0 = MAX(d->clip_x0, 0);
    d->clip_y0 = MAX(d->clip_y0, 0);

    /* render targets */
    {
        uint32_t ci = r[A2_RB_COLOR_INFO], di = r[A2_RB_DEPTH_INFO];
        uint32_t pitch = r[A2_RB_SURFACE_INFO] & 0x3fff;

        d->color_fmt = ci & 0xf;
        d->color_swap = (ci >> 9) & 3;
        surface_setup(s, &d->color, ci & 0xfffff000, pitch,
                      color_fmts[d->color_fmt].bpp);
        d->depth_24 = di & 1;
        surface_setup(s, &d->depth, di & 0xfffff000, pitch,
                      d->depth_24 ? 4 : 2);
        d->color_write = mode == A2_EDRAM_COLOR_DEPTH;
        d->depth_enabled = true;
        if (pitch) {
            d->clip_x1 = MIN(d->clip_x1, (int)pitch);
        }
    }

    if (mode == A2_EDRAM_COPY) {
        uint32_t n = MIN(num_indices, 3u);

        for (uint32_t i = 0; i < n; i++) {
            rv[i] = get_vertex(d, fetch_index(d, i, src, init, index_base,
                                              imm, imm_dwords));
        }
        resolve(d, rv, n);
        return;
    }

    switch (prim) {
    case A2_PT_TRILIST:
    case A2_PT_RECTLIST:
        for (uint32_t i = 0; i + 2 < num_indices; i += 3) {
            AdrenoVertex tri[3];

            for (int k = 0; k < 3; k++) {
                tri[k] = *get_vertex(d, fetch_index(d, i + k, src, init,
                                                    index_base, imm,
                                                    imm_dwords));
            }
            if (prim == A2_PT_RECTLIST) {
                draw_rect(d, &tri[0], &tri[1], &tri[2]);
            } else {
                draw_triangle(d, &tri[0], &tri[1], &tri[2]);
            }
        }
        break;
    case A2_PT_TRISTRIP:
    case A2_PT_TRIFAN: {
        AdrenoVertex first, prev2, prev1;

        for (uint32_t i = 0; i < num_indices; i++) {
            AdrenoVertex cur = *get_vertex(d, fetch_index(d, i, src, init,
                                                          index_base, imm,
                                                          imm_dwords));

            if (i == 0) {
                first = prev2 = cur;
            } else if (i == 1) {
                prev1 = cur;
            } else {
                if (prim == A2_PT_TRIFAN) {
                    draw_triangle(d, &first, &prev1, &cur);
                } else if (i & 1) {
                    draw_triangle(d, &prev1, &prev2, &cur);
                } else {
                    draw_triangle(d, &prev2, &prev1, &cur);
                }
                prev2 = prev1;
            }
            if (i >= 1) {
                prev1 = cur;
            }
        }
        break;
    }
    default:
        adreno_trace(s, "  primitive type %u not drawn\n", prim);
        break;
    }
    (void)index_size;
}
