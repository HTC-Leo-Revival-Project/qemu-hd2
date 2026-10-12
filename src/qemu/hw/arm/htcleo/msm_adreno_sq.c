/*
 * Adreno 200 shader sequencer: an interpreter for a2xx shader microcode
 *
 * A shader is a list of control flow (CF) instructions, two per 96-bit
 * slot at the start of the program, followed by the 96-bit ALU and fetch
 * instructions the EXEC clauses run.  An ALU instruction co-issues a
 * vector operation (srcs 1-2[-3]) and a scalar operation (src 3); both
 * write temporaries or, with export_data set, the shader's outputs.
 *
 * Programs are decoded once and cached by content.  They run on AL lanes
 * at once (vertices, or a 4x4 block of pixels as four 2x2 quads), with a
 * mask of active lanes: predicated instructions and clauses narrow it,
 * loops drop the lanes that break, and a predicated jump or call that
 * splits the lanes runs both paths to the end separately.
 *
 * Formats and semantics follow freedreno (src/freedreno/ir2/instr-a2xx.h,
 * disasm-a2xx.c) and Xenia's Xenos interpreter (the Xbox 360 GPU is the
 * same design): src/xenia/gpu/ucode.h, shader_interpreter.cc.
 *
 * Exports: vertex shader 0-15 interpolators, 62 position, 63 point size;
 * pixel shader 0 colour, 61 depth.
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

/* sources the vector opcodes read */
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

/* ------------------------------------------------------------------ */
/* Decoded program                                                     */
/* ------------------------------------------------------------------ */

typedef struct SqSrc {
    uint8_t reg;            /* temporary 0-63 or constant 0-255 */
    bool temp, rel;         /* rel: temporary + aL */
    bool absv, neg;
    bool addressed, a0rel;  /* constants: + a0 or + aL */
    uint8_t swz[4];
} SqSrc;

typedef struct SqAlu {
    uint8_t vop, sop, nvsrc;
    bool vrun;              /* the vector op writes or changes state */
    uint8_t skind;          /* 0 none, 1 src3 .w/.x, 2 c#.w and r#.x */
    uint8_t vdst, sdst;
    bool vdst_rel, sdst_rel, export, vclamp, sclamp;
    uint8_t vres, sres, c0, c1;     /* write masks */
    SqSrc src[3];
    SqSrc sconst;           /* skind 2 */
    uint8_t streg, stcomp;
    bool sneg;
} SqAlu;

typedef struct SqFetch {
    bool vtx;
    uint8_t op;
    uint8_t dst, src;
    bool dst_rel, src_rel;
    uint8_t dswz[4];        /* 0-3 component, 4 zero, 5 one, 7 keep */
    /* vertex */
    uint16_t vconst;
    uint8_t fmt, scomp, stride;
    bool sign, norm, round;
    int8_t exp_adj;
    int32_t offset;
    /* texture */
    uint8_t sswz[3];
    AdrenoTexArgs tex;
} SqFetch;

typedef struct SqInsn {
    bool valid, fetch, predicated, pred_cond;
    union {
        SqAlu alu;
        SqFetch f;
    };
} SqInsn;

typedef struct SqCf {
    uint8_t op;
    uint16_t addr;
    uint8_t count;
    uint16_t seq;
    uint8_t bool_addr, loop_id;
    bool cond, repeat, pred_break, uncond, predicated;
} SqCf;

static void decode_src(SqSrc *sr, const uint32_t *w, int n)
{
    uint32_t reg = (w[2] >> (8 * (3 - n))) & 0xff;
    uint32_t swiz = (w[1] >> (8 * (3 - n))) & 0xff;
    bool t1 = (w[2] >> 31) & 1, t2 = (w[2] >> 30) & 1;

    memset(sr, 0, sizeof(*sr));
    sr->temp = (w[2] >> (32 - n)) & 1;
    sr->neg = (w[1] >> (27 - n)) & 1;
    if (sr->temp) {
        sr->reg = reg & 0x3f;
        sr->rel = reg & 0x40;
        sr->absv = reg & 0x80;
    } else {
        sr->reg = reg;
        sr->absv = (w[0] >> 7) & 1;
        /* const_0_rel_abs for the first constant, const_1 for later ones */
        switch (n) {
        case 1:
            sr->addressed = (w[1] >> 31) & 1;
            break;
        case 2:
            sr->addressed = t1 ? (w[1] >> 31) & 1 : (w[1] >> 30) & 1;
            break;
        default:
            sr->addressed = (t1 && t2) ? (w[1] >> 31) & 1 : (w[1] >> 30) & 1;
            break;
        }
        sr->a0rel = (w[1] >> 29) & 1;
    }
    for (int k = 0; k < 4; k++) {
        sr->swz[k] = ((swiz >> (2 * k)) + k) & 3;
    }
}

