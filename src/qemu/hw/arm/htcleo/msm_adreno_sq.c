/*
 * Adreno 200 shader sequencer: an interpreter for a2xx shader microcode
 *
 * A shader is a list of control flow (CF) instructions, two per 96-bit
 * slot at the start of the program, followed by the 96-bit ALU and fetch
 * instructions the EXEC clauses run.  An ALU instruction co-issues a
 * vector operation (srcs 1-2[-3]) and a scalar operation (src 3); both
 * write temporaries or, with export_data set, the shader's outputs.
 *
 * Formats and semantics follow freedreno (src/freedreno/ir2/instr-a2xx.h,
 * disasm-a2xx.c) and Xenia's Xenos interpreter (the Xbox 360 GPU is the
 * same design): src/xenia/gpu/ucode.h, shader_interpreter.cc.
 *
 * Exports: vertex shader 0-15 interpolators, 62 position, 63 point size;
 * pixel shader 0-3 colours, 61 depth.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <math.h>
#include <float.h>
#include "msm_adreno.h"

/* CF opcodes */
enum {
    CF_NOP, CF_EXEC, CF_EXEC_END, CF_COND_EXEC, CF_COND_EXEC_END,
    CF_COND_PRED_EXEC, CF_COND_PRED_EXEC_END, CF_LOOP_START, CF_LOOP_END,
    CF_COND_CALL, CF_RETURN, CF_COND_JMP, CF_ALLOC,
    CF_COND_EXEC_PRED_CLEAN, CF_COND_EXEC_PRED_CLEAN_END,
    CF_MARK_VS_FETCH_DONE,
};

/* vector opcodes */
enum {
    V_ADD, V_MUL, V_MAX, V_MIN, V_SETE, V_SETGT, V_SETGTE, V_SETNE,
    V_FRAC, V_TRUNC, V_FLOOR, V_MULADD, V_CNDE, V_CNDGTE, V_CNDGT,
    V_DOT4, V_DOT3, V_DOT2ADD, V_CUBE, V_MAX4, V_PRED_SETE_PUSH,
    V_PRED_SETNE_PUSH, V_PRED_SETGT_PUSH, V_PRED_SETGTE_PUSH,
    V_KILLE, V_KILLGT, V_KILLGTE, V_KILLNE, V_DST, V_MOVA,
};

/* scalar opcodes */
enum {
    S_ADD, S_ADD_PREV, S_MUL, S_MUL_PREV, S_MUL_PREV2, S_MAX, S_MIN,
    S_SETE, S_SETGT, S_SETGTE, S_SETNE, S_FRAC, S_TRUNC, S_FLOOR,
    S_EXP, S_LOG_CLAMP, S_LOG, S_RECIP_CLAMP, S_RECIP_FF, S_RECIP,
    S_RECIPSQ_CLAMP, S_RECIPSQ_FF, S_RECIPSQ, S_MOVA, S_MOVA_FLOOR,
    S_SUB, S_SUB_PREV, S_PRED_SETE, S_PRED_SETNE, S_PRED_SETGT,
    S_PRED_SETGTE, S_PRED_SET_INV, S_PRED_SET_POP, S_PRED_SET_CLR,
    S_PRED_SET_RESTORE, S_KILLE, S_KILLGT, S_KILLGTE, S_KILLNE,
    S_KILLONE, S_SQRT, S_41, S_MUL_CONST_0, S_MUL_CONST_1,
    S_ADD_CONST_0, S_ADD_CONST_1, S_SUB_CONST_0, S_SUB_CONST_1,
    S_SIN, S_COS, S_RETAIN_PREV,
};

/* operand components the vector opcodes read, per source */
static const uint8_t vec_srcs[32] = {
    [V_ADD] = 2, [V_MUL] = 2, [V_MAX] = 2, [V_MIN] = 2, [V_SETE] = 2,
    [V_SETGT] = 2, [V_SETGTE] = 2, [V_SETNE] = 2, [V_FRAC] = 1,
    [V_TRUNC] = 1, [V_FLOOR] = 1, [V_MULADD] = 3, [V_CNDE] = 3,
    [V_CNDGTE] = 3, [V_CNDGT] = 3, [V_DOT4] = 2, [V_DOT3] = 2,
    [V_DOT2ADD] = 3, [V_CUBE] = 2, [V_MAX4] = 1, [V_PRED_SETE_PUSH] = 2,
    [V_PRED_SETNE_PUSH] = 2, [V_PRED_SETGT_PUSH] = 2,
    [V_PRED_SETGTE_PUSH] = 2, [V_KILLE] = 2, [V_KILLGT] = 2,
    [V_KILLGTE] = 2, [V_KILLNE] = 2, [V_DST] = 2, [V_MOVA] = 2,
};

