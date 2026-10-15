/*
 * Adreno 200 host GPU backend: OpenGL 4.5 on a surfaceless EGL context
 *
 * Xenia's approach for Xenos, on the Adreno 200 that inherited its design:
 * the guest's pixel shaders are translated to GLSL (msm_adreno_glsl.c)
 * and run on the host GPU, and GMEM, the 256 KiB of on-chip render memory
 * the driver bins into, is a host GPU buffer the translated shaders read
 * and write themselves, in the guest's byte layout.  Fragment shader
 * interlock (GL_ARB_fragment_shader_interlock, as Xenia's rasterizer-
 * ordered-view render backend uses on D3D12) keeps the read-modify-write
 * of each pixel in primitive order, so depth/stencil, blending, colour
 * masks, MSAA sample grids and the driver's reinterpreting fast clears
 * behave exactly as in the software render backend.
 *
 * Vertices are still shaded, clipped and set up by msm_adreno_draw.c;
 * this backend rasterizes the screen-space triangles it gets.  Textures
 * are decoded from guest memory to host textures and kept while the
 * memory under them hashes the same.  GMEM goes back to the CPU when a
 * resolve copies it to memory, and to the host GPU after the CPU changed
 * it.
 *
 * The context is current on the thread the command processor runs in
 * (the vCPU's), the only one that draws.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "msm_adreno_gl.h"
#include "msm_adreno_sq.h"

#ifdef CONFIG_OPENGL
#include <epoxy/gl.h>
#include <epoxy/egl.h>

/* an empty framebuffer covering every window coordinate a draw can use */
#define FB_SIZE         16384
#define TEX_CACHE_MAX   768

/* where the vertices come from */
enum {
    GEOM_CPU,               /* set up by msm_adreno_draw.c */
    GEOM_TRIS,              /* the vertex shader on the host GPU */
    GEOM_POINTS,
};

typedef struct GlProgKey {
    uint64_t ps_hash, vs_hash;
    uint32_t flat_mask;
    uint32_t cube_mask, vs_cube_mask;
    uint8_t ninterp, param_gen, param_gen_reg, geom, persp, pad[3];
} GlProgKey;

typedef struct GlProg {
    GlProgKey key;
    GLuint prog;
    bool exports_depth;
    AdrenoGlslInfo info, vinfo;
} GlProg;

typedef struct GlTexKey {
    uint32_t fc[6];
} GlTexKey;

typedef struct GlTex {
    GlTexKey key;
    GLuint tex;
    uint64_t hash;
    AdrenoMemStamp stamp;       /* when hashed or checked */
    uint32_t lo, hi;            /* the memory it was decoded from */
    uint32_t npages;
    uint64_t *pages;            /* its pages' RAM addresses + 1 (0: none) */
    uint64_t used;
    AdrenoTexImage im;
} GlTex;

typedef struct GlSamplerKey {
    uint32_t wrap[2], mag, min;
    float min_lod, max_lod, bias;
    float border[4];
} GlSamplerKey;

typedef struct GlSampler {
    GlSamplerKey key;
    GLuint smp;
} GlSampler;

/* the Draw uniform block (std140) */
typedef struct TexU {
    float size[4];          /* w, h, 1/w, 1/h of level 0 */
    float scale[4];         /* 2^exp_adj, LOD bias */
    int32_t swz[4];
} TexU;

typedef struct DrawU {
    uint32_t bools[8];
    uint32_t loops[32];
    TexU tex[2 * ADRENO_GL_MAX_UNITS];  /* pixel, then vertex shader's */
    int32_t cb[4];          /* PS, VS constant bases */
    int32_t csurf[4];       /* colour: GMEM offset, pitch, bpp, sx | sy << 4 */
    int32_t cfmt[4];        /* channel widths */
    int32_t cinfo[4];       /* kind, swap, colour write, colour mask */
    int32_t dsurf[4];       /* depth: GMEM offset, bpp */
    uint32_t rb[4];         /* DEPTHCONTROL, BLEND_CONTROL, COLORCONTROL,
                               STENCILREFMASK */
    uint32_t rb2[4];        /* STENCILREFMASK_BF, blending */
    float blendc[4];
    float misc[4];          /* alpha ref, window offset x, y, pixel offset */
    /* vertex shading on the host GPU */
    int32_t vb[ADRENO_GL_MAX_VBUFS][4];   /* vertex data: word index in
                                             vd[], first byte, endian swap,
                                             bytes */
    float vp0[4];           /* viewport x scale, offset, y scale, offset */
    float vp1[4];           /* z scale, offset; point half size */
    float vp2[4];           /* window offset x, y (when applied); pixel
                               offset */
    uint32_t vte[4];        /* PA_CL_VTE_CNTL, PA_CL_CLIP_CNTL */
    float psz[4];           /* point min, max half size; size from VS */
} DrawU;

typedef struct AdrenoGl {
    EGLDisplay dpy;
    EGLContext ctx;
    QemuThread owner;
    GLuint fbo, vao;
    GLuint gmem;
    /*
     * What draws read (uniforms, constants, vertices, indices) is written
     * once into this persistently mapped buffer; it starts over when the
     * host GPU is idle (no driver copies, no waits between draws).
     */
    GLuint stream;
    uint8_t *stream_map;
    size_t stream_pos;
    GLint ubo_align, ssbo_align;
    GLuint gmem_stage;          /* GMEM's way to and from the CPU */
    uint8_t *gmem_map;
    /* stats: GPU timestamps at the first draw of a kick and its end */
    GLuint tq[2];
    bool tq_started;
    uint32_t tq_kick;
    /* GMEM snapshots for resolves */
    GLuint snap[32];
    uint8_t *snap_map[32];
    int nsnap, snap_used;
    int cur_geom;
    int barriers;
    const struct GlProg *last_prog;
    bool stream_wrapped;
    GLuint dummy2d, dummy_arr;
    GHashTable *progs, *texes, *samplers;
    bool gpu_newer, cpu_newer;
    uint64_t ndraw;
    AdrenoTlb tlb;
    uint8_t *scratch;
    size_t scratch_size;
    /* by RAM page: the command kick it last changed in (dirty logging) */
    uint32_t *page_kick;
    size_t npage_kick;
} AdrenoGl;

/* ------------------------------------------------------------------ */
/* GLSL: what every program shares                                     */
/* ------------------------------------------------------------------ */

static const char glsl_common[] =
    "layout(std140, binding = 0) uniform Consts { vec4 u_c[512]; };\n"
    "struct TexU { vec4 size; vec4 scale; ivec4 swz; };\n"
    "layout(std140, binding = 1) uniform Draw {\n"
    "    uvec4 u_bools[2];\n"
    "    uvec4 u_loops[8];\n"
    "    TexU u_tex[32];\n"
    "    ivec4 u_cb;\n"
    "    ivec4 u_csurf;\n"
    "    ivec4 u_cfmt;\n"
    "    ivec4 u_cinfo;\n"
    "    ivec4 u_dsurf;\n"
    "    uvec4 u_rb;\n"
    "    uvec4 u_rb2;\n"
    "    vec4 u_blendc;\n"
    "    vec4 u_misc;\n"
    "    ivec4 u_vb[16];\n"
    "    vec4 u_vp0;\n"
    "    vec4 u_vp1;\n"
    "    vec4 u_vp2;\n"
    "    uvec4 u_vte;\n"
    "    vec4 u_psz;\n"
    "};\n"
    /* the half floats of the software render backend, bit for bit */
    "float h2f(uint h) {\n"
    "    uint e = (h >> 10) & 0x1fu, man = h & 0x3ffu;\n"
    "    float f;\n"
    "    if (e == 0u) {\n"
    "        f = ldexp(float(man), -24);\n"
    "    } else if (e == 31u) {\n"
    "        f = uintBitsToFloat(man != 0u ? 0x7fc00000u : 0x7f800000u);\n"
    "    } else {\n"
    "        f = ldexp(float(man | 0x400u), int(e) - 25);\n"
    "    }\n"
    "    return (h & 0x8000u) != 0u ? -f : f;\n"
    "}\n"
    "uint f2h(float f) {\n"
    "    uint u = floatBitsToUint(f);\n"
    "    uint sign = (u >> 16) & 0x8000u;\n"
    "    int e = int((u >> 23) & 0xffu) - 127 + 15;\n"
    "    uint man = u & 0x7fffffu;\n"
    "    if (((u >> 23) & 0xffu) == 0xffu) {\n"
    "        return sign | 0x7c00u | (man != 0u ? 0x200u : 0u);\n"
    "    }\n"
    "    if (e >= 31) {\n"
    "        return sign | 0x7c00u;\n"
    "    }\n"
    "    if (e <= 0) {\n"
    "        if (e < -10) {\n"
    "            return sign;\n"
    "        }\n"
    "        return sign | ((man | 0x800000u) >> uint(14 - e));\n"
    "    }\n"
    "    return sign | (uint(e) << 10) | (man >> 13);\n"
    "}\n";

/*
 * Vertex fetches read the guest's vertex buffers, copied for the draw
 * into vd[]: u_vb[slot] = word index, first byte (relative to the fetch
 * constant's address), endian swap, bytes.
 */
