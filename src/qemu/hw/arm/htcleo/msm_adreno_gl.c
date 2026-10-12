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
#include "msm_adreno_gl.h"

#ifdef CONFIG_OPENGL
#include <epoxy/gl.h>
#include <epoxy/egl.h>

/* an empty framebuffer covering every window coordinate a draw can use */
#define FB_SIZE         16384
#define TEX_CACHE_MAX   768

typedef struct GlProgKey {
    uint64_t ps_hash;
    uint32_t flat_mask;
    uint32_t cube_mask;
    uint8_t ninterp, param_gen, param_gen_reg, pad;
} GlProgKey;

typedef struct GlProg {
    GlProgKey key;
    GLuint prog;
    bool exports_depth;
    AdrenoGlslInfo info;
} GlProg;

typedef struct GlTexKey {
    uint32_t fc[6];
} GlTexKey;

typedef struct GlTex {
    GlTexKey key;
    GLuint tex;
    uint64_t hash;
    uint32_t epoch;
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
    TexU tex[ADRENO_GL_MAX_UNITS];
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
} DrawU;

typedef struct AdrenoGl {
    EGLDisplay dpy;
    EGLContext ctx;
    QemuThread owner;
    GLuint fbo, vao;
    GLuint gmem, consts, draw_ubo, vbo;
    GLsizeiptr vbo_size;
    GLuint dummy2d, dummy_arr;
    GHashTable *progs, *texes, *samplers;
    bool gpu_newer, cpu_newer;
    uint64_t ndraw;
    AdrenoTlb tlb;
    uint8_t *scratch;
    size_t scratch_size;
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
    "    TexU u_tex[16];\n"
    "    ivec4 u_cb;\n"
    "    ivec4 u_csurf;\n"
    "    ivec4 u_cfmt;\n"
    "    ivec4 u_cinfo;\n"
    "    ivec4 u_dsurf;\n"
    "    uvec4 u_rb;\n"
    "    uvec4 u_rb2;\n"
    "    vec4 u_blendc;\n"
    "    vec4 u_misc;\n"
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
        g_string_append_printf(o, "layout(location = %d) %s %s vec4 v_i%d;\n",
                               2 + i, (k->flat_mask & (1u << i)) ?
                               "flat" : "noperspective", dir, i);
    }
}

static GString *vs_source(const GlProgKey *k)
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

