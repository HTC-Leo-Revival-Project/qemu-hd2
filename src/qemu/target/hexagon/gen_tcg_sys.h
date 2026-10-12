/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HEXAGON_GEN_TCG_SYS_H
#define HEXAGON_GEN_TCG_SYS_H

/* System mode instructions */
#define fGEN_TCG_Y2_swi(SHORTCODE) \
    gen_helper_swi(tcg_env, RsV)

#define fGEN_TCG_Y2_cswi(SHORTCODE) \
    gen_helper_cswi(tcg_env, RsV)

#define fGEN_TCG_Y2_ciad(SHORTCODE) \
    gen_helper_ciad(tcg_env, RsV)

#define fGEN_TCG_Y4_siad(SHORTCODE) \
    gen_helper_siad(tcg_env, RsV)

#define fGEN_TCG_Y2_iassignw(SHORTCODE) \
    gen_helper_iassignw(tcg_env, RsV)

#define fGEN_TCG_Y2_iassignr(SHORTCODE) \
    gen_helper_iassignr(RdV, tcg_env, RsV)

#define fGEN_TCG_Y2_getimask(SHORTCODE) \
    gen_helper_getimask(RdV, tcg_env, RsV)

#define fGEN_TCG_Y2_setimask(SHORTCODE) \
    gen_helper_setimask(tcg_env, PtV, RsV)

#define fGEN_TCG_Y2_setprio(SHORTCODE) \
    gen_helper_setprio(tcg_env, PtV, RsV)

#define fGEN_TCG_Y2_crswap0(SHORTCODE) \
    do { \
        TCGv_i32 tmp = tcg_temp_new_i32(); \
        tcg_gen_mov_tl(tmp, RxV); \
        tcg_gen_mov_tl(RxV, hex_t_sreg[HEX_SREG_SGP0]); \
        if (ctx->need_commit) { \
            tcg_gen_mov_tl(ctx->t_sreg_new_value[HEX_SREG_SGP0], tmp); \
        } else { \
            tcg_gen_mov_tl(hex_t_sreg[HEX_SREG_SGP0], tmp); \
        } \
    } while (0)

#define fGEN_TCG_Y4_crswap1(SHORTCODE) \
    do { \
        TCGv_i32 tmp = tcg_temp_new_i32(); \
        tcg_gen_mov_tl(tmp, RxV); \
        tcg_gen_mov_tl(RxV, hex_t_sreg[HEX_SREG_SGP1]); \
        if (ctx->need_commit) { \
            tcg_gen_mov_tl(ctx->t_sreg_new_value[HEX_SREG_SGP1], tmp); \
        } else { \
            tcg_gen_mov_tl(hex_t_sreg[HEX_SREG_SGP1], tmp); \
        } \
    } while (0)

#define fGEN_TCG_Y4_crswap10(SHORTCODE) \
    do { \
        TCGv_i64 tmp = tcg_temp_new_i64(); \
        tcg_gen_mov_i64(tmp, RxxV); \
        tcg_gen_concat_i32_i64(RxxV, \
                               hex_t_sreg[HEX_SREG_SGP0], \
                               hex_t_sreg[HEX_SREG_SGP1]); \
        if (ctx->need_commit) { \
            tcg_gen_extrl_i64_i32( \
                ctx->t_sreg_new_value[HEX_SREG_SGP0], tmp); \
            tcg_gen_extrh_i64_i32( \
                ctx->t_sreg_new_value[HEX_SREG_SGP1], tmp); \
        } else { \
            tcg_gen_extrl_i64_i32(hex_t_sreg[HEX_SREG_SGP0], tmp); \
            tcg_gen_extrh_i64_i32(hex_t_sreg[HEX_SREG_SGP1], tmp); \
        } \
    } while (0)

#define fGEN_TCG_Y2_wait(SHORTCODE) \
    do { \
        RsV = RsV; \
        gen_helper_wait(tcg_env, tcg_constant_tl(ctx->pkt.pc)); \
    } while (0)

#define fGEN_TCG_Y2_resume(SHORTCODE) \
    gen_helper_resume(tcg_env, RsV)

