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
   three-same, pairwise, two-register and across-lanes forms of these ops, on LLVM vectors), kept as they were for
   tests/llvm_sail_ir.c, which holds the generated IR to theirs word by word */
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
static V widen(SnxIr* ir, V v, int sign) {
    unsigned bits = LLVMGetIntTypeWidth(LLVMGetElementType(LLVMTypeOf(v)));
    LLVMTypeRef t = LLVMVectorType(LLVMIntTypeInContext(ir->ctx, 2 * bits), ir_vec_count(v));
    return sign ? LLVMBuildSExt(B, v, t, "") : LLVMBuildZExt(B, v, t, "");
}

/* the even (odd 0) or odd elements of a, then of b */
static V unzip(SnxIr* ir, V a, V b, unsigned odd) {
    unsigned n = ir_vec_count(a), mask[64];
    for (unsigned i = 0; i < n; ++i) mask[i] = 2 * i + odd;
    return ir_vec_shuffle(ir, a, b, mask, n);
}
/* the pairwise sums' operands of a and b: unzip's evens and odds */
static V pairwise(SnxIr* ir, V a, V b, const char* op) {
    V even = unzip(ir, a, b, 0), odd = unzip(ir, a, b, 1);
    return op ? call2(ir, op, even, odd) : LLVMBuildAdd(B, even, odd, "");
}

/* ADDV, SADDLV, UADDLV (long: widened first), SMAXV, UMAXV, SMINV, UMINV: one element into a scalar Vd */
static void across(SnxIr* ir, const SnxA64Args* a, const char* reduce, int widen_sign) {
    unsigned esz = (unsigned)a->esz, lanes = ir_vec_lanes(a->q, esz);
    V n = reg(ir, (unsigned)a->rn, esz, lanes);
    if (widen_sign >= 0) n = widen(ir, n, widen_sign);
    V r = ir_intrinsic(ir, reduce, LLVMTypeOf(n), &n, 1);
    ir_vec_write(ir, (unsigned)a->rd, ir_vec_splat(ir, r, 1));
}

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
        case SNX_A64_OP_UMIN_v: return call2(ir, "llvm.umin", n, m);
        case SNX_A64_OP_MUL_v: return LLVMBuildMul(B, n, m, "");
        case SNX_A64_OP_ADDP_v: return pairwise(ir, n, m, NULL);
        case SNX_A64_OP_SMAXP_v: return pairwise(ir, n, m, "llvm.smax");
        case SNX_A64_OP_SMINP_v: return pairwise(ir, n, m, "llvm.smin");
        case SNX_A64_OP_UMAXP_v: return pairwise(ir, n, m, "llvm.umax");
        default: return pairwise(ir, n, m, "llvm.umin");
    }
}

void snx_ir_vec_reference(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc) {
    const SnxA64Insn* insn = (const SnxA64Insn*)decoded;
    const SnxA64Args* a = &insn->args;
    unsigned esz = (unsigned)a->esz, lanes = ir_vec_lanes(a->q, esz);
    V n, args[2];
    (void)word;
    (void)pc;
    switch (insn->op) {
        case SNX_A64_OP_ADDV: across(ir, a, "llvm.vector.reduce.add", -1); return;
        case SNX_A64_OP_SADDLV: across(ir, a, "llvm.vector.reduce.add", 1); return;
        case SNX_A64_OP_UADDLV: across(ir, a, "llvm.vector.reduce.add", 0); return;
        case SNX_A64_OP_SMAXV: across(ir, a, "llvm.vector.reduce.smax", -1); return;
        case SNX_A64_OP_UMAXV: across(ir, a, "llvm.vector.reduce.umax", -1); return;
        case SNX_A64_OP_SMINV: across(ir, a, "llvm.vector.reduce.smin", -1); return;
        case SNX_A64_OP_UMINV: across(ir, a, "llvm.vector.reduce.umin", -1); return;
        default: break;
    }
    n = reg(ir, (unsigned)a->rn, esz, lanes);
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