static const char glsl_vfetch[] =
    "layout(std430, binding = 2) readonly buffer Vdata { uint vd[]; };\n"
    "uint vfetch_word(int s, int va) {\n"
    "    ivec4 b = u_vb[s];\n"
    "    int a = va - b.y;\n"
    "    if (a < 0 || a + 4 > b.w) {\n"
    "        return 0u;\n"
    "    }\n"
    "    uint i = uint(b.x) + uint(a >> 2), sh = uint(a & 3) * 8u;\n"
    "    return sh == 0u ? vd[i] : (vd[i] >> sh) | (vd[i + 1u] << (32u - sh));\n"
    "}\n"
    "int vunit(int s) {\n"
    "    return (u_vb[s].z & 4) != 0 ? 4 : 1;\n"
    "}\n"
    "uint vswap(uint w, int s) {\n"
    "    switch (u_vb[s].z & 3) {\n"
    "    case 1: return ((w & 0x00ff00ffu) << 8) | ((w >> 8) & 0x00ff00ffu);\n"
    "    case 2: return (w >> 24) | ((w >> 8) & 0xff00u) |\n"
    "                   ((w << 8) & 0xff0000u) | (w << 24);\n"
    "    case 3: return (w << 16) | (w >> 16);\n"
    "    }\n"
    "    return w;\n"
    "}\n"
    "vec4 vunpack(uvec4 dw, int n, ivec4 bits, bool sgn, bool norm, vec4 fv) {\n"
    "    int pos = 0;\n"
    "    for (int k = 0; k < n; k++) {\n"
    "        uint w = dw[pos >> 5];\n"
    "        int b = bits[k];\n"
    "        if (sgn) {\n"
    "            fv[k] = float(bitfieldExtract(int(w), pos & 31, b));\n"
    "            if (norm) {\n"
    "                fv[k] = max(-1.0, fv[k] / float((1 << (b - 1)) - 1));\n"
    "            }\n"
    "        } else {\n"
    "            fv[k] = float(bitfieldExtract(w, pos & 31, b));\n"
    "            if (norm) {\n"
    "                fv[k] /= float((1u << uint(b)) - 1u);\n"
    "            }\n"
    "        }\n"
    "        pos += b;\n"
    "    }\n"
    "    return fv;\n"
    "}\n";

/*
 * The render backend, as msm_adreno_draw.c's depth_stencil() and
 * write_color() on GMEM, which is "gm" here.  A pixel's own samples are
 * written with plain stores when whole words, else with atomics, since
 * the neighbouring pixels sharing the word are not interlocked with it.
 */