#define fGEN_TCG_Y2_start(SHORTCODE) \
    gen_helper_start(tcg_env, RsV)

#define fGEN_TCG_Y2_stop(SHORTCODE) \
    do { \
        RsV = RsV; \
        gen_helper_stop(tcg_env); \
    } while (0)

/*
 * TLB access.  QDSP6 V2 has the same encodings without operands: the entry
 * is in vid/s22 and the index in s23, so the GPR operands the V5 decoding
 * finds are left as they are.
 */
#define fGEN_TCG_Y2_tlbw(SHORTCODE) \
    do { \
        if (ctx->is_v2) { \
            gen_helper_v2_tlbw(tcg_env); \
        } else { \
            gen_helper_tlbw(tcg_env, RssV, RtV); \
        } \
    } while (0)

#define fGEN_TCG_Y2_tlbr(SHORTCODE) \
    do { \
        if (ctx->is_v2) { \
            gen_helper_v2_tlbr(tcg_env); \
            tcg_gen_concat_i32_i64(RddV, hex_gpr[RddN], hex_gpr[RddN + 1]); \
        } else { \
            gen_helper_tlbr(RddV, tcg_env, RsV); \
        } \
    } while (0)

#define fGEN_TCG_Y2_tlbp(SHORTCODE) \
    do { \
        if (ctx->is_v2) { \
            gen_helper_v2_tlbp(tcg_env); \
            tcg_gen_mov_tl(RdV, hex_gpr[RdN]); \
        } else { \
            gen_helper_tlbp(RdV, tcg_env, RsV); \
        } \
    } while (0)

#define fGEN_TCG_Y2_tfrscrr(SHORTCODE) \
    do { \
        if (ctx->is_v2 && SsN == HEX_SREG_SSR) { \
            TCGv_i32 hi = tcg_temp_new_i32(); \
            tcg_gen_ld_i32(hi, tcg_env, \
                           offsetof(CPUHexagonState, v2_ssr_hi)); \
            tcg_gen_andi_tl(RdV, SsV, 0x0007ffff); \
            tcg_gen_or_tl(RdV, RdV, hi); \
        } else { \
            tcg_gen_mov_tl(RdV, SsV); \
        } \
    } while (0)

#define fGEN_TCG_Y2_tfrsrcr(SHORTCODE) \
    tcg_gen_mov_tl(SdV, RsV)

#define fGEN_TCG_Y4_tfrscpp(SHORTCODE) \
    tcg_gen_mov_i64(RddV, SssV)

#define fGEN_TCG_Y4_tfrspcp(SHORTCODE) \
    tcg_gen_mov_i64(SddV, RssV)

#define fGEN_TCG_G4_tfrgcrr(SHORTCODE) \
    tcg_gen_mov_tl(RdV, GsV)

#define fGEN_TCG_G4_tfrgrcr(SHORTCODE) \
    tcg_gen_mov_tl(GdV, RsV)

#define fGEN_TCG_G4_tfrgcpp(SHORTCODE) \
    tcg_gen_mov_i64(RddV, GssV)

#define fGEN_TCG_G4_tfrgpcp(SHORTCODE) \
    tcg_gen_mov_i64(GddV, RssV)


/*
 * rte (return from exception)
 *     Clear the EX bit in SSR
 *     Jump to ELR
 */
#define fGEN_TCG_J2_rte(SHORTCODE) \
    do { \
        TCGv_i32 new_ssr = tcg_temp_new_i32(); \
        tcg_gen_deposit_tl(new_ssr, hex_t_sreg[HEX_SREG_SSR], \
                           tcg_constant_tl(0), \
                           reg_field_info[SSR_EX].offset, \
                           reg_field_info[SSR_EX].width); \
        gen_log_sreg_write(ctx, HEX_SREG_SSR, new_ssr); \
        gen_jumpr(ctx, hex_t_sreg[ctx->is_v2 ? HEX_SREG_BADVA1 : HEX_SREG_ELR]); \
    } while (0)

#endif
