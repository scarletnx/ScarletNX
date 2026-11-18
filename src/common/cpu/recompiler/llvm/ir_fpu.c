/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The scalar floating-point instructions, keyed on the generated decoder's ops: the data processing with one, two and
   three sources, the compares and conditional compares, FCSEL, the FMOVs (immediate, register, and to and from the
   general registers), the conversions between precisions and to and from integers and fixed point, FJCVTZS, BFCVT and
   the FRINTs, FEAT_FRINTTS's included; halves for FEAT_FP16. Every one is emitted from its Sail (ir_sail.c, the
   floating point of src/common/cpu/aarch64/sail/a64.roots), whose arithmetic tools/sailgen/llvm.h lowers */
#include "common/cpu/recompiler/llvm/ir_fpu.h"
#include "common/cpu/recompiler/llvm/ir_sail.h"

int snx_ir_fpu_handles(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_FADD_s:
        case SNX_A64_OP_FSUB_s:
        case SNX_A64_OP_FDIV_s:
        case SNX_A64_OP_FMUL_s:
        case SNX_A64_OP_FNMUL_s:
        case SNX_A64_OP_FMAX_s:
        case SNX_A64_OP_FMIN_s:
        case SNX_A64_OP_FMAXNM_s:
        case SNX_A64_OP_FMINNM_s:
        case SNX_A64_OP_FMADD:
        case SNX_A64_OP_FMSUB:
        case SNX_A64_OP_FNMADD:
        case SNX_A64_OP_FNMSUB:
        case SNX_A64_OP_FMOV_s:
        case SNX_A64_OP_FABS_s:
        case SNX_A64_OP_FNEG_s:
        case SNX_A64_OP_FSQRT_s:
        case SNX_A64_OP_FRINTN_s:
        case SNX_A64_OP_FRINTP_s:
        case SNX_A64_OP_FRINTM_s:
        case SNX_A64_OP_FRINTZ_s:
        case SNX_A64_OP_FRINTA_s:
        case SNX_A64_OP_FRINTX_s:
        case SNX_A64_OP_FRINTI_s:
        case SNX_A64_OP_BFCVT_s:
        case SNX_A64_OP_FRINT32Z_s:
        case SNX_A64_OP_FRINT32X_s:
        case SNX_A64_OP_FRINT64Z_s:
        case SNX_A64_OP_FRINT64X_s:
        case SNX_A64_OP_FCVT_s_ds:
        case SNX_A64_OP_FCVT_s_hs:
        case SNX_A64_OP_FCVT_s_sd:
        case SNX_A64_OP_FCVT_s_hd:
        case SNX_A64_OP_FCVT_s_sh:
        case SNX_A64_OP_FCVT_s_dh:
        case SNX_A64_OP_FMOVI_s:
        case SNX_A64_OP_FCMP:
        case SNX_A64_OP_FCCMP:
        case SNX_A64_OP_FCSEL:
        case SNX_A64_OP_SCVTF_g:
        case SNX_A64_OP_UCVTF_g:
        case SNX_A64_OP_FCVTNS_g:
        case SNX_A64_OP_FCVTNU_g:
        case SNX_A64_OP_FCVTPS_g:
        case SNX_A64_OP_FCVTPU_g:
        case SNX_A64_OP_FCVTMS_g:
        case SNX_A64_OP_FCVTMU_g:
        case SNX_A64_OP_FCVTZS_g:
        case SNX_A64_OP_FCVTZU_g:
        case SNX_A64_OP_FCVTAS_g:
        case SNX_A64_OP_FCVTAU_g:
        case SNX_A64_OP_FJCVTZS:
        case SNX_A64_OP_FMOV_ws:
        case SNX_A64_OP_FMOV_sw:
        case SNX_A64_OP_FMOV_xd:
        case SNX_A64_OP_FMOV_dx:
        case SNX_A64_OP_FMOV_xu:
        case SNX_A64_OP_FMOV_ux:
        case SNX_A64_OP_FMOV_xh:
        case SNX_A64_OP_FMOV_hx: return 1;
        default: return 0;
    }
}

void snx_ir_fpu(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc) {
    if (ir->fpu_reference)
        ir->fpu_reference(ir, insn, word, pc);
    else if (!snx_ir_sail(ir, word, pc) && !ir->fallbacks++) {
        /* the decoder's op and the Sail's decode clauses disagree on the word: the module is refused, naming it */
        ir->fallback_word = word;
        ir->fallback_pc = pc;
    }
}
