/*
 * Adreno 200 shader sequencer: a2xx microcode to GLSL
 *
 * The host GPU backend (msm_adreno_gl.c) runs the guest's shaders as GLSL,
 * the way Xenia translates Xenos microcode for its host backends.  The
 * output mirrors the interpreter (msm_adreno_sq.c) operation by operation,
 * which is the reference: one GLSL invocation is one lane, so lane masks
 * become plain control flow.
 *
 * A program becomes "void sq_main()" over globals:
 *   vec4 r[64]                     temporaries
 *   bool p0; int a0; float ps      predicate, address register, the
 *                                  scalar unit's previous result
 *   bool killed                    a kill instruction fired
 *   vec4 o_color; float o_depth    pixel shader exports
 *   vec4 o_pos; float o_psize; vec4 o_interp[16]
 *                                  vertex shader exports
 * and the uniforms the caller declares: CONST(i) (the 512 ALU constants,
 * relocated by SQ_VS_CONST/SQ_PS_CONST), bconst(i), lconst(i), and per
 * texture unit u_tex[u] (size, exponent scale, swizzle).  Vertex fetches
 * call vfetch_word(slot, byte offset) on the guest's vertex data.
 *
 * Straight-line programs (no loops, calls or jumps) are emitted in order;
 * the others as a switch over the control flow index in a loop.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "msm_adreno_sq.h"
#include "msm_adreno_gl.h"

static const char comp[] = "xyzw";

/* Helpers every translated program uses: the interpreter's arithmetic */
static const char glsl_helpers[] =
    "#define FLT_MAX 3.402823466e+38\n"
    "float sat(float f) { return f > 1.0 ? 1.0 : (f >= 0.0 ? f : 0.0); }\n"
    "vec4 sat4(vec4 v) { return vec4(sat(v.x), sat(v.y), sat(v.z), "
    "sat(v.w)); }\n"
    "float mul0(float a, float b) { return (a != 0.0 && b != 0.0) ? a * b "
    ": 0.0; }\n"
    "vec4 mul0v(vec4 a, vec4 b) { return vec4(mul0(a.x, b.x), "
    "mul0(a.y, b.y), mul0(a.z, b.z), mul0(a.w, b.w)); }\n"
    "vec4 vmax(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }\n"
    "vec4 vmin(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }\n"
    "float smax(float a, float b) { return a >= b ? a : b; }\n"
    "float cmax(float a, float b) { return a > b ? a : b; }\n"
    "vec4 cube(vec4 s) {\n"
    "    float x = s.z, y = s.w, z = s.x;\n"
    "    float ax = abs(x), ay = abs(y), az = abs(z);\n"
    "    if (az >= ax && az >= ay) {\n"
    "        return vec4(-y, z < 0.0 ? -x : x, 2.0 * z, z < 0.0 ? 5.0 : 4.0);\n"
    "    } else if (ay >= ax) {\n"
    "        return vec4(y < 0.0 ? -z : z, x, 2.0 * y, y < 0.0 ? 3.0 : 2.0);\n"
    "    }\n"
    "    return vec4(-y, x < 0.0 ? z : -z, 2.0 * x, x < 0.0 ? 1.0 : 0.0);\n"
    "}\n"
    "vec4 tswz(vec4 v, ivec4 s) {\n"
    "    vec4 o;\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "        o[k] = s[k] < 4 ? v[s[k]] : (s[k] == 5 ? 1.0 : 0.0);\n"
    "    }\n"
    "    return o;\n"
    "}\n"
    "bool bconst(int a) { return ((u_bools[a >> 7][(a >> 5) & 3] >> "
    "uint(a & 31)) & 1u) != 0u; }\n"
    "uint lconst(int i) { return u_loops[i >> 2][i & 3]; }\n"
    "vec4 r[64];\n"
    "bool p0 = false, killed = false;\n"
    "int a0 = 0;\n"
    "float ps = 0.0;\n"
    "vec4 o_color = vec4(0.0);\n"
    "float o_depth = 0.0;\n"
    "vec4 o_pos = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "float o_psize = 0.0;\n"
    "vec4 o_interp[16];\n";

typedef struct Tr {
    GString *o;
    const AdrenoProgram *p;
    AdrenoGlslInfo *info;
    uint32_t cube_mask;
    bool fragment;
    bool linear;            /* straight-line: aL is 0 */
} Tr;