typedef struct SqState {
    AdrenoShaderCtx *c;
    bool pred;
    int a0;
    float prev;
    int loop_depth;
    int loop_iter[4];
    uint32_t loop_const[4];
    int call_depth;
    uint32_t call_ret[4];
    uint32_t vfetch_full[3];    /* last full vertex fetch (for mini fetch) */
    uint32_t vfetch_addr;       /* its address in bytes */
} SqState;

static inline float flush_denorm(float f)
{
    return fpclassify(f) == FP_SUBNORMAL ? copysignf(0.0f, f) : f;
}

static inline float sat(float f)
{
    return f > 1.0f ? 1.0f : (f >= 0.0f ? f : 0.0f);
}

/* D3D9-style multiply: 0 * anything (inf, nan) = 0 */
static inline float mul0(float a, float b)
{
    return (a != 0.0f && b != 0.0f) ? a * b : 0.0f;
}

static int loop_al(SqState *st)
{
    if (!st->loop_depth) {
        return 0;
    }
    uint32_t lc = st->loop_const[st->loop_depth - 1];
    int start = (lc >> 8) & 0xff;
    int step = (int8_t)(lc >> 16);
    return start + st->loop_iter[st->loop_depth - 1] * step;
}

static float *temp_reg(SqState *st, uint32_t reg, bool rel)
{
    int i = reg & 0x3f;

    if (rel) {
        i += loop_al(st);
    }
    return st->c->r[i & 63];
}

static void const_reg(SqState *st, uint32_t idx, bool addressed, bool a0rel,
                      float out[4])
{
    MsmAdrenoState *s = st->c->s;
    int i = idx;
    uint32_t base;

    if (addressed) {
        i += a0rel ? st->a0 : loop_al(st);
    }
    base = s->regs[st->c->type == A2_SHADER_VS ? A2_SQ_VS_CONST
                                               : A2_SQ_PS_CONST] & 0x1ff;
    i = (i + base) & 511;
    for (int k = 0; k < 4; k++) {
        out[k] = adreno_f(s->regs[A2_SQ_CONSTANT_0 + i * 4 + k]);
    }
}

static bool bool_const(SqState *st, uint32_t addr)
{
    uint32_t w = st->c->s->regs[A2_SQ_CF_BOOLEANS + (addr >> 5)];

    return (w >> (addr & 31)) & 1;
}

/* ------------------------------------------------------------------ */
/* Exports                                                             */
/* ------------------------------------------------------------------ */

