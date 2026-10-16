/*
 * Adreno 200 3D pipe: vertex fetch and shading, primitive assembly,
 * clipping, rasterization, render backend and resolves
 *
 * A draw either renders (RB_MODECONTROL COLOR_DEPTH or DEPTH_ONLY) or,
 * in EDRAM_COPY mode, resolves: the rectangle the vertices cover is
 * copied from GMEM to RB_COPY_DEST_* in memory and optionally cleared.
 *
 * Vertices are shaded AL at a time.  Primitives (triangles, rectangles,
 * and points and lines expanded to quads in screen space) are clipped,
 * mapped through the viewport and set up once; then the rasterizer threads
 * walk them in order, each owning every Nth band of 4 pixel rows, so a
 * pixel's writes keep the API order.  Pixels are shaded as 4x4 blocks of
 * 2x2 quads; with no kill, depth export or alpha test the depth/stencil
 * test runs before the pixel shader.
 *
 * Render targets live in GMEM when their base falls in RB_EDRAM_INFO's
 * range, else in memory (GPU addresses; tiled unless RB_COLOR_INFO.LINEAR,
 * the blob renders into tiled textures that way).  GMEM surfaces are
 * linear with RB_SURFACE_INFO's pitch: GMEM is invisible to the CPU, so
 * its layout only has to agree with itself.
 *
 * Colour formats pack channels from bit 0 up (R, G, B, A); SWAP exchanges
 * R and B.
 *
 * Coverage uses 1/16-pixel fixed point and the top-left rule; attributes
 * are interpolated perspective-correctly unless flat shaded.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "msm_adreno_gl.h"
#include "msm_adreno_gles.h"

#define SUBPIX      16
#define MAX_CLIPPED 12

/* ------------------------------------------------------------------ */
/* Colour formats                                                      */
/* ------------------------------------------------------------------ */

typedef AdrenoColorFmt ColorFmt;

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

const AdrenoColorFmt *adreno_color_fmt(unsigned fmt)
{
    return &color_fmts[fmt & 15];
}

uint16_t adreno_float_to_half(float f)
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

float adreno_half_to_float(uint16_t h)
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

static inline void swap_rb(float *c)
{
    float t = c[0];

    c[0] = c[2];
    c[2] = t;
}

/* pixel bytes -> channels c0..c3 (before SWAP) */
static void color_unpack(unsigned fmt, const uint8_t *p, float c[4])
{
    const ColorFmt *f = &color_fmts[fmt & 15];

    c[0] = c[1] = c[2] = 0.0f;
    c[3] = 1.0f;
    if (f->kind == 2) {
        for (int k = 0; k < 4 && f->bits[k]; k++) {
            c[k] = adreno_half_to_float(lduw_le_p(p + 2 * k));
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
            stw_le_p(p + 2 * k, adreno_float_to_half(c[k]));
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

/*
 * Byte offset of block (x, y) in a tiled 2D surface: the Xenos layout
 * Adreno 200 inherited (32x32 macro tiles, bank and pipe interleave), as in
 * Xenia's texture_address.h.  pitch is in blocks, a multiple of 32.
 */
uint32_t adreno_tiled_offset(int x, int y, uint32_t pitch, unsigned bpb_log2)
{
    uint32_t outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t oib = (outer | inner) << bpb_log2;
    uint32_t bank = (y >> 4) & 1;
    uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);

    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 0xf) |
           (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) |
           ((oib >> 8) << 12);
}

typedef struct Surface {
    uint8_t *host;      /* GMEM, or NULL for memory */
    uint32_t gpuaddr;   /* memory surfaces */
    uint32_t offset;    /* GMEM byte offset */
    uint32_t pitch;     /* pixels */
    uint32_t bpp;
    uint32_t limit;     /* bytes addressable from the base */
    uint8_t sx, sy;     /* MSAA sample grid per pixel */
    bool tiled;         /* memory surfaces: tiled unless LINEAR */
} Surface;

/*
 * A surface with MSAA_SAMPLES 2X/4X is laid out as a 2x1/2x2 grid of
 * samples per pixel, so pixel rows are 2x wider (the driver's fast clears
 * draw to a 4X surface of a quarter the pitch to cover a bin quickly).
 */
static void surface_setup(MsmAdrenoState *s, Surface *sf, uint32_t base,
                          uint32_t pitch, uint32_t bpp, bool linear)
{
    unsigned msaa = (s->regs[A2_RB_SURFACE_INFO] >> 14) & 3;
    uint32_t info = s->regs[A2_RB_EDRAM_INFO];
    uint32_t ebase = info & 0xffffc000;
    uint32_t esize = (16 * 1024) << (info & 0xf);

    esize = MIN(esize, ADRENO_GMEM_SIZE);
    sf->pitch = pitch;
    sf->bpp = bpp;
    sf->sx = msaa ? 2 : 1;
    sf->sy = msaa >= 2 ? 2 : 1;
    sf->tiled = false;
    if (base - ebase < esize) {
        sf->host = s->gmem;
        sf->offset = base - ebase;
        sf->limit = ADRENO_GMEM_SIZE - sf->offset;
    } else {
        /* rendering straight to memory, e.g. to a (tiled) texture */
        sf->host = NULL;
        sf->gpuaddr = base;
        sf->offset = 0;
        sf->limit = UINT32_MAX;
        sf->tiled = !linear && is_power_of_2(bpp);
    }
}

static bool surface_rw1(MsmAdrenoState *s, AdrenoTlb *tlb, Surface *sf,
                        int x, int y, uint8_t *buf, bool write)
{
    uint64_t off;

    if (sf->tiled) {
        off = adreno_tiled_offset(x, y, ROUND_UP(sf->pitch * sf->sx, 32),
                                  ctz32(sf->bpp));
    } else {
        off = ((uint64_t)y * sf->pitch * sf->sx + x) * sf->bpp;
    }
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
    } else if (write) {
        adreno_tlb_write(s, tlb, sf->gpuaddr + off, buf, sf->bpp);
    } else {
        adreno_tlb_read(s, tlb, sf->gpuaddr + off, buf, sf->bpp);
    }
    return true;
}

