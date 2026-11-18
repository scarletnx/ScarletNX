/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions; gengvec.c's
   expanders), LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro */
/* The SIMD emitters written by hand that the ones tools/sailgen generates from the Sail took the place of (ir_vec.c's
   three-same and two-register forms of these ops, on LLVM vectors), kept as they were for tests/llvm_sail_ir.c, which
   holds the generated IR to theirs word by word */
#include "support/ir_vec_reference.h"
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "common/cpu/aarch64/a64_decoder.h"

typedef LLVMValueRef V;
#define B ir->b

static V reg(SnxIr* ir, unsigned n, unsigned esz, unsigned lanes) { return ir_vec_read(ir, n, esz, 0, lanes); }
static V mask_of(SnxIr* ir, V condition, V like) { return LLVMBuildSExt(B, condition, LLVMTypeOf(like), ""); }
static V call2(SnxIr* ir, const char* name, V a, V b) {
    V args[2];
    args[0] = a;
    args[1] = b;
    return ir_intrinsic(ir, name, LLVMTypeOf(a), args, 2);
}
static V cmp(SnxIr* ir, LLVMIntPredicate p, V a, V b) { return LLVMBuildICmp(B, p, a, b, ""); }

/* the three-same vector form: Vd = op(Vn, Vm) lane by lane */
static V same(SnxIr* ir, SnxA64Op op, V n, V m) {
    switch (op) {
        case SNX_A64_OP_ADD_v: return LLVMBuildAdd(B, n, m, "");
        case SNX_A64_OP_SUB_v: return LLVMBuildSub(B, n, m, "");
        case SNX_A64_OP_CMGT_v: return mask_of(ir, cmp(ir, LLVMIntSGT, n, m), n);
        case SNX_A64_OP_CMHI_v: return mask_of(ir, cmp(ir, LLVMIntUGT, n, m), n);
        case SNX_A64_OP_CMGE_v: return mask_of(ir, cmp(ir, LLVMIntSGE, n, m), n);
        case SNX_A64_OP_CMHS_v: return mask_of(ir, cmp(ir, LLVMIntUGE, n, m), n);
        case SNX_A64_OP_CMTST_v:
            return mask_of(ir, cmp(ir, LLVMIntNE, LLVMBuildAnd(B, n, m, ""), ir_vec_like(ir, n, 0)), n);
        case SNX_A64_OP_CMEQ_v: return mask_of(ir, cmp(ir, LLVMIntEQ, n, m), n);
        case SNX_A64_OP_SMAX_v: return call2(ir, "llvm.smax", n, m);
        case SNX_A64_OP_UMAX_v: return call2(ir, "llvm.umax", n, m);
        case SNX_A64_OP_SMIN_v: return call2(ir, "llvm.smin", n, m);
        default: return call2(ir, "llvm.umin", n, m);
    }
}

void snx_ir_vec_reference(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc) {
    const SnxA64Insn* insn = (const SnxA64Insn*)decoded;
    const SnxA64Args* a = &insn->args;
    unsigned esz = (unsigned)a->esz, lanes = ir_vec_lanes(a->q, esz);
    V n = reg(ir, (unsigned)a->rn, esz, lanes), args[2];
    (void)word;
    (void)pc;
    switch (insn->op) {
        case SNX_A64_OP_NEG_v: ir_vec_write(ir, (unsigned)a->rd, LLVMBuildNeg(B, n, "")); break;
        case SNX_A64_OP_ABS_v:
            args[0] = n;
            args[1] = LLVMConstInt(ir->t_i1, 0, 0);
            ir_vec_write(ir, (unsigned)a->rd, ir_intrinsic(ir, "llvm.abs", LLVMTypeOf(n), args, 2));
            break;
        default: ir_vec_write(ir, (unsigned)a->rd, same(ir, insn->op, n, reg(ir, (unsigned)a->rm, esz, lanes))); break;
    }
}