static void swz4(GString *o, const uint8_t swz[4])
{
    if (swz[0] == 0 && swz[1] == 1 && swz[2] == 2 && swz[3] == 3) {
        return;
    }
    g_string_append_c(o, '.');
    for (int k = 0; k < 4; k++) {
        g_string_append_c(o, comp[swz[k]]);
    }
}

static void mask_str(GString *o, unsigned m)
{
    for (int k = 0; k < 4; k++) {
        if (m & (1 << k)) {
            g_string_append_c(o, comp[k]);
        }
    }
}

/* the temporary reg (+ aL) */
static void temp_ref(Tr *t, unsigned reg, bool rel)
{
    if (rel && !t->linear) {
        g_string_append_printf(t->o, "r[(%u + aL) & 63]", reg & 63);
    } else {
        g_string_append_printf(t->o, "r[%u]", reg & 63);
    }
}

/* a source operand as a vec4 expression */
static void src_expr(Tr *t, const SqSrc *sr)
{
    GString *o = t->o;

    if (sr->neg) {
        g_string_append(o, "(-");
    }
    if (sr->absv) {
        g_string_append(o, "abs(");
    }
    if (sr->temp) {
        temp_ref(t, sr->reg, sr->rel);
    } else if (!sr->addressed) {
        g_string_append_printf(o, "CONST(%u)", sr->reg);
    } else if (!sr->a0rel) {
        g_string_append_printf(o, "CONST(%u + aL)", sr->reg);
    } else {
        g_string_append_printf(o, "CONST(%u + a0)", sr->reg);
    }
    swz4(o, sr->swz);
    if (sr->absv) {
        g_string_append_c(o, ')');
    }
    if (sr->neg) {
        g_string_append_c(o, ')');
    }
}

static void emit_vector(Tr *t, const SqAlu *a)
{
    GString *o = t->o;

    for (int n = 0; n < a->nvsrc; n++) {
        g_string_append_printf(o, "    vec4 s%d = ", n);
        src_expr(t, &a->src[n]);
        g_string_append(o, ";\n");
    }
    g_string_append(o, "    vec4 vr = ");
    switch (a->vop) {
    case V_ADD: g_string_append(o, "s0 + s1;\n"); break;
    case V_MUL: g_string_append(o, "mul0v(s0, s1);\n"); break;
    case V_MAX: g_string_append(o, "vmax(s0, s1);\n"); break;
    case V_MIN: g_string_append(o, "vmin(s0, s1);\n"); break;
    case V_SETE: g_string_append(o, "vec4(equal(s0, s1));\n"); break;
    case V_SETGT: g_string_append(o, "vec4(greaterThan(s0, s1));\n"); break;
    case V_SETGTE:
        g_string_append(o, "vec4(greaterThanEqual(s0, s1));\n");
        break;
    case V_SETNE: g_string_append(o, "vec4(notEqual(s0, s1));\n"); break;
    case V_FRAC: g_string_append(o, "s0 - floor(s0);\n"); break;
    case V_TRUNC: g_string_append(o, "trunc(s0);\n"); break;
    case V_FLOOR: g_string_append(o, "floor(s0);\n"); break;
    case V_MULADD: g_string_append(o, "mul0v(s0, s1) + s2;\n"); break;
    case V_CNDE:
        g_string_append(o, "mix(s2, s1, equal(s0, vec4(0.0)));\n");
        break;
    case V_CNDGTE:
        g_string_append(o, "mix(s2, s1, greaterThanEqual(s0, vec4(0.0)));\n");
        break;
    case V_CNDGT:
        g_string_append(o, "mix(s2, s1, greaterThan(s0, vec4(0.0)));\n");
        break;
    case V_DOT4:
        g_string_append(o, "vec4(mul0(s0.x, s1.x) + mul0(s0.y, s1.y) + "
                        "mul0(s0.z, s1.z) + mul0(s0.w, s1.w));\n");
        break;
    case V_DOT3:
        g_string_append(o, "vec4(mul0(s0.x, s1.x) + mul0(s0.y, s1.y) + "
                        "mul0(s0.z, s1.z));\n");
        break;
    case V_DOT2ADD:
        g_string_append(o, "vec4(mul0(s0.x, s1.x) + mul0(s0.y, s1.y) + "
                        "s2.x);\n");
        break;
    case V_CUBE: g_string_append(o, "cube(s0);\n"); break;
    case V_MAX4:
        g_string_append(o, "vec4(cmax(cmax(s0.x, s0.y), cmax(s0.z, s0.w)));\n");
        break;
    case V_PRED_SETE_PUSH:
    case V_PRED_SETNE_PUSH:
    case V_PRED_SETGT_PUSH:
    case V_PRED_SETGTE_PUSH: {
        static const char *cmp[] = { "==", "!=", ">", ">=" };
        const char *c = cmp[a->vop - V_PRED_SETE_PUSH];

        g_string_append_printf(o, "vec4((s0.x == 0.0 && s1.x %s 0.0) ? 0.0 "
                               ": s0.x + 1.0);\n", c);
        g_string_append_printf(o, "    p0 = s0.w == 0.0 && s1.w %s 0.0;\n",
                               c);
        break;
    }
    case V_KILLE:
    case V_KILLGT:
    case V_KILLGTE:
    case V_KILLNE: {
        static const char *fn[] = {
            "equal", "greaterThan", "greaterThanEqual", "notEqual",
        };

        g_string_append_printf(o, "vec4(any(%s(s0, s1)) ? 1.0 : 0.0);\n",
                               fn[a->vop - V_KILLE]);
        g_string_append(o, "    killed = killed || vr.x != 0.0;\n");
        break;
    }
    case V_DST:
        g_string_append(o, "vec4(1.0, mul0(s0.y, s1.y), s0.z, s1.w);\n");
        break;
    case V_MOVA:
        g_string_append(o, "vmax(s0, s1);\n"
                        "    a0 = int(floor(clamp(s0.w, -256.0, 255.0) + "
                        "0.5));\n");
        break;
    default:
        g_string_append(o, "vec4(0.0);\n");
        break;
    }
    if (a->vclamp) {
        g_string_append(o, "    vr = sat4(vr);\n");
    }
}