static GString *fs_source(const GlProgKey *k, const AdrenoProgram *ps,
                          AdrenoGlslInfo *info)
{
    GString *o = g_string_new(
        "#version 450 core\n"
        "#extension GL_ARB_fragment_shader_interlock : require\n"
        "layout(pixel_interlock_ordered) in;\n");

    g_string_append(o, glsl_common);
    g_string_append(o, "#define CONST(i) u_c[((i) + u_cb.x) & 511]\n");
    adreno_glsl_translate(ps, true, k->cube_mask, o, info);
    g_string_append(o, glsl_rop);
    g_string_append(o,
        "layout(location = 0) noperspective in vec4 v_zw;\n"
        "layout(location = 1) flat in float v_face;\n");
    interp_decls(o, k, "in");
    g_string_append(o,
        "void main() {\n"
        "    for (int i = 0; i < 64; i++) {\n"
        "        r[i] = vec4(0.0);\n"
        "    }\n"
        "    float rhw = v_zw.y;\n");
    for (int i = 0; i < k->ninterp; i++) {
        if (k->flat_mask & (1u << i)) {
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
            "    r[%u] = vec4(v_face != 0.0 ? -fx : fx, gl_FragCoord.y - "
            "u_misc.z, v_zw.z, v_zw.w);\n", k->param_gen_reg & 63);
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
        "    if (alive && depth_stencil(x, y, z, v_face != 0.0)) {\n"
        "        write_color(x, y, o_color);\n"
        "    }\n"
        "    endInvocationInterlockARB();\n"
        "}\n", ps->exports_depth ? "o_depth" : "v_zw.x");
    return o;
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
    for (int i = 0; i < 32; i++) {
        if ((g->ps->tex_mask & (1u << i)) &&
            ((s->regs[A2_SQ_FETCH_0 + i * 6 + 5] >> 9) & 3) == 3) {
            key.cube_mask |= 1u << i;
        }
    }
    p = g_hash_table_lookup(gl->progs, &key);
    if (p) {
        return p->prog ? p : NULL;
    }
    p = g_new0(GlProg, 1);
    p->key = key;
    p->exports_depth = g->ps->exports_depth;
    g_hash_table_insert(gl->progs, &p->key, p);

    vs = vs_source(&key);
    fs = fs_source(&key, g->ps, &p->info);
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
    g_free(t);
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
    bool fresh = false;

    /* what the texels depend on: not clamping, filters, swizzle, bias */
    key.fc[0] = fc[0] & ~(0x1ffu << 10);
    key.fc[1] = fc[1];
    key.fc[2] = fc[2];
    key.fc[3] = fc[3] & 1;
    key.fc[4] = fc[4] & 0x3fc;
    key.fc[5] = fc[5] & ~3u;
    t = g_hash_table_lookup(gl->texes, &key);
    if (t && t->epoch == s->mem_epoch) {
        t->used = gl->ndraw;
        return t->tex ? t : NULL;
    }
    h = adreno_tex_hash(s, &gl->tlb, idx);
    if (t && t->hash == h) {
        t->epoch = s->mem_epoch;
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
    t->epoch = s->mem_epoch;
    t->used = gl->ndraw;
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
        size_t texel = im.is_float ? 16 : 4;
        void *buf = scratch(gl, (size_t)im.w * im.h * texel);
        GLenum type = im.is_float ? GL_FLOAT : GL_UNSIGNED_BYTE;

        for (int layer = 0; layer < im.layers; layer++) {
            for (int level = 0; level < im.levels; level++) {
                uint32_t w = MAX(im.w >> level, 1u);
                uint32_t hh = MAX(im.h >> level, 1u);

                adreno_tex_decode(s, &gl->tlb, idx, level, layer, buf);
                if (im.layers > 1) {
                    glTextureSubImage3D(t->tex, level, 0, 0, layer, w, hh, 1,
                                        GL_RGBA, type, buf);
                } else {
                    glTextureSubImage2D(t->tex, level, 0, 0, w, hh, GL_RGBA,
                                        type, buf);
                }
            }
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

static void bind_textures(AdrenoGl *gl, MsmAdrenoState *s, const GlProg *p,
                          DrawU *du)
{
    for (int u = 0; u < p->info.ntex; u++) {
        const AdrenoGlslTexUnit *tu = &p->info.tex[u];
        bool cube = p->key.cube_mask & (1u << tu->const_idx);
        GlTex *t = gl_texture(gl, s, tu->const_idx);
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

/* ------------------------------------------------------------------ */
/* GMEM                                                                */
/* ------------------------------------------------------------------ */

void adreno_gl_gmem_to_cpu(MsmAdrenoState *s)
{
    AdrenoGl *gl = s->gl;

    if (gl && gl->gpu_newer) {
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glGetNamedBufferSubData(gl->gmem, 0, ADRENO_GMEM_SIZE, s->gmem);
        gl->gpu_newer = false;
    }
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

void adreno_gl_draw(MsmAdrenoState *s, const AdrenoGlDraw *g)
{
    AdrenoGl *gl = s->gl;
    const uint32_t *r = s->regs;
    const AdrenoColorFmt *cf;
    GlProg *p;
    DrawU du;
    GLsizeiptr vbytes;
    uint32_t cc = r[A2_RB_COLORCONTROL], bc = r[A2_RB_BLEND_CONTROL];

    if (!g->nverts || g->clip_x0 >= g->clip_x1 || g->clip_y0 >= g->clip_y1) {
        return;
    }
    gl->ndraw++;
    adreno_tlb_flush(&gl->tlb);
    p = gl_program(gl, s, g);
    if (!p) {
        return;
    }
    if (gl->cpu_newer) {
        glNamedBufferSubData(gl->gmem, 0, ADRENO_GMEM_SIZE, s->gmem);
        gl->cpu_newer = false;
    }
    if (s->const_dirty) {
        glNamedBufferSubData(gl->consts, 0, 512 * 16,
                             &r[A2_SQ_CONSTANT_0]);
        s->const_dirty = false;
    }

    memset(&du, 0, sizeof(du));
    memcpy(du.bools, &r[A2_SQ_CF_BOOLEANS], sizeof(du.bools));
    memcpy(du.loops, &r[A2_SQ_CF_LOOP], sizeof(du.loops));
    bind_textures(gl, s, p, &du);
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
    glNamedBufferSubData(gl->draw_ubo, 0, sizeof(du), &du);

    vbytes = (GLsizeiptr)g->nverts * ADRENO_GL_VTX_VEC4(g->ninterp) * 16;
    if (vbytes > gl->vbo_size) {
        gl->vbo_size = MAX(vbytes, gl->vbo_size * 2);
    }
    /* orphan the last draw's vertices */
    glNamedBufferData(gl->vbo, gl->vbo_size, NULL, GL_STREAM_DRAW);
    glNamedBufferSubData(gl->vbo, 0, vbytes, g->verts);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gl->vbo);

    glUseProgram(p->prog);
    glScissor(g->clip_x0, g->clip_y0, g->clip_x1 - g->clip_x0,
              g->clip_y1 - g->clip_y0);
    glDrawArrays(GL_TRIANGLES, 0, g->nverts);
    /* the next draw reads what this one wrote to GMEM */
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
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
    glCreateBuffers(1, &gl->consts);
    glNamedBufferStorage(gl->consts, 512 * 16, &s->regs[A2_SQ_CONSTANT_0],
                         GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, gl->consts);
    glCreateBuffers(1, &gl->draw_ubo);
    glNamedBufferStorage(gl->draw_ubo, sizeof(DrawU), NULL,
                         GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, gl->draw_ubo);
    glCreateBuffers(1, &gl->vbo);
    gl->vbo_size = 64 * 1024;

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
    s->const_dirty = false;
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

#endif