static void decode_alu(SqAlu *a, const uint32_t *w)
{
    uint32_t vmask = (w[0] >> 16) & 0xf, smask = (w[0] >> 20) & 0xf;

    a->vop = (w[2] >> 24) & 0x1f;
    a->sop = (w[0] >> 26) & 0x3f;
    a->export = (w[0] >> 15) & 1;
    a->vdst = w[0] & 0x3f;
    a->vdst_rel = (w[0] >> 6) & 1;
    a->sdst = (w[0] >> 8) & 0x3f;
    a->sdst_rel = (w[0] >> 14) & 1;
    a->vclamp = (w[0] >> 24) & 1;
    a->sclamp = (w[0] >> 25) & 1;
    a->vres = a->export ? vmask & ~smask : vmask;
    a->sres = a->export ? smask & ~vmask : smask;
    a->c1 = a->export ? vmask & smask : 0;
    a->c0 = (a->export && a->sdst_rel) ? 0xf & ~(vmask | smask) : 0;
    a->nvsrc = vec_srcs[a->vop];
    a->vrun = a->vres || (a->vop >= V_PRED_SETE_PUSH && a->vop <= V_KILLNE) ||
              a->vop == V_MOVA;
    for (int n = 1; n <= 3; n++) {
        decode_src(&a->src[n - 1], w, n);
    }
    if (a->sop >= S_MUL_CONST_0 && a->sop <= S_SUB_CONST_1) {
        uint32_t swiz = w[1] & 0xff;
        bool both_temp = ((w[2] >> 31) & 1) && ((w[2] >> 30) & 1);

        a->skind = 2;
        memset(&a->sconst, 0, sizeof(a->sconst));
        a->sconst.reg = w[2] & 0xff;
        a->sconst.addressed = both_temp ? (w[1] >> 31) & 1 : (w[1] >> 30) & 1;
        a->sconst.a0rel = (w[1] >> 29) & 1;
        a->sconst.swz[3] = ((swiz >> 6) + 3) & 3;
        a->streg = (a->sop & 1) | (((w[2] >> 29) & 1) << 1) | (swiz & 0x3c);
        a->stcomp = swiz & 3;
        a->sneg = (w[1] >> 24) & 1;
    } else if (a->sop == S_PRED_SET_CLR || a->sop == S_RETAIN_PREV ||
               a->sop == S_41 || a->sop > S_RETAIN_PREV) {
        a->skind = 0;
    } else {
        a->skind = 1;
    }
}

static void decode_fetch(SqFetch *f, const uint32_t *w)
{
    uint32_t dswz = w[1] & 0xfff;

    memset(f, 0, sizeof(*f));
    f->op = w[0] & 0x1f;
    f->vtx = f->op == 0;
    f->src = (w[0] >> 5) & 0x3f;
    f->src_rel = (w[0] >> 11) & 1;
    f->dst = (w[0] >> 12) & 0x3f;
    f->dst_rel = (w[0] >> 18) & 1;
    for (int k = 0; k < 4; k++) {
        f->dswz[k] = (dswz >> (3 * k)) & 7;
    }
    if (f->vtx) {
        /*
         * Xenos has "mini fetches" (dword 1 bit 30) that reuse the
         * previous fetch's constant and address.  The Adreno 200 blob sets
         * that bit on fetches that name their own constant, stride and
         * index, and freedreno documents it as reserved: every fetch is a
         * full one.  Strides and offsets are bytes, not dwords.
         */
        f->vconst = ((w[0] >> 20) & 0x1f) * 3 + ((w[0] >> 25) & 3);
        f->scomp = (w[0] >> 30) & 3;
        f->sign = (w[1] >> 12) & 1;
        f->norm = !((w[1] >> 13) & 1);
        f->round = (w[1] >> 15) & 1;
        f->fmt = (w[1] >> 16) & 0x3f;
        f->exp_adj = ((int32_t)(w[1] << 2)) >> 26;
        f->stride = w[2] & 0xff;
        f->offset = ((int32_t)(w[2] << 1)) >> 9;
    } else {
        uint32_t sswz = (w[0] >> 26) & 0x3f;

        for (int k = 0; k < 3; k++) {
            f->sswz[k] = (sswz >> (2 * k)) & 3;
        }
        f->tex.const_idx = (w[0] >> 20) & 0x1f;
        f->tex.unnormalized = (w[0] >> 25) & 1;
        f->tex.mag = (w[1] >> 12) & 3;
        f->tex.min = (w[1] >> 14) & 3;
        f->tex.mip = (w[1] >> 16) & 3;
        f->tex.use_reg_lod = (w[1] >> 29) & 1;
        f->tex.lod_bias = (((int32_t)(w[2] << 23)) >> 25) / 16.0f;
        f->tex.offset[0] = (((int32_t)(w[2] << 11)) >> 27) * 0.5f;
        f->tex.offset[1] = (((int32_t)(w[2] << 6)) >> 27) * 0.5f;
        f->tex.offset[2] = (((int32_t)(w[2] << 1)) >> 27) * 0.5f;
    }
}

static AdrenoProgram *decode_program(const uint32_t *code, uint32_t size,
                                     int type)
{
    AdrenoProgram *p = g_new0(AdrenoProgram, 1);
    uint32_t ninsn = MIN(size / 3 + 1, ADRENO_IMEM_INSNS);
    uint32_t ncf_max = 2 * ninsn;

    p->type = type;
    p->ninsn = ninsn;
    p->insn = g_new0(SqInsn, ninsn);
    p->cf = g_new0(SqCf, ncf_max);
    for (uint32_t i = 0; i < ncf_max; i++) {
        const uint32_t *pair = &code[3 * (i >> 1)];
        uint32_t d0, d1;
        SqCf *c = &p->cf[i];

        if (i & 1) {
            d0 = (pair[1] >> 16) | (pair[2] << 16);
            d1 = pair[2] >> 16;
        } else {
            d0 = pair[0];
            d1 = pair[1] & 0xffff;
        }
        c->op = (d1 >> 12) & 0xf;
        c->cond = (d1 >> 10) & 1;
        c->bool_addr = (d1 >> 2) & 0xff;
        p->ncf = i + 1;
        switch (c->op) {
        case CF_EXEC: case CF_EXEC_END: case CF_COND_EXEC:
        case CF_COND_EXEC_END: case CF_COND_PRED_EXEC:
        case CF_COND_PRED_EXEC_END: case CF_COND_EXEC_PRED_CLEAN:
        case CF_COND_EXEC_PRED_CLEAN_END:
            c->addr = d0 & 0xfff;
            c->count = (d0 >> 12) & 7;
            c->seq = (d0 >> 16) & 0xfff;
            for (int k = 0; k < c->count; k++) {
                uint32_t a = c->addr + k;
                SqInsn *in;
                const uint32_t *w;

                if (a >= ninsn) {
                    break;
                }
                in = &p->insn[a];
                w = &code[3 * a];
                in->valid = true;
                in->fetch = (c->seq >> (2 * k)) & 1;
                if (in->fetch) {
                    decode_fetch(&in->f, w);
                    in->predicated = (w[1] >> 31) & 1;
                    in->pred_cond = (w[2] >> 31) & 1;
                    if (!in->f.vtx && in->f.op == 1) {
                        p->tex_mask |= 1u << in->f.tex.const_idx;
                    }
                } else {
                    decode_alu(&in->alu, w);
                    in->predicated = (w[1] >> 28) & 1;
                    in->pred_cond = (w[1] >> 27) & 1;
                    if ((in->alu.vop >= V_KILLE && in->alu.vop <= V_KILLNE) ||
                        (in->alu.sop >= S_KILLE &&
                         in->alu.sop <= S_KILLONE)) {
                        p->kills = true;
                    }
                    if (in->alu.export && in->alu.vdst == 61) {
                        p->exports_depth = true;
                    }
                }
            }
            /* the CF list ends before the first clause */
            if (c->addr && 2u * c->addr < ncf_max) {
                ncf_max = 2 * c->addr;
            }
            break;
        case CF_LOOP_START:
        case CF_LOOP_END:
            c->addr = d0 & 0x1fff;
            c->repeat = (d0 >> 13) & 1;
            c->loop_id = (d0 >> 16) & 0x1f;
            c->pred_break = (d0 >> 21) & 1;
            break;
        case CF_COND_CALL:
        case CF_COND_JMP:
            c->addr = d0 & 0x1fff;
            c->uncond = (d0 >> 13) & 1;
            c->predicated = (d0 >> 14) & 1;
            break;
        }
    }
    return p;
}