static void emit_scalar(Tr *t, const SqAlu *a)
{
    GString *o = t->o;
    const char *e = NULL;

    if (a->sop == S_RETAIN_PREV || a->sop == S_41 || a->sop > S_RETAIN_PREV) {
        return;
    }
    if (a->skind == 1) {
        g_string_append(o, "    vec4 s3 = ");
        src_expr(t, &a->src[2]);
        g_string_append(o, ";\n    float sa = s3.w, sb = s3.x;\n");
    } else if (a->skind == 2) {
        g_string_append(o, "    float sa = ");
        if (a->sneg) {
            g_string_append_c(o, '-');
        }
        src_expr(t, &a->sconst);
        g_string_append_printf(o, ".w;\n    float sb = %sr[%u].%c;\n",
                               a->sneg ? "-" : "", a->streg & 63,
                               comp[a->stcomp & 3]);
    } else {
        g_string_append(o, "    float sa = 0.0, sb = 0.0;\n");
    }

    switch (a->sop) {
    case S_ADD: case S_ADD_CONST_0: case S_ADD_CONST_1: e = "sa + sb"; break;
    case S_ADD_PREV: e = "sa + ps"; break;
    case S_MUL: case S_MUL_CONST_0: case S_MUL_CONST_1:
        e = "mul0(sa, sb)";
        break;
    case S_MUL_PREV: e = "mul0(sa, ps)"; break;
    case S_MUL_PREV2:
        e = "(ps == -FLT_MAX || isinf(ps) || isnan(ps) || isinf(sb) || "
            "isnan(sb) || sb <= 0.0) ? -FLT_MAX : mul0(sa, ps)";
        break;
    case S_MAX: e = "smax(sa, sb)"; break;
    case S_MIN: e = "sa < sb ? sa : sb"; break;
    case S_SETE: e = "sa == 0.0 ? 1.0 : 0.0"; break;
    case S_SETGT: e = "sa > 0.0 ? 1.0 : 0.0"; break;
    case S_SETGTE: e = "sa >= 0.0 ? 1.0 : 0.0"; break;
    case S_SETNE: e = "sa != 0.0 ? 1.0 : 0.0"; break;
    case S_FRAC: e = "sa - floor(sa)"; break;
    case S_TRUNC: e = "trunc(sa)"; break;
    case S_FLOOR: e = "floor(sa)"; break;
    case S_EXP: e = "exp2(sa)"; break;
    case S_LOG_CLAMP: e = "sa <= 0.0 ? -FLT_MAX : log2(sa)"; break;
    case S_LOG: e = "log2(sa)"; break;
    case S_RECIP_CLAMP:
        e = "sa == 0.0 ? ((floatBitsToUint(sa) >> 31) != 0u ? -FLT_MAX : "
            "FLT_MAX) : 1.0 / sa";
        break;
    case S_RECIP_FF: e = "sa == 0.0 ? sa : 1.0 / sa"; break;
    case S_RECIP: e = "1.0 / sa"; break;
    case S_RECIPSQ_CLAMP: e = "sa == 0.0 ? FLT_MAX : 1.0 / sqrt(sa)"; break;
    case S_RECIPSQ_FF: e = "sa == 0.0 ? 0.0 : 1.0 / sqrt(sa)"; break;
    case S_RECIPSQ: e = "1.0 / sqrt(sa)"; break;
    case S_MOVA:
    case S_MOVA_FLOOR:
        g_string_append_printf(o, "    a0 = int(floor(clamp(sa, -256.0, "
                               "255.0)%s));\n",
                               a->sop == S_MOVA ? " + 0.5" : "");
        e = "smax(sa, sb)";
        break;
    case S_SUB: case S_SUB_CONST_0: case S_SUB_CONST_1: e = "sa - sb"; break;
    case S_SUB_PREV: e = "sa - ps"; break;
    case S_PRED_SETE:
    case S_PRED_SETNE:
    case S_PRED_SETGT:
    case S_PRED_SETGTE: {
        static const char *cmp[] = { "==", "!=", ">", ">=" };

        g_string_append_printf(o, "    p0 = sa %s 0.0;\n",
                               cmp[a->sop - S_PRED_SETE]);
        e = "p0 ? 0.0 : 1.0";
        break;
    }
    case S_PRED_SET_INV:
        g_string_append(o, "    p0 = sa == 1.0;\n");
        e = "p0 ? 0.0 : (sa == 0.0 ? 1.0 : sa)";
        break;
    case S_PRED_SET_POP:
        g_string_append(o, "    p0 = sa - 1.0 <= 0.0;\n");
        e = "p0 ? 0.0 : sa - 1.0";
        break;
    case S_PRED_SET_CLR:
        g_string_append(o, "    p0 = false;\n");
        e = "FLT_MAX";
        break;
    case S_PRED_SET_RESTORE:
        g_string_append(o, "    p0 = sa == 0.0;\n");
        e = "p0 ? 0.0 : sa";
        break;
    case S_KILLE: case S_KILLGT: case S_KILLGTE: case S_KILLNE:
    case S_KILLONE: {
        static const char *cond[] = {
            "sa == 0.0", "sa > 0.0", "sa >= 0.0", "sa != 0.0", "sa == 1.0",
        };

        g_string_append_printf(o, "    bool sk = %s;\n"
                               "    killed = killed || sk;\n",
                               cond[a->sop - S_KILLE]);
        e = "sk ? 1.0 : 0.0";
        break;
    }
    case S_SQRT: e = "sqrt(sa)"; break;
    case S_SIN: e = "sin(sa)"; break;
    case S_COS: e = "cos(sa)"; break;
    default: e = "0.0"; break;
    }
    g_string_append_printf(o, "    ps = %s;\n", e);
}