static void do_export(SqState *st, uint32_t reg, const float v[4],
                      uint32_t mask)
{
    AdrenoShaderCtx *c = st->c;
    float *dst = NULL;
    float tmp[4];

    if (c->type == A2_SHADER_VS) {
        AdrenoVertex *o = c->vout;

        if (reg == 62) {
            dst = o->pos;
        } else if (reg == 63) {
            dst = tmp;
            memset(tmp, 0, sizeof(tmp));
        } else if (reg < A2_MAX_INTERP) {
            dst = o->interp[reg];
        }
        if (dst) {
            for (int k = 0; k < 4; k++) {
                if (mask & (1 << k)) {
                    dst[k] = v[k];
                }
            }
        }
        if (reg == 63 && (mask & 1)) {
            o->psize = v[0];
        }
    } else {
        AdrenoPixelOut *o = c->pout;

        if (reg < 4) {
            dst = o->color[reg];
            for (int k = 0; k < 4; k++) {
                if (mask & (1 << k)) {
                    dst[k] = v[k];
                }
            }
        } else if (reg == 61 && (mask & 1)) {
            o->depth = v[0];
            o->depth_written = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */
/* ------------------------------------------------------------------ */

static void alu_src(SqState *st, const uint32_t *w, int n, float out[4])
{
    /* n = 1..3 */
    uint32_t reg = (w[2] >> (8 * (3 - n))) & 0xff;
    uint32_t swiz = (w[1] >> (8 * (3 - n))) & 0xff;
    bool is_temp = (w[2] >> (32 - n)) & 1;
    bool neg = (w[1] >> (27 - n)) & 1;
    bool absv;
    float v[4];

    if (is_temp) {
        memcpy(v, temp_reg(st, reg, reg & 0x40), sizeof(v));
        absv = reg & 0x80;
    } else {
        /* const_0_rel_abs for the first constant, const_1 for later ones */
        bool addressed;
        bool t1 = (w[2] >> 31) & 1, t2 = (w[2] >> 30) & 1;

        switch (n) {
        case 1:
            addressed = (w[1] >> 31) & 1;
            break;
        case 2:
            addressed = t1 ? (w[1] >> 31) & 1 : (w[1] >> 30) & 1;
            break;
        default:
            addressed = (t1 && t2) ? (w[1] >> 31) & 1 : (w[1] >> 30) & 1;
            break;
        }
        const_reg(st, reg, addressed, (w[1] >> 29) & 1, v);
        absv = (w[0] >> 7) & 1;
    }
    for (int k = 0; k < 4; k++) {
        float f = flush_denorm(v[(((swiz >> (2 * k)) + k) & 3)]);

        if (absv) {
            f = fabsf(f);
        }
        out[k] = neg ? -f : f;
    }
}

static void exec_alu(SqState *st, const uint32_t *w)
{
    AdrenoShaderCtx *c = st->c;
    uint32_t vop = (w[2] >> 24) & 0x1f;
    uint32_t sop = (w[0] >> 26) & 0x3f;
    uint32_t vmask = (w[0] >> 16) & 0xf;
    uint32_t smask = (w[0] >> 20) & 0xf;
    bool export = (w[0] >> 15) & 1;
    float a[4] = {0}, b[4] = {0}, cc[4] = {0}, vr[4] = {0};
    float sa = 0, sb = 0, sr;
    bool replicate = false;
    uint32_t vres_mask = export ? vmask & ~smask : vmask;
    uint32_t sres_mask = export ? smask & ~vmask : smask;

    /* vector operation */
    if (vres_mask || (vop >= V_PRED_SETE_PUSH && vop <= V_KILLNE) ||
        vop == V_MOVA) {
        int ns = vec_srcs[vop];

        if (ns >= 1) {
            alu_src(st, w, 1, a);
        }
        if (ns >= 2) {
            alu_src(st, w, 2, b);
        }
        if (ns >= 3) {
            alu_src(st, w, 3, cc);
        }
        switch (vop) {
        case V_ADD:
            for (int k = 0; k < 4; k++) vr[k] = a[k] + b[k];
            break;
        case V_MUL:
            for (int k = 0; k < 4; k++) vr[k] = mul0(a[k], b[k]);
            break;
        case V_MAX:
            for (int k = 0; k < 4; k++) vr[k] = a[k] >= b[k] ? a[k] : b[k];
            break;
        case V_MIN:
            for (int k = 0; k < 4; k++) vr[k] = a[k] < b[k] ? a[k] : b[k];
            break;
        case V_SETE:
            for (int k = 0; k < 4; k++) vr[k] = a[k] == b[k];
            break;
        case V_SETGT:
            for (int k = 0; k < 4; k++) vr[k] = a[k] > b[k];
            break;
        case V_SETGTE:
            for (int k = 0; k < 4; k++) vr[k] = a[k] >= b[k];
            break;
        case V_SETNE:
            for (int k = 0; k < 4; k++) vr[k] = a[k] != b[k];
            break;
        case V_FRAC:
            for (int k = 0; k < 4; k++) vr[k] = a[k] - floorf(a[k]);
            break;
        case V_TRUNC:
            for (int k = 0; k < 4; k++) vr[k] = truncf(a[k]);
            break;
        case V_FLOOR:
            for (int k = 0; k < 4; k++) vr[k] = floorf(a[k]);
            break;
        case V_MULADD:
            for (int k = 0; k < 4; k++) vr[k] = mul0(a[k], b[k]) + cc[k];
            break;
        case V_CNDE:
            for (int k = 0; k < 4; k++) vr[k] = a[k] == 0.0f ? b[k] : cc[k];
            break;
        case V_CNDGTE:
            for (int k = 0; k < 4; k++) vr[k] = a[k] >= 0.0f ? b[k] : cc[k];
            break;
        case V_CNDGT:
            for (int k = 0; k < 4; k++) vr[k] = a[k] > 0.0f ? b[k] : cc[k];
            break;
        case V_DOT4:
            vr[0] = mul0(a[0], b[0]) + mul0(a[1], b[1]) + mul0(a[2], b[2]) +
                    mul0(a[3], b[3]);
            replicate = true;
            break;
        case V_DOT3:
            vr[0] = mul0(a[0], b[0]) + mul0(a[1], b[1]) + mul0(a[2], b[2]);
            replicate = true;
            break;
        case V_DOT2ADD:
            vr[0] = mul0(a[0], b[0]) + mul0(a[1], b[1]) + cc[0];
            replicate = true;
            break;
        case V_CUBE: {
            /* operand a is .zzxy */
            float x = a[2], y = a[3], z = a[0];
            float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);

            if (az >= ax && az >= ay) {
                vr[0] = -y;
                vr[1] = z < 0 ? -x : x;
                vr[2] = z;
                vr[3] = z < 0 ? 5.0f : 4.0f;
            } else if (ay >= ax) {
                vr[0] = y < 0 ? -z : z;
                vr[1] = x;
                vr[2] = y;
                vr[3] = y < 0 ? 3.0f : 2.0f;
            } else {
                vr[0] = -y;
                vr[1] = x < 0 ? z : -z;
                vr[2] = x;
                vr[3] = x < 0 ? 1.0f : 0.0f;
            }
            vr[2] *= 2.0f;
            break;
        }
        case V_MAX4:
            vr[0] = MAX(MAX(a[0], a[1]), MAX(a[2], a[3]));
            replicate = true;
            break;
        case V_PRED_SETE_PUSH:
            st->pred = a[3] == 0.0f && b[3] == 0.0f;
            vr[0] = (a[0] == 0.0f && b[0] == 0.0f) ? 0.0f : a[0] + 1.0f;
            replicate = true;
            break;
        case V_PRED_SETNE_PUSH:
            st->pred = a[3] == 0.0f && b[3] != 0.0f;
            vr[0] = (a[0] == 0.0f && b[0] != 0.0f) ? 0.0f : a[0] + 1.0f;
            replicate = true;
            break;
        case V_PRED_SETGT_PUSH:
            st->pred = a[3] == 0.0f && b[3] > 0.0f;
            vr[0] = (a[0] == 0.0f && b[0] > 0.0f) ? 0.0f : a[0] + 1.0f;
            replicate = true;
            break;
        case V_PRED_SETGTE_PUSH:
            st->pred = a[3] == 0.0f && b[3] >= 0.0f;
            vr[0] = (a[0] == 0.0f && b[0] >= 0.0f) ? 0.0f : a[0] + 1.0f;
            replicate = true;
            break;
        case V_KILLE:
        case V_KILLGT:
        case V_KILLGTE:
        case V_KILLNE: {
            bool kill = false;

            for (int k = 0; k < 4; k++) {
                switch (vop) {
                case V_KILLE: kill |= a[k] == b[k]; break;
                case V_KILLGT: kill |= a[k] > b[k]; break;
                case V_KILLGTE: kill |= a[k] >= b[k]; break;
                default: kill |= a[k] != b[k]; break;
                }
            }
            if (kill && c->pout) {
                c->pout->killed = true;
            }
            vr[0] = kill;
            replicate = true;
            break;
        }
        case V_DST:
            vr[0] = 1.0f;
            vr[1] = mul0(a[1], b[1]);
            vr[2] = a[2];
            vr[3] = b[3];
            break;
        case V_MOVA:
            st->a0 = (int)floorf(MIN(MAX(a[3], -256.0f), 255.0f) + 0.5f);
            for (int k = 0; k < 4; k++) vr[k] = a[k] >= b[k] ? a[k] : b[k];
            break;
        default:
            break;
        }
        if (replicate) {
            vr[1] = vr[2] = vr[3] = vr[0];
        }
    }

    /* scalar operation: src3 .w (and .x), or c#.w and r#.x */
    if (sop >= S_MUL_CONST_0 && sop <= S_SUB_CONST_1) {
        uint32_t swiz = w[1] & 0xff;
        uint32_t creg = w[2] & 0xff;
        uint32_t treg = (sop & 1) | (((w[2] >> 29) & 1) << 1) | (swiz & 0x3c);
        bool neg = (w[1] >> 24) & 1;
        float cv[4];

        bool both_temp = ((w[2] >> 31) & 1) && ((w[2] >> 30) & 1);

        const_reg(st, creg, both_temp ? (w[1] >> 31) & 1 : (w[1] >> 30) & 1,
                  (w[1] >> 29) & 1, cv);
        sa = flush_denorm(cv[((swiz >> 6) + 3) & 3]);
        sb = flush_denorm(c->r[treg][swiz & 3]);
        if (neg) {
            sa = -sa;
            sb = -sb;
        }
    } else if (sop != S_PRED_SET_CLR && sop != S_RETAIN_PREV &&
               sop != S_41 && sop <= S_RETAIN_PREV) {
        float v[4];

        alu_src(st, w, 3, v);
        sa = v[3];
        sb = v[0];
    }

    switch (sop) {
    case S_ADD: case S_ADD_CONST_0: case S_ADD_CONST_1:
        st->prev = sa + sb;
        break;
    case S_ADD_PREV:
        st->prev = sa + st->prev;
        break;
    case S_MUL: case S_MUL_CONST_0: case S_MUL_CONST_1:
        st->prev = mul0(sa, sb);
        break;
    case S_MUL_PREV:
        st->prev = mul0(sa, st->prev);
        break;
    case S_MUL_PREV2:
        if (st->prev == -FLT_MAX || !isfinite(st->prev) || !isfinite(sb) ||
            sb <= 0.0f) {
            st->prev = -FLT_MAX;
        } else {
            st->prev = mul0(sa, st->prev);
        }
        break;
    case S_MAX:
        st->prev = sa >= sb ? sa : sb;
        break;
    case S_MIN:
        st->prev = sa < sb ? sa : sb;
        break;
    case S_SETE:
        st->prev = sa == 0.0f;
        break;
    case S_SETGT:
        st->prev = sa > 0.0f;
        break;
    case S_SETGTE:
        st->prev = sa >= 0.0f;
        break;
    case S_SETNE:
        st->prev = sa != 0.0f;
        break;
    case S_FRAC:
        st->prev = sa - floorf(sa);
        break;
    case S_TRUNC:
        st->prev = truncf(sa);
        break;
    case S_FLOOR:
        st->prev = floorf(sa);
        break;
    case S_EXP:
        st->prev = exp2f(sa);
        break;
    case S_LOG_CLAMP:
        st->prev = log2f(sa);
        if (isinf(st->prev) && st->prev < 0) {
            st->prev = -FLT_MAX;
        }
        break;
    case S_LOG:
        st->prev = log2f(sa);
        break;
    case S_RECIP_CLAMP:
        st->prev = 1.0f / sa;
        if (isinf(st->prev)) {
            st->prev = st->prev < 0 ? -FLT_MAX : FLT_MAX;
        }
        break;
    case S_RECIP_FF:
        st->prev = 1.0f / sa;
        if (isinf(st->prev)) {
            st->prev = st->prev < 0 ? -0.0f : 0.0f;
        }
        break;
    case S_RECIP:
        st->prev = 1.0f / sa;
        break;
    case S_RECIPSQ_CLAMP:
        st->prev = 1.0f / sqrtf(sa);
        if (isinf(st->prev)) {
            st->prev = st->prev < 0 ? -FLT_MAX : FLT_MAX;
        }
        break;
    case S_RECIPSQ_FF:
        st->prev = 1.0f / sqrtf(sa);
        if (isinf(st->prev)) {
            st->prev = st->prev < 0 ? -0.0f : 0.0f;
        }
        break;
    case S_RECIPSQ:
        st->prev = 1.0f / sqrtf(sa);
        break;
    case S_MOVA:
        st->a0 = (int)floorf(MIN(MAX(sa, -256.0f), 255.0f) + 0.5f);
        st->prev = sa >= sb ? sa : sb;
        break;
    case S_MOVA_FLOOR:
        st->a0 = (int)floorf(MIN(MAX(sa, -256.0f), 255.0f));
        st->prev = sa >= sb ? sa : sb;
        break;
    case S_SUB: case S_SUB_CONST_0: case S_SUB_CONST_1:
        st->prev = sa - sb;
        break;
    case S_SUB_PREV:
        st->prev = sa - st->prev;
        break;
    case S_PRED_SETE:
        st->pred = sa == 0.0f;
        st->prev = !st->pred;
        break;
    case S_PRED_SETNE:
        st->pred = sa != 0.0f;
        st->prev = !st->pred;
        break;
    case S_PRED_SETGT:
        st->pred = sa > 0.0f;
        st->prev = !st->pred;
        break;
    case S_PRED_SETGTE:
        st->pred = sa >= 0.0f;
        st->prev = !st->pred;
        break;
    case S_PRED_SET_INV:
        st->pred = sa == 1.0f;
        st->prev = st->pred ? 0.0f : (sa == 0.0f ? 1.0f : sa);
        break;
    case S_PRED_SET_POP: {
        float n = sa - 1.0f;

        st->pred = n <= 0.0f;
        st->prev = st->pred ? 0.0f : n;
        break;
    }
    case S_PRED_SET_CLR:
        st->pred = false;
        st->prev = FLT_MAX;
        break;
    case S_PRED_SET_RESTORE:
        st->pred = sa == 0.0f;
        st->prev = st->pred ? 0.0f : sa;
        break;
    case S_KILLE: case S_KILLGT: case S_KILLGTE: case S_KILLNE:
    case S_KILLONE: {
        bool kill;

        switch (sop) {
        case S_KILLE: kill = sa == 0.0f; break;
        case S_KILLGT: kill = sa > 0.0f; break;
        case S_KILLGTE: kill = sa >= 0.0f; break;
        case S_KILLNE: kill = sa != 0.0f; break;
        default: kill = sa == 1.0f; break;
        }
        if (kill && c->pout) {
            c->pout->killed = true;
        }
        st->prev = kill;
        break;
    }
    case S_SQRT:
        st->prev = sqrtf(sa);
        break;
    case S_SIN:
        st->prev = sinf(sa);
        break;
    case S_COS:
        st->prev = cosf(sa);
        break;
    default:
        break;
    }

    if ((w[0] >> 24) & 1) {
        for (int k = 0; k < 4; k++) {
            vr[k] = sat(vr[k]);
        }
    }
    sr = (w[0] >> 25) & 1 ? sat(st->prev) : st->prev;

    if (export) {
        uint32_t c1 = vmask & smask;
        uint32_t c0 = ((w[0] >> 14) & 1) ? 0xf & ~(vmask | smask) : 0;
        float ev[4];

        for (int k = 0; k < 4; k++) {
            uint32_t bit = 1u << k;

            if (vres_mask & bit) {
                ev[k] = vr[k];
            } else if (sres_mask & bit) {
                ev[k] = sr;
            } else if (c1 & bit) {
                ev[k] = 1.0f;
            } else {
                ev[k] = 0.0f;
            }
        }
        do_export(st, w[0] & 0x3f, ev, vres_mask | sres_mask | c0 | c1);
        return;
    }
    if (vres_mask) {
        float *d = temp_reg(st, w[0] & 0x3f, (w[0] >> 6) & 1);

        for (int k = 0; k < 4; k++) {
            if (vres_mask & (1 << k)) {
                d[k] = vr[k];
            }
        }
    }
    if (sres_mask) {
        float *d = temp_reg(st, (w[0] >> 8) & 0x3f, (w[0] >> 14) & 1);

        for (int k = 0; k < 4; k++) {
            if (sres_mask & (1 << k)) {
                d[k] = sr;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Fetch                                                               */
/* ------------------------------------------------------------------ */

static void fetch_store(SqState *st, const uint32_t *w, const float v[4])
{
    float *d = temp_reg(st, (w[0] >> 12) & 0x3f, (w[0] >> 18) & 1);
    uint32_t swiz = w[1] & 0xfff;

    for (int k = 0; k < 4; k++) {
        switch ((swiz >> (3 * k)) & 7) {
        case 0: case 1: case 2: case 3:
            d[k] = v[(swiz >> (3 * k)) & 3];
            break;
        case 5:
            d[k] = 1.0f;
            break;
        case 7:
            break;
        default:
            d[k] = 0.0f;
            break;
        }
    }
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

/*
 * Unpack nc components of the given bit widths (packed from bit 0 up) as
 * unsigned/signed, normalized or not.
 */
static void unpack_fixed(const uint32_t *dw, int nc, const uint8_t *bits,
                         bool sign, bool norm, float out[4])
{
    int pos = 0;

    for (int k = 0; k < nc; k++) {
        uint64_t raw = ((uint64_t)dw[(pos >> 5) + 1] << 32) | dw[pos >> 5];
        uint32_t v = (raw >> (pos & 31)) & ((1ull << bits[k]) - 1);

        if (sign) {
            int32_t sv = (int32_t)(v << (32 - bits[k])) >> (32 - bits[k]);

            out[k] = sv;
            if (norm) {
                out[k] = MAX(-1.0f, out[k] / (float)((1u << (bits[k] - 1)) - 1));
            }
        } else {
            out[k] = v;
            if (norm) {
                out[k] /= (float)((1ull << bits[k]) - 1);
            }
        }
        pos += bits[k];
    }
}

static void exec_vfetch(SqState *st, const uint32_t *w)
{
    MsmAdrenoState *s = st->c->s;
    bool mini = (w[1] >> 30) & 1;
    const uint32_t *full = mini ? st->vfetch_full : w;
    uint32_t cidx = ((full[0] >> 20) & 0x1f) * 3 + ((full[0] >> 25) & 3);
    uint32_t fc0 = s->regs[A2_SQ_FETCH_0 + cidx * 2];
    uint32_t fc1 = s->regs[A2_SQ_FETCH_0 + cidx * 2 + 1];
    uint32_t fmt = (w[1] >> 16) & 0x3f;
    bool sign = (w[1] >> 12) & 1;
    bool norm = !((w[1] >> 13) & 1);
    int exp_adj = ((int32_t)(w[1] << 2)) >> 26;
    int32_t offset = ((int32_t)(w[2] << 1)) >> 9;
    uint32_t dw[5] = {0};
    float v[4] = {0, 0, 0, 1};
    int ndw;

    if (!mini) {
        float *src = temp_reg(st, (w[0] >> 5) & 0x3f, (w[0] >> 11) & 1);
        float idx = src[(w[0] >> 30) & 3];
        uint32_t stride = w[2] & 0xff;

        if ((w[1] >> 15) & 1) {
            idx += 0.5f;
        }
        /* unlike Xenos, a2xx strides and offsets are in bytes */
        memcpy(st->vfetch_full, w, 12);
        st->vfetch_addr = (fc0 & ~3u) + stride * (uint32_t)(int)floorf(idx);
    }

    switch (fmt) {
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
    default:
        ndw = 4;
        break;
    }
    adreno_read(s, st->vfetch_addr + offset, dw, ndw * 4);
    for (int k = 0; k < ndw; k++) {
        dw[k] = le32_to_cpu(dw[k]);
        switch (fc1 & 3) {      /* endian swap */
        case 1:
            dw[k] = ((dw[k] & 0x00ff00ff) << 8) | ((dw[k] >> 8) & 0x00ff00ff);
            break;
        case 2:
            dw[k] = bswap32(dw[k]);
            break;
        case 3:
            dw[k] = (dw[k] << 16) | (dw[k] >> 16);
            break;
        }
    }

    switch (fmt) {
    case 2:     /* FMT_8 */
        unpack_fixed(dw, 1, (const uint8_t[]){8}, sign, norm, v);
        break;
    case 10:    /* FMT_8_8 */
        unpack_fixed(dw, 2, (const uint8_t[]){8, 8}, sign, norm, v);
        break;
    case 16:    /* FMT_8_8_8 */
        unpack_fixed(dw, 3, (const uint8_t[]){8, 8, 8}, sign, norm, v);
        break;
    case 6:     /* FMT_8_8_8_8 */
        unpack_fixed(dw, 4, (const uint8_t[]){8, 8, 8, 8}, sign, norm, v);
        break;
    case 7:     /* FMT_2_10_10_10 */
        unpack_fixed(dw, 4, (const uint8_t[]){10, 10, 10, 2}, sign, norm, v);
        break;
    case 21:    /* FMT_10_10_10_2 */
        unpack_fixed(dw, 4, (const uint8_t[]){2, 10, 10, 10}, sign, norm, v);
        break;
    case 24:    /* FMT_16 */
        unpack_fixed(dw, 1, (const uint8_t[]){16}, sign, norm, v);
        break;
    case 25:    /* FMT_16_16 */
        unpack_fixed(dw, 2, (const uint8_t[]){16, 16}, sign, norm, v);
        break;
    case 26:    /* FMT_16_16_16_16 */
        unpack_fixed(dw, 4, (const uint8_t[]){16, 16, 16, 16}, sign, norm,
                     v);
        break;
    case 30:    /* FMT_16_FLOAT */
        v[0] = half_to_float(dw[0]);
        break;
    case 31:    /* FMT_16_16_FLOAT */
        v[0] = half_to_float(dw[0]);
        v[1] = half_to_float(dw[0] >> 16);
        break;
    case 32:    /* FMT_16_16_16_16_FLOAT */
        v[0] = half_to_float(dw[0]);
        v[1] = half_to_float(dw[0] >> 16);
        v[2] = half_to_float(dw[1]);
        v[3] = half_to_float(dw[1] >> 16);
        break;
    case 33: case 34: case 35:  /* FMT_32[_32[_32_32]] */
        for (int k = 0; k < fmt - 32; k++) {
            if (sign) {
                v[k] = (int32_t)dw[k];
                if (norm) {
                    v[k] /= 2147483647.0f;
                }
            } else {
                v[k] = dw[k];
                if (norm) {
                    v[k] /= 4294967295.0f;
                }
            }
        }
        break;
    case 36: case 37: case 38: case 57:    /* 32-bit floats */
        for (int k = 0; k < ndw; k++) {
            v[k] = adreno_f(dw[k]);
        }
        break;
    default:
        adreno_trace(s, "sq: vertex format %u\n", fmt);
        break;
    }
    if (exp_adj) {
        for (int k = 0; k < 4; k++) {
            v[k] = ldexpf(v[k], exp_adj);
        }
    }
    fetch_store(st, w, v);
}

static void exec_tfetch(SqState *st, const uint32_t *w)
{
    uint32_t op = w[0] & 0x1f;
    float *src = temp_reg(st, (w[0] >> 5) & 0x3f, (w[0] >> 11) & 1);
    uint32_t sswiz = (w[0] >> 26) & 0x3f;
    float coord[4], out[4] = {0, 0, 0, 0};

    if (op != 1) {
        /* LOD/gradient queries and setters: not modelled */
        if (op < 24) {
            fetch_store(st, w, out);
        }
        return;
    }
    for (int k = 0; k < 3; k++) {
        coord[k] = src[(sswiz >> (2 * k)) & 3];
    }
    coord[3] = 0;
    adreno_sample_texture(st->c->s, (w[0] >> 20) & 0x1f, coord, w[1], w[2],
                          out);
    fetch_store(st, w, out);
}

/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

static void exec_clause(SqState *st, uint32_t addr, uint32_t count,
                        uint32_t seq)
{
    const AdrenoShaderCtx *c = st->c;

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t *w = &c->code[((addr + i) % ADRENO_IMEM_INSNS) * 3];
        if ((seq >> (2 * i)) & 1) {
            /* fetch */
            if (((w[1] >> 31) & 1) && (((w[2] >> 31) & 1) != st->pred)) {
                continue;
            }
            if ((w[0] & 0x1f) == 0) {
                exec_vfetch(st, w);
            } else {
                exec_tfetch(st, w);
            }
        } else {
            if (((w[1] >> 28) & 1) && (((w[1] >> 27) & 1) != st->pred)) {
                continue;
            }
            exec_alu(st, w);
        }
    }
}

void adreno_shader_run(AdrenoShaderCtx *c)
{
    SqState st = { .c = c };
    uint32_t pc = 0;
    int guard = 0;

    while (guard++ < 65536) {
        const uint32_t *pair = &c->code[3 * ((pc >> 1) % ADRENO_IMEM_INSNS)];
        uint32_t d0, d1, op;
        uint32_t next = pc + 1;

        if (pc & 1) {
            d0 = (pair[1] >> 16) | (pair[2] << 16);
            d1 = pair[2] >> 16;
        } else {
            d0 = pair[0];
            d1 = pair[1] & 0xffff;
        }
        op = (d1 >> 12) & 0xf;

        switch (op) {
        case CF_NOP:
        case CF_ALLOC:
        case CF_MARK_VS_FETCH_DONE:
            break;
        case CF_EXEC:
        case CF_EXEC_END:
        case CF_COND_EXEC:
        case CF_COND_EXEC_END:
        case CF_COND_PRED_EXEC:
        case CF_COND_PRED_EXEC_END:
        case CF_COND_EXEC_PRED_CLEAN:
        case CF_COND_EXEC_PRED_CLEAN_END: {
            bool run = true;

            if (op == CF_COND_EXEC || op == CF_COND_EXEC_END ||
                op == CF_COND_EXEC_PRED_CLEAN ||
                op == CF_COND_EXEC_PRED_CLEAN_END) {
                run = ((d1 >> 10) & 1) == bool_const(&st, (d1 >> 2) & 0xff);
            } else if (op == CF_COND_PRED_EXEC ||
                       op == CF_COND_PRED_EXEC_END) {
                run = ((d1 >> 10) & 1) == st.pred;
            }
            if (run) {
                exec_clause(&st, d0 & 0xfff, (d0 >> 12) & 7,
                            (d0 >> 16) & 0xfff);
            }
            if (op == CF_EXEC_END || op == CF_COND_EXEC_END ||
                op == CF_COND_PRED_EXEC_END ||
                op == CF_COND_EXEC_PRED_CLEAN_END) {
                return;
            }
            break;
        }
        case CF_LOOP_START: {
            uint32_t id = (d0 >> 16) & 0x1f;
            uint32_t lc = c->s->regs[A2_SQ_CF_LOOP + id];

            if (st.loop_depth >= 4) {
                next = d0 & 0x1fff;
                break;
            }
            st.loop_const[st.loop_depth] = lc;
            if (!((d0 >> 13) & 1)) {
                st.loop_iter[st.loop_depth] = 0;
            }
            if ((uint32_t)st.loop_iter[st.loop_depth] >= (lc & 0xff)) {
                next = d0 & 0x1fff;
                break;
            }
            st.loop_depth++;
            break;
        }
        case CF_LOOP_END: {
            int d = st.loop_depth - 1;
            uint32_t iter;

            if (d < 0) {
                break;
            }
            iter = ++st.loop_iter[d];
            if (iter < (st.loop_const[d] & 0xff) &&
                (!((d0 >> 21) & 1) || ((d1 >> 10) & 1) != st.pred)) {
                next = d0 & 0x1fff;
                break;
            }
            st.loop_depth--;
            break;
        }
        case CF_COND_CALL:
        case CF_COND_JMP: {
            bool take = true;

            if (!((d0 >> 13) & 1)) {
                if ((d0 >> 14) & 1) {
                    take = ((d1 >> 10) & 1) == st.pred;
                } else {
                    take = ((d1 >> 10) & 1) ==
                           bool_const(&st, (d1 >> 2) & 0xff);
                }
            }
            if (!take) {
                break;
            }
            if (op == CF_COND_CALL) {
                if (st.call_depth >= 4) {
                    break;
                }
                st.call_ret[st.call_depth++] = pc + 1;
            }
            next = d0 & 0x1fff;
            break;
        }
        case CF_RETURN:
            if (st.call_depth) {
                next = st.call_ret[--st.call_depth];
            }
            break;
        }
        pc = next;
    }
    adreno_trace(c->s, "sq: shader did not end\n");
}

const char *adreno_shader_disasm_type(int type)
{
    return type == A2_SHADER_VS ? "vs" : "ps";
}
