#!/usr/bin/env python3

##
##  Copyright(c) 2022-2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
##
##  This program is free software; you can redistribute it and/or modify
##  it under the terms of the GNU General Public License as published by
##  the Free Software Foundation; either version 2 of the License, or
##  (at your option) any later version.
##
##  This program is distributed in the hope that it will be useful,
##  but WITHOUT ANY WARRANTY; without even the implied warranty of
##  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##  GNU General Public License for more details.
##
##  You should have received a copy of the GNU General Public License
##  along with this program; if not, see <http://www.gnu.org/licenses/>.
##

import sys
import re
import string
import hex_common

##
## Generate the code to analyze the instruction
##     For A2_add: Rd32=add(Rs32,Rt32), { RdV=RsV+RtV;}
##     We produce:
##     static void analyze_A2_add(DisasContext *ctx)
##     {
##         Insn *insn G_GNUC_UNUSED = ctx->insn;
##         const int RdN = insn->regno[0];
##         ctx_log_reg_write(ctx, RdN, false);
##         const int RsN = insn->regno[1];
##         ctx_log_reg_read(ctx, RsN);
##         const int RtN = insn->regno[2];
##         ctx_log_reg_read(ctx, RtN);
##     }
##
def gen_analyze_func(f, tag, regs, imms):
    f.write(f"static void analyze_{tag}(DisasContext *ctx)\n")
    f.write("{\n")

    if hex_common.tag_ignore(tag):
        f.write("}\n\n")
        return

    if hex_common.is_sysemu_tag(tag):
        f.write("#ifndef CONFIG_USER_ONLY\n")

    f.write("    Insn *insn G_GNUC_UNUSED = ctx->insn;\n")
    if (hex_common.is_hvx_insn(tag)):
        if hex_common.has_hvx_helper(tag):
            f.write(
                "    const bool G_GNUC_UNUSED insn_has_hvx_helper = true;\n"
            )
            f.write("    ctx_start_hvx_insn(ctx);\n")
        else:
            f.write(
                "    const bool G_GNUC_UNUSED insn_has_hvx_helper = false;\n"
            )

    ## Declare all the registers
    for regno, register in enumerate(regs):
        reg_type, reg_id = register
        reg = hex_common.get_register(tag, reg_type, reg_id)
        if reg.is_read() or reg.is_written():
            reg.decl_reg_num(f, regno)

    ## Analyze the register reads
    for regno, register in enumerate(regs):
        reg_type, reg_id = register
        reg = hex_common.get_register(tag, reg_type, reg_id)
        if reg.is_read():
            reg.analyze_read(f, regno)

    f.write("    mark_implicit_reads(ctx);\n")

    ## Analyze the register writes
    for regno, register in enumerate(regs):
        reg_type, reg_id = register
        reg = hex_common.get_register(tag, reg_type, reg_id)
        if reg.is_written():
            reg.analyze_write(f, tag, regno)

    f.write("    mark_implicit_writes(ctx);\n")

    ## Without a commit the destination is written straight into the
    ## register while the instruction runs.  When it is also one of the
    ## instruction's sources, generated code that writes the destination
    ## before its last use of the source reads the clobbered value
    ## (Rd = cmpy(Rd, Rt) on V2 code, among others), so commit.
    for register in regs:
        reg_type, reg_id = register
        wreg = hex_common.get_register(tag, reg_type, reg_id)
        if reg_type != "R" or not wreg.is_written() or wreg.is_read():
            continue
        wlen = 2 if wreg.is_pair() else 1
        for register2 in regs:
            reg_type2, reg_id2 = register2
            rreg = hex_common.get_register(tag, reg_type2, reg_id2)
            if reg_type2 != "R" or not rreg.is_read() or rreg.is_written():
                continue
            rlen = 2 if rreg.is_pair() else 1
            f.write(f"    if ({wreg.reg_num} < {rreg.reg_num} + {rlen} && "
                    f"{rreg.reg_num} < {wreg.reg_num} + {wlen}) {{\n")
            f.write("        ctx->read_after_write = true;\n")
            f.write("    }\n")

    if hex_common.is_sysemu_tag(tag):
        f.write("#endif /* !CONFIG_USER_ONLY */\n")

    f.write("}\n\n")


def main():
    args = hex_common.parse_common_args(
        "Emit functions analyzing register accesses"
    )
    tagregs = hex_common.get_tagregs()
    tagimms = hex_common.get_tagimms()

    with open(args.out, "w") as f:
        f.write("#ifndef HEXAGON_ANALYZE_FUNCS_C_INC\n")
        f.write("#define HEXAGON_ANALYZE_FUNCS_C_INC\n\n")

        for tag in hex_common.tags:
            gen_analyze_func(f, tag, tagregs[tag], tagimms[tag])

        f.write("#endif    /* HEXAGON_ANALYZE_FUNCS_C_INC */\n")


if __name__ == "__main__":
    main()