static void emit_alu(Tr *t, const SqAlu *a)
{
    GString *o = t->o;

    if (a->vrun) {
        emit_vector(t, a);
    }
    emit_scalar(t, a);
    if (a->sres || a->export) {
        g_string_append_printf(o, "    vec4 sr = vec4(%s);\n",
                               a->sclamp ? "sat(ps)" : "ps");
    }
    if (a->export) {
        unsigned cm = a->vres | a->sres | a->c0 | a->c1;
        const char *dst = NULL;

        g_string_append(o, "    vec4 ev = vec4(");
        for (int k = 0; k < 4; k++) {
            unsigned bit = 1u << k;

            if (a->vres & bit) {
                g_string_append_printf(o, "vr.%c", comp[k]);
            } else if (a->sres & bit) {
                g_string_append_printf(o, "sr.%c", comp[k]);
            } else {
                g_string_append(o, (a->c1 & bit) ? "1.0" : "0.0");
            }
            g_string_append(o, k < 3 ? ", " : ");\n");
        }
        if (!t->fragment) {
            if (a->vdst == 62) {
                dst = "o_pos";
            } else if (a->vdst == 63) {
                if (cm & 1) {
                    g_string_append(o, "    o_psize = ev.x;\n");
                }
                return;
            } else if (a->vdst < A2_MAX_INTERP) {
                g_string_append_printf(o, "    o_interp[%u].", a->vdst);
                mask_str(o, cm);
                g_string_append(o, " = ev.");
                mask_str(o, cm);
                g_string_append(o, ";\n");
                return;
            }
        } else if (a->vdst == 0) {
            dst = "o_color";
        } else if (a->vdst == 61) {
            if (cm & 1) {
                g_string_append(o, "    o_depth = ev.x;\n");
            }
            return;
        }
        if (dst && cm) {
            g_string_append_printf(o, "    %s.", dst);
            mask_str(o, cm);
            g_string_append(o, " = ev.");
            mask_str(o, cm);
            g_string_append(o, ";\n");
        }
        return;
    }
    if (a->vres) {
        g_string_append(o, "    ");
        temp_ref(t, a->vdst, a->vdst_rel);
        g_string_append_c(o, '.');
        mask_str(o, a->vres);
        g_string_append(o, " = vr.");
        mask_str(o, a->vres);
        g_string_append(o, ";\n");
    }
    if (a->sres) {
        g_string_append(o, "    ");
        temp_ref(t, a->sdst, a->sdst_rel);
        g_string_append_c(o, '.');
        mask_str(o, a->sres);
        g_string_append(o, " = sr.");
        mask_str(o, a->sres);
        g_string_append(o, ";\n");
    }
}