static void free_program(gpointer data)
{
    AdrenoProgram *p = data;

    g_free(p->cf);
    g_free(p->insn);
    g_free(p);
}

/* The decoded program in instruction memory of this type, cached */
const AdrenoProgram *adreno_program(MsmAdrenoState *s, int type)
{
    const uint32_t *code = s->imem[type];
    uint32_t size = s->imem_size[type];
    uint64_t h = 0xcbf29ce484222325ull ^ ((uint64_t)type << 40) ^ size;
    AdrenoProgram *p;

    for (uint32_t i = 0; i < size; i++) {
        h = (h ^ code[i]) * 0x100000001b3ull;
    }
    if (!s->programs) {
        s->programs = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, free_program);
    }
    p = g_hash_table_lookup(s->programs, &h);
    if (!p) {
        uint64_t *key = g_new(uint64_t, 1);

        if (g_hash_table_size(s->programs) > 512) {
            g_hash_table_remove_all(s->programs);
        }
        p = decode_program(code, size, type);
        *key = h;
        g_hash_table_insert(s->programs, key, p);
    }
    return p;
}

void adreno_programs_free(MsmAdrenoState *s)
{
    if (s->programs) {
        g_hash_table_destroy(s->programs);
        s->programs = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

typedef struct SqExec {
    AdrenoLanes *L;
    const AdrenoProgram *p;
    const uint32_t *regs;
    uint32_t cbase;         /* SQ_VS_CONST / SQ_PS_CONST base */
    uint32_t pred;          /* per-lane predicate */
    int a0[AL];
    float prev[AL];
} SqExec;

typedef struct SqCtl {
    int loop_depth;
    int loop_iter[4];
    uint32_t loop_const[4];
    uint32_t loop_mask[4];
    int call_depth;
    uint32_t call_ret[4];
} SqCtl;

#define LANES(l) for (int l = 0; l < AL; l++)

static inline float sat(float f)
{
    return f > 1.0f ? 1.0f : (f >= 0.0f ? f : 0.0f);
}

/* D3D9-style multiply: 0 * anything (inf, nan) = 0 */
static inline float mul0(float a, float b)
{
    return (a != 0.0f && b != 0.0f) ? a * b : 0.0f;
}

static int loop_al(const SqCtl *ctl)
{
    uint32_t lc;

    if (!ctl->loop_depth) {
        return 0;
    }
    lc = ctl->loop_const[ctl->loop_depth - 1];
    return (int)((lc >> 8) & 0xff) +
           ctl->loop_iter[ctl->loop_depth - 1] * (int8_t)(lc >> 16);
}

static inline const float *const_ptr(SqExec *X, int idx)
{
    return (const float *)&X->regs[A2_SQ_CONSTANT_0 +
                                   ((idx + X->cbase) & 511) * 4];
}

static void read_src(SqExec *X, const SqSrc *sr, int al, float out[4][AL])
{
    AdrenoLanes *L = X->L;

    if (sr->temp) {
        int idx = (sr->reg + (sr->rel ? al : 0)) & 63;

        for (int k = 0; k < 4; k++) {
            memcpy(out[k], L->r[idx][sr->swz[k]], sizeof(float) * AL);
        }
    } else if (!sr->addressed || !sr->a0rel) {
        const float *c = const_ptr(X, sr->reg + (sr->addressed ? al : 0));

        for (int k = 0; k < 4; k++) {
            float v = c[sr->swz[k]];

            LANES(l) {
                out[k][l] = v;
            }
        }
    } else {
        LANES(l) {
            const float *c = const_ptr(X, sr->reg + X->a0[l]);

            for (int k = 0; k < 4; k++) {
                out[k][l] = c[sr->swz[k]];
            }
        }
    }
    if (sr->absv) {
        for (int k = 0; k < 4; k++) {
            LANES(l) {
                out[k][l] = fabsf(out[k][l]);
            }
        }
    }
    if (sr->neg) {
        for (int k = 0; k < 4; k++) {
            LANES(l) {
                out[k][l] = -out[k][l];
            }
        }
    }
}

static void write_lanes(float dst[4][AL], const float v[4][AL], uint32_t cm,
                        uint32_t mask)
{
    for (int k = 0; k < 4; k++) {
        if (!(cm & (1 << k))) {
            continue;
        }
        if (mask == (1u << AL) - 1) {
            memcpy(dst[k], v[k], sizeof(float) * AL);
        } else {
            LANES(l) {
                if (mask & (1u << l)) {
                    dst[k][l] = v[k][l];
                }
            }
        }
    }
}

static void do_export(SqExec *X, uint32_t reg, const float v[4][AL],
                      uint32_t cm, uint32_t mask)
{
    AdrenoLanes *L = X->L;

    if (X->p->type == A2_SHADER_VS) {
        if (reg == 62) {
            write_lanes(L->pos, v, cm, mask);
        } else if (reg == 63) {
            if (cm & 1) {
                LANES(l) {
                    if (mask & (1u << l)) {
                        L->psize[l] = v[0][l];
                    }
                }
            }
        } else if (reg < A2_MAX_INTERP) {
            write_lanes(L->interp[reg], v, cm, mask);
        }
    } else if (reg == 0) {
        write_lanes(L->color, v, cm, mask);
    } else if (reg == 61 && (cm & 1)) {
        LANES(l) {
            if (mask & (1u << l)) {
                L->depth[l] = v[0][l];
            }
        }
    }
}

/* lanes where v is == 0 (0), != 0 (1), > 0 (2), >= 0 (3) */
static uint32_t lanes_where(const float *v, int op)
{
    uint32_t m = 0;

    LANES(l) {
        bool b;

        switch (op) {
        case 0: b = v[l] == 0.0f; break;
        case 1: b = v[l] != 0.0f; break;
        case 2: b = v[l] > 0.0f; break;
        default: b = v[l] >= 0.0f; break;
        }
        m |= (uint32_t)b << l;
    }
    return m;
}

static void exec_vector(SqExec *X, const SqAlu *a, int al, uint32_t mask,
                        float vr[4][AL])
{
    AdrenoLanes *L = X->L;
    float s[3][4][AL];
    bool replicate = false;

    for (int n = 0; n < a->nvsrc; n++) {
        read_src(X, &a->src[n], al, s[n]);
    }
    switch (a->vop) {
#define VOP(expr) for (int k = 0; k < 4; k++) LANES(l) vr[k][l] = (expr)
    case V_ADD:  VOP(s[0][k][l] + s[1][k][l]); break;
    case V_MUL:  VOP(mul0(s[0][k][l], s[1][k][l])); break;
    case V_MAX:  VOP(s[0][k][l] >= s[1][k][l] ? s[0][k][l] : s[1][k][l]);
        break;
    case V_MIN:  VOP(s[0][k][l] < s[1][k][l] ? s[0][k][l] : s[1][k][l]);
        break;
    case V_SETE:  VOP(s[0][k][l] == s[1][k][l]); break;
    case V_SETGT: VOP(s[0][k][l] > s[1][k][l]); break;
    case V_SETGTE: VOP(s[0][k][l] >= s[1][k][l]); break;
    case V_SETNE: VOP(s[0][k][l] != s[1][k][l]); break;
    case V_FRAC:  VOP(s[0][k][l] - floorf(s[0][k][l])); break;
    case V_TRUNC: VOP(truncf(s[0][k][l])); break;
    case V_FLOOR: VOP(floorf(s[0][k][l])); break;
    case V_MULADD: VOP(mul0(s[0][k][l], s[1][k][l]) + s[2][k][l]); break;
    case V_CNDE:  VOP(s[0][k][l] == 0.0f ? s[1][k][l] : s[2][k][l]); break;
    case V_CNDGTE: VOP(s[0][k][l] >= 0.0f ? s[1][k][l] : s[2][k][l]); break;
    case V_CNDGT: VOP(s[0][k][l] > 0.0f ? s[1][k][l] : s[2][k][l]); break;
    case V_DOT4:
        LANES(l) {
            vr[0][l] = mul0(s[0][0][l], s[1][0][l]) +
                       mul0(s[0][1][l], s[1][1][l]) +
                       mul0(s[0][2][l], s[1][2][l]) +
                       mul0(s[0][3][l], s[1][3][l]);
        }
        replicate = true;
        break;
    case V_DOT3:
        LANES(l) {
            vr[0][l] = mul0(s[0][0][l], s[1][0][l]) +
                       mul0(s[0][1][l], s[1][1][l]) +
                       mul0(s[0][2][l], s[1][2][l]);
        }
        replicate = true;
        break;
    case V_DOT2ADD:
        LANES(l) {
            vr[0][l] = mul0(s[0][0][l], s[1][0][l]) +
                       mul0(s[0][1][l], s[1][1][l]) + s[2][0][l];
        }
        replicate = true;
        break;
    case V_CUBE:
        LANES(l) {
            /* operand a is .zzxy */
            float x = s[0][2][l], y = s[0][3][l], z = s[0][0][l];
            float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);

            if (az >= ax && az >= ay) {
                vr[0][l] = -y;
                vr[1][l] = z < 0 ? -x : x;
                vr[2][l] = 2 * z;
                vr[3][l] = z < 0 ? 5.0f : 4.0f;
            } else if (ay >= ax) {
                vr[0][l] = y < 0 ? -z : z;
                vr[1][l] = x;
                vr[2][l] = 2 * y;
                vr[3][l] = y < 0 ? 3.0f : 2.0f;
            } else {
                vr[0][l] = -y;
                vr[1][l] = x < 0 ? z : -z;
                vr[2][l] = 2 * x;
                vr[3][l] = x < 0 ? 1.0f : 0.0f;
            }
        }
        break;
    case V_MAX4:
        LANES(l) {
            vr[0][l] = MAX(MAX(s[0][0][l], s[0][1][l]),
                           MAX(s[0][2][l], s[0][3][l]));
        }
        replicate = true;
        break;
    case V_PRED_SETE_PUSH:
    case V_PRED_SETNE_PUSH:
    case V_PRED_SETGT_PUSH:
    case V_PRED_SETGTE_PUSH: {
        uint32_t p = 0;

        LANES(l) {
            float b3 = s[1][3][l], b0 = s[1][0][l];
            bool c3, c0;

            switch (a->vop) {
            case V_PRED_SETE_PUSH: c3 = b3 == 0; c0 = b0 == 0; break;
            case V_PRED_SETNE_PUSH: c3 = b3 != 0; c0 = b0 != 0; break;
            case V_PRED_SETGT_PUSH: c3 = b3 > 0; c0 = b0 > 0; break;
            default: c3 = b3 >= 0; c0 = b0 >= 0; break;
            }
            p |= (uint32_t)(s[0][3][l] == 0.0f && c3) << l;
            vr[0][l] = (s[0][0][l] == 0.0f && c0) ? 0.0f : s[0][0][l] + 1.0f;
        }
        X->pred = (X->pred & ~mask) | (p & mask);
        replicate = true;
        break;
    }
    case V_KILLE:
    case V_KILLGT:
    case V_KILLGTE:
    case V_KILLNE: {
        uint32_t kill = 0;

        LANES(l) {
            bool k = false;

            for (int c = 0; c < 4; c++) {
                float x = s[0][c][l], y = s[1][c][l];

                switch (a->vop) {
                case V_KILLE: k |= x == y; break;
                case V_KILLGT: k |= x > y; break;
                case V_KILLGTE: k |= x >= y; break;
                default: k |= x != y; break;
                }
            }
            kill |= (uint32_t)k << l;
            vr[0][l] = k;
        }
        L->killed |= kill & mask;
        replicate = true;
        break;
    }
    case V_DST:
        LANES(l) {
            vr[0][l] = 1.0f;
            vr[1][l] = mul0(s[0][1][l], s[1][1][l]);
            vr[2][l] = s[0][2][l];
            vr[3][l] = s[1][3][l];
        }
        break;
    case V_MOVA:
        LANES(l) {
            if (mask & (1u << l)) {
                X->a0[l] = (int)floorf(MIN(MAX(s[0][3][l], -256.0f), 255.0f) +
                                       0.5f);
            }
        }
        VOP(s[0][k][l] >= s[1][k][l] ? s[0][k][l] : s[1][k][l]);
        break;
    default:
        memset(vr, 0, sizeof(float) * 4 * AL);
        break;
#undef VOP
    }
    if (replicate) {
        memcpy(vr[1], vr[0], sizeof(vr[0]));
        memcpy(vr[2], vr[0], sizeof(vr[0]));
        memcpy(vr[3], vr[0], sizeof(vr[0]));
    }
    if (a->vclamp) {
        for (int k = 0; k < 4; k++) {
            LANES(l) {
                vr[k][l] = sat(vr[k][l]);
            }
        }
    }
}

static void exec_scalar(SqExec *X, const SqAlu *a, int al, uint32_t mask)
{
    AdrenoLanes *L = X->L;
    float sa[AL], sb[AL], r[AL];
    float *pv = X->prev;
    uint32_t pmask = 0;
    bool sets_pred = false;

    if (a->sop == S_RETAIN_PREV || a->sop == S_41 || a->sop > S_RETAIN_PREV) {
        return;
    }
    if (a->skind == 1) {
        float v[4][AL];

        read_src(X, &a->src[2], al, v);
        memcpy(sa, v[3], sizeof(sa));
        memcpy(sb, v[0], sizeof(sb));
    } else if (a->skind == 2) {
        float v[4][AL];

        read_src(X, &a->sconst, al, v);
        LANES(l) {
            sa[l] = a->sneg ? -v[3][l] : v[3][l];
            sb[l] = a->sneg ? -L->r[a->streg][a->stcomp][l]
                            : L->r[a->streg][a->stcomp][l];
        }
    } else {
        memset(sa, 0, sizeof(sa));
        memset(sb, 0, sizeof(sb));
    }

    switch (a->sop) {
#define SOP(expr) LANES(l) r[l] = (expr)
    case S_ADD: case S_ADD_CONST_0: case S_ADD_CONST_1:
        SOP(sa[l] + sb[l]); break;
    case S_ADD_PREV: SOP(sa[l] + pv[l]); break;
    case S_MUL: case S_MUL_CONST_0: case S_MUL_CONST_1:
        SOP(mul0(sa[l], sb[l])); break;
    case S_MUL_PREV: SOP(mul0(sa[l], pv[l])); break;
    case S_MUL_PREV2:
        SOP((pv[l] == -FLT_MAX || !isfinite(pv[l]) || !isfinite(sb[l]) ||
             sb[l] <= 0.0f) ? -FLT_MAX : mul0(sa[l], pv[l]));
        break;
    case S_MAX: SOP(sa[l] >= sb[l] ? sa[l] : sb[l]); break;
    case S_MIN: SOP(sa[l] < sb[l] ? sa[l] : sb[l]); break;
    case S_SETE: SOP(sa[l] == 0.0f); break;
    case S_SETGT: SOP(sa[l] > 0.0f); break;
    case S_SETGTE: SOP(sa[l] >= 0.0f); break;
    case S_SETNE: SOP(sa[l] != 0.0f); break;
    case S_FRAC: SOP(sa[l] - floorf(sa[l])); break;
    case S_TRUNC: SOP(truncf(sa[l])); break;
    case S_FLOOR: SOP(floorf(sa[l])); break;
    case S_EXP: SOP(exp2f(sa[l])); break;
    case S_LOG_CLAMP: SOP(sa[l] <= 0.0f ? -FLT_MAX : log2f(sa[l])); break;
    case S_LOG: SOP(log2f(sa[l])); break;
    case S_RECIP_CLAMP:
        SOP(sa[l] == 0.0f ? copysignf(FLT_MAX, sa[l]) : 1.0f / sa[l]); break;
    case S_RECIP_FF:
        SOP(sa[l] == 0.0f ? copysignf(0.0f, sa[l]) : 1.0f / sa[l]); break;
    case S_RECIP: SOP(1.0f / sa[l]); break;
    case S_RECIPSQ_CLAMP:
        SOP(sa[l] == 0.0f ? FLT_MAX : 1.0f / sqrtf(sa[l])); break;
    case S_RECIPSQ_FF: SOP(sa[l] == 0.0f ? 0.0f : 1.0f / sqrtf(sa[l])); break;
    case S_RECIPSQ: SOP(1.0f / sqrtf(sa[l])); break;
    case S_MOVA:
    case S_MOVA_FLOOR:
        LANES(l) {
            float c = MIN(MAX(sa[l], -256.0f), 255.0f);

            if (mask & (1u << l)) {
                X->a0[l] = (int)floorf(a->sop == S_MOVA ? c + 0.5f : c);
            }
            r[l] = sa[l] >= sb[l] ? sa[l] : sb[l];
        }
        break;
    case S_SUB: case S_SUB_CONST_0: case S_SUB_CONST_1:
        SOP(sa[l] - sb[l]); break;
    case S_SUB_PREV: SOP(sa[l] - pv[l]); break;
    case S_PRED_SETE:
    case S_PRED_SETNE:
    case S_PRED_SETGT:
    case S_PRED_SETGTE:
        pmask = lanes_where(sa, a->sop == S_PRED_SETE ? 0 :
                                a->sop == S_PRED_SETNE ? 1 :
                                a->sop == S_PRED_SETGT ? 2 : 3);
        sets_pred = true;
        SOP(((pmask >> l) & 1) ? 0.0f : 1.0f);
        break;
    case S_PRED_SET_INV:
        LANES(l) {
            bool p = sa[l] == 1.0f;

            pmask |= (uint32_t)p << l;
            r[l] = p ? 0.0f : (sa[l] == 0.0f ? 1.0f : sa[l]);
        }
        sets_pred = true;
        break;
    case S_PRED_SET_POP:
        LANES(l) {
            float n = sa[l] - 1.0f;
            bool p = n <= 0.0f;

            pmask |= (uint32_t)p << l;
            r[l] = p ? 0.0f : n;
        }
        sets_pred = true;
        break;
    case S_PRED_SET_CLR:
        SOP(FLT_MAX);
        sets_pred = true;
        break;
    case S_PRED_SET_RESTORE:
        pmask = lanes_where(sa, 0);
        sets_pred = true;
        SOP(((pmask >> l) & 1) ? 0.0f : sa[l]);
        break;
    case S_KILLE: case S_KILLGT: case S_KILLGTE: case S_KILLNE:
    case S_KILLONE: {
        uint32_t kill = 0;

        switch (a->sop) {
        case S_KILLE: kill = lanes_where(sa, 0); break;
        case S_KILLGT: kill = lanes_where(sa, 2); break;
        case S_KILLGTE: kill = lanes_where(sa, 3); break;
        case S_KILLNE: kill = lanes_where(sa, 1); break;
        default:
            LANES(l) {
                kill |= (uint32_t)(sa[l] == 1.0f) << l;
            }
            break;
        }
        L->killed |= kill & mask;
        SOP((float)((kill >> l) & 1));
        break;
    }
    case S_SQRT: SOP(sqrtf(sa[l])); break;
    case S_SIN: SOP(sinf(sa[l])); break;
    case S_COS: SOP(cosf(sa[l])); break;
    default: SOP(0.0f); break;
#undef SOP
    }
    if (sets_pred) {
        X->pred = (X->pred & ~mask) | (pmask & mask);
    }
    LANES(l) {
        if (mask & (1u << l)) {
            pv[l] = r[l];
        }
    }
}

static void exec_alu(SqExec *X, const SqAlu *a, int al, uint32_t mask)
{
    AdrenoLanes *L = X->L;
    float vr[4][AL];
    float sr[4][AL];

    if (a->vrun) {
        exec_vector(X, a, al, mask, vr);
    }
    exec_scalar(X, a, al, mask);
    if (a->sres || a->export) {
        LANES(l) {
            float v = a->sclamp ? sat(X->prev[l]) : X->prev[l];

            sr[0][l] = sr[1][l] = sr[2][l] = sr[3][l] = v;
        }
    }

    if (a->export) {
        float ev[4][AL];

        for (int k = 0; k < 4; k++) {
            uint32_t bit = 1u << k;

            if (a->vres & bit) {
                memcpy(ev[k], vr[k], sizeof(ev[k]));
            } else if (a->sres & bit) {
                memcpy(ev[k], sr[k], sizeof(ev[k]));
            } else {
                float c = (a->c1 & bit) ? 1.0f : 0.0f;

                LANES(l) {
                    ev[k][l] = c;
                }
            }
        }
        do_export(X, a->vdst, (const float (*)[AL])ev,
                  a->vres | a->sres | a->c0 | a->c1, mask);
        return;
    }
    if (a->vres) {
        write_lanes(L->r[(a->vdst + (a->vdst_rel ? al : 0)) & 63],
                    (const float (*)[AL])vr, a->vres, mask);
    }
    if (a->sres) {
        write_lanes(L->r[(a->sdst + (a->sdst_rel ? al : 0)) & 63],
                    (const float (*)[AL])sr, a->sres, mask);
    }
}

static void fetch_store(SqExec *X, const SqFetch *f, int al,
                        const float v[4][AL], uint32_t mask)
{
    float (*d)[AL] = X->L->r[(f->dst + (f->dst_rel ? al : 0)) & 63];

    for (int k = 0; k < 4; k++) {
        unsigned sel = f->dswz[k];

        if (sel == 7) {
            continue;
        }
        LANES(l) {
            if (mask & (1u << l)) {
                d[k][l] = sel < 4 ? v[sel][l] : (sel == 5 ? 1.0f : 0.0f);
            }
        }
    }
}

/*
 * Unpack nc components of the given bit widths (packed from bit 0 up) as
 * unsigned/signed, normalized or not.
 */
static void unpack_fixed(const uint32_t *dw, int nc, const uint8_t *bits,
                         bool sign, bool norm, float *out)
{
    int pos = 0;

    for (int k = 0; k < nc; k++) {
        uint64_t raw = ((uint64_t)dw[(pos >> 5) + 1] << 32) | dw[pos >> 5];
        uint32_t v = (raw >> (pos & 31)) & ((1ull << bits[k]) - 1);

        if (sign) {
            int32_t sv = (int32_t)(v << (32 - bits[k])) >> (32 - bits[k]);

            out[k] = sv;
            if (norm) {
                out[k] = MAX(-1.0f, out[k] /
                                    (float)((1u << (bits[k] - 1)) - 1));
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

static int vfetch_dwords(unsigned fmt)
{
    switch (fmt) {
    case 2: case 10: case 6: case 24: case 25: case 7: case 21: case 16:
    case 30: case 31: case 33: case 36:
        return 1;
    case 26: case 32: case 34: case 37:
        return 2;
    case 57:
        return 3;
    }
    return 4;
}

static void decode_vertex(const SqFetch *f, uint32_t *dw, float *v)
{
    switch (f->fmt) {
    case 2:     /* FMT_8 */
        unpack_fixed(dw, 1, (const uint8_t[]){8}, f->sign, f->norm, v);
        break;
    case 10:    /* FMT_8_8 */
        unpack_fixed(dw, 2, (const uint8_t[]){8, 8}, f->sign, f->norm, v);
        break;
    case 16:    /* FMT_8_8_8 */
        unpack_fixed(dw, 3, (const uint8_t[]){8, 8, 8}, f->sign, f->norm, v);
        break;
    case 6:     /* FMT_8_8_8_8 */
        unpack_fixed(dw, 4, (const uint8_t[]){8, 8, 8, 8}, f->sign, f->norm,
                     v);
        break;
    case 7:     /* FMT_2_10_10_10 */
        unpack_fixed(dw, 4, (const uint8_t[]){10, 10, 10, 2}, f->sign,
                     f->norm, v);
        break;
    case 21:    /* FMT_10_10_10_2 */
        unpack_fixed(dw, 4, (const uint8_t[]){2, 10, 10, 10}, f->sign,
                     f->norm, v);
        break;
    case 24:    /* FMT_16 */
        unpack_fixed(dw, 1, (const uint8_t[]){16}, f->sign, f->norm, v);
        break;
    case 25:    /* FMT_16_16 */
        unpack_fixed(dw, 2, (const uint8_t[]){16, 16}, f->sign, f->norm, v);
        break;
    case 26:    /* FMT_16_16_16_16 */
        unpack_fixed(dw, 4, (const uint8_t[]){16, 16, 16, 16}, f->sign,
                     f->norm, v);
        break;
    case 30:    /* FMT_16_FLOAT */
        v[0] = adreno_half_to_float(dw[0]);
        break;
    case 31:    /* FMT_16_16_FLOAT */
        v[0] = adreno_half_to_float(dw[0]);
        v[1] = adreno_half_to_float(dw[0] >> 16);
        break;
    case 32:    /* FMT_16_16_16_16_FLOAT */
        v[0] = adreno_half_to_float(dw[0]);
        v[1] = adreno_half_to_float(dw[0] >> 16);
        v[2] = adreno_half_to_float(dw[1]);
        v[3] = adreno_half_to_float(dw[1] >> 16);
        break;
    case 33: case 34: case 35:  /* FMT_32[_32[_32_32]] */
        for (int k = 0; k < f->fmt - 32; k++) {
            v[k] = f->sign ? (float)(int32_t)dw[k] : (float)dw[k];
            if (f->norm) {
                v[k] /= f->sign ? 2147483647.0f : 4294967295.0f;
            }
        }
        break;
    case 36: case 37: case 38: case 57:    /* 32-bit floats */
        for (int k = 0; k < vfetch_dwords(f->fmt); k++) {
            v[k] = adreno_f(dw[k]);
        }
        break;
    }
}

static void exec_vfetch(SqExec *X, const SqFetch *f, int al, uint32_t mask)
{
    AdrenoLanes *L = X->L;
    uint32_t fc0 = X->regs[A2_SQ_FETCH_0 + f->vconst * 2];
    uint32_t fc1 = X->regs[A2_SQ_FETCH_0 + f->vconst * 2 + 1];
    float (*src)[AL] = L->r[(f->src + (f->src_rel ? al : 0)) & 63];
    int ndw = vfetch_dwords(f->fmt);
    float v[4][AL];

    LANES(l) {
        uint32_t dw[5] = {0};
        float o[4] = {0, 0, 0, 1};
        float idx;

        if (!(mask & (1u << l))) {
            continue;
        }
        idx = src[f->scomp][l] + (f->round ? 0.5f : 0.0f);
        adreno_tlb_read(L->s, L->tlb, (fc0 & ~3u) +
                        f->stride * (uint32_t)(int)floorf(idx) + f->offset,
                        dw, ndw * 4);
        for (int k = 0; k < ndw; k++) {
            dw[k] = le32_to_cpu(dw[k]);
            switch (fc1 & 3) {      /* endian swap */
            case 1:
                dw[k] = ((dw[k] & 0x00ff00ff) << 8) |
                        ((dw[k] >> 8) & 0x00ff00ff);
                break;
            case 2:
                dw[k] = bswap32(dw[k]);
                break;
            case 3:
                dw[k] = (dw[k] << 16) | (dw[k] >> 16);
                break;
            }
        }
        decode_vertex(f, dw, o);
        for (int k = 0; k < 4; k++) {
            v[k][l] = f->exp_adj ? ldexpf(o[k], f->exp_adj) : o[k];
        }
    }
    fetch_store(X, f, al, (const float (*)[AL])v, mask);
}

static void exec_tfetch(SqExec *X, const SqFetch *f, int al, uint32_t mask)
{
    AdrenoLanes *L = X->L;
    float (*src)[AL] = L->r[(f->src + (f->src_rel ? al : 0)) & 63];
    float coord[3][AL], out[4][AL];

    if (f->op != 1) {
        /* LOD/gradient queries and setters: not modelled */
        if (f->op < 24) {
            memset(out, 0, sizeof(out));
            fetch_store(X, f, al, (const float (*)[AL])out, mask);
        }
        return;
    }
    for (int k = 0; k < 3; k++) {
        memcpy(coord[k], src[f->sswz[k]], sizeof(coord[k]));
    }
    adreno_sample(L, &f->tex, coord, mask, out);
    fetch_store(X, f, al, (const float (*)[AL])out, mask);
}

static void exec_clause(SqExec *X, const SqCf *c, const SqCtl *ctl,
                        uint32_t mask)
{
    const AdrenoProgram *p = X->p;
    int al = loop_al(ctl);

    for (int i = 0; i < c->count; i++) {
        uint32_t a = c->addr + i;
        const SqInsn *in;
        uint32_t m = mask;

        if (a >= p->ninsn) {
            return;
        }
        in = &p->insn[a];
        if (!in->valid) {
            continue;
        }
        if (in->predicated) {
            m &= in->pred_cond ? X->pred : ~X->pred;
            if (!m) {
                continue;
            }
        }
        if (in->fetch) {
            if (in->f.vtx) {
                exec_vfetch(X, &in->f, al, m);
            } else {
                exec_tfetch(X, &in->f, al, m);
            }
        } else {
            exec_alu(X, &in->alu, al, m);
        }
    }
}

static bool bool_const(SqExec *X, uint32_t addr)
{
    return (X->regs[A2_SQ_CF_BOOLEANS + (addr >> 5)] >> (addr & 31)) & 1;
}

static void run(SqExec *X, uint32_t pc, uint32_t mask, SqCtl ctl, int depth)
{
    const AdrenoProgram *p = X->p;
    int guard = 0;

    while (mask && pc < p->ncf && guard++ < 65536) {
        const SqCf *c = &p->cf[pc];
        uint32_t next = pc + 1;

        switch (c->op) {
        case CF_EXEC: case CF_EXEC_END:
        case CF_COND_EXEC: case CF_COND_EXEC_END:
        case CF_COND_PRED_EXEC: case CF_COND_PRED_EXEC_END:
        case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END: {
            uint32_t m = mask;

            if (c->op == CF_COND_EXEC || c->op == CF_COND_EXEC_END ||
                c->op == CF_COND_EXEC_PRED_CLEAN ||
                c->op == CF_COND_EXEC_PRED_CLEAN_END) {
                if (c->cond != bool_const(X, c->bool_addr)) {
                    m = 0;
                }
            } else if (c->op == CF_COND_PRED_EXEC ||
                       c->op == CF_COND_PRED_EXEC_END) {
                m &= c->cond ? X->pred : ~X->pred;
            }
            if (m) {
                exec_clause(X, c, &ctl, m);
            }
            if (c->op == CF_EXEC_END || c->op == CF_COND_EXEC_END ||
                c->op == CF_COND_PRED_EXEC_END ||
                c->op == CF_COND_EXEC_PRED_CLEAN_END) {
                return;
            }
            break;
        }
        case CF_LOOP_START: {
            uint32_t lc = X->regs[A2_SQ_CF_LOOP + c->loop_id];
            int d = ctl.loop_depth;

            if (d >= 4) {
                next = c->addr;
                break;
            }
            ctl.loop_const[d] = lc;
            if (!c->repeat) {
                ctl.loop_iter[d] = 0;
            }
            if ((uint32_t)ctl.loop_iter[d] >= (lc & 0xff)) {
                next = c->addr;
                break;
            }
            ctl.loop_mask[d] = mask;
            ctl.loop_depth++;
            break;
        }
        case CF_LOOP_END: {
            int d = ctl.loop_depth - 1;
            uint32_t cont = mask;

            if (d < 0) {
                break;
            }
            ctl.loop_iter[d]++;
            if (c->pred_break) {
                /* lanes whose predicate equals the condition leave */
                cont &= c->cond ? ~X->pred : X->pred;
            }
            if ((uint32_t)ctl.loop_iter[d] < (ctl.loop_const[d] & 0xff) &&
                cont) {
                mask = cont;
                next = c->addr;
                break;
            }
            mask = ctl.loop_mask[d];
            ctl.loop_depth--;
            break;
        }
        case CF_COND_CALL:
        case CF_COND_JMP: {
            uint32_t take = mask;

            if (!c->uncond) {
                if (c->predicated) {
                    take &= c->cond ? X->pred : ~X->pred;
                } else if (c->cond != bool_const(X, c->bool_addr)) {
                    take = 0;
                }
            }
            if (!take) {
                break;
            }
            if (c->op == CF_COND_CALL) {
                if (ctl.call_depth >= 4) {
                    break;
                }
                ctl.call_ret[ctl.call_depth++] = pc + 1;
            }
            if (take != mask) {
                /* the lanes split: the taken ones run on by themselves */
                if (depth < 8) {
                    run(X, c->addr, take, ctl, depth + 1);
                }
                if (c->op == CF_COND_CALL) {
                    ctl.call_depth--;
                }
                mask &= ~take;
                break;
            }
            next = c->addr;
            break;
        }
        case CF_RETURN:
            if (ctl.call_depth) {
                next = ctl.call_ret[--ctl.call_depth];
            }
            break;
        default:
            break;
        }
        pc = next;
    }
}

void adreno_shader_exec(AdrenoLanes *L, uint32_t mask)
{
    MsmAdrenoState *s = L->s;
    SqExec X = {
        .L = L, .p = L->prog, .regs = s->regs,
        .cbase = s->regs[L->prog->type == A2_SHADER_VS ? A2_SQ_VS_CONST
                                                      : A2_SQ_PS_CONST] &
                 0x1ff,
    };
    SqCtl ctl = { 0 };

    L->killed = 0;
    run(&X, 0, mask, ctl, 0);
}
