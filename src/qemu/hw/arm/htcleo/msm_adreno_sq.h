/*
 * Adreno 200 shader sequencer: decoded a2xx microcode, shared by the
 * interpreter (msm_adreno_sq.c) and the GLSL translator (msm_adreno_glsl.c)
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_HTCLEO_MSM_ADRENO_SQ_H
#define HW_ARM_HTCLEO_MSM_ADRENO_SQ_H

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

#endif
