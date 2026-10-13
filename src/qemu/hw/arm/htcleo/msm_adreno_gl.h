/*
 * Adreno 200 host GPU backend (OpenGL 4.5): interface to the 3D pipe
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_ADRENO_GL_H
#define HW_ARM_HTCLEO_MSM_ADRENO_GL_H

#include "msm_adreno.h"

#define ADRENO_GL_MAX_UNITS 16
#define ADRENO_GL_MAX_VBUFS 16

/* a texture fetch of a translated program: one host texture unit each */
typedef struct AdrenoGlslTexUnit {
    uint8_t const_idx, mag, min, mip;
    bool unnorm;
    float bias;
    float off[2];
} AdrenoGlslTexUnit;

typedef struct AdrenoGlslInfo {
    int ntex;
    AdrenoGlslTexUnit tex[ADRENO_GL_MAX_UNITS];
    int nvtx;                           /* vertex fetch constants used */
    uint16_t vtx[ADRENO_GL_MAX_VBUFS];  /* by slot: fetch constant 0-95 */
    /* per slot: the largest stride, and the bytes read around an element */
    uint32_t vtx_stride[ADRENO_GL_MAX_VBUFS];
    int32_t vtx_lo[ADRENO_GL_MAX_VBUFS], vtx_hi[ADRENO_GL_MAX_VBUFS];
} AdrenoGlslInfo;

/*
 * Append the GLSL for program p: helpers, the texture functions and
 * "void sq_main()".  cube_mask: fetch constants that are cube maps;
 * texture units (bindings and u_tex[]) are numbered from unit_base.
 */
void adreno_glsl_translate(const AdrenoProgram *p, bool fragment,
                           uint32_t cube_mask, int unit_base, GString *o,
                           AdrenoGlslInfo *info);

/*
 * A draw for the host GPU: the primitives are set up already (screen
 * space, clipped, culled), three vertices per triangle of
 * ADRENO_GL_VTX_VEC4(ninterp) vec4s:
 *   0: x, y (window coordinates, with the window offset), z, 1/w
 *      (1 when not perspective-correct)
 *   1: point sprite coordinate s, t; 1 if back facing; 0
 *   2+: interpolators, multiplied by 1/w; flat ones (flat_mask) are the
 *      provoking vertex's
 * The pixel shader, depth/stencil test and blending run on the host GPU
 * against GMEM, which the host GPU keeps.
 */
#define ADRENO_GL_VTX_VEC4(n)   (2 + (n))

typedef struct AdrenoGlDraw {
    const AdrenoProgram *ps;
    /*
     * With vs set the host GPU shades the vertices too: idx are the
     * vertex indices of a list, strip or fan of triangles or of points
     * (prim), verts is NULL.
     */
    const AdrenoProgram *vs;
    unsigned prim;
    const uint32_t *idx;
    uint32_t nidx, min_idx, max_idx;
    bool persp, provoking_last, win_offset_vtx;
    int ninterp;
    uint32_t flat_mask;
    bool param_gen;
    unsigned param_gen_reg;
    int win_x, win_y;
    float pix_ofs;              /* 0.5 - the pixel centre */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    /* render targets: GMEM byte offsets */
    uint32_t color_off, depth_off, pitch;
    unsigned sx, sy;
    unsigned color_fmt, color_swap;
    bool color_write, depth_24;
    const float *verts;
    uint32_t nverts;
} AdrenoGlDraw;

/* the backend is up (it starts on first use); false: software rendering */
bool adreno_gl_available(MsmAdrenoState *s);
void adreno_gl_draw(MsmAdrenoState *s, const AdrenoGlDraw *g);
/* bring s->gmem up to date with the host GPU's copy */
void adreno_gl_gmem_to_cpu(MsmAdrenoState *s);
/* s->gmem changed: the host GPU's copy is stale */
void adreno_gl_gmem_from_cpu(MsmAdrenoState *s);

/*
 * Copies of GMEM for resolves done later: snapshot() queues a copy on the
 * host GPU (false: none free, or no host GPU), wait() waits for all of
 * them, release() frees them.
 */
bool adreno_gl_gmem_snapshot(MsmAdrenoState *s, int *slot);
void adreno_gl_snapshots_wait(MsmAdrenoState *s);
uint8_t *adreno_gl_snapshot_ptr(MsmAdrenoState *s, int slot);
void adreno_gl_snapshots_release(MsmAdrenoState *s);

/* msm_adreno_tex.c: textures for the host GPU */
typedef struct AdrenoTexImage {
    uint32_t w, h;              /* level 0 */
    int levels;
    int layers;                 /* 6 for a cube map */
    bool is_float;              /* RGBA32F texels, else RGBA8 */
    /* sampling */
    unsigned clamp[2];
    unsigned mag, min, mip;     /* the fetch constant's filters */
    int min_level, max_level;
    float lod_bias;
    float border[4];
    uint8_t swz[4];             /* 0-3 component, 4 zero, 5 one */
    int exp_adj;
} AdrenoTexImage;

bool adreno_tex_image(MsmAdrenoState *s, unsigned idx, AdrenoTexImage *im);
/* a hash of the memory [*lo, *hi) the texture's levels occupy */
uint64_t adreno_tex_hash(MsmAdrenoState *s, AdrenoTlb *tlb, unsigned idx,
                         uint32_t *lo, uint32_t *hi);
/* decode a level (w >> level by h >> level texels) of a layer */
void adreno_tex_decode(MsmAdrenoState *s, AdrenoTlb *tlb, unsigned idx,
                       int level, int layer, void *out);

#endif