/* write fetched value fv to the destination through its swizzle */
static void emit_fetch_store(Tr *t, const SqFetch *f)
{
    GString *o = t->o;

    for (int k = 0; k < 4; k++) {
        unsigned sel = f->dswz[k];

        if (sel == 7) {
            continue;
        }
        g_string_append(o, "    ");
        temp_ref(t, f->dst, f->dst_rel);
        if (sel < 4) {
            g_string_append_printf(o, ".%c = fv.%c;\n", comp[k], comp[sel]);
        } else {
            g_string_append_printf(o, ".%c = %s;\n", comp[k],
                                   sel == 5 ? "1.0" : "0.0");
        }
    }
}

static int tex_unit(Tr *t, const SqFetch *f)
{
    AdrenoGlslInfo *info = t->info;
    AdrenoGlslTexUnit u;

    /* compared with memcmp: no stray padding bytes */
    memset(&u, 0, sizeof(u));
    u.const_idx = f->tex.const_idx;
    u.mag = f->tex.mag;
    u.min = f->tex.min;
    u.mip = f->tex.mip;
    u.unnorm = f->tex.unnormalized;
    u.bias = f->tex.lod_bias;
    u.off[0] = f->tex.offset[0];
    u.off[1] = f->tex.offset[1];
    for (int i = 0; i < info->ntex; i++) {
        if (!memcmp(&info->tex[i], &u, sizeof(u))) {
            return i;
        }
    }
    if (info->ntex == ADRENO_GL_MAX_UNITS) {
        return -1;
    }
    info->tex[info->ntex] = u;
    return info->ntex++;
}

/*
 * A vertex fetch: the element's dwords from vfetch_word() (the guest's
 * buffer at the fetch constant's address), endian-swapped, decoded.
 */