static const char glsl_rop[] =
    "layout(std430, binding = 0) coherent buffer Gmem { uint gm[]; };\n"
    "uint gm_get(uint a, uint n) {\n"
    "    uint w = gm[a >> 2];\n"
    "    return n == 4u ? w : (w >> ((a & 3u) * 8u)) & ((1u << (n * 8u)) - 1u);\n"
    "}\n"
    "void gm_put(uint a, uint n, uint v) {\n"
    "    if (n == 4u) {\n"
    "        gm[a >> 2] = v;\n"
    "        return;\n"
    "    }\n"
    "    uint sh = (a & 3u) * 8u;\n"
    "    uint m = ((1u << (n * 8u)) - 1u) << sh;\n"
    "    atomicAnd(gm[a >> 2], ~m);\n"
    "    atomicOr(gm[a >> 2], (v << sh) & m);\n"
    "}\n"
    "uvec4 px_get(uint a, uint bpp) {\n"
    "    if (bpp == 3u) {\n"
    "        return uvec4(gm_get(a, 1u) | (gm_get(a + 1u, 1u) << 8) |\n"
    "                     (gm_get(a + 2u, 1u) << 16), 0u, 0u, 0u);\n"
    "    }\n"
    "    if (bpp <= 4u) {\n"
    "        return uvec4(gm_get(a, bpp), 0u, 0u, 0u);\n"
    "    }\n"
    "    uint i = a >> 2;\n"
    "    if (bpp == 8u) {\n"
    "        return uvec4(gm[i], gm[i + 1u], 0u, 0u);\n"
    "    }\n"
    "    return uvec4(gm[i], gm[i + 1u], gm[i + 2u], gm[i + 3u]);\n"
    "}\n"
    "void px_put(uint a, uint bpp, uvec4 w) {\n"
    "    if (bpp == 3u) {\n"
    "        gm_put(a, 1u, w.x & 0xffu);\n"
    "        gm_put(a + 1u, 1u, (w.x >> 8) & 0xffu);\n"
    "        gm_put(a + 2u, 1u, (w.x >> 16) & 0xffu);\n"
    "    } else if (bpp <= 4u) {\n"
    "        gm_put(a, bpp, w.x);\n"
    "    } else {\n"
    "        uint i = a >> 2;\n"
    "        gm[i] = w.x;\n"
    "        gm[i + 1u] = w.y;\n"
    "        if (bpp == 16u) {\n"
    "            gm[i + 2u] = w.z;\n"
    "            gm[i + 3u] = w.w;\n"
    "        }\n"
    "    }\n"
    "}\n"
    /* sample (i, j) of pixel (x, y) of a surface at GMEM offset off */
    "bool surf_addr(uint off, uint bpp, int x, int y, int i, int j,\n"
    "               out uint a) {\n"
    "    uint sx = uint(u_csurf.w) & 15u, sy = uint(u_csurf.w) >> 4;\n"
    "    uint e = (uint(y) * sy + uint(j)) * uint(u_csurf.y) * sx +\n"
    "             uint(x) * sx + uint(i);\n"
    "    a = off + e * bpp;\n"
    "    return e < 262144u && e * bpp + bpp <= 262144u - off;\n"
    "}\n"
    "bool cmpf(uint f, float a, float b) {\n"
    "    switch (f & 7u) {\n"
    "    case 0u: return false;\n"
    "    case 1u: return a < b;\n"
    "    case 2u: return a == b;\n"
    "    case 3u: return a <= b;\n"
    "    case 4u: return a > b;\n"
    "    case 5u: return a != b;\n"
    "    case 6u: return a >= b;\n"
    "    }\n"
    "    return true;\n"
    "}\n"
    "uint sten_op(uint op, uint v, uint ref) {\n"
    "    switch (op & 7u) {\n"
    "    case 1u: return 0u;\n"
    "    case 2u: return ref;\n"
    "    case 3u: return v < 255u ? v + 1u : 255u;\n"
    "    case 4u: return v > 0u ? v - 1u : 0u;\n"
    "    case 5u: return ~v & 0xffu;\n"
    "    case 6u: return (v + 1u) & 0xffu;\n"
    "    case 7u: return (v - 1u) & 0xffu;\n"
    "    }\n"
    "    return v;\n"
    "}\n"
    "bool depth_stencil(int x, int y, float z, bool face) {\n"
    "    uint dc = u_rb.x;\n"
    "    bool sen = (dc & 1u) != 0u, zen = (dc & 2u) != 0u;\n"
    "    bool zw = (dc & 4u) != 0u;\n"
    "    if (!sen && !zen) {\n"
    "        return true;\n"
    "    }\n"
    "    uint off = uint(u_dsurf.x), bpp = uint(u_dsurf.y), a;\n"
    "    bool d24 = bpp == 4u;\n"
    "    if (!surf_addr(off, bpp, x, y, 0, 0, a)) {\n"
    "        return true;\n"
    "    }\n"
    "    uint raw = gm_get(a, bpp);\n"
    "    float zmax = d24 ? 16777215.0 : 65535.0;\n"
    "    uint sz = d24 ? raw >> 8 : raw, st = d24 ? raw & 0xffu : 0u;\n"
    "    bool bf = face && (dc & 0x80u) != 0u;\n"
    "    uint rm = bf ? u_rb2.x : u_rb.w;\n"
    "    uint ref = rm & 0xffu, msk = (rm >> 8) & 0xffu;\n"
    "    uint wm = (rm >> 16) & 0xffu;\n"
    "    uint sh = bf ? 20u : 8u;\n"
    "    bool pass = true, zpass = true;\n"
    "    uint zq = uint(roundEven(sat(z) * zmax));\n"
    "    if (sen) {\n"
    "        pass = cmpf((dc >> sh) & 7u, float(ref & msk), float(st & msk));\n"
    "    }\n"
    "    if (pass && zen) {\n"
    "        zpass = cmpf((dc >> 4) & 7u, float(zq), float(sz));\n"
    "    }\n"
    "    if (sen && d24) {\n"
    "        uint op = !pass ? (dc >> (sh + 3u)) :\n"
    "                  !zpass ? (dc >> (sh + 9u)) : (dc >> (sh + 6u));\n"
    "        st = (st & ~wm) | (sten_op(op, st, ref) & wm);\n"
    "    }\n"
    "    if (pass && zpass && zen && zw) {\n"
    "        sz = zq;\n"
    "    }\n"
    "    if (sen || (zen && zw)) {\n"
    "        uint v = d24 ? (sz << 8) | st : sz;\n"
    "        int sx = u_csurf.w & 15, sy = u_csurf.w >> 4;\n"
    "        for (int j = 0; j < sy; j++) {\n"
    "            for (int i = 0; i < sx; i++) {\n"
    "                if (surf_addr(off, bpp, x, y, i, j, a)) {\n"
    "                    gm_put(a, bpp, v);\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    return pass && zpass;\n"
    "}\n"
    /* colour formats: channels from bit 0 up, before SWAP */
    "vec4 c_unpack(uvec4 w) {\n"
    "    vec4 c = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    int kind = u_cinfo.x;\n"
    "    if (kind == 2) {\n"
    "        for (int k = 0; k < 4 && u_cfmt[k] != 0; k++) {\n"
    "            c[k] = h2f((w[k >> 1] >> (16 * (k & 1))) & 0xffffu);\n"
    "        }\n"
    "    } else if (kind == 3) {\n"
    "        for (int k = 0; k < 4 && u_cfmt[k] != 0; k++) {\n"
    "            c[k] = uintBitsToFloat(w[k]);\n"
    "        }\n"
    "    } else {\n"
    "        int pos = 0;\n"
    "        for (int k = 0; k < 4 && u_cfmt[k] != 0; k++) {\n"
    "            uint mask = (1u << uint(u_cfmt[k])) - 1u;\n"
    "            uint x = (w.x >> uint(pos)) & mask;\n"
    "            if (kind == 1) {\n"
    "                c[k] = max(-1.0, float(bitfieldExtract(int(x), 0,\n"
    "                           u_cfmt[k])) / float(mask >> 1));\n"
    "            } else {\n"
    "                c[k] = float(x) / float(mask);\n"
    "            }\n"
    "            pos += u_cfmt[k];\n"
    "        }\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "uvec4 c_pack(vec4 c) {\n"
    "    uvec4 w = uvec4(0u);\n"
    "    int kind = u_cinfo.x;\n"
    "    if (kind == 2) {\n"
    "        for (int k = 0; k < 4 && u_cfmt[k] != 0; k++) {\n"
    "            w[k >> 1] |= f2h(c[k]) << (16 * (k & 1));\n"
    "        }\n"
    "    } else if (kind == 3) {\n"
    "        for (int k = 0; k < 4 && u_cfmt[k] != 0; k++) {\n"
    "            w[k] = floatBitsToUint(c[k]);\n"
    "        }\n"
    "    } else {\n"
    "        int pos = 0;\n"
    "        for (int k = 0; k < 4 && u_cfmt[k] != 0; k++) {\n"
    "            uint mask = (1u << uint(u_cfmt[k])) - 1u;\n"
    "            uint x;\n"
    "            if (kind == 1) {\n"
    "                x = uint(int(roundEven(clamp(c[k], -1.0, 1.0) *\n"
    "                                       float(mask >> 1)))) & mask;\n"
    "            } else {\n"
    "                x = uint(roundEven(sat(c[k]) * float(mask)));\n"
    "            }\n"
    "            w.x |= x << uint(pos);\n"
    "            pos += u_cfmt[k];\n"
    "        }\n"
    "    }\n"
    "    return w;\n"
    "}\n"
    "float bfac(uint f, vec4 s, vec4 d, int k) {\n"
    "    vec4 c = u_blendc;\n"
    "    switch (f) {\n"
    "    case 0u: return 0.0;\n"
    "    case 1u: return 1.0;\n"
    "    case 4u: return s[k];\n"
    "    case 5u: return 1.0 - s[k];\n"
    "    case 6u: return s.w;\n"
    "    case 7u: return 1.0 - s.w;\n"
    "    case 8u: return d[k];\n"
    "    case 9u: return 1.0 - d[k];\n"
    "    case 10u: return d.w;\n"
    "    case 11u: return 1.0 - d.w;\n"
    "    case 12u: return c[k];\n"
    "    case 13u: return 1.0 - c[k];\n"
    "    case 14u: return c.w;\n"
    "    case 15u: return 1.0 - c.w;\n"
    "    case 16u: return k == 3 ? 1.0 : (s.w < 1.0 - d.w ? s.w : 1.0 - d.w);\n"
    "    }\n"
    "    return 1.0;\n"
    "}\n"
    "float bop(uint op, float s, float d, float sf, float df) {\n"
    "    switch (op & 7u) {\n"
    "    case 1u: return s * sf - d * df;\n"
    "    case 2u: return s < d ? s : d;\n"
    "    case 3u: return s > d ? s : d;\n"
    "    case 4u: return d * df - s * sf;\n"
    "    }\n"
    "    return s * sf + d * df;\n"
    "}\n"
    "void write_color(int x, int y, vec4 src) {\n"
    "    uint cmask = uint(u_cinfo.w);\n"
    "    if (u_cinfo.z == 0 || cmask == 0u) {\n"
    "        return;\n"
    "    }\n"
    "    uint off = uint(u_csurf.x), bpp = uint(u_csurf.z), a;\n"
    "    bool swap = u_cinfo.y == 1, blend = u_rb2.y != 0u;\n"
    "    vec4 dst = vec4(0.0), o;\n"
    "    if (u_cinfo.x == 0) {\n"
    "        src = sat4(src);\n"
    "    }\n"
    "    if (blend || cmask != 15u) {\n"
    "        if (!surf_addr(off, bpp, x, y, 0, 0, a)) {\n"
    "            return;\n"
    "        }\n"
    "        dst = c_unpack(px_get(a, bpp));\n"
    "        if (swap) {\n"
    "            dst = dst.zyxw;\n"
    "        }\n"
    "    }\n"
    "    o = src;\n"
    "    if (blend) {\n"
    "        uint bc = u_rb.y;\n"
    "        for (int k = 0; k < 4; k++) {\n"
    "            uint sf = k < 3 ? bc & 31u : (bc >> 16) & 31u;\n"
    "            uint df = k < 3 ? (bc >> 8) & 31u : (bc >> 24) & 31u;\n"
    "            uint op = k < 3 ? (bc >> 5) & 7u : (bc >> 21) & 7u;\n"
    "            o[k] = bop(op, src[k], dst[k], bfac(sf, src, dst, k),\n"
    "                       bfac(df, src, dst, k));\n"
    "        }\n"
    "    }\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "        if ((cmask & (1u << k)) == 0u) {\n"
    "            o[k] = dst[k];\n"
    "        }\n"
    "    }\n"
    "    if (swap) {\n"
    "        o = o.zyxw;\n"
    "    }\n"
    "    uvec4 w = c_pack(o);\n"
    "    int sx = u_csurf.w & 15, sy = u_csurf.w >> 4;\n"
    "    for (int j = 0; j < sy; j++) {\n"
    "        for (int i = 0; i < sx; i++) {\n"
    "            if (!surf_addr(off, bpp, x, y, i, j, a)) {\n"
    "                return;\n"
    "            }\n"
    "            px_put(a, bpp, w);\n"
    "        }\n"
    "    }\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Programs                                                            */
/* ------------------------------------------------------------------ */

static guint key_hash(gconstpointer k, size_t n)
{
    const uint8_t *p = k;
    uint32_t h = 2166136261u;

    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static guint prog_hash(gconstpointer k)
{
    return key_hash(k, sizeof(GlProgKey));
}

static gboolean prog_equal(gconstpointer a, gconstpointer b)
{
    return !memcmp(a, b, sizeof(GlProgKey));
}

static guint tex_hash(gconstpointer k)
{
    return key_hash(k, sizeof(GlTexKey));
}

static gboolean tex_equal(gconstpointer a, gconstpointer b)
{
    return !memcmp(a, b, sizeof(GlTexKey));
}

static guint smp_hash(gconstpointer k)
{
    return key_hash(k, sizeof(GlSamplerKey));
}

static gboolean smp_equal(gconstpointer a, gconstpointer b)
{
    return !memcmp(a, b, sizeof(GlSamplerKey));
}

/* ADRENO_GL_DEBUG=1: GL errors to stderr, every program's source to cwd */
static bool gl_debug;

static void GLAPIENTRY debug_cb(GLenum source, GLenum type, GLuint id,
                                GLenum severity, GLsizei length,
                                const GLchar *msg, const void *opaque)
{
    if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH ||
        severity == GL_DEBUG_SEVERITY_MEDIUM) {
        error_report("msm-adreno: GL: %s", msg);
    }
}

static void dump_source(const char *what, const char *src, const char *log)
{
    g_autofree char *path = g_strdup_printf("adreno-gl-%s-%08x.glsl", what,
                                            g_str_hash(src));

    error_report("msm-adreno: %s shader does not compile (source in %s):\n%s",
                 what, path, log);
    g_file_set_contents(path, src, -1, NULL);
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = "";

        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        dump_source(type == GL_VERTEX_SHADER ? "vs" : "fs", src, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void interp_decls(GString *o, const GlProgKey *k, const char *dir)
{
    for (int i = 0; i < k->ninterp; i++) {
        const char *q;

        if (k->flat_mask & (1u << i)) {
            q = "flat";
        } else if (k->geom == GEOM_CPU || !k->persp) {
            /* GEOM_CPU: multiplied by 1/w already */
            q = "noperspective";
        } else {
            q = "smooth";
        }
        g_string_append_printf(o, "layout(location = %d) %s %s vec4 v_i%d;\n",
                               2 + i, q, dir, i);
    }
}

/* vertices set up by msm_adreno_draw.c, in window coordinates */
static GString *vs_source_cpu(const GlProgKey *k)
{
    GString *o = g_string_new("#version 450 core\n");

    g_string_append(o, glsl_common);
    g_string_append(o,
        "layout(std430, binding = 1) readonly buffer Verts { vec4 vd[]; };\n"
        "layout(location = 0) noperspective out vec4 v_zw;\n"
        "layout(location = 1) flat out float v_face;\n");
    interp_decls(o, k, "out");
    g_string_append_printf(o,
        "void main() {\n"
        "    int b = gl_VertexID * %d;\n"
        "    vec4 p = vd[b], m = vd[b + 1];\n"
        "    gl_Position = vec4((p.xy + u_misc.w) * (1.0 / %d.0) - 1.0, "
        "0.0, 1.0);\n"
        "    v_zw = vec4(p.z, p.w, m.x, m.y);\n"
        "    v_face = m.z;\n", ADRENO_GL_VTX_VEC4(k->ninterp), FB_SIZE / 2);
    for (int i = 0; i < k->ninterp; i++) {
        g_string_append_printf(o, "    v_i%d = vd[b + %d];\n", i, 2 + i);
    }
    g_string_append(o, "}\n");
    return o;
}

/*
 * The guest's vertex shader, then what msm_adreno_draw.c does to its
 * position: the clip planes (as clip distances, in the guest's clip
 * space), the viewport transform of PA_CL_VTE_CNTL and the window offset,
 * to the host's window coordinates.  Depth is not clipped (depth clamp)
 * but clamped by the render backend, as the software one does.
 */
static GString *vs_source_gpu(const GlProgKey *k, const AdrenoProgram *vs,
                              AdrenoGlslInfo *info)
{
    GString *o = g_string_new("#version 450 core\n");
    bool points = k->geom == GEOM_POINTS;

    g_string_append(o, glsl_common);
    g_string_append(o, glsl_vfetch);
    g_string_append(o, "#define CONST(i) u_c[((i) + u_cb.y) & 511]\n");
    adreno_glsl_translate(vs, false, k->vs_cube_mask, ADRENO_GL_MAX_UNITS, o,
                          info);
    g_string_append(o, "out gl_PerVertex { vec4 gl_Position; "
                    "float gl_PointSize; float gl_ClipDistance[7]; };\n");
    interp_decls(o, k, "out");
    g_string_append_printf(o,
        "void main() {\n"
        "    for (int i = 0; i < 64; i++) {\n"
        "        r[i] = vec4(0.0);\n"
        "    }\n"
        "    for (int i = 0; i < 16; i++) {\n"
        "        o_interp[i] = vec4(0.0);\n"
        "    }\n"
        "    r[0].x = float(gl_VertexID);\n"
        "    sq_main();\n"
        "    vec4 p = o_pos;\n"
        "    uint vte = u_vte.x, clip = u_vte.y;\n"
        "    bool cl = %s && (clip & 0x10000u) == 0u;\n"
        "    gl_ClipDistance[0] = cl ? p.w : 1.0;\n"
        "    gl_ClipDistance[1] = cl ? p.w + p.x : 1.0;\n"
        "    gl_ClipDistance[2] = cl ? p.w - p.x : 1.0;\n"
        "    gl_ClipDistance[3] = cl ? p.w + p.y : 1.0;\n"
        "    gl_ClipDistance[4] = cl ? p.w - p.y : 1.0;\n"
        "    gl_ClipDistance[5] = cl ? p.w - p.z : 1.0;\n"
        "    gl_ClipDistance[6] = !cl ? 1.0 : (clip & 0x80000u) != 0u ? p.z "
        ": p.z + p.w;\n"
        "    float sx = (vte & 1u) != 0u ? u_vp0.x : 1.0;\n"
        "    float ox = ((vte & 2u) != 0u ? u_vp0.y : 0.0) + u_vp2.x + "
        "u_vp2.z;\n"
        "    float sy = (vte & 4u) != 0u ? u_vp0.z : 1.0;\n"
        "    float oy = ((vte & 8u) != 0u ? u_vp0.w : 0.0) + u_vp2.y + "
        "u_vp2.z;\n"
        "    float sz = (vte & 16u) != 0u ? u_vp1.x : 1.0;\n"
        "    float oz = (vte & 32u) != 0u ? u_vp1.y : 0.0;\n"
        "    const float k = 1.0 / %d.0;\n"
        "    if ((vte & 0x700u) == 0x400u) {\n"
        /* the usual: x/w, y/w, z/w; linear in clip space this way */
        "        gl_Position = vec4((sx * p.x + ox * p.w) * k - p.w,\n"
        "                           (sy * p.y + oy * p.w) * k - p.w,\n"
        "                           sz * p.z + oz * p.w, p.w);\n"
        "    } else {\n"
        "        float rhw = (vte & 0x400u) != 0u ? 1.0 / p.w : p.w;\n"
        "        bool xy = (vte & 0x100u) != 0u, zf = (vte & 0x200u) != 0u;\n"
        "        float x = xy ? p.x : p.x * rhw, y = xy ? p.y : p.y * rhw;\n"
        "        float z = zf ? p.z : p.z * rhw;\n"
        "        float wp = rhw != 0.0 ? 1.0 / rhw : 1.0;\n"
        "        gl_Position = vec4(((sx * x + ox) * k - 1.0) * wp,\n"
        "                           ((sy * y + oy) * k - 1.0) * wp,\n"
        "                           (sz * z + oz) * wp, wp);\n"
        "    }\n", points ? "false" : "true", FB_SIZE / 2);
    if (points) {
        /* PA_SU_POINT_SIZE holds half the size; the shader's is the size */
        g_string_append(o,
            "    float hs = u_psz.z != 0.0 ? o_psize * 0.5 : u_vp1.z;\n"
            "    if (u_psz.y > 0.0) {\n"
            "        hs = min(max(hs, u_psz.x), u_psz.y);\n"
            "    }\n"
            "    gl_PointSize = hs > 0.0 ? 2.0 * hs : 1.0;\n");
    }
    for (int i = 0; i < k->ninterp; i++) {
        g_string_append_printf(o, "    v_i%d = o_interp[%d];\n", i, i);
    }
    g_string_append(o, "}\n");
    return o;
}

static GString *fs_source(const GlProgKey *k, const AdrenoProgram *ps,
                          AdrenoGlslInfo *info)
{
    GString *o = g_string_new(
        "#version 450 core\n"
        "#extension GL_ARB_fragment_shader_interlock : require\n"
        "layout(pixel_interlock_ordered) in;\n");
    bool cpu = k->geom == GEOM_CPU;

    g_string_append(o, glsl_common);
    g_string_append(o, "#define CONST(i) u_c[((i) + u_cb.x) & 511]\n");
    adreno_glsl_translate(ps, true, k->cube_mask, 0, o, info);
    g_string_append(o, glsl_rop);
    if (cpu) {
        g_string_append(o,
            "layout(location = 0) noperspective in vec4 v_zw;\n"
            "layout(location = 1) flat in float v_face;\n");
    }
    interp_decls(o, k, "in");
    g_string_append(o,
        "void main() {\n"
        "    for (int i = 0; i < 64; i++) {\n"
        "        r[i] = vec4(0.0);\n"
        "    }\n");
    if (cpu) {
        g_string_append(o,
            "    float rhw = v_zw.y;\n"
            "    bool face = v_face != 0.0;\n"
            "    vec2 pc = v_zw.zw;\n"
            "    float zi = v_zw.x;\n");
    } else {
        g_string_append_printf(o,
            "    bool face = !gl_FrontFacing;\n"
            "    vec2 pc = %s;\n"
            "    float zi = gl_FragCoord.z;\n",
            k->geom == GEOM_POINTS ? "gl_PointCoord" : "vec2(0.0)");
    }
    for (int i = 0; i < k->ninterp; i++) {
        if (!cpu || (k->flat_mask & (1u << i))) {
            g_string_append_printf(o, "    r[%d] = v_i%d;\n", i, i);
        } else {
            g_string_append_printf(o, "    r[%d] = rhw != 0.0 ? v_i%d / rhw "
                                   ": v_i%d;\n", i, i, i);
        }
    }
    if (k->param_gen) {
        /* screen position (sign of x: back facing), point coordinate */
        g_string_append_printf(o,
            "    float fx = gl_FragCoord.x - u_misc.y;\n"
            "    r[%u] = vec4(face ? -fx : fx, gl_FragCoord.y - u_misc.z, "
            "pc);\n", k->param_gen_reg & 63);
    }
    g_string_append_printf(o,
        "    sq_main();\n"
        "    bool alive = !killed;\n"
        "    uint cc = u_rb.z;\n"
        "    if ((cc & 8u) != 0u && !cmpf(cc & 7u, o_color.w, u_misc.x)) {\n"
        "        alive = false;\n"
        "    }\n"
        "    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
        "    float z = %s;\n"
        "    beginInvocationInterlockARB();\n"
        "    if (alive && depth_stencil(x, y, z, face)) {\n"
        "        write_color(x, y, o_color);\n"
        "    }\n"
        "    endInvocationInterlockARB();\n"
        "}\n", ps->exports_depth ? "o_depth" : "zi");
    return o;
}

static uint32_t cube_mask(MsmAdrenoState *s, uint32_t tex_mask)
{
    uint32_t m = 0;

    for (int i = 0; i < 32; i++) {
        if ((tex_mask & (1u << i)) &&
            ((s->regs[A2_SQ_FETCH_0 + i * 6 + 5] >> 9) & 3) == 3) {
            m |= 1u << i;
        }
    }
    return m;
}

static GlProg *gl_program(AdrenoGl *gl, MsmAdrenoState *s,
                          const AdrenoGlDraw *g)
{
    GlProgKey key;
    GlProg *p;
    g_autoptr(GString) vs = NULL;
    g_autoptr(GString) fs = NULL;
    GLuint v, f;
    GLint ok = 0;

    memset(&key, 0, sizeof(key));
    key.ps_hash = g->ps->hash;
    key.ninterp = g->ninterp;
    key.flat_mask = g->flat_mask & ((1u << g->ninterp) - 1);
    key.param_gen = g->param_gen;
    key.param_gen_reg = g->param_gen ? g->param_gen_reg : 0;
    key.cube_mask = cube_mask(s, g->ps->tex_mask);
    if (g->vs) {
        key.geom = (g->prim == A2_PT_POINTLIST ||
                    g->prim == A2_PT_POINTLIST_PSIZE) ? GEOM_POINTS
                                                      : GEOM_TRIS;
        key.vs_hash = g->vs->hash;
        key.vs_cube_mask = cube_mask(s, g->vs->tex_mask);
        key.persp = g->persp;
    }
    p = g_hash_table_lookup(gl->progs, &key);
    if (p) {
        return p->prog ? p : NULL;
    }
    p = g_new0(GlProg, 1);
    p->key = key;
    p->exports_depth = g->ps->exports_depth;
    g_hash_table_insert(gl->progs, &p->key, p);

    vs = g->vs ? vs_source_gpu(&key, g->vs, &p->vinfo) : vs_source_cpu(&key);
    fs = fs_source(&key, g->ps, &p->info);
    if (gl_debug) {
        g_autofree char *vp = g_strdup_printf("adreno-gl-%016" PRIx64
                                              "-%016" PRIx64 "-%d.vs.glsl",
                                              key.vs_hash, key.ps_hash,
                                              key.geom);
        g_autofree char *fp = g_strdup_printf("adreno-gl-%016" PRIx64
                                              "-%016" PRIx64 "-%d.fs.glsl",
                                              key.vs_hash, key.ps_hash,
                                              key.geom);

        g_file_set_contents(vp, vs->str, -1, NULL);
        g_file_set_contents(fp, fs->str, -1, NULL);
    }
    v = compile(GL_VERTEX_SHADER, vs->str);
    f = compile(GL_FRAGMENT_SHADER, fs->str);
    if (!v || !f) {
        return NULL;
    }
    p->prog = glCreateProgram();
    glAttachShader(p->prog, v);
    glAttachShader(p->prog, f);
    glLinkProgram(p->prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = "";

        glGetProgramInfoLog(p->prog, sizeof(log), NULL, log);
        dump_source("fs", fs->str, log);
        glDeleteProgram(p->prog);
        p->prog = 0;
        return NULL;
    }
    adreno_trace(s, "  gl: program %016" PRIx64 " (%d interpolators, %d "
                 "textures)\n", key.ps_hash, key.ninterp, p->info.ntex);
    return p;
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */

static void *scratch(AdrenoGl *gl, size_t n)
{
    if (gl->scratch_size < n) {
        g_free(gl->scratch);
        gl->scratch = g_malloc(n);
        gl->scratch_size = n;
    }
    return gl->scratch;
}

static void tex_free(gpointer data)
{
    GlTex *t = data;

    glDeleteTextures(1, &t->tex);
    g_free(t->pages);
    g_free(t);
}

/*
 * The CPU's writes to textures: QEMU's dirty logging of guest RAM (client
 * VGA, which nothing else here uses) tells which pages changed; the bits
 * are folded into page_kick, so that textures sharing pages all see them.
 */
static uint64_t page_ram(MsmAdrenoState *s, uint32_t va)
{
    hwaddr pa, xlat, len = 0x1000;
    MemoryRegion *mr;
    uint64_t ram = 0;

    if (!adreno_translate(s, va, &pa)) {
        return 0;
    }
    WITH_RCU_READ_LOCK_GUARD() {
        mr = address_space_translate(&address_space_memory, pa, &xlat, &len,
                                     false, MEMTXATTRS_UNSPECIFIED);
        if (memory_region_is_ram(mr) && len >= 0x1000 && !(xlat & 0xfff)) {
            ram = memory_region_get_ram_addr(mr) + xlat + 1;
        } else {
            mr = NULL;
        }
    }
    if (mr && !memory_region_is_logging(mr, DIRTY_MEMORY_VGA)) {
        memory_region_set_log(mr, true, DIRTY_MEMORY_VGA);
    }
    return ram;
}

/* the kick a RAM page last changed in (as far as we have looked) */
static uint32_t page_changed(AdrenoGl *gl, MsmAdrenoState *s, uint64_t ram)
{
    size_t pg = ram >> 12;

    if (pg >= gl->npage_kick) {
        size_t n = MAX(pg + 1, gl->npage_kick * 2);

        gl->page_kick = g_renew(uint32_t, gl->page_kick, n);
        memset(gl->page_kick + gl->npage_kick, 0,
               (n - gl->npage_kick) * sizeof(uint32_t));
        gl->npage_kick = n;
    }
    if (physical_memory_test_and_clear_dirty(ram, 0x1000, DIRTY_MEMORY_VGA,
                                             NULL)) {
        gl->page_kick[pg] = s->kick_epoch;
    }
    return gl->page_kick[pg];
}

/* note the texture's pages (and take their dirty bits) */
static void tex_pages(AdrenoGl *gl, MsmAdrenoState *s, GlTex *t)
{
    uint32_t first = t->lo & ~0xfffu;

    t->npages = t->hi > first ? DIV_ROUND_UP(t->hi - first, 0x1000) : 0;
    t->pages = g_renew(uint64_t, t->pages, t->npages);
    for (uint32_t i = 0; i < t->npages; i++) {
        t->pages[i] = page_ram(s, first + i * 0x1000);
        if (t->pages[i]) {
            page_changed(gl, s, t->pages[i] - 1);
        }
    }
}

/* the texture's pages are where they were, and the CPU left them alone */
static bool tex_pages_unchanged(AdrenoGl *gl, MsmAdrenoState *s, GlTex *t)
{
    uint32_t first = t->lo & ~0xfffu;

    for (uint32_t i = 0; i < t->npages; i++) {
        uint64_t ram = page_ram(s, first + i * 0x1000);

        if (!ram || ram != t->pages[i] ||
            (int32_t)(page_changed(gl, s, ram - 1) - t->stamp.kick) > 0) {
            return false;
        }
    }
    return true;
}

static gboolean tex_stale(gpointer key, gpointer value, gpointer opaque)
{
    GlTex *t = value;
    uint64_t now = *(uint64_t *)opaque;

    return t->used + 4096 < now;
}

/* the host texture of fetch constant idx, (re)decoded when it changed */
static GlTex *gl_texture(AdrenoGl *gl, MsmAdrenoState *s, unsigned idx)
{
    const uint32_t *fc = &s->regs[A2_SQ_FETCH_0 + idx * 6];
    GlTexKey key;
    GlTex *t;
    AdrenoTexImage im;
    uint64_t h;
    uint32_t lo, hi;
    bool fresh = false;

    /* what the texels depend on: not clamping, filters, swizzle, bias */
    key.fc[0] = fc[0] & ~(0x1ffu << 10);
    key.fc[1] = fc[1];
    key.fc[2] = fc[2];
    key.fc[3] = fc[3] & 1;
    key.fc[4] = fc[4] & 0x3fc;
    key.fc[5] = fc[5] & ~3u;
    t = g_hash_table_lookup(gl->texes, &key);
    if (t && !adreno_gpu_wrote(s, &t->stamp, t->lo, t->hi) &&
        (t->stamp.kick == s->kick_epoch || tex_pages_unchanged(gl, s, t))) {
        adreno_trace(s, "  gl: texture %u %08x-%08x unchanged\n", idx, t->lo,
                     t->hi);
        t->stamp.kick = s->kick_epoch;
        t->stamp.writes = s->gpu_writes;
        t->used = gl->ndraw;
        return t->tex ? t : NULL;
    }
    {
        int64_t t0 = s->stats_on ? g_get_monotonic_time() : 0;

        /* resolves to memory not done yet may have written it */
        adreno_resolves_flush(s);
        h = adreno_tex_hash(s, &gl->tlb, idx, &lo, &hi);
        if (s->stats_on) {
            s->st.tex += g_get_monotonic_time() - t0;
            s->st.hashes++;
        }
    }
    if (t && t->hash == h) {
        adreno_trace(s, "  gl: texture %u %08x-%08x same hash\n", idx, lo, hi);
        t->stamp.kick = s->kick_epoch;
        t->stamp.writes = s->gpu_writes;
        tex_pages(gl, s, t);
        t->used = gl->ndraw;
        return t->tex ? t : NULL;
    }
    if (!t) {
        if (g_hash_table_size(gl->texes) >= TEX_CACHE_MAX) {
            g_hash_table_foreach_remove(gl->texes, tex_stale, &gl->ndraw);
            if (g_hash_table_size(gl->texes) >= TEX_CACHE_MAX) {
                g_hash_table_remove_all(gl->texes);
            }
        }
        t = g_new0(GlTex, 1);
        t->key = key;
        g_hash_table_insert(gl->texes, &t->key, t);
    }
    t->hash = h;
    t->lo = lo;
    t->hi = hi;
    t->stamp.kick = s->kick_epoch;
    t->stamp.writes = s->gpu_writes;
    tex_pages(gl, s, t);
    t->used = gl->ndraw;
    if (s->stats_on) {
        s->st.decodes++;
    }
    if (!adreno_tex_image(s, idx, &im)) {
        if (t->tex) {
            glDeleteTextures(1, &t->tex);
            t->tex = 0;
        }
        return NULL;
    }
    im.levels = MIN(im.levels, 32 - clz32(MAX(im.w, im.h)));
    if (!t->tex || t->im.w != im.w || t->im.h != im.h ||
        t->im.levels != im.levels || t->im.layers != im.layers ||
        t->im.is_float != im.is_float) {
        GLenum fmt = im.is_float ? GL_RGBA32F : GL_RGBA8;

        if (t->tex) {
            glDeleteTextures(1, &t->tex);
        }
        if (im.layers > 1) {
            glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &t->tex);
            glTextureStorage3D(t->tex, im.levels, fmt, im.w, im.h,
                               im.layers);
        } else {
            glCreateTextures(GL_TEXTURE_2D, 1, &t->tex);
            glTextureStorage2D(t->tex, im.levels, fmt, im.w, im.h);
        }
        fresh = true;
    }
    t->im = im;
    {
        int64_t t0 = s->stats_on ? g_get_monotonic_time() : 0;
        size_t texel = im.is_float ? 16 : 4;
        void *buf = scratch(gl, (size_t)im.w * im.h * texel);
        GLenum type = im.is_float ? GL_FLOAT : GL_UNSIGNED_BYTE;

        for (int layer = 0; layer < im.layers; layer++) {
            for (int level = 0; level < im.levels; level++) {
                uint32_t w = MAX(im.w >> level, 1u);
                uint32_t hh = MAX(im.h >> level, 1u);

                adreno_tex_decode(s, &gl->tlb, idx, level, layer, buf);
                if (gl_debug && level == 0 && layer == 0 && !im.is_float) {
                    static int n;
                    g_autofree char *path = g_strdup_printf(
                        "adreno-tex-%03d-%08x-%ux%u.rgba", n++, t->lo, w, hh);

                    g_file_set_contents(path, buf, w * hh * 4, NULL);
                }
                if (im.layers > 1) {
                    glTextureSubImage3D(t->tex, level, 0, 0, layer, w, hh, 1,
                                        GL_RGBA, type, buf);
                } else {
                    glTextureSubImage2D(t->tex, level, 0, 0, w, hh, GL_RGBA,
                                        type, buf);
                }
            }
        }
        if (s->stats_on) {
            s->st.tex += g_get_monotonic_time() - t0;
        }
    }
    adreno_trace(s, "  gl: texture %u %ux%u x%d levels %d %s (%s)\n", idx,
                 im.w, im.h, im.layers, im.levels,
                 im.is_float ? "float" : "rgba8", fresh ? "new" : "changed");
    return t;
}

static GLenum wrap_mode(unsigned m)
{
    switch (m) {
    case 0: return GL_REPEAT;
    case 1: return GL_MIRRORED_REPEAT;
    case 3: case 5: case 7: return GL_MIRROR_CLAMP_TO_EDGE;
    case 6: return GL_CLAMP_TO_BORDER;
    }
    return GL_CLAMP_TO_EDGE;
}

static GLuint gl_sampler(AdrenoGl *gl, const AdrenoTexImage *im,
                         unsigned mag, unsigned min, unsigned mip)
{
    GlSamplerKey key;
    GlSampler *sm;

    memset(&key, 0, sizeof(key));
    key.wrap[0] = wrap_mode(im->clamp[0]);
    key.wrap[1] = wrap_mode(im->clamp[1]);
    key.mag = mag == 1 ? GL_LINEAR : GL_NEAREST;
    if (mip == 2 || im->levels <= 1) {
        key.min = min == 1 ? GL_LINEAR : GL_NEAREST;
    } else if (min == 1) {
        key.min = mip == 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
    } else {
        key.min = mip == 1 ? GL_NEAREST_MIPMAP_LINEAR
                           : GL_NEAREST_MIPMAP_NEAREST;
    }
    key.min_lod = im->min_level;
    key.max_lod = im->max_level;
    key.bias = im->lod_bias;
    memcpy(key.border, im->border, sizeof(key.border));
    sm = g_hash_table_lookup(gl->samplers, &key);
    if (!sm) {
        sm = g_new0(GlSampler, 1);
        sm->key = key;
        glCreateSamplers(1, &sm->smp);
        glSamplerParameteri(sm->smp, GL_TEXTURE_WRAP_S, key.wrap[0]);
        glSamplerParameteri(sm->smp, GL_TEXTURE_WRAP_T, key.wrap[1]);
        glSamplerParameteri(sm->smp, GL_TEXTURE_MAG_FILTER, key.mag);
        glSamplerParameteri(sm->smp, GL_TEXTURE_MIN_FILTER, key.min);
        glSamplerParameterf(sm->smp, GL_TEXTURE_MIN_LOD, key.min_lod);
        glSamplerParameterf(sm->smp, GL_TEXTURE_MAX_LOD, key.max_lod);
        glSamplerParameterf(sm->smp, GL_TEXTURE_LOD_BIAS, key.bias);
        glSamplerParameterfv(sm->smp, GL_TEXTURE_BORDER_COLOR, key.border);
        g_hash_table_insert(gl->samplers, &sm->key, sm);
    }
    return sm->smp;
}

/* the texture units of a program's info, from unit base */
static void bind_textures(AdrenoGl *gl, MsmAdrenoState *s,
                          const AdrenoGlslInfo *info, uint32_t cubes,
                          int base, DrawU *du)
{
    for (int i = 0; i < info->ntex; i++) {
        const AdrenoGlslTexUnit *tu = &info->tex[i];
        bool cube = cubes & (1u << tu->const_idx);
        GlTex *t = gl_texture(gl, s, tu->const_idx);
        int u = base + i;
        TexU *x = &du->tex[u];

        if (!t || (t->im.layers > 1) != cube) {
            /* no such texture: (0, 0, 0, 1), as the software sampler */
            glBindTextureUnit(u, cube ? gl->dummy_arr : gl->dummy2d);
            glBindSampler(u, 0);
            x->size[0] = x->size[1] = x->size[2] = x->size[3] = 1.0f;
            x->scale[0] = 1.0f;
            for (int k = 0; k < 4; k++) {
                x->swz[k] = k;
            }
            continue;
        }
        glBindTextureUnit(u, t->tex);
        glBindSampler(u, gl_sampler(gl, &t->im,
                                    tu->mag == 3 ? t->im.mag : tu->mag,
                                    tu->min == 3 ? t->im.min : tu->min,
                                    tu->mip == 3 ? t->im.mip : tu->mip));
        adreno_trace(s, "  gl: unit %d = fetch %u tex %u (%ux%u at %08x)\n",
                     u, tu->const_idx, t->tex, t->im.w, t->im.h, t->lo);
        x->size[0] = t->im.w;
        x->size[1] = t->im.h;
        x->size[2] = 1.0f / t->im.w;
        x->size[3] = 1.0f / t->im.h;
        x->scale[0] = ldexpf(1.0f, t->im.exp_adj);
        x->scale[1] = t->im.lod_bias;
        for (int k = 0; k < 4; k++) {
            x->swz[k] = t->im.swz[k];
        }
    }
}

#define STREAM_SIZE (64u << 20)

static void stream_reset(AdrenoGl *gl, MsmAdrenoState *s)
{
    gl->stream_pos = 0;
    /* the constants must go in again */
    s->const_dirty = true;
}

/* len bytes of the stream buffer at an aligned offset */
static size_t stream_alloc(AdrenoGl *gl, MsmAdrenoState *s, size_t len,
                           size_t align)
{
    size_t pos = ROUND_UP(gl->stream_pos, align);

    if (pos + len > STREAM_SIZE) {
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

        /* full: wait for the host GPU to be done with it all */
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
        glDeleteSync(fence);
        stream_reset(gl, s);
        gl->stream_wrapped = true;
        pos = 0;
        assert(len <= STREAM_SIZE);
    }
    gl->stream_pos = pos + len;
    return pos;
}

/* ------------------------------------------------------------------ */
/* GMEM                                                                */
/* ------------------------------------------------------------------ */

void adreno_gl_gmem_to_cpu(MsmAdrenoState *s)
{
    AdrenoGl *gl = s->gl;

    if (gl && gl->gpu_newer) {
        int64_t t0 = s->stats_on ? g_get_monotonic_time() : 0;
        GLsync fence;

        /* through a buffer in host memory: GMEM stays in video memory */
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glCopyNamedBufferSubData(gl->gmem, gl->gmem_stage, 0, 0,
                                 ADRENO_GMEM_SIZE);
        fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
        glDeleteSync(fence);
        stream_reset(gl, s);
        memcpy(s->gmem, gl->gmem_map, ADRENO_GMEM_SIZE);
        gl->gpu_newer = false;
        if (s->stats_on) {
            s->st.sync += g_get_monotonic_time() - t0;
        }
    }
}

static void gmem_upload(AdrenoGl *gl, MsmAdrenoState *s)
{
    if (gl->cpu_newer) {
        /*
         * The CPU changes GMEM only after copying it back, which waited
         * for the staging buffer to be idle.
         */
        memcpy(gl->gmem_map, s->gmem, ADRENO_GMEM_SIZE);
        glCopyNamedBufferSubData(gl->gmem_stage, gl->gmem, 0, 0,
                                 ADRENO_GMEM_SIZE);
        gl->cpu_newer = false;
    }
}

bool adreno_gl_gmem_snapshot(MsmAdrenoState *s, int *slot)
{
    AdrenoGl *gl = s->gl;

    if (!gl || !qemu_thread_is_self(&gl->owner) ||
        gl->snap_used == ARRAY_SIZE(gl->snap)) {
        return false;
    }
    if (gl->snap_used == gl->nsnap) {
        GLuint b;

        glCreateBuffers(1, &b);
        glNamedBufferStorage(b, ADRENO_GMEM_SIZE, NULL,
                             GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT |
                             GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT);
        gl->snap_map[gl->nsnap] = glMapNamedBufferRange(
            b, 0, ADRENO_GMEM_SIZE,
            GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        gl->snap[gl->nsnap++] = b;
    }
    gmem_upload(gl, s);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glCopyNamedBufferSubData(gl->gmem, gl->snap[gl->snap_used], 0, 0,
                             ADRENO_GMEM_SIZE);
    /* the bin is done: the host GPU starts on it while we go on */
    glFlush();
    *slot = gl->snap_used++;
    return true;
}

void adreno_gl_snapshots_wait(MsmAdrenoState *s)
{
    AdrenoGl *gl = s->gl;
    int64_t t0 = s->stats_on ? g_get_monotonic_time() : 0;
    GLsync fence;

    if (gl->tq_started) {
        glQueryCounter(gl->tq[1], GL_TIMESTAMP);
    }
    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
    glDeleteSync(fence);
    stream_reset(gl, s);
    if (s->stats_on) {
        s->st.sync += g_get_monotonic_time() - t0;
    }
    if (gl->tq_started) {
        GLuint64 a = 0, b = 0;

        glGetQueryObjectui64v(gl->tq[0], GL_QUERY_RESULT, &a);
        glGetQueryObjectui64v(gl->tq[1], GL_QUERY_RESULT, &b);
        s->st.gpu += (b - a) / 1000;
        gl->tq_started = false;
    }
}

uint8_t *adreno_gl_snapshot_ptr(MsmAdrenoState *s, int slot)
{
    return s->gl->snap_map[slot];
}

void adreno_gl_snapshots_release(MsmAdrenoState *s)
{
    s->gl->snap_used = 0;
}

void adreno_gl_gmem_from_cpu(MsmAdrenoState *s)
{
    AdrenoGl *gl = s->gl;

    if (gl) {
        assert(!gl->gpu_newer);
        gl->cpu_newer = true;
    }
}

/* ------------------------------------------------------------------ */
/* Draws                                                               */
/* ------------------------------------------------------------------ */

/*
 * The vertex data a host vertex shader fetches: for each fetch constant
 * the program uses, the bytes the indices of the draw reach, into vd[].
 */
static void upload_vertices(AdrenoGl *gl, MsmAdrenoState *s, const GlProg *p,
                            const AdrenoGlDraw *g, DrawU *du,
                            size_t *idx_ofs)
{
    const AdrenoGlslInfo *vi = &p->vinfo;
    size_t total = 16, base, at;
    int64_t lo[ADRENO_GL_MAX_VBUFS], hi[ADRENO_GL_MAX_VBUFS];

    for (int i = 0; i < vi->nvtx; i++) {
        uint32_t stride = vi->vtx_stride[i];
        uint32_t unit = ADRENO_VFETCH_UNIT(s->regs[A2_SQ_FETCH_0 +
                                                  vi->vtx[i] * 2 + 1]);

        lo[i] = ((int64_t)g->min_idx * stride + vi->vtx_lo[i]) * unit;
        hi[i] = ((int64_t)g->max_idx * stride + vi->vtx_hi[i]) * unit;
        hi[i] = MIN(hi[i], lo[i] + (16 << 20));
        total += ROUND_UP(hi[i] - lo[i], 16);
    }
    /* the indices, then the vertex data (vd[] of SSBO binding 2) */
    *idx_ofs = stream_alloc(gl, s, g->nidx * 4, 4);
    memcpy(gl->stream_map + *idx_ofs, g->idx, g->nidx * 4);
    base = stream_alloc(gl, s, total, MAX(gl->ssbo_align, 16));
    at = 0;
    for (int i = 0; i < vi->nvtx; i++) {
        unsigned c = vi->vtx[i];
        uint32_t fc0 = s->regs[A2_SQ_FETCH_0 + c * 2];
        uint32_t fc1 = s->regs[A2_SQ_FETCH_0 + c * 2 + 1];
        uint32_t len = hi[i] - lo[i];

        adreno_tlb_read(s, &gl->tlb, (fc0 & ~3u) + (uint32_t)lo[i],
                        gl->stream_map + base + at, len);
        du->vb[i][0] = at / 4;
        du->vb[i][1] = lo[i];
        du->vb[i][2] = (fc1 & 3) | (ADRENO_VFETCH_UNIT(fc1) == 4 ? 4 : 0);
        du->vb[i][3] = len;
        at += ROUND_UP(len, 16);
    }
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, gl->stream, base, total);
}

/* rasterizer state for the host GPU's own primitive assembly */
static void raster_state(AdrenoGl *gl, MsmAdrenoState *s, int geom,
                         const AdrenoGlDraw *g)
{
    uint32_t sc = s->regs[A2_PA_SU_SC_MODE_CNTL];

    if (geom != gl->cur_geom) {
        for (int i = 0; i < 7; i++) {
            if (geom == GEOM_CPU) {
                glDisable(GL_CLIP_DISTANCE0 + i);
            } else {
                glEnable(GL_CLIP_DISTANCE0 + i);
            }
        }
        gl->cur_geom = geom;
    }
    if (geom != GEOM_TRIS || !(sc & 3)) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace((sc & 3) == 3 ? GL_FRONT_AND_BACK :
                   (sc & 1) ? GL_FRONT : GL_BACK);
    }
    /*
     * Window y is the guest's (down), so what the guest sees clockwise
     * the host sees counter-clockwise.  FACE: front is clockwise.
     */
    glFrontFace((sc & (1 << 2)) ? GL_CCW : GL_CW);
    glProvokingVertex(g->provoking_last ? GL_LAST_VERTEX_CONVENTION
                                        : GL_FIRST_VERTEX_CONVENTION);
}

static float fx124(uint32_t v)
{
    return (v & 0xffff) / 16.0f;
}

void adreno_gl_draw(MsmAdrenoState *s, const AdrenoGlDraw *g)
{
    AdrenoGl *gl = s->gl;
    const uint32_t *r = s->regs;
    const AdrenoColorFmt *cf;
    GlProg *p;
    DrawU du;
    uint32_t cc = r[A2_RB_COLORCONTROL], bc = r[A2_RB_BLEND_CONTROL];
    size_t idx_ofs = 0;
    int geom;

    if ((g->vs ? !g->nidx : !g->nverts) ||
        g->clip_x0 >= g->clip_x1 || g->clip_y0 >= g->clip_y1) {
        return;
    }
    gl->ndraw++;
    adreno_tlb_flush(&gl->tlb);
    if (s->stats_on && !gl->tq_started) {
        if (!gl->tq[0]) {
            glGenQueries(2, gl->tq);
        }
        glQueryCounter(gl->tq[0], GL_TIMESTAMP);
        gl->tq_started = true;
    }
    p = gl_program(gl, s, g);
    if (!p) {
        return;
    }
    geom = p->key.geom;
    gmem_upload(gl, s);

    /*
     * Textures first: checking one may do the pending resolves, which
     * waits for the host GPU and starts the stream buffer over.
     */
    memset(&du, 0, sizeof(du));
    bind_textures(gl, s, &p->info, p->key.cube_mask, 0, &du);
    if (geom != GEOM_CPU) {
        bind_textures(gl, s, &p->vinfo, p->key.vs_cube_mask,
                      ADRENO_GL_MAX_UNITS, &du);
    }
    memcpy(du.bools, &r[A2_SQ_CF_BOOLEANS], sizeof(du.bools));
    memcpy(du.loops, &r[A2_SQ_CF_LOOP], sizeof(du.loops));
    du.cb[0] = r[A2_SQ_PS_CONST] & 0x1ff;
    du.cb[1] = r[A2_SQ_VS_CONST] & 0x1ff;
    cf = adreno_color_fmt(g->color_fmt);
    du.csurf[0] = g->color_off;
    du.csurf[1] = g->pitch;
    du.csurf[2] = cf->bpp;
    du.csurf[3] = g->sx | g->sy << 4;
    for (int k = 0; k < 4; k++) {
        du.cfmt[k] = cf->bits[k];
    }
    du.cinfo[0] = cf->kind;
    du.cinfo[1] = g->color_swap;
    du.cinfo[2] = g->color_write;
    du.cinfo[3] = r[A2_RB_COLOR_MASK] & 0xf;
    du.dsurf[0] = g->depth_off;
    du.dsurf[1] = g->depth_24 ? 4 : 2;
    du.rb[0] = r[A2_RB_DEPTHCONTROL];
    du.rb[1] = bc;
    du.rb[2] = cc;
    du.rb[3] = r[A2_RB_STENCILREFMASK];
    du.rb2[0] = r[A2_RB_STENCILREFMASK_BF];
    du.rb2[1] = !(cc & (1 << 5)) && bc != 0x00010001;
    du.blendc[0] = (r[A2_RB_BLEND_RED] & 0xff) / 255.0f;
    du.blendc[1] = (r[A2_RB_BLEND_GREEN] & 0xff) / 255.0f;
    du.blendc[2] = (r[A2_RB_BLEND_BLUE] & 0xff) / 255.0f;
    du.blendc[3] = (r[A2_RB_BLEND_ALPHA] & 0xff) / 255.0f;
    du.misc[0] = adreno_f(r[A2_RB_ALPHA_REF]);
    du.misc[1] = g->win_x;
    du.misc[2] = g->win_y;
    du.misc[3] = g->pix_ofs;
    if (geom != GEOM_CPU) {
        uint32_t mm = r[A2_PA_SU_POINT_MINMAX];

        du.vp0[0] = adreno_f(r[A2_PA_CL_VPORT_XSCALE]);
        du.vp0[1] = adreno_f(r[A2_PA_CL_VPORT_XOFFSET]);
        du.vp0[2] = adreno_f(r[A2_PA_CL_VPORT_YSCALE]);
        du.vp0[3] = adreno_f(r[A2_PA_CL_VPORT_YOFFSET]);
        du.vp1[0] = adreno_f(r[A2_PA_CL_VPORT_ZSCALE]);
        du.vp1[1] = adreno_f(r[A2_PA_CL_VPORT_ZOFFSET]);
        du.vp1[2] = fx124(r[A2_PA_SU_POINT_SIZE] >> 16);
        du.vp2[0] = g->win_offset_vtx ? g->win_x : 0;
        du.vp2[1] = g->win_offset_vtx ? g->win_y : 0;
        du.vp2[2] = g->pix_ofs;
        du.vte[0] = r[A2_PA_CL_VTE_CNTL];
        du.vte[1] = r[A2_PA_CL_CLIP_CNTL];
        du.psz[0] = fx124(mm);
        du.psz[1] = fx124(mm >> 16);
        du.psz[2] = g->prim == A2_PT_POINTLIST_PSIZE || g->vs->exports_psize;
    }

    /* what the draw reads, into the stream buffer: again if it filled up */
    do {
        int64_t t0 = s->stats_on ? g_get_monotonic_time() : 0;
        size_t u;

        gl->stream_wrapped = false;
        if (s->const_dirty) {
            size_t c = stream_alloc(gl, s, 512 * 16, gl->ubo_align);

            memcpy(gl->stream_map + c, &r[A2_SQ_CONSTANT_0], 512 * 16);
            glBindBufferRange(GL_UNIFORM_BUFFER, 0, gl->stream, c, 512 * 16);
            s->const_dirty = false;
        }
        if (geom == GEOM_CPU) {
            size_t vbytes = (size_t)g->nverts *
                            ADRENO_GL_VTX_VEC4(g->ninterp) * 16;
            size_t v = stream_alloc(gl, s, vbytes, MAX(gl->ssbo_align, 16));

            memcpy(gl->stream_map + v, g->verts, vbytes);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, gl->stream, v,
                              vbytes);
        } else {
            upload_vertices(gl, s, p, g, &du, &idx_ofs);
        }
        u = stream_alloc(gl, s, sizeof(du), gl->ubo_align);
        memcpy(gl->stream_map + u, &du, sizeof(du));
        glBindBufferRange(GL_UNIFORM_BUFFER, 1, gl->stream, u, sizeof(du));
        if (s->stats_on) {
            s->st.upload += g_get_monotonic_time() - t0;
        }
    } while (gl->stream_wrapped);

    /*
     * GMEM is only accessed in the interlocked critical sections, which
     * run in primitive order, also across draws that use the same program
     * (NVIDIA; a draw of another program may overtake the ones before).
     * A barrier between every two draws costs the host GPU a wait for
     * idle each, so there is one where the program changes;
     * ADRENO_GL_BARRIER=1: between all draws.
     */
    if (gl->barriers == 1 || p != gl->last_prog) {
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    raster_state(gl, s, geom, g);
    glUseProgram(p->prog);
    glScissor(g->clip_x0, g->clip_y0, g->clip_x1 - g->clip_x0,
              g->clip_y1 - g->clip_y0);
    adreno_trace(s, "  gl: draw geom %d program %016" PRIx64 "/%016" PRIx64
                 " indices %u-%u\n", geom, p->key.vs_hash, p->key.ps_hash,
                 g->min_idx, g->max_idx);
    if (geom == GEOM_CPU) {
        glDrawArrays(GL_TRIANGLES, 0, g->nverts);
    } else {
        GLenum mode = geom == GEOM_POINTS ? GL_POINTS :
                      g->prim == A2_PT_TRISTRIP ? GL_TRIANGLE_STRIP :
                      g->prim == A2_PT_TRIFAN ? GL_TRIANGLE_FAN : GL_TRIANGLES;

        glDrawElements(mode, g->nidx, GL_UNSIGNED_INT,
                       (const void *)(uintptr_t)idx_ofs);
    }
    gl->last_prog = p;
    gl->gpu_newer = true;
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static bool gl_init(MsmAdrenoState *s)
{
    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    static const uint8_t black[4] = { 0, 0, 0, 255 };
    AdrenoGl *gl = g_new0(AdrenoGl, 1);
    EGLint major, minor;

    if (epoxy_has_egl_extension(EGL_NO_DISPLAY,
                                "EGL_MESA_platform_surfaceless")) {
        gl->dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                           EGL_DEFAULT_DISPLAY, NULL);
    }
    if (gl->dpy == EGL_NO_DISPLAY) {
        gl->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (gl->dpy == EGL_NO_DISPLAY || !eglInitialize(gl->dpy, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API)) {
        error_report("msm-adreno: no EGL display for the host GPU backend");
        g_free(gl);
        return false;
    }
    gl->ctx = eglCreateContext(gl->dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                               ctx_attr);
    if (gl->ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(gl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->ctx)) {
        error_report("msm-adreno: no OpenGL 4.5 context (EGL error 0x%x)",
                     eglGetError());
        g_free(gl);
        return false;
    }
    if (epoxy_gl_version() < 45 ||
        (!epoxy_has_gl_extension("GL_ARB_fragment_shader_interlock") &&
         !epoxy_has_gl_extension("GL_NV_fragment_shader_interlock"))) {
        error_report("msm-adreno: %s lacks OpenGL 4.5 with fragment shader "
                     "interlock; rendering in software",
                     (const char *)glGetString(GL_RENDERER));
        eglMakeCurrent(gl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(gl->dpy, gl->ctx);
        g_free(gl);
        return false;
    }
    qemu_thread_get_self(&gl->owner);
    gl_debug = getenv("ADRENO_GL_DEBUG") != NULL;
    gl->barriers = getenv("ADRENO_GL_BARRIER") ?
                   atoi(getenv("ADRENO_GL_BARRIER")) : 0;
    if (gl_debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debug_cb, NULL);
    }
    info_report("msm-adreno: rendering on %s, OpenGL %s",
                (const char *)glGetString(GL_RENDERER),
                (const char *)glGetString(GL_VERSION));

    gl->progs = g_hash_table_new(prog_hash, prog_equal);
    gl->texes = g_hash_table_new_full(tex_hash, tex_equal, NULL, tex_free);
    gl->samplers = g_hash_table_new(smp_hash, smp_equal);

    /* no attachments: the shaders write GMEM */
    glCreateFramebuffers(1, &gl->fbo);
    glNamedFramebufferParameteri(gl->fbo, GL_FRAMEBUFFER_DEFAULT_WIDTH,
                                 FB_SIZE);
    glNamedFramebufferParameteri(gl->fbo, GL_FRAMEBUFFER_DEFAULT_HEIGHT,
                                 FB_SIZE);
    glBindFramebuffer(GL_FRAMEBUFFER, gl->fbo);
    glViewport(0, 0, FB_SIZE, FB_SIZE);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glCreateVertexArrays(1, &gl->vao);
    glBindVertexArray(gl->vao);

    glCreateBuffers(1, &gl->gmem);
    glNamedBufferStorage(gl->gmem, ADRENO_GMEM_SIZE, s->gmem,
                         GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gl->gmem);
    glCreateBuffers(1, &gl->gmem_stage);
    glNamedBufferStorage(gl->gmem_stage, ADRENO_GMEM_SIZE, NULL,
                         GL_MAP_READ_BIT | GL_MAP_WRITE_BIT |
                         GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT |
                         GL_CLIENT_STORAGE_BIT);
    gl->gmem_map = glMapNamedBufferRange(gl->gmem_stage, 0, ADRENO_GMEM_SIZE,
                                         GL_MAP_READ_BIT | GL_MAP_WRITE_BIT |
                                         GL_MAP_PERSISTENT_BIT |
                                         GL_MAP_COHERENT_BIT);
    glCreateBuffers(1, &gl->stream);
    glNamedBufferStorage(gl->stream, STREAM_SIZE, NULL,
                         GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                         GL_MAP_COHERENT_BIT);
    gl->stream_map = glMapNamedBufferRange(gl->stream, 0, STREAM_SIZE,
                                           GL_MAP_WRITE_BIT |
                                           GL_MAP_PERSISTENT_BIT |
                                           GL_MAP_COHERENT_BIT);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &gl->ubo_align);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &gl->ssbo_align);
    glVertexArrayElementBuffer(gl->vao, gl->stream);
    gl->cur_geom = GEOM_CPU;
    /* the guest's depth range; points' (0, 0) at the guest's top */
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, GL_LOWER_LEFT);

    glCreateTextures(GL_TEXTURE_2D, 1, &gl->dummy2d);
    glTextureStorage2D(gl->dummy2d, 1, GL_RGBA8, 1, 1);
    glTextureSubImage2D(gl->dummy2d, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                        black);
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &gl->dummy_arr);
    glTextureStorage3D(gl->dummy_arr, 1, GL_RGBA8, 1, 1, 6);
    for (int i = 0; i < 6; i++) {
        glTextureSubImage3D(gl->dummy_arr, 0, 0, 0, i, 1, 1, 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, black);
    }
    s->const_dirty = true;
    s->gl = gl;
    return true;
}

bool adreno_gl_available(MsmAdrenoState *s)
{
    AdrenoGl *gl = s->gl;

    if (!gl) {
        if (s->gl_failed || !s->backend || strcmp(s->backend, "gl")) {
            return false;
        }
        if (!gl_init(s)) {
            s->gl_failed = true;
            return false;
        }
        gl = s->gl;
    }
    /* the context lives on the thread that made it */
    return qemu_thread_is_self(&gl->owner);
}

#else

bool adreno_gl_available(MsmAdrenoState *s)
{
    return false;
}

void adreno_gl_draw(MsmAdrenoState *s, const AdrenoGlDraw *g)
{
}

void adreno_gl_gmem_to_cpu(MsmAdrenoState *s)
{
}

void adreno_gl_gmem_from_cpu(MsmAdrenoState *s)
{
}

bool adreno_gl_gmem_snapshot(MsmAdrenoState *s, int *slot)
{
    return false;
}

void adreno_gl_snapshots_wait(MsmAdrenoState *s)
{
}

uint8_t *adreno_gl_snapshot_ptr(MsmAdrenoState *s, int slot)
{
    return NULL;
}

void adreno_gl_snapshots_release(MsmAdrenoState *s)
{
}

#endif