/* read sample 0, or write every sample of the pixel */
static bool surface_rw(MsmAdrenoState *s, AdrenoTlb *tlb, Surface *sf,
                       int x, int y, uint8_t *buf, bool write)
{
    if (x < 0 || y < 0) {
        return false;
    }
    if (!write || (sf->sx == 1 && sf->sy == 1)) {
        return surface_rw1(s, tlb, sf, x * sf->sx, y * sf->sy, buf, write);
    }
    for (int j = 0; j < sf->sy; j++) {
        for (int i = 0; i < sf->sx; i++) {
            if (!surface_rw1(s, tlb, sf, x * sf->sx + i, y * sf->sy + j, buf,
                             true)) {
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Draw state                                                          */
/* ------------------------------------------------------------------ */

/* a primitive set up for rasterization */
typedef struct Prim {
    int64_t A[3], B[3], C[3];       /* edge functions, >= 0 inside */
    bool tl[3];
    float inv_area;
    int minx, maxx, miny, maxy;     /* pixel bounds, inclusive */
    float z[3], rhw[3];
    /* interpolators, pre-multiplied by rhw; flat ones hold the value */
    float a[3][A2_MAX_INTERP][4];
    float pc[3][2];                 /* point sprite coordinates */
    bool backface;
} Prim;

typedef struct DrawCtx {
    MsmAdrenoState *s;
    const uint32_t *r;
    const AdrenoProgram *ps;
    uint32_t prog_cntl;
    int ninterp;
    uint32_t flat_mask;
    bool provoking_last;
    /* raster state */
    int clip_x0, clip_y0, clip_x1, clip_y1;    /* inclusive-exclusive */
    float pix_center;
    int win_x, win_y;
    bool win_offset_vtx;
    Surface color, depth;
    unsigned color_fmt, color_swap;
    bool depth_24;
    bool color_write;
    bool early_z;
    bool param_gen;
    unsigned param_gen_reg;
    /* primitives */
    Prim *prims;
    uint32_t nprims, cap;
    int64_t area;                   /* sum of bounding boxes */
    uint32_t nthreads;
    bool persp;
    /* host GPU backend: screen-space triangles instead of prims */
    bool gl;
    float *glv;
    uint32_t glv_n, glv_cap;
} DrawCtx;

static Prim *new_prim(DrawCtx *d)
{
    if (d->nprims == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->prims = g_renew(Prim, d->prims, d->cap);
    }
    return &d->prims[d->nprims++];
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
    float pc[2];
} SVert;

static void to_screen(DrawCtx *d, const AdrenoVertex *v, SVert *o)
{
    const uint32_t *r = d->r;
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
    o->pc[0] = o->pc[1] = 0.0f;
}

/* ------------------------------------------------------------------ */
/* Primitive setup                                                     */
/* ------------------------------------------------------------------ */

/* a triangle for the host GPU (see AdrenoGlDraw) */
static void gl_emit(DrawCtx *d, const SVert *const v[3], const int64_t x[3],
                    const int64_t y[3], const SVert *prov, bool backface)
{
    int nf = ADRENO_GL_VTX_VEC4(d->ninterp) * 4;

    if (d->glv_n + 3 > d->glv_cap) {
        d->glv_cap = MAX(d->glv_cap * 2, 96);
        d->glv = g_renew(float, d->glv, (size_t)d->glv_cap * nf);
    }
    for (int i = 0; i < 3; i++) {
        float *o = d->glv + (size_t)d->glv_n++ * nf;
        float rhw = d->persp ? v[i]->rhw : 1.0f;

        o[0] = x[i] / (float)SUBPIX;
        o[1] = y[i] / (float)SUBPIX;
        o[2] = v[i]->z;
        o[3] = rhw;
        o[4] = v[i]->pc[0];
        o[5] = v[i]->pc[1];
        o[6] = backface;
        o[7] = 0.0f;
        for (int n = 0; n < d->ninterp; n++) {
            if (d->flat_mask & (1u << n)) {
                memcpy(o + 8 + 4 * n, prov->v->interp[n], 16);
            } else {
                for (int k = 0; k < 4; k++) {
                    o[8 + 4 * n + k] = v[i]->v->interp[n][k] * rhw;
                }
            }
        }
    }
}

static void setup_triangle(DrawCtx *d, const SVert *a, const SVert *b,
                           const SVert *c, const SVert *prov, bool cull)
{
    uint32_t sc = d->r[A2_PA_SU_SC_MODE_CNTL];
    const SVert *v[3] = { a, b, c };
    int64_t x[3], y[3];
    int64_t area;
    int64_t cofs = llrintf(d->pix_center * SUBPIX);
    int minx, maxx, miny, maxy;
    bool cw, backface;
    Prim *p;

    for (int i = 0; i < 3; i++) {
        if (!isfinite(v[i]->x) || !isfinite(v[i]->y) ||
            fabsf(v[i]->x) > 1e6f || fabsf(v[i]->y) > 1e6f) {
            return;
        }
        x[i] = llrintf(v[i]->x * SUBPIX);
        y[i] = llrintf(v[i]->y * SUBPIX);
    }
    area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (area == 0) {
        return;
    }
    /* positive area: clockwise on screen (y down) */
    cw = area > 0;
    backface = (sc & (1 << 2)) ? !cw : cw;
    if (cull && ((backface && (sc & 2)) || (!backface && (sc & 1)))) {
        return;
    }
    if (area < 0) {
        const SVert *t = v[1];
        int64_t tx = x[1], ty = y[1];

        v[1] = v[2];
        v[2] = t;
        x[1] = x[2];
        y[1] = y[2];
        x[2] = tx;
        y[2] = ty;
        area = -area;
    }

    minx = MAX(d->clip_x0,
               (int)((MIN(MIN(x[0], x[1]), x[2]) - cofs) / SUBPIX) - 1);
    maxx = MIN(d->clip_x1 - 1,
               (int)((MAX(MAX(x[0], x[1]), x[2]) - cofs) / SUBPIX) + 1);
    miny = MAX(d->clip_y0,
               (int)((MIN(MIN(y[0], y[1]), y[2]) - cofs) / SUBPIX) - 1);
    maxy = MIN(d->clip_y1 - 1,
               (int)((MAX(MAX(y[0], y[1]), y[2]) - cofs) / SUBPIX) + 1);
    if (minx > maxx || miny > maxy) {
        return;
    }
    if (d->gl) {
        gl_emit(d, v, x, y, prov, backface);
        return;
    }
    p = new_prim(d);
    p->backface = backface;
    p->minx = minx;
    p->maxx = maxx;
    p->miny = miny;
    p->maxy = maxy;
    d->area += (int64_t)(p->maxx - p->minx + 1) * (p->maxy - p->miny + 1);
    p->inv_area = 1.0f / (float)area;
    for (int i = 0; i < 3; i++) {
        /* edge i is opposite vertex i */
        int j = (i + 1) % 3, k = (i + 2) % 3;

        p->A[i] = y[j] - y[k];
        p->B[i] = x[k] - x[j];
        p->C[i] = x[j] * y[k] - x[k] * y[j];
        /* top edge (horizontal, inside below) or left edge */
        p->tl[i] = (p->A[i] == 0 && p->B[i] > 0) || p->A[i] > 0;
        p->z[i] = v[i]->z;
        p->rhw[i] = v[i]->rhw;
        p->pc[i][0] = v[i]->pc[0];
        p->pc[i][1] = v[i]->pc[1];
        for (int n = 0; n < d->ninterp; n++) {
            const float *src = (d->flat_mask & (1u << n)) ?
                               prov->v->interp[n] : v[i]->v->interp[n];
            float m = (d->flat_mask & (1u << n)) ? 1.0f : v[i]->rhw;

            for (int k2 = 0; k2 < 4; k2++) {
                p->a[i][n][k2] = src[k2] * m;
            }
        }
    }
}

static void draw_triangle(DrawCtx *d, const AdrenoVertex *a,
                          const AdrenoVertex *b, const AdrenoVertex *c)
{
    uint32_t clip = d->r[A2_PA_CL_CLIP_CNTL];
    AdrenoVertex buf[2][MAX_CLIPPED];
    SVert sv[MAX_CLIPPED], prov;
    int n = 3, cur = 0;

    buf[0][0] = *a;
    buf[0][1] = *b;
    buf[0][2] = *c;
    if (!(clip & (1 << 16))) {
        bool dx = clip & (1 << 19);
        static const float planes_gl[][4] = {
            { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { -1, 0, 0, 1 },
            { 0, 1, 0, 1 }, { 0, -1, 0, 1 }, { 0, 0, -1, 1 },
            { 0, 0, 1, 1 },
        };
        bool inside = true;

        for (int i = 0; i < 3 && inside; i++) {
            const float *p = buf[0][i].pos;

            inside = p[3] > 0 && fabsf(p[0]) <= p[3] &&
                     fabsf(p[1]) <= p[3] && p[2] <= p[3] &&
                     (dx ? p[2] >= 0 : p[2] >= -p[3]);
        }
        if (!inside) {
            for (size_t i = 0; i < ARRAY_SIZE(planes_gl) && n >= 3; i++) {
                float pl[4];

                memcpy(pl, planes_gl[i], sizeof(pl));
                if (i == 6 && dx) {
                    pl[3] = 0;      /* z >= 0 */
                }
                n = clip_plane(buf[cur], n, buf[cur ^ 1], pl, d->ninterp);
                cur ^= 1;
            }
            if (n < 3) {
                return;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        to_screen(d, &buf[cur][i], &sv[i]);
        if (d->s->trace && d->s->draws < d->s->trace_vtx_draws) {
            adreno_trace(d->s, "    vtx %.2f,%.2f,%.3f rhw %.3f  i0 %.3f %.3f "
                         "%.3f %.3f\n", sv[i].x, sv[i].y, sv[i].z, sv[i].rhw,
                         buf[cur][i].interp[0][0], buf[cur][i].interp[0][1],
                         buf[cur][i].interp[0][2], buf[cur][i].interp[0][3]);
        }
    }
    to_screen(d, d->provoking_last ? c : a, &prov);
    for (int i = 1; i + 1 < n; i++) {
        setup_triangle(d, &sv[0], &sv[i], &sv[i + 1], &prov, true);
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

/* a screen-space quad from four corners (0 1 / 2 3), not culled */
static void setup_quad(DrawCtx *d, SVert q[4], const SVert *prov)
{
    setup_triangle(d, &q[0], &q[1], &q[2], prov, false);
    setup_triangle(d, &q[1], &q[3], &q[2], prov, false);
}

/* fixed 12.4 to float */
static float fx124(uint32_t v)
{
    return (v & 0xffff) / 16.0f;
}

static void draw_point(DrawCtx *d, const AdrenoVertex *v, bool vs_size)
{
    const uint32_t *r = d->r;
    SVert c, q[4];
    float half;
    float mn = fx124(r[A2_PA_SU_POINT_MINMAX]);
    float mx = fx124(r[A2_PA_SU_POINT_MINMAX] >> 16);

    if ((r[A2_PA_CL_VTE_CNTL] & (1 << 10)) && v->pos[3] <= 0.0f) {
        return;     /* behind the eye */
    }
    to_screen(d, v, &c);
    /* PA_SU_POINT_SIZE holds half the size; the shader's is the diameter */
    half = vs_size ? v->psize * 0.5f : fx124(r[A2_PA_SU_POINT_SIZE] >> 16);
    if (mx > 0.0f) {
        half = MIN(MAX(half, mn), mx);
    }
    if (half <= 0.0f) {
        half = 0.5f;
    }
    for (int i = 0; i < 4; i++) {
        q[i] = c;
        q[i].x += (i & 1) ? half : -half;
        q[i].y += (i & 2) ? half : -half;
        q[i].pc[0] = (i & 1) ? 1.0f : 0.0f;
        q[i].pc[1] = (i & 2) ? 1.0f : 0.0f;
    }
    setup_quad(d, q, &c);
}

static void draw_line(DrawCtx *d, const AdrenoVertex *a,
                      const AdrenoVertex *b)
{
    float half = fx124(d->r[A2_PA_SU_LINE_CNTL]);
    SVert e[2], q[4], prov;
    float dx, dy;

    if ((d->r[A2_PA_CL_VTE_CNTL] & (1 << 10)) &&
        (a->pos[3] <= 0.0f || b->pos[3] <= 0.0f)) {
        return;
    }
    to_screen(d, a, &e[0]);
    to_screen(d, b, &e[1]);
    to_screen(d, d->provoking_last ? b : a, &prov);
    if (half <= 0.0f) {
        half = 0.5f;
    }
    dx = e[1].x - e[0].x;
    dy = e[1].y - e[0].y;
    /* widen across the minor axis, as GL's aliased lines */
    for (int i = 0; i < 4; i++) {
        q[i] = e[i >> 1];
        if (fabsf(dx) >= fabsf(dy)) {
            q[i].y += (i & 1) ? half : -half;
        } else {
            q[i].x += (i & 1) ? half : -half;
        }
    }
    setup_quad(d, q, &prov);
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
static bool depth_stencil(DrawCtx *d, AdrenoTlb *tlb, int x, int y, float z,
                          bool backface)
{
    const uint32_t *r = d->r;
    uint32_t dc = r[A2_RB_DEPTHCONTROL];
    bool stencil_en = dc & 1, z_en = dc & 2, z_write = dc & 4;
    uint8_t buf[4];
    uint32_t raw, stored_z, sten, zq;
    bool pass = true, zpass = true;
    bool bf = backface && (dc & (1 << 7));
    uint32_t refmask = r[bf ? A2_RB_STENCILREFMASK_BF : A2_RB_STENCILREFMASK];
    uint32_t ref = refmask & 0xff, mask = (refmask >> 8) & 0xff;
    uint32_t wmask = (refmask >> 16) & 0xff;
    unsigned sfunc, sfail, szpass, szfail;

    if (!stencil_en && !z_en) {
        return true;
    }
    if (!surface_rw(d->s, tlb, &d->depth, x, y, buf, false)) {
        return true;
    }
    if (d->depth_24) {
        raw = ldl_le_p(buf);
        stored_z = raw >> 8;
        sten = raw & 0xff;
    } else {
        raw = lduw_le_p(buf);
        stored_z = raw;
        sten = 0;
    }
    if (bf) {
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
    zq = d->depth_24 ? (uint32_t)lrintf(clamp01(z) * 16777215.0f)
                     : (uint32_t)lrintf(clamp01(z) * 65535.0f);
    if (pass && z_en) {
        /* in the buffer's fixed point */
        zpass = compare((dc >> 4) & 7, zq, stored_z);
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
    if (pass && zpass && z_en && z_write) {
        stored_z = zq;
    }
    if (stencil_en || (z_en && z_write)) {
        if (d->depth_24) {
            stl_le_p(buf, (stored_z << 8) | sten);
        } else {
            stw_le_p(buf, stored_z);
        }
        surface_rw(d->s, tlb, &d->depth, x, y, buf, true);
    }
    return pass && zpass;
}

static void write_color(DrawCtx *d, AdrenoTlb *tlb, int x, int y,
                        const float in[4])
{
    const uint32_t *r = d->r;
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
        if (!surface_rw(d->s, tlb, &d->color, x, y, buf, false)) {
            return;
        }
        color_unpack(d->color_fmt, buf, dst);
        if (d->color_swap == 1) {
            swap_rb(dst);
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
        swap_rb(out);
    }
    color_pack(d->color_fmt, out, buf);
    surface_rw(d->s, tlb, &d->color, x, y, buf, true);
}

/* ------------------------------------------------------------------ */
/* Rasterizer                                                          */
/* ------------------------------------------------------------------ */

/* lane l of a 4x4 block: quad q = l / 4 at (2 (q & 1), 2 (q >> 1)) */
static const uint8_t lane_x[AL] = {
    0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3,
};
static const uint8_t lane_y[AL] = {
    0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3,
};

static void shade_block(DrawCtx *d, AdrenoWorker *w, const Prim *p,
                        int bx, int by, uint32_t cover, const int64_t e[3][AL])
{
    AdrenoLanes *L = &w->lanes;
    const uint32_t *r = d->r;
    uint32_t cc = r[A2_RB_COLORCONTROL];
    bool persp = d->persp;
    float z[AL];
    uint32_t exec = 0;

    /* barycentrics, depth and the early depth/stencil test */
    for (int l = 0; l < AL; l++) {
        float b0 = e[0][l] * p->inv_area, b1 = e[1][l] * p->inv_area;
        float b2 = e[2][l] * p->inv_area;
        float rhw = b0 * p->rhw[0] + b1 * p->rhw[1] + b2 * p->rhw[2];
        float pw[3];

        z[l] = b0 * p->z[0] + b1 * p->z[1] + b2 * p->z[2];
        if (persp && rhw != 0.0f) {
            pw[0] = b0 / rhw;
            pw[1] = b1 / rhw;
            pw[2] = b2 / rhw;
        } else {
            pw[0] = b0;
            pw[1] = b1;
            pw[2] = b2;
        }
        if ((cover & (1u << l)) && d->early_z &&
            !depth_stencil(d, &w->tlb, bx + lane_x[l], by + lane_y[l], z[l],
                           p->backface)) {
            cover &= ~(1u << l);
        }
        for (int n = 0; n < d->ninterp; n++) {
            if (d->flat_mask & (1u << n)) {
                for (int k = 0; k < 4; k++) {
                    L->r[n][k][l] = p->a[0][n][k];
                }
            } else {
                /* without perspective correction pw = b and a = v * rhw */
                float m0 = pw[0], m1 = pw[1], m2 = pw[2];

                if (!persp) {
                    m0 = p->rhw[0] ? b0 / p->rhw[0] : b0;
                    m1 = p->rhw[1] ? b1 / p->rhw[1] : b1;
                    m2 = p->rhw[2] ? b2 / p->rhw[2] : b2;
                }
                for (int k = 0; k < 4; k++) {
                    L->r[n][k][l] = m0 * p->a[0][n][k] + m1 * p->a[1][n][k] +
                                    m2 * p->a[2][n][k];
                }
            }
        }
        if (d->param_gen) {
            /* screen position (sign of x: back facing), point coordinate */
            float fx = (bx + lane_x[l] - d->win_x) + 0.5f;

            L->r[d->param_gen_reg][0][l] = p->backface ? -fx : fx;
            L->r[d->param_gen_reg][1][l] = (by + lane_y[l] - d->win_y) + 0.5f;
            L->r[d->param_gen_reg][2][l] = b0 * p->pc[0][0] +
                                           b1 * p->pc[1][0] +
                                           b2 * p->pc[2][0];
            L->r[d->param_gen_reg][3][l] = b0 * p->pc[0][1] +
                                           b1 * p->pc[1][1] +
                                           b2 * p->pc[2][1];
        }
    }
    if (!cover) {
        return;
    }
    /* whole quads run, so that the derivatives are there */
    for (int q = 0; q < AL; q += 4) {
        if (cover & (0xfu << q)) {
            exec |= 0xfu << q;
        }
    }
    L->prog = d->ps;
    L->quads = true;
    memset(L->depth, 0, sizeof(L->depth));
    adreno_shader_exec(L, exec);
    cover &= ~L->killed;

    for (int l = 0; l < AL; l++) {
        int x = bx + lane_x[l], y = by + lane_y[l];
        float c[4];

        if (!(cover & (1u << l))) {
            continue;
        }
        for (int k = 0; k < 4; k++) {
            c[k] = L->color[k][l];
        }
        if ((cc & (1 << 3)) &&
            !compare(cc & 7, c[3], adreno_f(r[A2_RB_ALPHA_REF]))) {
            continue;
        }
        if (!d->early_z &&
            !depth_stencil(d, &w->tlb, x, y,
                           d->ps->exports_depth ? L->depth[l] : z[l],
                           p->backface)) {
            continue;
        }
        w->pixels++;
        write_color(d, &w->tlb, x, y, c);
    }
}

static void raster_prim(DrawCtx *d, AdrenoWorker *w, const Prim *p,
                        uint32_t nthr)
{
    int64_t cofs = llrintf(d->pix_center * SUBPIX);
    int by0 = p->miny & ~3, bx0 = p->minx & ~3;
    int64_t lstep[3][AL];

    /* each lane's offset from the block's first sample, per edge */
    for (int i = 0; i < 3; i++) {
        for (int l = 0; l < AL; l++) {
            lstep[i][l] = (p->A[i] * lane_x[l] + p->B[i] * lane_y[l]) *
                          SUBPIX;
        }
    }
    for (int by = by0; by <= p->maxy; by += 4) {
        int64_t sy = (int64_t)by * SUBPIX + cofs;
        bool rows_in = by >= p->miny && by + 3 <= p->maxy;

        if ((uint32_t)(by >> 2) % nthr != (uint32_t)w->index) {
            continue;
        }
        for (int bx = bx0; bx <= p->maxx; bx += 4) {
            int64_t sx = (int64_t)bx * SUBPIX + cofs;
            int64_t e[3][AL];
            uint32_t cover = 0xffff;
            bool all_in = rows_in && bx >= p->minx && bx + 3 <= p->maxx;
            bool skip = false;

            for (int i = 0; i < 3 && !skip; i++) {
                int64_t e0 = p->A[i] * sx + p->B[i] * sy + p->C[i];
                /* the block's corners: all outside one edge = no pixel */
                int64_t c0 = e0, c1 = e0 + 3 * SUBPIX * p->A[i];
                int64_t c2 = e0 + 3 * SUBPIX * p->B[i];
                int64_t c3 = c1 + 3 * SUBPIX * p->B[i];

                if (c0 < 0 && c1 < 0 && c2 < 0 && c3 < 0) {
                    skip = true;
                    break;
                }
                if (c0 <= 0 || c1 <= 0 || c2 <= 0 || c3 <= 0) {
                    all_in = false;
                }
                for (int l = 0; l < AL; l++) {
                    e[i][l] = e0 + lstep[i][l];
                }
            }
            if (skip) {
                continue;
            }
            if (!all_in) {
                cover = 0;
                for (int l = 0; l < AL; l++) {
                    int x = bx + lane_x[l], y = by + lane_y[l];
                    bool in = x >= p->minx && x <= p->maxx && y >= p->miny &&
                              y <= p->maxy;

                    for (int i = 0; i < 3; i++) {
                        if (e[i][l] < 0 || (e[i][l] == 0 && !p->tl[i])) {
                            in = false;
                        }
                    }
                    cover |= (uint32_t)in << l;
                }
            }
            if (cover) {
                shade_block(d, w, p, bx, by, cover,
                            (const int64_t (*)[AL])e);
            }
        }
    }
}

void adreno_raster_worker(AdrenoWorker *w)
{
    DrawCtx *d = w->s->job;

    adreno_tlb_flush(&w->tlb);
    w->lanes.s = w->s;
    w->lanes.tlb = &w->tlb;
    w->lanes.texgen = w->s->texgen;
    for (uint32_t i = 0; i < d->nprims; i++) {
        raster_prim(d, w, &d->prims[i], d->nthreads);
    }
}

static void raster_run(DrawCtx *d)
{
    MsmAdrenoState *s = d->s;

    if (!d->nprims) {
        return;
    }
    /* small draws stay on the vCPU thread */
    d->nthreads = d->area < 8192 ? 1 : s->nthreads;
    s->job = d;
    for (uint32_t i = 1; i < d->nthreads; i++) {
        s->workers[i].pixels = 0;
        qemu_sem_post(&s->workers[i].go);
    }
    adreno_raster_worker(&s->workers[0]);
    for (uint32_t i = 1; i < d->nthreads; i++) {
        qemu_sem_wait(&s->done);
        s->pixels += s->workers[i].pixels;
    }
    s->pixels += s->workers[0].pixels;
    s->workers[0].pixels = 0;
    s->job = NULL;
}

/* ------------------------------------------------------------------ */
/* Resolve (EDRAM_COPY)                                                */
/* ------------------------------------------------------------------ */

/* a resolve's copy, which may wait for the end of the command kick */
struct AdrenoResolveJob {
    Surface src;
    unsigned sfmt, dfmt, sswap, dswap, dmask;
    uint32_t dpitch, dbase;
    int offx, offy, rx0, ry0, x0, y0, x1, y1;
    bool raw, tiled;
    int slot;               /* the GMEM snapshot it copies from */
};

/* copy the rectangle from GMEM (the copy at gmem) to memory */
static void resolve_copy(MsmAdrenoState *s, AdrenoResolveJob *j,
                         uint8_t *gmem)
{
    AdrenoTlb *tlb = &s->workers[0].tlb;
    const ColorFmt *sf = &color_fmts[j->sfmt], *df = &color_fmts[j->dfmt];
    Surface *src = &j->src;
    g_autofree uint8_t *row = g_malloc((j->x1 - j->x0) *
                                       MAX(sf->bpp, df->bpp));
    int64_t t0 = s->stats_on ? g_get_monotonic_time() : 0;

    adreno_tlb_flush(tlb);
    if (src->host) {
        src->host = gmem;
    }
    for (int y = j->y0; y < j->y1; y++) {
        uint32_t dy = y - j->ry0 + j->offy;
        uint32_t daddr = j->dbase + (dy * j->dpitch +
                                     (j->x0 - j->rx0 + j->offx)) * df->bpp;

        if (j->raw && !j->tiled && src->host && src->sx == 1) {
            uint64_t off = src->offset + ((uint64_t)y * src->pitch + j->x0) *
                           sf->bpp;

            if (off + (j->x1 - j->x0) * sf->bpp > ADRENO_GMEM_SIZE) {
                break;
            }
            adreno_tlb_write(s, tlb, daddr, gmem + off,
                             (j->x1 - j->x0) * sf->bpp);
            continue;
        }
        for (int x = j->x0; x < j->x1; x++) {
            uint8_t px[16] = {0};
            uint8_t *o = j->tiled ? px : row + (x - j->x0) * df->bpp;
            uint32_t dx = x - j->rx0 + j->offx;
            float c[4];

            surface_rw(s, tlb, src, x, y, px, false);
            if (!j->raw) {
                color_unpack(j->sfmt, px, c);
                if (j->sswap == 1) {
                    swap_rb(c);
                }
                if (j->dmask != 0xf) {
                    uint8_t oldpx[16] = {0};
                    float old[4];

                    adreno_tlb_read(s, tlb, j->tiled ?
                                    j->dbase + adreno_tiled_offset(dx, dy,
                                        j->dpitch, ctz32(df->bpp)) :
                                    daddr + (x - j->x0) * df->bpp,
                                    oldpx, df->bpp);
                    color_unpack(j->dfmt, oldpx, old);
                    if (j->dswap == 1) {
                        swap_rb(old);
                    }
                    for (int k = 0; k < 4; k++) {
                        if (!(j->dmask & (1 << k))) {
                            c[k] = old[k];
                        }
                    }
                }
                if (j->dswap == 1) {
                    swap_rb(c);
                }
                color_pack(j->dfmt, c, o);
            } else if (!j->tiled) {
                memcpy(o, px, df->bpp);
            }
            if (j->tiled) {
                /* a tiled destination (a texture): pixel by pixel */
                adreno_tlb_write(s, tlb, j->dbase +
                                 adreno_tiled_offset(dx, dy, j->dpitch,
                                                     ctz32(df->bpp)),
                                 o, df->bpp);
            }
        }
        if (!j->tiled) {
            adreno_tlb_write(s, tlb, daddr, row, (j->x1 - j->x0) * df->bpp);
        }
    }
    if (s->stats_on) {
        s->st.resolves++;
        s->st.resolve += g_get_monotonic_time() - t0;
    }
}

void adreno_resolves_flush(MsmAdrenoState *s)
{
    if (!s->nresolves) {
        return;
    }
    adreno_gl_snapshots_wait(s);
    for (uint32_t i = 0; i < s->nresolves; i++) {
        AdrenoResolveJob *j = &s->resolves[i];

        resolve_copy(s, j, adreno_gl_snapshot_ptr(s, j->slot));
    }
    s->nresolves = 0;
    adreno_gl_snapshots_release(s);
}

static void resolve(DrawCtx *d, const AdrenoVertex *vs, int nv)
{
    MsmAdrenoState *s = d->s;
    AdrenoTlb *tlb = &s->workers[0].tlb;
    const uint32_t *r = d->r;
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
    AdrenoResolveJob job;

    for (int i = 0; i < nv; i++) {
        SVert sv;

        to_screen(d, &vs[i], &sv);
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
                  r[A2_RB_SURFACE_INFO] & 0x3fff, sf->bpp, ci & (1 << 6));
    if (!dpitch) {
        dpitch = src.pitch;
    }
    job = (AdrenoResolveJob) {
        .src = src, .sfmt = sfmt, .dfmt = dfmt, .sswap = sswap,
        .dswap = dswap, .dmask = dmask, .dpitch = dpitch, .dbase = dbase,
        .offx = offx, .offy = offy, .rx0 = rx0, .ry0 = ry0,
        .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1,
        .raw = sfmt == dfmt && sswap == dswap && dmask == 0xf && !(dinfo & 7),
        .tiled = !(dinfo & (1 << 3)) && is_power_of_2(df->bpp),
    };
    {
        /* the destination rows, whole tile rows when tiled */
        uint64_t rowb = (uint64_t)dpitch * df->bpp;
        uint64_t lo = dbase + ((y0 - ry0 + offy) & ~31) * rowb;
        uint64_t hi = dbase + ROUND_UP(y1 - ry0 + offy, 32) * rowb;

        adreno_mem_written(s, lo, MIN(hi, UINT32_MAX));
    }
    /*
     * With the host GPU, a copy of GMEM is taken on the GPU and copied to
     * memory when the commands are done: no waiting for the GPU per bin.
     */
    if (!(copyc & 0xf8) && src.host && adreno_gl_gmem_snapshot(s, &job.slot)) {
        if (s->nresolves == s->cap_resolves) {
            s->cap_resolves = MAX(16, s->cap_resolves * 2);
            s->resolves = g_renew(AdrenoResolveJob, s->resolves,
                                  s->cap_resolves);
        }
        s->resolves[s->nresolves++] = job;
        return;
    }
    adreno_resolves_flush(s);
    adreno_gl_gmem_to_cpu(s);
    resolve_copy(s, &job, s->gmem);

    /* clear the source after the copy */
    if ((copyc >> 4) & 0xf) {
        uint32_t cv = r[A2_CLEAR_COLOR];
        float c[4] = {
            (cv & 0xff) / 255.0f, ((cv >> 8) & 0xff) / 255.0f,
            ((cv >> 16) & 0xff) / 255.0f, ((cv >> 24) & 0xff) / 255.0f,
        };
        uint8_t px[16];

        if (sswap == 1) {
            swap_rb(c);
        }
        color_pack(sfmt, c, px);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                surface_rw(s, tlb, &src, x, y, px, true);
            }
        }
    }
    if (copyc & (1 << 3)) {
        uint32_t di = r[A2_RB_DEPTH_INFO];
        Surface ds;
        uint8_t px[4];

        surface_setup(s, &ds, di & 0xfffff000, src.pitch,
                      (di & 1) ? 4 : 2, true);
        stl_le_p(px, r[A2_RB_DEPTH_CLEAR]);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                surface_rw(s, tlb, &ds, x, y, px, true);
            }
        }
    }
    if (copyc & 0xf8) {
        adreno_gl_gmem_from_cpu(s);
    }
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

/* the draw's vertex indices, with VGT_INDX_OFFSET and the index clamp */
static void fetch_indices(MsmAdrenoState *s, AdrenoTlb *tlb, uint32_t *idx,
                          uint32_t n, unsigned src, uint32_t init,
                          uint32_t index_base, const uint32_t *imm,
                          uint32_t imm_dwords)
{
    bool i32 = init & (1 << 11), i8 = init & (1 << 13);
    uint32_t ofs = s->regs[A2_VGT_INDX_OFFSET];
    uint32_t lo = s->regs[A2_VGT_MIN_VTX_INDX];
    uint32_t hi = s->regs[A2_VGT_MAX_VTX_INDX];

    if (src == 0) {
        /* DMA: the index buffer in one read */
        unsigned size = i32 ? 4 : i8 ? 1 : 2;
        g_autofree uint8_t *buf = g_malloc((size_t)n * size);

        adreno_tlb_read(s, tlb, index_base, buf, n * size);
        for (uint32_t i = 0; i < n; i++) {
            idx[i] = i32 ? ldl_le_p(buf + 4 * i) :
                     i8 ? buf[i] : lduw_le_p(buf + 2 * i);
        }
    } else if (src == 1) {
        for (uint32_t i = 0; i < n; i++) {
            if (i32) {
                idx[i] = i < imm_dwords ? imm[i] : 0;
            } else {
                idx[i] = (i / 2) < imm_dwords ?
                         (imm[i / 2] >> (16 * (i & 1))) & 0xffff : 0;
            }
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            idx[i] = i;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        idx[i] = MIN(MAX(idx[i] + ofs, lo), hi);
    }
}

/* shade the vertices of the index list, AL at a time */
static AdrenoVertex *shade_vertices(DrawCtx *d, const uint32_t *idx,
                                    uint32_t n, uint32_t *slot)
{
    MsmAdrenoState *s = d->s;
    AdrenoWorker *w = &s->workers[0];
    AdrenoLanes *L = &w->lanes;
    uint32_t lo = UINT32_MAX, hi = 0, nu = 0;
    g_autofree int32_t *map = NULL;
    g_autofree uint32_t *uniq = g_new(uint32_t, n);
    AdrenoVertex *out;

    for (uint32_t i = 0; i < n; i++) {
        lo = MIN(lo, idx[i]);
        hi = MAX(hi, idx[i]);
    }
    /* share vertices that repeat in the index list */
    if (hi - lo < 1u << 20) {
        map = g_new(int32_t, hi - lo + 1);
        memset(map, 0xff, sizeof(int32_t) * (hi - lo + 1));
        for (uint32_t i = 0; i < n; i++) {
            int32_t *m = &map[idx[i] - lo];

            if (*m < 0) {
                *m = nu;
                uniq[nu++] = idx[i];
            }
            slot[i] = *m;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uniq[i] = idx[i];
            slot[i] = i;
        }
        nu = n;
    }

    out = g_new(AdrenoVertex, nu);
    L->s = s;
    L->tlb = &w->tlb;
    L->prog = adreno_program(s, A2_SHADER_VS);
    L->quads = false;
    L->texgen = s->texgen;
    adreno_tlb_flush(&w->tlb);
    for (uint32_t b = 0; b < nu; b += AL) {
        uint32_t cnt = MIN(AL, nu - b);
        uint32_t mask = (cnt == AL) ? 0xffff : (1u << cnt) - 1;

        memset(L->r, 0, sizeof(L->r));
        for (uint32_t l = 0; l < cnt; l++) {
            L->r[0][0][l] = uniq[b + l];
            L->pos[0][l] = L->pos[1][l] = L->pos[2][l] = 0.0f;
            L->pos[3][l] = 1.0f;
            L->psize[l] = 0.0f;
        }
        memset(L->interp, 0, sizeof(L->interp));
        adreno_shader_exec(L, mask);
        for (uint32_t l = 0; l < cnt; l++) {
            AdrenoVertex *v = &out[b + l];

            for (int k = 0; k < 4; k++) {
                v->pos[k] = L->pos[k][l];
            }
            v->psize = L->psize[l];
            for (int i = 0; i < d->ninterp; i++) {
                for (int k = 0; k < 4; k++) {
                    v->interp[i][k] = L->interp[i][k][l];
                }
            }
        }
    }
    return out;
}

static AdrenoGlDraw gl_draw_desc(DrawCtx *d)
{
    AdrenoGlDraw g = {
        .ps = d->ps,
        .ninterp = d->ninterp,
        .flat_mask = d->flat_mask,
        .param_gen = d->param_gen,
        .param_gen_reg = d->param_gen_reg,
        .persp = d->persp,
        .provoking_last = d->provoking_last,
        .win_offset_vtx = d->win_offset_vtx,
        .win_x = d->win_x,
        .win_y = d->win_y,
        .pix_ofs = 0.5f - d->pix_center,
        .clip_x0 = d->clip_x0, .clip_y0 = d->clip_y0,
        .clip_x1 = d->clip_x1, .clip_y1 = d->clip_y1,
        .color_off = d->color.offset,
        .depth_off = d->depth.offset,
        .pitch = d->color.pitch,
        .sx = d->color.sx, .sy = d->color.sy,
        .color_fmt = d->color_fmt,
        .color_swap = d->color_swap,
        .color_write = d->color_write,
        .depth_24 = d->depth_24,
    };

    return g;
}

/*
 * Triangle lists, strips and fans and points go to the host GPU whole: it
 * runs the vertex shader too.  Rectangles and lines take the CPU's vertex
 * path.
 */
static bool gl_draw_gpu(DrawCtx *d, unsigned prim, const uint32_t *idx,
                        uint32_t n)
{
    MsmAdrenoState *s = d->s;
    AdrenoGlDraw g;

    if (!d->gl) {
        return false;
    }
    switch (prim) {
    case A2_PT_TRILIST: case A2_PT_TRISTRIP: case A2_PT_TRIFAN:
    case A2_PT_POINTLIST: case A2_PT_POINTLIST_PSIZE:
        break;
    default:
        return false;
    }
    g = gl_draw_desc(d);
    g.vs = adreno_program(s, A2_SHADER_VS);
    g.prim = prim;
    g.idx = idx;
    g.nidx = n;
    g.min_idx = UINT32_MAX;
    g.max_idx = 0;
    for (uint32_t i = 0; i < n; i++) {
        g.min_idx = MIN(g.min_idx, idx[i]);
        g.max_idx = MAX(g.max_idx, idx[i]);
    }
    adreno_gl_draw(s, &g);
    s->st.gpu_draws++;
    return true;
}

void adreno_draw(MsmAdrenoState *s, uint32_t init, uint32_t num_indices,
                 uint32_t index_base, uint32_t index_size,
                 const uint32_t *imm, uint32_t imm_dwords)
{
    const uint32_t *r = s->regs;
    unsigned prim = init & 0x3f, src = (init >> 6) & 3;
    unsigned mode = r[A2_RB_MODECONTROL] & 7;
    g_autofree DrawCtx *d = g_new0(DrawCtx, 1);
    g_autofree uint32_t *idx = NULL;
    g_autofree uint32_t *slot = NULL;
    g_autofree AdrenoVertex *verts = NULL;
    uint32_t tl, br, n;

    s->draws++;
    /* textures may have changed since the last draw */
    s->texgen++;
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
    d->r = r;
    d->prog_cntl = r[A2_SQ_PROGRAM_CNTL];
    d->ninterp = MIN(((d->prog_cntl >> 20) & 0xf) + 1, A2_MAX_INTERP);
    /* PARAM_SHADE: 1 = interpolated (Gouraud), 0 = flat */
    d->flat_mask = ~r[A2_SQ_INTERPOLATOR_CNTL] & 0xffff;
    d->provoking_last = r[A2_PA_SU_SC_MODE_CNTL] & (1 << 19);
    d->persp = !(r[A2_PA_SU_SC_MODE_CNTL] & (1 << 20));
    d->pix_center = (r[A2_PA_SU_VTX_CNTL] & 1) ? 0.5f : 0.0f;
    d->win_x = sextract32(r[A2_PA_SC_WINDOW_OFFSET], 0, 15);
    d->win_y = sextract32(r[A2_PA_SC_WINDOW_OFFSET], 16, 15);
    d->win_offset_vtx = r[A2_PA_SU_SC_MODE_CNTL] & (1 << 16);
    d->param_gen = d->prog_cntl & (1 << 18);
    d->param_gen_reg = (r[A2_SQ_CONTEXT_MISC] >> 8) & 63;

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
        /*
         * The blob fast-clears an RGBA8888 bin with COLORX_4_4_4_4 on a 4X
         * MSAA surface of a quarter its pitch, with the 8888 clear colour
         * in the shader: only 32-bit samples fill the bin, with that
         * colour.  (A 565 bin is cleared with 565 the same way.)
         */
        if (((r[A2_RB_SURFACE_INFO] >> 14) & 3) && d->color_fmt == 0) {
            d->color_fmt = 5;
        }
        surface_setup(s, &d->color, ci & 0xfffff000, pitch,
                      color_fmts[d->color_fmt].bpp, ci & (1 << 6));
        d->depth_24 = di & 1;
        surface_setup(s, &d->depth, di & 0xfffff000, pitch,
                      d->depth_24 ? 4 : 2, true);
        d->color_write = mode == A2_EDRAM_COLOR_DEPTH;
        if (pitch) {
            d->clip_x1 = MIN(d->clip_x1, (int)pitch);
        }
    }

    if (mode != A2_EDRAM_COPY) {
        uint32_t dc = r[A2_RB_DEPTHCONTROL];

        d->ps = adreno_program(s, A2_SHADER_PS);
        /* depth/stencil before shading when nothing can drop the pixel */
        d->early_z = (dc & 3) && !d->ps->kills && !d->ps->exports_depth &&
                     !(r[A2_RB_COLORCONTROL] & (3 << 3));
        /*
         * The host GPU renders to GMEM; render targets in memory (the
         * blob's texture copies) are drawn here.  Two host backends can do
         * it: desktop OpenGL ("gl") and OpenGL ES ("gles", for a phone).
         */
        d->gl = (!d->color_write || d->color.host) &&
                (!(dc & 3) || d->depth.host) &&
                (adreno_gl_available(s) || adreno_gles_available(s));
        if (s->stats_on) {
            /* what this draw is, whichever backend ends up taking it */
            if (d->color_write && !d->color.host) {
                s->st.d_mem++;
            }
            if (d->color.sx > 1 || d->color.sy > 1) {
                s->st.d_msaa++;
            }
            if (!(r[A2_RB_COLORCONTROL] & (1 << 5)) &&
                r[A2_RB_BLEND_CONTROL] != 0x00010001) {
                s->st.d_blend++;
            }
            if (!d->color_write) {
                s->st.d_nocolor++;
            }
            if (d->clip_x0 >= d->clip_x1 || d->clip_y0 >= d->clip_y1) {
                s->st.d_empty++;
            }
        }
    }

    /* the vertices */
    n = num_indices;
    if (mode == A2_EDRAM_COPY) {
        n = MIN(n, 3u);
    }
    idx = g_new(uint32_t, n);
    slot = g_new(uint32_t, n);
    adreno_tlb_flush(&s->workers[0].tlb);
    fetch_indices(s, &s->workers[0].tlb, idx, n, src, init, index_base, imm,
                  imm_dwords);
    if (mode != A2_EDRAM_COPY && gl_draw_gpu(d, prim, idx, n)) {
        return;
    }
    verts = shade_vertices(d, idx, n, slot);

    if (mode == A2_EDRAM_COPY) {
        AdrenoVertex rv[3];

        for (uint32_t i = 0; i < n; i++) {
            rv[i] = verts[slot[i]];
        }
        resolve(d, rv, n);
        return;
    }

    if (!d->gl) {
        adreno_resolves_flush(s);
        adreno_gl_gmem_to_cpu(s);
    }

#define V(i) (&verts[slot[(i)]])
    switch (prim) {
    case A2_PT_TRILIST:
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            draw_triangle(d, V(i), V(i + 1), V(i + 2));
        }
        break;
    case A2_PT_RECTLIST:
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            draw_rect(d, V(i), V(i + 1), V(i + 2));
        }
        break;
    case A2_PT_TRISTRIP:
        for (uint32_t i = 2; i < n; i++) {
            if (i & 1) {
                draw_triangle(d, V(i - 1), V(i - 2), V(i));
            } else {
                draw_triangle(d, V(i - 2), V(i - 1), V(i));
            }
        }
        break;
    case A2_PT_TRIFAN:
        for (uint32_t i = 2; i < n; i++) {
            draw_triangle(d, V(0), V(i - 1), V(i));
        }
        break;
    case A2_PT_POINTLIST:
    case A2_PT_POINTLIST_PSIZE: {
        bool vs_size = prim == A2_PT_POINTLIST_PSIZE;

        /* a shader that writes gl_PointSize exports it */
        for (uint32_t i = 0; i < n && !vs_size; i++) {
            vs_size = V(i)->psize > 0.0f;
        }
        for (uint32_t i = 0; i < n; i++) {
            draw_point(d, V(i), vs_size);
        }
        break;
    }
    case A2_PT_LINELIST:
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            draw_line(d, V(i), V(i + 1));
        }
        break;
    case A2_PT_LINESTRIP:
    case A2_PT_LINELOOP:
        for (uint32_t i = 0; i + 1 < n; i++) {
            draw_line(d, V(i), V(i + 1));
        }
        if (prim == A2_PT_LINELOOP && n > 2) {
            draw_line(d, V(n - 1), V(0));
        }
        break;
    default:
        adreno_trace(s, "  primitive type %u not drawn\n", prim);
        break;
    }
#undef V
    if (d->gl) {
        AdrenoGlDraw g = gl_draw_desc(d);

        g.verts = d->glv;
        g.nverts = d->glv_n;
        adreno_gl_draw(s, &g);
        s->st.cpu_draws++;
        g_free(d->glv);
    } else {
        raster_run(d);
        s->st.soft_draws++;
        /* the CPU drew into GMEM or memory */
        adreno_gl_gmem_from_cpu(s);
        if (!d->color.host || !d->depth.host) {
            adreno_mem_written(s, 0, UINT32_MAX);
        }
    }
    g_free(d->prims);
    (void)index_size;
}