static void emit_vfetch(Tr *t, const SqFetch *f)
{
    GString *o = t->o;
    AdrenoGlslInfo *info = t->info;
    int ndw = 4;
    int slot = -1;

    for (int i = 0; i < info->nvtx; i++) {
        if (info->vtx[i] == f->vconst) {
            slot = i;
        }
    }
    if (slot < 0) {
        if (info->nvtx == ADRENO_GL_MAX_VBUFS) {
            return;
        }
        slot = info->nvtx;
        info->vtx[info->nvtx++] = f->vconst;
    }
    switch (f->fmt) {
    case 2: case 10: case 6: case 24: case 25: case 7: case 21: case 16:
    case 30: case 31: case 33: case 36:
        ndw = 1;
        break;
    case 26: case 32: case 34: case 37:
        ndw = 2;
        break;
    case 57:
        ndw = 3;
        break;
    }
    g_string_append(o, "    float vi = ");
    temp_ref(t, f->src, f->src_rel);
    g_string_append_printf(o, ".%c%s;\n", comp[f->scomp & 3],
                           f->round ? " + 0.5" : "");
    g_string_append_printf(o, "    int va = %u * int(floor(vi)) + %d;\n"
                           "    uvec4 dw = uvec4(0u);\n", f->stride,
                           f->offset);
    for (int k = 0; k < ndw; k++) {
        g_string_append_printf(o, "    dw[%d] = vswap(vfetch_word(%d, va + "
                               "%d), %d);\n", k, slot, 4 * k, slot);
    }
    g_string_append(o, "    vec4 fv = vec4(0.0, 0.0, 0.0, 1.0);\n");

#define UNPACK(n, b0, b1, b2, b3) \
    g_string_append_printf(o, "    fv = vunpack(dw, %d, ivec4(%d, %d, %d, " \
                           "%d), %s, %s, fv);\n", n, b0, b1, b2, b3, \
                           f->sign ? "true" : "false", \
                           f->norm ? "true" : "false")
    switch (f->fmt) {
    case 2: UNPACK(1, 8, 0, 0, 0); break;
    case 10: UNPACK(2, 8, 8, 0, 0); break;
    case 16: UNPACK(3, 8, 8, 8, 0); break;
    case 6: UNPACK(4, 8, 8, 8, 8); break;
    case 7: UNPACK(4, 10, 10, 10, 2); break;
    case 21: UNPACK(4, 2, 10, 10, 10); break;
    case 24: UNPACK(1, 16, 0, 0, 0); break;
    case 25: UNPACK(2, 16, 16, 0, 0); break;
    case 26: UNPACK(4, 16, 16, 16, 16); break;
    case 30:
        g_string_append(o, "    fv.x = h2f(dw.x & 0xffffu);\n");
        break;
    case 31:
        g_string_append(o, "    fv.xy = vec2(h2f(dw.x & 0xffffu), "
                        "h2f(dw.x >> 16));\n");
        break;
    case 32:
        g_string_append(o, "    fv = vec4(h2f(dw.x & 0xffffu), "
                        "h2f(dw.x >> 16), h2f(dw.y & 0xffffu), "
                        "h2f(dw.y >> 16));\n");
        break;
    case 33: case 34: case 35:
        for (int k = 0; k < f->fmt - 32; k++) {
            g_string_append_printf(o, "    fv[%d] = %s(dw[%d])%s;\n", k,
                                   f->sign ? "float(int" : "float(",
                                   k, f->sign ? ")" : "");
            if (f->norm) {
                g_string_append_printf(o, "    fv[%d] /= %s;\n", k,
                                       f->sign ? "2147483647.0"
                                               : "4294967295.0");
            }
        }
        break;
    case 36: case 37: case 38: case 57:
        for (int k = 0; k < ndw; k++) {
            g_string_append_printf(o, "    fv[%d] = uintBitsToFloat(dw[%d]);"
                                   "\n", k, k);
        }
        break;
    }
#undef UNPACK
    if (f->exp_adj) {
        g_string_append_printf(o, "    fv *= %.9g;\n", ldexp(1.0, f->exp_adj));
    }
    emit_fetch_store(t, f);
}

static void emit_tfetch(Tr *t, const SqFetch *f)
{
    GString *o = t->o;
    int u;

    if (f->op != 1) {
        /* LOD/gradient queries and setters: not modelled */
        if (f->op < 24) {
            g_string_append(o, "    vec4 fv = vec4(0.0);\n");
            emit_fetch_store(t, f);
        }
        return;
    }
    u = tex_unit(t, f);
    if (u < 0) {
        return;
    }
    g_string_append_printf(o, "    vec4 fv = tex_%d(", u);
    temp_ref(t, f->src, f->src_rel);
    g_string_append_printf(o, ".%c%c%c);\n", comp[f->sswz[0]],
                           comp[f->sswz[1]], comp[f->sswz[2]]);
    emit_fetch_store(t, f);
}

static void emit_clause(Tr *t, const SqCf *c)
{
    const AdrenoProgram *p = t->p;
    GString *o = t->o;

    for (int i = 0; i < c->count; i++) {
        uint32_t a = c->addr + i;
        const SqInsn *in;

        if (a >= p->ninsn) {
            return;
        }
        in = &p->insn[a];
        if (!in->valid) {
            continue;
        }
        if (in->predicated) {
            g_string_append_printf(o, "  if (%sp0) {\n",
                                   in->pred_cond ? "" : "!");
        } else {
            g_string_append(o, "  {\n");
        }
        if (in->fetch) {
            if (in->f.vtx) {
                emit_vfetch(t, &in->f);
            } else {
                emit_tfetch(t, &in->f);
            }
        } else {
            emit_alu(t, &in->alu);
        }
        g_string_append(o, "  }\n");
    }
}

static bool cf_is_exec(unsigned op)
{
    switch (op) {
    case CF_EXEC: case CF_EXEC_END: case CF_COND_EXEC: case CF_COND_EXEC_END:
    case CF_COND_PRED_EXEC: case CF_COND_PRED_EXEC_END:
    case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END:
        return true;
    }
    return false;
}

static bool cf_is_end(unsigned op)
{
    return op == CF_EXEC_END || op == CF_COND_EXEC_END ||
           op == CF_COND_PRED_EXEC_END || op == CF_COND_EXEC_PRED_CLEAN_END;
}

/* an EXEC: its condition, then the clause */
static void emit_exec(Tr *t, const SqCf *c)
{
    GString *o = t->o;

    switch (c->op) {
    case CF_COND_EXEC: case CF_COND_EXEC_END:
    case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END:
        g_string_append_printf(o, " if (%sbconst(%u)) {\n",
                               c->cond ? "" : "!", c->bool_addr);
        break;
    case CF_COND_PRED_EXEC: case CF_COND_PRED_EXEC_END:
        g_string_append_printf(o, " if (%sp0) {\n", c->cond ? "" : "!");
        break;
    default:
        g_string_append(o, " {\n");
        break;
    }
    emit_clause(t, c);
    g_string_append(o, " }\n");
}

void adreno_glsl_translate(const AdrenoProgram *p, bool fragment,
                           uint32_t cube_mask, GString *o,
                           AdrenoGlslInfo *info)
{
    Tr t = { .o = o, .p = p, .info = info, .fragment = fragment,
             .cube_mask = cube_mask, .linear = true };
    g_autoptr(GString) body = g_string_new(NULL);

    memset(info, 0, sizeof(*info));
    for (uint32_t i = 0; i < p->ncf; i++) {
        unsigned op = p->cf[i].op;

        if (op == CF_LOOP_START || op == CF_LOOP_END || op == CF_COND_CALL ||
            op == CF_RETURN || op == CF_COND_JMP) {
            t.linear = false;
        }
        if (t.linear && cf_is_end(op)) {
            break;
        }
    }

    t.o = body;
    if (t.linear) {
        g_string_append(body, "void sq_main() {\n  const int aL = 0;\n");
        for (uint32_t i = 0; i < p->ncf; i++) {
            const SqCf *c = &p->cf[i];

            if (cf_is_exec(c->op)) {
                emit_exec(&t, c);
                if (cf_is_end(c->op)) {
                    break;
                }
            }
        }
        g_string_append(body, "}\n");
    } else {
        g_string_append(body,
            "void sq_main() {\n"
            "  int pc = 0, guard = 0, ld = 0, cd = 0, aL = 0;\n"
            "  int li[4] = int[4](0, 0, 0, 0), cret[4] = int[4](0, 0, 0, 0);\n"
            "  uint lcst[4] = uint[4](0u, 0u, 0u, 0u);\n"
            "  while (pc >= 0 && ++guard < 65536) {\n"
            "   aL = ld == 0 ? 0 : int((lcst[ld - 1] >> 8) & 0xffu) +\n"
            "        li[ld - 1] * bitfieldExtract(int(lcst[ld - 1]), 16, 8);\n"
            "   switch (pc) {\n");
        for (uint32_t i = 0; i < p->ncf; i++) {
            const SqCf *c = &p->cf[i];

            g_string_append_printf(body, "   case %u:\n", i);
            switch (c->op) {
            case CF_EXEC: case CF_EXEC_END: case CF_COND_EXEC:
            case CF_COND_EXEC_END: case CF_COND_PRED_EXEC:
            case CF_COND_PRED_EXEC_END: case CF_COND_EXEC_PRED_CLEAN:
            case CF_COND_EXEC_PRED_CLEAN_END:
                emit_exec(&t, c);
                g_string_append_printf(body, "    pc = %d;\n",
                                       cf_is_end(c->op) ? -1 : (int)i + 1);
                break;
            case CF_LOOP_START:
                g_string_append_printf(body,
                    "    if (ld >= 4) { pc = %u; break; }\n"
                    "    lcst[ld] = lconst(%u);\n"
                    "    %s\n"
                    "    if (uint(li[ld]) >= (lcst[ld] & 0xffu)) {\n"
                    "        pc = %u;\n"
                    "    } else {\n"
                    "        ld++;\n"
                    "        pc = %u;\n"
                    "    }\n",
                    c->addr, c->loop_id,
                    c->repeat ? "" : "li[ld] = 0;", c->addr, i + 1);
                break;
            case CF_LOOP_END:
                g_string_append_printf(body,
                    "    pc = %u;\n"
                    "    if (ld > 0) {\n"
                    "        li[ld - 1]++;\n"
                    "        if (uint(li[ld - 1]) < (lcst[ld - 1] & 0xffu)%s) {\n"
                    "            pc = %u;\n"
                    "        } else {\n"
                    "            ld--;\n"
                    "        }\n"
                    "    }\n",
                    i + 1, !c->pred_break ? "" :
                           (c->cond ? " && !p0" : " && p0"), c->addr);
                break;
            case CF_COND_CALL:
            case CF_COND_JMP:
                if (c->uncond) {
                    g_string_append(body, "    if (true) {\n");
                } else if (c->predicated) {
                    g_string_append_printf(body, "    if (%sp0) {\n",
                                           c->cond ? "" : "!");
                } else {
                    g_string_append_printf(body, "    if (%sbconst(%u)) {\n",
                                           c->cond ? "" : "!", c->bool_addr);
                }
                if (c->op == CF_COND_CALL) {
                    g_string_append_printf(body,
                        "        if (cd < 4) {\n"
                        "            cret[cd++] = %u;\n"
                        "            pc = %u;\n"
                        "        } else {\n"
                        "            pc = %u;\n"
                        "        }\n", i + 1, c->addr, i + 1);
                } else {
                    g_string_append_printf(body, "        pc = %u;\n",
                                           c->addr);
                }
                g_string_append_printf(body, "    } else {\n"
                                       "        pc = %u;\n    }\n", i + 1);
                break;
            case CF_RETURN:
                g_string_append_printf(body, "    pc = cd > 0 ? cret[--cd] : "
                                       "%u;\n", i + 1);
                break;
            default:
                g_string_append_printf(body, "    pc = %u;\n", i + 1);
                break;
            }
            g_string_append(body, "    break;\n");
        }
        g_string_append(body, "   default:\n    pc = -1;\n    break;\n"
                        "   }\n  }\n}\n");
    }

    /* the helpers, the texture units this program samples, then the body */
    g_string_append(o, glsl_helpers);
    for (int u = 0; u < info->ntex; u++) {
        const AdrenoGlslTexUnit *tu = &info->tex[u];
        bool cube = cube_mask & (1u << tu->const_idx);

        g_string_append_printf(o,
            "layout(binding = %d) uniform %s tu_%d;\n"
            "vec4 tex_%d(vec3 c) {\n"
            "    vec4 ts = u_tex[%d].size;\n"
            "    vec2 uv = c.xy;\n",
            u, cube ? "sampler2DArray" : "sampler2D", u, u, u);
        if (cube) {
            g_string_append(o, "    uv -= 1.0;\n");
        } else if (tu->unnorm) {
            g_string_append(o, "    uv *= ts.zw;\n");
        }
        if (tu->off[0] != 0.0f || tu->off[1] != 0.0f) {
            g_string_append_printf(o, "    uv += vec2(%.9g, %.9g) * ts.zw;\n",
                                   tu->off[0], tu->off[1]);
        }
        if (cube) {
            g_string_append_printf(o, "    vec3 tc = vec3(uv, float(clamp(int("
                                   "c.z), 0, 5)));\n");
        } else {
            g_string_append(o, "    vec2 tc = uv;\n");
        }
        if (!fragment) {
            g_string_append_printf(o, "    vec4 raw = textureLod(tu_%d, tc, "
                                   "max(%.9g + u_tex[%d].scale.y, 0.0));\n",
                                   u, tu->bias, u);
        } else if (tu->bias != 0.0f) {
            g_string_append_printf(o, "    vec4 raw = texture(tu_%d, tc, "
                                   "%.9g);\n", u, tu->bias);
        } else {
            g_string_append_printf(o, "    vec4 raw = texture(tu_%d, tc);\n",
                                   u);
        }
        g_string_append_printf(o, "    return tswz(raw * u_tex[%d].scale.x, "
                               "u_tex[%d].swz);\n}\n", u, u);
    }
    g_string_append_len(o, body->str, body->len);
}
