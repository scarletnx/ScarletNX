/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions, do_dot_vector,
   do_shift_imm_narrow, do_sat_addsub_*, do_2misc_narrow_vector, do_scalar_muladd_widening_idx; gengvec.c's
   expanders; translate.c's asimd_imm_const; vec_internal.h's do_sqrshl_bhs, do_urshr and do_srshr; vec_helper.c's
   do_sqrdmlah_h and do_sqrdmlah_s), LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro
   Copyright (c) 2020 Linaro */
/* The integer AdvSIMD instructions as LLVM IR on LLVM vectors, keyed on the generated decoder's ops and arguments:
   three same, three different, two-register miscellaneous, across lanes, pairwise, copy, modified immediate, shifts
   by immediate and by register, by element, permutes, table lookups, the dot and matrix products (FEAT_DotProd,
   FEAT_I8MM) and FEAT_RDM's accumulating forms; and the building blocks ir_vec_fp.c and ir_vec_crypto.c share.
   A register is read and written whole or by lanes in the CPU structure (V128, aligned to 8), as numbers on any
   host. Saturation sets FPSR.QC. The shifts by register that round or saturate are helpers/vector.c's. */
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "common/cpu/aarch64/cpu.h"

typedef LLVMValueRef V;
#define B ir->b

#define FPSR_QC (1u << 27)

/* ---- the building blocks ---- */

LLVMTypeRef ir_vec_lane(SnxIr* ir, unsigned esz) { return LLVMIntTypeInContext(ir->ctx, 8u << esz); }
LLVMTypeRef ir_vec_type(SnxIr* ir, unsigned esz, unsigned lanes) { return LLVMVectorType(ir_vec_lane(ir, esz), lanes); }
unsigned ir_vec_lanes(int q, unsigned esz) { return (q ? 16u : 8u) >> esz; }

static unsigned alignment_of(size_t offset) {
    unsigned a = 8;
    while (a > 1 && offset % a) a >>= 1;
    return a;
}

V ir_vec_read(SnxIr* ir, unsigned n, unsigned esz, unsigned first, unsigned lanes) {
    size_t at = (size_t)first << esz;
    V load = LLVMBuildLoad2(B, ir_vec_type(ir, esz, lanes), ir_cpu(ir, ir_off_v(n) + at), "");
    LLVMSetAlignment(load, alignment_of(at));
    ir_tag_cpu(ir, load);
    return ir_guest_order(ir, load);
}

static void store_at(SnxIr* ir, unsigned d, size_t at, V value) {
    ir_vectors_invalidate(ir);
    V store = LLVMBuildStore(B, ir_guest_order(ir, value), ir_cpu(ir, ir_off_v(d) + at));
    LLVMSetAlignment(store, alignment_of(at));
    ir_tag_cpu(ir, store);
}

void ir_vec_write(SnxIr* ir, unsigned d, V value) {
    LLVMTypeRef type = LLVMTypeOf(value), lane = LLVMGetElementType(type);
    unsigned lanes = LLVMGetVectorSize(type), bits = LLVMGetIntTypeWidth(lane), all = 128 / bits, mask[64];
    if (lanes != all) {
        for (unsigned i = 0; i < all; ++i) mask[i] = i < lanes ? i : lanes;
        value = ir_vec_shuffle(ir, value, LLVMConstNull(type), mask, all);
    }
    store_at(ir, d, 0, value);
}

void ir_vec_write_high(SnxIr* ir, unsigned d, V value) { store_at(ir, d, 8, value); }

V ir_vec_k(SnxIr* ir, LLVMTypeRef type, SnxU64 v) {
    V lanes[64];
    (void)ir;
    if (LLVMGetTypeKind(type) != LLVMVectorTypeKind) return LLVMConstInt(type, v, 0);
    unsigned n = LLVMGetVectorSize(type);
    for (unsigned i = 0; i < n; ++i) lanes[i] = LLVMConstInt(LLVMGetElementType(type), v, 0);
    return LLVMConstVector(lanes, n);
}

V ir_vec_splat(SnxIr* ir, V value, unsigned lanes) {
    LLVMTypeRef type = LLVMVectorType(LLVMTypeOf(value), lanes);
    unsigned mask[64] = {0};
    V one = LLVMBuildInsertElement(B, LLVMGetUndef(type), value, ir_i32(ir, 0), "");
    return ir_vec_shuffle(ir, one, LLVMGetUndef(type), mask, lanes);
}

V ir_vec_shuffle(SnxIr* ir, V a, V b, const unsigned* mask, unsigned n) {
    V m[64];
    for (unsigned i = 0; i < n; ++i) m[i] = ir_i32(ir, mask[i]);
    return LLVMBuildShuffleVector(B, a, b, LLVMConstVector(m, n), "");
}

V ir_vec_any(SnxIr* ir, V lanes) {
    if (LLVMGetTypeKind(LLVMTypeOf(lanes)) != LLVMVectorTypeKind) return lanes;
    return ir_intrinsic(ir, "llvm.vector.reduce.or", LLVMTypeOf(lanes), &lanes, 1);
}
V ir_vec_all(SnxIr* ir, V lanes) {
    if (LLVMGetTypeKind(LLVMTypeOf(lanes)) != LLVMVectorTypeKind) return lanes;
    return ir_intrinsic(ir, "llvm.vector.reduce.and", LLVMTypeOf(lanes), &lanes, 1);
}

void ir_vec_qc(SnxIr* ir, V saturated) {
    if (!snx_ir_fpsr_exact()) return; /* QC is FPSR's too (snx_ir_fpsr_flags) */
    V fpsr = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpsr));
    V bit = LLVMBuildShl(B, LLVMBuildZExt(B, ir_vec_any(ir, saturated), ir->t_i32, ""), ir_i32(ir, 27), "");
    ir_cpu_store(ir, LLVMBuildOr(B, fpsr, bit, ""), offsetof(CPU, fpsr));
}

V ir_vec_call(SnxIr* ir, const char* name, LLVMTypeRef ret, const V* operands, unsigned count) {
    LLVMTypeRef params[12];
    V args[12];
    params[0] = ir->t_ptr;
    args[0] = ir->c;
    for (unsigned i = 0; i < count; ++i) {
        args[i + 1] = operands[i];
        params[i + 1] = LLVMTypeOf(operands[i]);
    }
    V function = ir_extern(ir, name, ret, params, count + 1);
    ir_argmem(ir, function);
    ir_state_invalidate(ir);
    return LLVMBuildCall2(B, LLVMFunctionType(ret, params, count + 1, 0), function, args, count + 1, "");
}

/* ---- small pieces ---- */

static unsigned bits_of(V v) { return LLVMGetIntTypeWidth(LLVMGetElementType(LLVMTypeOf(v))); }
static SnxU64 ones(unsigned bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }
static LLVMTypeRef with_bits(SnxIr* ir, V like, unsigned bits) {
    return LLVMVectorType(LLVMIntTypeInContext(ir->ctx, bits), ir_vec_count(like));
}
static V widen(SnxIr* ir, V v, int sign) {
    LLVMTypeRef t = with_bits(ir, v, 2 * bits_of(v));
    return sign ? LLVMBuildSExt(B, v, t, "") : LLVMBuildZExt(B, v, t, "");
}
static V narrow(SnxIr* ir, V v) { return LLVMBuildTrunc(B, v, with_bits(ir, v, bits_of(v) / 2), ""); }
static V mask_of(SnxIr* ir, V condition, V like) { return LLVMBuildSExt(B, condition, LLVMTypeOf(like), ""); }
static V call2(SnxIr* ir, const char* name, V a, V b) {
    V args[2];
    args[0] = a;
    args[1] = b;
    return ir_intrinsic(ir, name, LLVMTypeOf(a), args, 2);
}
static V cmp(SnxIr* ir, LLVMIntPredicate p, V a, V b) { return LLVMBuildICmp(B, p, a, b, ""); }
static V sel(SnxIr* ir, V c, V a, V b) { return LLVMBuildSelect(B, c, a, b, ""); }

/* Vn's lanes of 8 << q bytes, or its element 0 for a scalar form (lanes 1) */
static V reg(SnxIr* ir, unsigned n, unsigned esz, unsigned lanes) { return ir_vec_read(ir, n, esz, 0, lanes); }
/* Vn's half q (the "2" forms read the high one) as lanes of esz */
static V half(SnxIr* ir, unsigned n, unsigned esz, int q) {
    unsigned lanes = 8u >> esz;
    return ir_vec_read(ir, n, esz, q ? lanes : 0, lanes);
}
/* element idx of Vm in every one of lanes */
static V element(SnxIr* ir, unsigned m, unsigned esz, unsigned idx, unsigned lanes) {
    V e = LLVMBuildExtractElement(B, ir_vec_read(ir, m, esz, idx, 1), ir_i32(ir, 0), "");
    return ir_vec_splat(ir, e, lanes);
}
/* a 64-bit narrow result: Vd's low half with the high zeroed, or (q, the "2" forms) its high half */
static void write_narrow(SnxIr* ir, unsigned d, V value, int q) {
    if (q)
        ir_vec_write_high(ir, d, value);
    else
        ir_vec_write(ir, d, value);
}

/* the even (odd 0) or odd elements of a, then of b */
static V unzip(SnxIr* ir, V a, V b, unsigned odd) {
    unsigned n = ir_vec_count(a), mask[64];
    for (unsigned i = 0; i < n; ++i) mask[i] = 2 * i + odd;
    return ir_vec_shuffle(ir, a, b, mask, n);
}
/* the pairwise sums' operands of a and b: unzip's evens and odds */
static V pairwise(SnxIr* ir, V a, V b, unsigned op) {
    V even = unzip(ir, a, b, 0), odd = unzip(ir, a, b, 1);
    switch (op) {
        case 0: return LLVMBuildAdd(B, even, odd, "");
        case 1: return call2(ir, "llvm.smax", even, odd);
        case 2: return call2(ir, "llvm.smin", even, odd);
        case 3: return call2(ir, "llvm.umax", even, odd);
        default: return call2(ir, "llvm.umin", even, odd);
    }
}

/* v (wide lanes) saturated to bits: from signed (sign) or unsigned, to signed (!uns) or unsigned; QC on a change */
static V saturate(SnxIr* ir, V v, unsigned bits, int sign, int uns) {
    V r;
    if (uns) {
        r = sign ? call2(ir, "llvm.smax", v, ir_vec_like(ir, v, 0)) : v;
        r = call2(ir, "llvm.umin", r, ir_vec_like(ir, v, ones(bits)));
    } else {
        SnxU64 top = ones(bits - 1);
        r = sign ? call2(ir, "llvm.smin", v, ir_vec_like(ir, v, top))
                 : call2(ir, "llvm.umin", v, ir_vec_like(ir, v, top));
        if (sign) { /* the minimum, sign-extended into lanes wider than 64 bits */
            V lowest[16];
            unsigned n = ir_vec_count(v);
            for (unsigned i = 0; i < n; ++i) lowest[i] = LLVMConstInt(LLVMGetElementType(LLVMTypeOf(v)), ~top, 1);
            V min = LLVMConstVector(lowest, n);
            r = call2(ir, "llvm.smax", r, min);
        }
    }
    ir_vec_qc(ir, cmp(ir, LLVMIntNE, r, v));
    return LLVMBuildTrunc(B, r, with_bits(ir, v, bits), "");
}

/* the saturating sums and differences (sadd.sat and its kin), QC where they saturate */
static V sat_addsub(SnxIr* ir, V a, V b, int uns, int sub) {
    static const char* const names[] = {"llvm.sadd.sat", "llvm.ssub.sat", "llvm.uadd.sat", "llvm.usub.sat"};
    V r = call2(ir, names[uns * 2 + sub], a, b), wrapped = sub ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, "");
    ir_vec_qc(ir, cmp(ir, LLVMIntNE, r, wrapped));
    return r;
}

/* a >> s with s in 1..bits (logical, or arithmetic for sign), the C shift's undefined top case made explicit */
static V shr(SnxIr* ir, V a, unsigned s, int sign) {
    unsigned bits = bits_of(a);
    if (s >= bits) return sign ? LLVMBuildAShr(B, a, ir_vec_like(ir, a, bits - 1), "") : ir_vec_like(ir, a, 0);
    return sign ? LLVMBuildAShr(B, a, ir_vec_like(ir, a, s), "") : LLVMBuildLShr(B, a, ir_vec_like(ir, a, s), "");
}
/* the rounding shift right by s in 1..bits (do_srshr, do_urshr) */
static V rshr(SnxIr* ir, V a, unsigned s, int sign) {
    unsigned bits = bits_of(a);
    if (s >= bits) return sign ? ir_vec_like(ir, a, 0) : LLVMBuildLShr(B, a, ir_vec_like(ir, a, bits - 1), "");
    return LLVMBuildAdd(B, shr(ir, a, s, sign),
                        LLVMBuildAnd(B, LLVMBuildLShr(B, a, ir_vec_like(ir, a, s - 1), ""), ir_vec_like(ir, a, 1), ""),
                        "");
}

/* SQDMULH, SQRDMULH, SQRDMLAH, SQRDMLSH of 16- or 32-bit lanes (do_sqrdmlah_h/s): (acc << bits + 2ab + round) >>
   bits, saturated, all of it in twice the width: (acc << (bits - 1) + ab + round / 2) >> (bits - 1) never
   overflows there */
static V qrdmlah(SnxIr* ir, V a, V b, V acc, int neg, int round) {
    unsigned bits = bits_of(a);
    V p = LLVMBuildMul(B, widen(ir, a, 1), widen(ir, b, 1), "");
    if (neg) p = LLVMBuildNeg(B, p, "");
    if (acc) p = LLVMBuildAdd(B, p, LLVMBuildShl(B, widen(ir, acc, 1), ir_vec_like(ir, p, bits - 1), ""), "");
    if (round) p = LLVMBuildAdd(B, p, ir_vec_like(ir, p, 1ull << (bits - 2)), "");
    return saturate(ir, LLVMBuildAShr(B, p, ir_vec_like(ir, p, bits - 1), ""), bits, 1, 0);
}

/* SQDMULL, SQDMLAL, SQDMLSL (acc_op 0, 1, 2): the doubled product of twice the width, saturating, then the
   saturating accumulation */
static V qdmull(SnxIr* ir, V a, V b, V acc, unsigned acc_op) {
    V p = LLVMBuildMul(B, widen(ir, a, 1), widen(ir, b, 1), "");
    p = sat_addsub(ir, p, p, 0, 0);
    if (acc_op) p = sat_addsub(ir, acc, p, 0, acc_op == 2);
    return p;
}

/* the carry-less product of the low bits of a and b, in a's lane width (PMUL; PMULL's 8-bit lanes widened) */
static V polymul(SnxIr* ir, V a, V b, unsigned steps) {
    V r = ir_vec_like(ir, a, 0);
    for (unsigned i = 0; i < steps; ++i) {
        V bit = LLVMBuildAnd(B, LLVMBuildLShr(B, b, ir_vec_like(ir, b, i), ""), ir_vec_like(ir, b, 1), "");
        V mask = LLVMBuildNeg(B, bit, "");
        r = LLVMBuildXor(B, r, LLVMBuildAnd(B, LLVMBuildShl(B, a, ir_vec_like(ir, a, i), ""), mask, ""), "");
    }
    return r;
}

/* SSHL, USHL: the signed low byte of each lane of b shifts a left, or right when negative (do_sqrshl_bhs without
   rounding or saturation): left by bits or more is 0, right by bits or more is 0 or the sign */
static V shl_register(SnxIr* ir, V a, V b, int sign) {
    unsigned bits = bits_of(a);
    V s = LLVMBuildSExt(B, LLVMBuildTrunc(B, b, with_bits(ir, b, 8), ""), LLVMTypeOf(a), "");
    /* in i8 lanes -(-128) wraps to 128 unsigned, still past the width */
    V top = ir_vec_like(ir, a, bits - 1), right = LLVMBuildNeg(B, s, "");
    V left = LLVMBuildShl(B, a, call2(ir, "llvm.umin", s, top), "");
    left = sel(ir, cmp(ir, LLVMIntSGT, s, top), ir_vec_like(ir, a, 0), left);
    V down = sign ? LLVMBuildAShr(B, a, call2(ir, "llvm.umin", right, top), "")
                : LLVMBuildLShr(B, a, call2(ir, "llvm.umin", right, top), "");
    if (!sign) down = sel(ir, cmp(ir, LLVMIntUGT, right, top), ir_vec_like(ir, a, 0), down);
    return sel(ir, cmp(ir, LLVMIntSLT, s, ir_vec_like(ir, s, 0)), down, left);
}

/* the shifts by register that round or saturate: helpers/vector.c's snx_vec_shl on lanes of Vn and Vm into Vd */
enum { SHL_UNS = 1, SHL_ROUND = 2, SHL_SAT = 4 };
static void shl_helper(SnxIr* ir, const SnxA64Args* a, unsigned lanes, unsigned flags) {
    V operands[4];
    operands[0] = ir_i32(ir, (SnxU32)a->rd);
    operands[1] = ir_i32(ir, (SnxU32)a->rn);
    operands[2] = ir_i32(ir, (SnxU32)a->rm);
    operands[3] = ir_i32(ir, (SnxU32)a->esz | lanes << 2 | flags << 8);
    ir_vec_call(ir, "snx_vec_shl", ir->t_void, operands, 4);
}

/* ---- three registers of the same type ---- */

enum {
    T_ADD, T_SUB, T_AND, T_BIC, T_ORR, T_ORN, T_EOR, T_BSL, T_BIT, T_BIF, T_SQADD, T_UQADD, T_SQSUB, T_UQSUB,
    T_SUQADD, T_USQADD, T_SSHL, T_USHL, T_CMGT, T_CMHI, T_CMGE, T_CMHS, T_CMTST, T_CMEQ, T_SHADD, T_UHADD, T_SHSUB,
    T_UHSUB, T_SRHADD, T_URHADD, T_SMAX, T_UMAX, T_SMIN, T_UMIN, T_SABD, T_UABD, T_SABA, T_UABA, T_MUL, T_PMUL,
    T_MLA, T_MLS, T_SQDMULH, T_SQRDMULH, T_SQRDMLAH, T_SQRDMLSH, T_ADDP, T_SMAXP, T_SMINP, T_UMAXP, T_UMINP
};

static V abd(SnxIr* ir, V a, V b, int sign) {
    V gt = cmp(ir, sign ? LLVMIntSGT : LLVMIntUGT, a, b);
    return sel(ir, gt, LLVMBuildSub(B, a, b, ""), LLVMBuildSub(B, b, a, ""));
}
/* (a op b) >> 1 without overflow: in twice the width */
static V halving(SnxIr* ir, V a, V b, int sign, int sub, int round) {
    V x = widen(ir, a, sign), y = widen(ir, b, sign), r = sub ? LLVMBuildSub(B, x, y, "") : LLVMBuildAdd(B, x, y, "");
    if (round) r = LLVMBuildAdd(B, r, ir_vec_like(ir, r, 1), "");
    return narrow(ir, LLVMBuildAShr(B, r, ir_vec_like(ir, r, 1), ""));
}

/* d's lanes for the operation on n and m (lanes of esz; d the destination's old value where needed) */
static V same(SnxIr* ir, unsigned op, V n, V m, V d) {
    switch (op) {
        case T_ADD: return LLVMBuildAdd(B, n, m, "");
        case T_SUB: return LLVMBuildSub(B, n, m, "");
        case T_AND: return LLVMBuildAnd(B, n, m, "");
        case T_BIC: return LLVMBuildAnd(B, n, LLVMBuildNot(B, m, ""), "");
        case T_ORR: return LLVMBuildOr(B, n, m, "");
        case T_ORN: return LLVMBuildOr(B, n, LLVMBuildNot(B, m, ""), "");
        case T_EOR: return LLVMBuildXor(B, n, m, "");
        /* (x & sel) | (y & ~sel) as y ^ ((x ^ y) & sel) */
        case T_BSL: return LLVMBuildXor(B, m, LLVMBuildAnd(B, LLVMBuildXor(B, n, m, ""), d, ""), "");
        case T_BIT: return LLVMBuildXor(B, d, LLVMBuildAnd(B, LLVMBuildXor(B, n, d, ""), m, ""), "");
        case T_BIF:
            return LLVMBuildXor(B, d, LLVMBuildAnd(B, LLVMBuildXor(B, n, d, ""), LLVMBuildNot(B, m, ""), ""), "");
        case T_SQADD: return sat_addsub(ir, n, m, 0, 0);
        case T_UQADD: return sat_addsub(ir, n, m, 1, 0);
        case T_SQSUB: return sat_addsub(ir, n, m, 0, 1);
        case T_UQSUB: return sat_addsub(ir, n, m, 1, 1);
        case T_SUQADD: /* signed d plus unsigned m, saturated signed (rn is rd) */
            return saturate(ir, LLVMBuildAdd(B, widen(ir, n, 1), widen(ir, m, 0), ""), bits_of(n), 1, 0);
        case T_USQADD: /* unsigned d plus signed m, saturated unsigned */
            return saturate(ir, LLVMBuildAdd(B, widen(ir, n, 0), widen(ir, m, 1), ""), bits_of(n), 1, 1);
        case T_SSHL: return shl_register(ir, n, m, 1);
        case T_USHL: return shl_register(ir, n, m, 0);
        case T_CMGT: return mask_of(ir, cmp(ir, LLVMIntSGT, n, m), n);
        case T_CMHI: return mask_of(ir, cmp(ir, LLVMIntUGT, n, m), n);
        case T_CMGE: return mask_of(ir, cmp(ir, LLVMIntSGE, n, m), n);
        case T_CMHS: return mask_of(ir, cmp(ir, LLVMIntUGE, n, m), n);
        case T_CMTST: return mask_of(ir, cmp(ir, LLVMIntNE, LLVMBuildAnd(B, n, m, ""), ir_vec_like(ir, n, 0)), n);
        case T_CMEQ: return mask_of(ir, cmp(ir, LLVMIntEQ, n, m), n);
        case T_SHADD: return halving(ir, n, m, 1, 0, 0);
        case T_UHADD: return halving(ir, n, m, 0, 0, 0);
        case T_SHSUB: return halving(ir, n, m, 1, 1, 0);
        case T_UHSUB: return halving(ir, n, m, 0, 1, 0);
        case T_SRHADD: return halving(ir, n, m, 1, 0, 1);
        case T_URHADD: return halving(ir, n, m, 0, 0, 1);
        case T_SMAX: return call2(ir, "llvm.smax", n, m);
        case T_UMAX: return call2(ir, "llvm.umax", n, m);
        case T_SMIN: return call2(ir, "llvm.smin", n, m);
        case T_UMIN: return call2(ir, "llvm.umin", n, m);
        case T_SABD: return abd(ir, n, m, 1);
        case T_UABD: return abd(ir, n, m, 0);
        case T_SABA: return LLVMBuildAdd(B, d, abd(ir, n, m, 1), "");
        case T_UABA: return LLVMBuildAdd(B, d, abd(ir, n, m, 0), "");
        case T_MUL: return LLVMBuildMul(B, n, m, "");
        case T_PMUL: return polymul(ir, n, m, 8);
        case T_MLA: return LLVMBuildAdd(B, d, LLVMBuildMul(B, n, m, ""), "");
        case T_MLS: return LLVMBuildSub(B, d, LLVMBuildMul(B, n, m, ""), "");
        case T_SQDMULH: return qrdmlah(ir, n, m, NULL, 0, 0);
        case T_SQRDMULH: return qrdmlah(ir, n, m, NULL, 0, 1);
        case T_SQRDMLAH: return qrdmlah(ir, n, m, d, 0, 1);
        case T_SQRDMLSH: return qrdmlah(ir, n, m, d, 1, 1);
        case T_ADDP: return pairwise(ir, n, m, 0);
        case T_SMAXP: return pairwise(ir, n, m, 1);
        case T_SMINP: return pairwise(ir, n, m, 2);
        case T_UMAXP: return pairwise(ir, n, m, 3);
        default: return pairwise(ir, n, m, 4);
    }
}
static int same_reads_d(unsigned op) {
    return op == T_BSL || op == T_BIT || op == T_BIF || op == T_SABA || op == T_UABA || op == T_MLA || op == T_MLS ||
           op == T_SQRDMLAH || op == T_SQRDMLSH;
}

/* the vector (lanes of q) or scalar (lanes 1) form; m_lane >= 0: the by-element form's element of Vm */
static void three_same(SnxIr* ir, const SnxA64Args* a, unsigned op, unsigned lanes, int m_lane) {
    unsigned esz = (unsigned)a->esz;
    V n = reg(ir, (unsigned)a->rn, esz, lanes), d = NULL;
    V m = m_lane >= 0 ? element(ir, (unsigned)a->rm, esz, (unsigned)m_lane, lanes)
                      : reg(ir, (unsigned)a->rm, esz, lanes);
    if (same_reads_d(op)) d = reg(ir, (unsigned)a->rd, esz, lanes);
    ir_vec_write(ir, (unsigned)a->rd, same(ir, op, n, m, d));
}

/* ---- three registers of different types: long, wide and narrowing ---- */

enum { L_MULL, L_MLAL, L_MLSL, L_ADDL, L_SUBL, L_ABAL, L_ABDL, L_ADDW, L_SUBW, L_QDMULL, L_QDMLAL, L_QDMLSL };

/* n and m (narrow lanes, already chosen), the destination's wide lanes d where it accumulates */
static V long_op(SnxIr* ir, unsigned op, V n, V m, V d, int sign) {
    switch (op) {
        case L_MULL: return LLVMBuildMul(B, widen(ir, n, sign), widen(ir, m, sign), "");
        case L_MLAL: return LLVMBuildAdd(B, d, LLVMBuildMul(B, widen(ir, n, sign), widen(ir, m, sign), ""), "");
        case L_MLSL: return LLVMBuildSub(B, d, LLVMBuildMul(B, widen(ir, n, sign), widen(ir, m, sign), ""), "");
        case L_ADDL: return LLVMBuildAdd(B, widen(ir, n, sign), widen(ir, m, sign), "");
        case L_SUBL: return LLVMBuildSub(B, widen(ir, n, sign), widen(ir, m, sign), "");
        case L_ABAL: return LLVMBuildAdd(B, d, widen(ir, abd(ir, n, m, sign), 0), "");
        case L_ABDL: return widen(ir, abd(ir, n, m, sign), 0);
        case L_QDMULL: return qdmull(ir, n, m, NULL, 0);
        case L_QDMLAL: return qdmull(ir, n, m, d, 1);
        default: return qdmull(ir, n, m, d, 2);
    }
}
static int long_reads_d(unsigned op) {
    return op == L_MLAL || op == L_MLSL || op == L_ABAL || op == L_QDMLAL || op == L_QDMLSL;
}

/* SMULL and the rest: the q half of Vn and Vm (or Vm's element m_lane), widened, into all of Vd */
static void three_long(SnxIr* ir, const SnxA64Args* a, unsigned op, int sign, int m_lane) {
    unsigned esz = (unsigned)a->esz, lanes = 8u >> esz;
    V n = half(ir, (unsigned)a->rn, esz, a->q), d = NULL;
    V m = m_lane >= 0 ? element(ir, (unsigned)a->rm, esz, (unsigned)m_lane, lanes)
                      : half(ir, (unsigned)a->rm, esz, a->q);
    if (long_reads_d(op)) d = reg(ir, (unsigned)a->rd, esz + 1, lanes);
    ir_vec_write(ir, (unsigned)a->rd, long_op(ir, op, n, m, d, sign));
}

/* the scalar SQDMULL, SQDMLAL, SQDMLSL: element 0 of Vn (esz) with element m_lane of Vm, into a 2 << esz byte Vd */
static void scalar_long(SnxIr* ir, const SnxA64Args* a, unsigned op, unsigned m_lane) {
    unsigned esz = (unsigned)a->esz;
    V n = reg(ir, (unsigned)a->rn, esz, 1), m = element(ir, (unsigned)a->rm, esz, m_lane, 1), d = NULL;
    if (long_reads_d(op)) d = reg(ir, (unsigned)a->rd, esz + 1, 1);
    ir_vec_write(ir, (unsigned)a->rd, long_op(ir, op, n, m, d, 1));
}

/* SADDW, UADDW, SSUBW, USUBW: all of Vn wide, the q half of Vm widened */
static void three_wide(SnxIr* ir, const SnxA64Args* a, int sign, int sub) {
    unsigned esz = (unsigned)a->esz, lanes = 8u >> esz;
    V n = reg(ir, (unsigned)a->rn, esz + 1, lanes), m = widen(ir, half(ir, (unsigned)a->rm, esz, a->q), sign);
    ir_vec_write(ir, (unsigned)a->rd, sub ? LLVMBuildSub(B, n, m, "") : LLVMBuildAdd(B, n, m, ""));
}

/* ADDHN, RADDHN, SUBHN, RSUBHN: the high halves of the wide sums or differences */
static void three_narrow(SnxIr* ir, const SnxA64Args* a, int sub, int round) {
    unsigned esz = (unsigned)a->esz, lanes = 8u >> esz, bits = 8u << esz;
    V n = reg(ir, (unsigned)a->rn, esz + 1, lanes), m = reg(ir, (unsigned)a->rm, esz + 1, lanes);
    V r = sub ? LLVMBuildSub(B, n, m, "") : LLVMBuildAdd(B, n, m, "");
    if (round) r = LLVMBuildAdd(B, r, ir_vec_like(ir, r, 1ull << (bits - 1)), "");
    write_narrow(ir, (unsigned)a->rd, narrow(ir, LLVMBuildLShr(B, r, ir_vec_like(ir, r, bits), "")), a->q);
}

/* PMULL, PMULL2 of 8-bit lanes: 16-bit carry-less products */
static void pmull8(SnxIr* ir, const SnxA64Args* a) {
    V n = widen(ir, half(ir, (unsigned)a->rn, 0, a->q), 0), m = widen(ir, half(ir, (unsigned)a->rm, 0, a->q), 0);
    ir_vec_write(ir, (unsigned)a->rd, polymul(ir, n, m, 8));
}

/* ---- two registers, miscellaneous ---- */

enum {
    M_ABS, M_NEG, M_SQABS, M_SQNEG, M_CLS, M_CLZ, M_CNT, M_NOT, M_RBIT, M_CMGT0, M_CMGE0, M_CMEQ0, M_CMLE0, M_CMLT0
};

static V misc(SnxIr* ir, unsigned op, V n) {
    V zero = ir_vec_like(ir, n, 0), f = LLVMConstInt(ir->t_i1, 0, 0), args[2];
    SnxU64 min = 1ull << (bits_of(n) - 1);
    args[0] = n;
    args[1] = f;
    switch (op) {
        case M_ABS: return ir_intrinsic(ir, "llvm.abs", LLVMTypeOf(n), args, 2);
        case M_NEG: return LLVMBuildNeg(B, n, "");
        case M_SQABS:
        case M_SQNEG: {
            V is_min = cmp(ir, LLVMIntEQ, n, ir_vec_like(ir, n, min));
            V r = op == M_SQABS ? ir_intrinsic(ir, "llvm.abs", LLVMTypeOf(n), args, 2) : LLVMBuildNeg(B, n, "");
            ir_vec_qc(ir, is_min);
            return sel(ir, is_min, ir_vec_like(ir, n, min - 1), r);
        }
        case M_CLS:
            args[0] = LLVMBuildXor(B, n, LLVMBuildAShr(B, n, ir_vec_like(ir, n, 1), ""), "");
            return LLVMBuildSub(B, ir_intrinsic(ir, "llvm.ctlz", LLVMTypeOf(n), args, 2), ir_vec_like(ir, n, 1), "");
        case M_CLZ: return ir_intrinsic(ir, "llvm.ctlz", LLVMTypeOf(n), args, 2);
        case M_CNT: return ir_intrinsic(ir, "llvm.ctpop", LLVMTypeOf(n), args, 1);
        case M_NOT: return LLVMBuildNot(B, n, "");
        case M_RBIT: return ir_intrinsic(ir, "llvm.bitreverse", LLVMTypeOf(n), args, 1);
        case M_CMGT0: return mask_of(ir, cmp(ir, LLVMIntSGT, n, zero), n);
        case M_CMGE0: return mask_of(ir, cmp(ir, LLVMIntSGE, n, zero), n);
        case M_CMEQ0: return mask_of(ir, cmp(ir, LLVMIntEQ, n, zero), n);
        case M_CMLE0: return mask_of(ir, cmp(ir, LLVMIntSLE, n, zero), n);
        default: return mask_of(ir, cmp(ir, LLVMIntSLT, n, zero), n);
    }
}
static void two_misc(SnxIr* ir, const SnxA64Args* a, unsigned op, unsigned esz, unsigned lanes) {
    ir_vec_write(ir, (unsigned)a->rd, misc(ir, op, reg(ir, (unsigned)a->rn, esz, lanes)));
}

/* REV16, REV32, REV64: the esz elements reversed inside each container of 1 << container bytes */
static void rev(SnxIr* ir, const SnxA64Args* a, unsigned container) {
    unsigned esz = (unsigned)a->esz, lanes = ir_vec_lanes(a->q, esz), group = 1u << (container - esz), mask[16];
    V n = reg(ir, (unsigned)a->rn, esz, lanes);
    for (unsigned i = 0; i < lanes; ++i) mask[i] = (i & ~(group - 1)) + (group - 1 - (i & (group - 1)));
    ir_vec_write(ir, (unsigned)a->rd, ir_vec_shuffle(ir, n, n, mask, lanes));
}

/* SADDLP, UADDLP, SADALP, UADALP */
static void addlp(SnxIr* ir, const SnxA64Args* a, int sign, int accumulate) {
    unsigned esz = (unsigned)a->esz, lanes = ir_vec_lanes(a->q, esz);
    V n = reg(ir, (unsigned)a->rn, esz, lanes);
    V r = LLVMBuildAdd(B, widen(ir, unzip(ir, n, n, 0), sign), widen(ir, unzip(ir, n, n, 1), sign), "");
    r = ir_vec_shuffle(ir, r, r, (const unsigned[]){0, 1, 2, 3, 4, 5, 6, 7}, lanes / 2);
    if (accumulate) r = LLVMBuildAdd(B, r, reg(ir, (unsigned)a->rd, esz + 1, lanes / 2), "");
    ir_vec_write(ir, (unsigned)a->rd, r);
}

/* XTN, SQXTN, UQXTN, SQXTUN (kind 0-3) of Vn's wide lanes, into the low or (q) high half of Vd; scalar: one lane
   into a scalar Vd */
static void extract_narrow(SnxIr* ir, const SnxA64Args* a, unsigned kind, int scalar) {
    unsigned esz = (unsigned)a->esz, lanes = scalar ? 1 : 8u >> esz, bits = 8u << esz;
    V n = reg(ir, (unsigned)a->rn, esz + 1, lanes), r;
    switch (kind) {
        case 0: r = narrow(ir, n); break;
        case 1: r = saturate(ir, n, bits, 1, 0); break;
        case 2: r = saturate(ir, n, bits, 0, 1); break;
        default: r = saturate(ir, n, bits, 1, 1); break;
    }
    if (scalar)
        ir_vec_write(ir, (unsigned)a->rd, r);
    else
        write_narrow(ir, (unsigned)a->rd, r, a->q);
}

/* SHLL, SHLL2: the q half widened and shifted left by its width */
static void shll(SnxIr* ir, const SnxA64Args* a) {
    unsigned esz = (unsigned)a->esz;
    V n = widen(ir, half(ir, (unsigned)a->rn, esz, a->q), 0);
    ir_vec_write(ir, (unsigned)a->rd, LLVMBuildShl(B, n, ir_vec_like(ir, n, 8u << esz), ""));
}

/* ---- across lanes ---- */

/* ADDV, SADDLV, UADDLV (long: widened first), SMAXV, UMAXV, SMINV, UMINV: one element into a scalar Vd */
static void across(SnxIr* ir, const SnxA64Args* a, const char* reduce, int widen_sign) {
    unsigned esz = (unsigned)a->esz, lanes = ir_vec_lanes(a->q, esz);
    V n = reg(ir, (unsigned)a->rn, esz, lanes);
    if (widen_sign >= 0) n = widen(ir, n, widen_sign);
    V r = ir_intrinsic(ir, reduce, LLVMTypeOf(n), &n, 1);
    ir_vec_write(ir, (unsigned)a->rd, ir_vec_splat(ir, r, 1));
}

/* ---- copy ---- */

/* the element size and index an imm5 encodes (the lowest set bit, and the bits above it) */
static unsigned imm5_esz(int imm) {
    unsigned esz = 0;
    while (esz < 4 && !((unsigned)imm & (1u << esz))) ++esz;
    return esz;
}

static void copy(SnxIr* ir, const SnxA64Insn* insn) {
    const SnxA64Args* a = &insn->args;
    unsigned esz = imm5_esz(a->imm), idx = (unsigned)a->imm >> (esz + 1), rd = (unsigned)a->rd, rn = (unsigned)a->rn;
    switch (insn->op) {
        case SNX_A64_OP_DUP_element_s: ir_vec_write(ir, rd, element(ir, rn, esz, idx, 1)); break;
        case SNX_A64_OP_DUP_element_v: ir_vec_write(ir, rd, element(ir, rn, esz, idx, ir_vec_lanes(a->q, esz))); break;
        case SNX_A64_OP_DUP_general: {
            V x = LLVMBuildTrunc(B, ir_rx(ir, rn), ir_vec_lane(ir, esz), "");
            ir_vec_write(ir, rd, ir_vec_splat(ir, x, ir_vec_lanes(a->q, esz)));
            break;
        }
        case SNX_A64_OP_INS_general: ir_set_ve(ir, rd, esz, idx, ir_rx(ir, rn)); break;
        case SNX_A64_OP_SMOV: {
            V e = ir_ve(ir, rn, esz, idx);
            V x = LLVMBuildSExt(B, e, a->q ? ir->t_i64 : ir->t_i32, "");
            ir_setx(ir, rd, ir_u64(ir, x));
            break;
        }
        case SNX_A64_OP_UMOV: ir_setx(ir, rd, ir_u64(ir, ir_ve(ir, rn, esz, idx))); break;
        default: { /* INS_element */
            unsigned di = (unsigned)a->di, desz = imm5_esz(a->di);
            ir_set_ve(ir, rd, desz, di >> (desz + 1), ir_ve(ir, rn, desz, (unsigned)a->si >> desz));
            break;
        }
    }
}

/* ---- modified immediate ---- */

/* AdvSIMDExpandImm (asimd_imm_const) */
static SnxU64 expand_imm(unsigned imm, unsigned cmode, unsigned op) {
    SnxU64 v = imm;
    switch (cmode) {
        case 0:
        case 1: break;
        case 2:
        case 3: v <<= 8; break;
        case 4:
        case 5: v <<= 16; break;
        case 6:
        case 7: v <<= 24; break;
        case 8:
        case 9: v |= v << 16; break;
        case 10:
        case 11: v = v << 8 | v << 24; break;
        case 12: v = v << 8 | 0xFF; break;
        case 13: v = v << 16 | 0xFFFF; break;
        case 14:
            if (op) {
                SnxU64 bytes = 0;
                for (unsigned i = 0; i < 8; ++i)
                    if (imm & (1u << i)) bytes |= 0xFFull << (8 * i);
                return bytes;
            }
            v |= v << 8 | v << 16 | v << 24;
            break;
        default: /* 15 */
            if (op) {
                SnxU64 d = (SnxU64)(imm & 0x3F) << 48 | (SnxU64)(imm & 0x80) << 56;
                return d | ((imm & 0x40) ? 0x3FC0000000000000ull : 0x4000000000000000ull);
            }
            v = (SnxU64)(imm & 0x80) << 24 | (SnxU64)(imm & 0x3F) << 19 | ((imm & 0x40) ? 0x1Full << 25 : 1ull << 30);
            break;
    }
    v = (v & 0xFFFFFFFFu) | v << 32;
    return op ? ~v : v;
}

/* MOVI, MVNI, ORR, BIC, FMOV (vector, immediate): ORR and BIC for the odd cmodes below 12 (BIC's immediate
   inverted, an AND) */
static void vimm(SnxIr* ir, const SnxA64Args* a) {
    unsigned cmode = (unsigned)a->cmode, lanes = a->q ? 2 : 1;
    V imm = ir_vec_splat(ir, ir_i64(ir, expand_imm((unsigned)a->abcdefgh, cmode, (unsigned)a->op)), lanes);
    if ((cmode & 1) && cmode < 12) {
        V d = reg(ir, (unsigned)a->rd, 3, lanes);
        imm = a->op ? LLVMBuildAnd(B, d, imm, "") : LLVMBuildOr(B, d, imm, "");
    }
    ir_vec_write(ir, (unsigned)a->rd, imm);
}

/* FMOV (vector, immediate) of halves: VFPExpandImm to 16 bits */
static void fmov_h(SnxIr* ir, const SnxA64Args* a) {
    unsigned imm = (unsigned)a->abcdefgh, b6 = (imm >> 6) & 1;
    SnxU64 h = (SnxU64)(imm >> 7) << 15 | (SnxU64)((b6 ^ 1) << 4 | (b6 ? 3u : 0u) << 2 | ((imm >> 4) & 3)) << 10 |
               (SnxU64)(imm & 15) << 6;
    ir_vec_write(ir, (unsigned)a->rd, ir_vec_splat(ir, LLVMConstInt(ir->t_i16, h, 0), ir_vec_lanes(a->q, 1)));
}

/* ---- shifts by immediate ---- */

enum {
    S_SSHR, S_USHR, S_SSRA, S_USRA, S_SRSHR, S_URSHR, S_SRSRA, S_URSRA, S_SRI, S_SHL, S_SLI, S_SQSHL, S_UQSHL, S_SQSHLU
};

/* the saturating shifts left by s (0..bits-1): back-shifting must give the operand */
static V qshl_imm(SnxIr* ir, V n, unsigned s, int sign, int uns) {
    unsigned bits = bits_of(n);
    V r = LLVMBuildShl(B, n, ir_vec_like(ir, n, s), ""), back, max, ok, neg;
    if (sign && uns) { /* SQSHLU: a negative operand is 0, the rest unsigned */
        neg = cmp(ir, LLVMIntSLT, n, ir_vec_like(ir, n, 0));
        back = LLVMBuildLShr(B, r, ir_vec_like(ir, n, s), "");
        ok = cmp(ir, LLVMIntEQ, back, n);
        ir_vec_qc(ir, LLVMBuildOr(B, neg, LLVMBuildNot(B, ok, ""), ""));
        return sel(ir, neg, ir_vec_like(ir, n, 0), sel(ir, ok, r, ir_vec_like(ir, n, ones(bits))));
    }
    back = sign ? LLVMBuildAShr(B, r, ir_vec_like(ir, n, s), "") : LLVMBuildLShr(B, r, ir_vec_like(ir, n, s), "");
    ok = cmp(ir, LLVMIntEQ, back, n);
    ir_vec_qc(ir, LLVMBuildNot(B, ok, ""));
    if (sign) {
        V top = ir_vec_like(ir, n, ones(bits - 1));
        max = sel(ir, cmp(ir, LLVMIntSLT, n, ir_vec_like(ir, n, 0)), LLVMBuildNot(B, top, ""), top);
    } else
        max = ir_vec_like(ir, n, ones(bits));
    return sel(ir, ok, r, max);
}

static V shift_imm(SnxIr* ir, unsigned op, V n, V d, unsigned s) {
    unsigned bits = bits_of(n);
    switch (op) {
        case S_SSHR: return shr(ir, n, s, 1);
        case S_USHR: return shr(ir, n, s, 0);
        case S_SSRA: return LLVMBuildAdd(B, d, shr(ir, n, s, 1), "");
        case S_USRA: return LLVMBuildAdd(B, d, shr(ir, n, s, 0), "");
        case S_SRSHR: return rshr(ir, n, s, 1);
        case S_URSHR: return rshr(ir, n, s, 0);
        case S_SRSRA: return LLVMBuildAdd(B, d, rshr(ir, n, s, 1), "");
        case S_URSRA: return LLVMBuildAdd(B, d, rshr(ir, n, s, 0), "");
        case S_SRI: {
            V mask;
            if (s >= bits) return d;
            mask = ir_vec_like(ir, n, ones(bits) >> s);
            return LLVMBuildOr(B, LLVMBuildAnd(B, d, LLVMBuildNot(B, mask, ""), ""), shr(ir, n, s, 0), "");
        }
        case S_SHL: return LLVMBuildShl(B, n, ir_vec_like(ir, n, s), "");
        case S_SLI: {
            V mask = ir_vec_like(ir, n, (ones(bits) << s) & ones(bits));
            V moved = LLVMBuildShl(B, n, ir_vec_like(ir, n, s), "");
            return LLVMBuildOr(B, LLVMBuildAnd(B, d, LLVMBuildNot(B, mask, ""), ""), moved, "");
        }
        case S_SQSHL: return qshl_imm(ir, n, s, 1, 0);
        case S_UQSHL: return qshl_imm(ir, n, s, 0, 0);
        default: return qshl_imm(ir, n, s, 1, 1);
    }
}
static int shift_reads_d(unsigned op) {
    return op == S_SSRA || op == S_USRA || op == S_SRSRA || op == S_URSRA || op == S_SRI || op == S_SLI;
}
static void shift_imm_form(SnxIr* ir, const SnxA64Args* a, unsigned op, unsigned lanes) {
    unsigned esz = (unsigned)a->esz;
    V n = reg(ir, (unsigned)a->rn, esz, lanes), d = shift_reads_d(op) ? reg(ir, (unsigned)a->rd, esz, lanes) : NULL;
    ir_vec_write(ir, (unsigned)a->rd, shift_imm(ir, op, n, d, (unsigned)a->imm));
}

/* SSHLL, USHLL: the q half widened, shifted left by 0..bits-1 */
static void shll_imm(SnxIr* ir, const SnxA64Args* a, int sign) {
    V n = widen(ir, half(ir, (unsigned)a->rn, (unsigned)a->esz, a->q), sign);
    ir_vec_write(ir, (unsigned)a->rd, LLVMBuildShl(B, n, ir_vec_like(ir, n, (unsigned)a->imm), ""));
}

/* SHRN, RSHRN, SQSHRN, UQSHRN, SQSHRUN and the rounding ones: the wide lanes shifted right by 1..bits (arithmetic
   when the source is signed), rounded, narrowed (saturating: kind 1 signed, 2 unsigned, 3 signed to unsigned, 0
   truncating); vector into the low or (q) high half of Vd, scalar (lanes 1) into a scalar Vd */
static void shift_narrow(SnxIr* ir, const SnxA64Args* a, unsigned kind, int round, int scalar) {
    unsigned esz = (unsigned)a->esz, lanes = scalar ? 1 : 8u >> esz, bits = 8u << esz, s = (unsigned)a->imm;
    int sign = kind == 1 || kind == 3;
    V n = reg(ir, (unsigned)a->rn, esz + 1, lanes);
    V r = round ? rshr(ir, n, s, sign) : shr(ir, n, s, sign);
    switch (kind) {
        case 0: r = narrow(ir, r); break;
        case 1: r = saturate(ir, r, bits, 1, 0); break;
        case 2: r = saturate(ir, r, bits, 0, 1); break;
        default: r = saturate(ir, r, bits, 1, 1); break;
    }
    if (scalar)
        ir_vec_write(ir, (unsigned)a->rd, r);
    else
        write_narrow(ir, (unsigned)a->rd, r, a->q);
}

/* ---- permutes ---- */

static void permute(SnxIr* ir, const SnxA64Args* a, SnxA64Op op) {
    unsigned esz = (unsigned)a->esz, lanes = ir_vec_lanes(a->q, esz), mask[16];
    V n = reg(ir, (unsigned)a->rn, esz, lanes), m = reg(ir, (unsigned)a->rm, esz, lanes);
    for (unsigned i = 0; i < lanes; ++i) switch (op) {
            case SNX_A64_OP_UZP1: mask[i] = 2 * i; break;
            case SNX_A64_OP_UZP2: mask[i] = 2 * i + 1; break;
            case SNX_A64_OP_TRN1: mask[i] = (i & ~1u) + (i & 1) * lanes; break;
            case SNX_A64_OP_TRN2: mask[i] = (i & 1) ? lanes + i : i + 1; break;
            case SNX_A64_OP_ZIP1: mask[i] = i / 2 + (i & 1) * lanes; break;
            default: mask[i] = lanes / 2 + i / 2 + (i & 1) * lanes; break; /* ZIP2 */
        }
    ir_vec_write(ir, (unsigned)a->rd, ir_vec_shuffle(ir, n, m, mask, lanes));
}

/* EXT: bytes imm.. of Vm:Vn (8 or 16 of each) */
static void ext(SnxIr* ir, const SnxA64Args* a, unsigned bytes) {
    unsigned mask[16];
    V n = reg(ir, (unsigned)a->rn, 0, bytes), m = reg(ir, (unsigned)a->rm, 0, bytes);
    for (unsigned i = 0; i < bytes; ++i) mask[i] = (unsigned)a->imm + i;
    ir_vec_write(ir, (unsigned)a->rd, ir_vec_shuffle(ir, n, m, mask, bytes));
}

/* TBL, TBX: each index byte of Vm picks a byte of the len + 1 consecutive table registers from Vn (modulo 32), out
   of range giving 0 (TBL) or Vd's byte (TBX). pshufb or AArch64's TBL where the host has them, else one byte at a
   time */
static void tbl(SnxIr* ir, const SnxA64Args* a) {
    unsigned bytes = a->q ? 16 : 8, regs = (unsigned)a->len + 1;
    V idx = reg(ir, (unsigned)a->rm, 0, bytes), r = NULL, d = a->tbx ? reg(ir, (unsigned)a->rd, 0, bytes) : NULL;
    V tables[4];
    for (unsigned t = 0; t < regs; ++t) tables[t] = reg(ir, ((unsigned)a->rn + t) % 32, 0, 16);
    if (ir->features->aarch64) {
        static const char* const tbl_names[] = {"llvm.aarch64.neon.tbl1", "llvm.aarch64.neon.tbl2",
                                                "llvm.aarch64.neon.tbl3", "llvm.aarch64.neon.tbl4"};
        static const char* const tbx_names[] = {"llvm.aarch64.neon.tbx1", "llvm.aarch64.neon.tbx2",
                                                "llvm.aarch64.neon.tbx3", "llvm.aarch64.neon.tbx4"};
        V args[6];
        unsigned count = 0;
        if (a->tbx) args[count++] = d;
        for (unsigned t = 0; t < regs; ++t) args[count++] = tables[t];
        args[count++] = idx;
        r = ir_intrinsic(ir, a->tbx ? tbx_names[regs - 1] : tbl_names[regs - 1], LLVMTypeOf(idx), args, count);
    } else if (ir->features->x86 && ir->features->sse42) {
        LLVMTypeRef bytes16 = ir_vec_type(ir, 0, 16);
        static const unsigned twice[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7};
        V wide = bytes == 16 ? idx : ir_vec_shuffle(ir, idx, idx, twice, 16);
        V out = LLVMConstNull(bytes16);
        for (unsigned t = 0; t < regs; ++t) {
            V rel = LLVMBuildSub(B, wide, ir_vec_k(ir, bytes16, 16 * t), ""), args[2];
            V outside = mask_of(ir, cmp(ir, LLVMIntUGE, rel, ir_vec_k(ir, bytes16, 16)), rel);
            args[0] = tables[t];
            args[1] = LLVMBuildOr(B, rel, LLVMBuildAnd(B, outside, ir_vec_k(ir, bytes16, 0x80), ""), "");
            out = LLVMBuildOr(B, out, ir_intrinsic(ir, "llvm.x86.ssse3.pshuf.b.128", NULL, args, 2), "");
        }
        r = bytes == 16 ? out : ir_vec_shuffle(ir, out, out, (const unsigned[]){0, 1, 2, 3, 4, 5, 6, 7}, 8);
        if (a->tbx) r = sel(ir, cmp(ir, LLVMIntUGE, idx, ir_vec_like(ir, idx, 16 * regs)), d, r);
    } else {
        r = a->tbx ? d : ir_vec_like(ir, idx, 0);
        for (unsigned i = 0; i < bytes; ++i) {
            V x = LLVMBuildExtractElement(B, idx, ir_i32(ir, i), "");
            V byte = LLVMBuildExtractElement(B, r, ir_i32(ir, i), "");
            for (unsigned t = 0; t < regs; ++t) {
                V rel = LLVMBuildSub(B, x, LLVMConstInt(ir->t_i8, 16 * t, 0), "");
                V in = LLVMBuildICmp(B, LLVMIntULT, rel, LLVMConstInt(ir->t_i8, 16, 0), "");
                V low = LLVMBuildAnd(B, rel, LLVMConstInt(ir->t_i8, 15, 0), "");
                V pick = LLVMBuildExtractElement(B, tables[t], low, "");
                byte = LLVMBuildSelect(B, in, pick, byte, "");
            }
            r = LLVMBuildInsertElement(B, r, byte, ir_i32(ir, i), "");
        }
    }
    ir_vec_write(ir, (unsigned)a->rd, r);
}

/* ---- dot and matrix products ---- */

/* the sums of each four consecutive lanes of p (i32 lanes) */
static V sum4(SnxIr* ir, V p) {
    unsigned n = ir_vec_count(p) / 4, mask[4][4];
    V r = NULL;
    for (unsigned j = 0; j < 4; ++j) {
        for (unsigned i = 0; i < n; ++i) mask[j][i] = 4 * i + j;
        V part = ir_vec_shuffle(ir, p, p, mask[j], n);
        r = r ? LLVMBuildAdd(B, r, part, "") : part;
    }
    return r;
}
static V bytes_to_i32(SnxIr* ir, V v, int sign) {
    LLVMTypeRef t = with_bits(ir, v, 32);
    return sign ? LLVMBuildSExt(B, v, t, "") : LLVMBuildZExt(B, v, t, "");
}

/* SDOT, UDOT, USDOT, SUDOT (n_sign and m_sign), vector or by element (the 32-bit group m_lane of Vm) */
static void dot(SnxIr* ir, const SnxA64Args* a, int n_sign, int m_sign, int m_lane) {
    unsigned lanes = ir_vec_lanes(a->q, 2);
    V n = bytes_to_i32(ir, reg(ir, (unsigned)a->rn, 0, 4 * lanes), n_sign), m;
    if (m_lane >= 0) {
        V group = ir_vec_read(ir, (unsigned)a->rm, 0, 4 * (unsigned)m_lane, 4);
        unsigned mask[16];
        for (unsigned i = 0; i < 4 * lanes; ++i) mask[i] = i & 3;
        m = ir_vec_shuffle(ir, group, group, mask, 4 * lanes);
    } else
        m = reg(ir, (unsigned)a->rm, 0, 4 * lanes);
    m = bytes_to_i32(ir, m, m_sign);
    V d = reg(ir, (unsigned)a->rd, 2, lanes);
    ir_vec_write(ir, (unsigned)a->rd, LLVMBuildAdd(B, d, sum4(ir, LLVMBuildMul(B, n, m, "")), ""));
}

/* SMMLA, UMMLA, USMMLA: Vd's 2x2 i32 += Vn's 2x8 bytes times the transpose of Vm's */
static void mmla(SnxIr* ir, const SnxA64Args* a, int n_sign, int m_sign) {
    V n = bytes_to_i32(ir, reg(ir, (unsigned)a->rn, 0, 16), n_sign);
    V m = bytes_to_i32(ir, reg(ir, (unsigned)a->rm, 0, 16), m_sign);
    V d = reg(ir, (unsigned)a->rd, 2, 4), sums[4];
    unsigned mask[8];
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            for (unsigned x = 0; x < 8; ++x) mask[x] = 8 * i + x;
            V row = ir_vec_shuffle(ir, n, n, mask, 8);
            for (unsigned x = 0; x < 8; ++x) mask[x] = 8 * j + x;
            V col = ir_vec_shuffle(ir, m, m, mask, 8);
            row = LLVMBuildMul(B, row, col, "");
            sums[2 * i + j] = ir_intrinsic(ir, "llvm.vector.reduce.add", LLVMTypeOf(row), &row, 1);
        }
    V r = LLVMGetUndef(LLVMTypeOf(d));
    for (unsigned i = 0; i < 4; ++i) r = LLVMBuildInsertElement(B, r, sums[i], ir_i32(ir, i), "");
    ir_vec_write(ir, (unsigned)a->rd, LLVMBuildAdd(B, d, r, ""));
}

/* ---- the entry points ---- */

/* the integer ops: the three-same table's index, or -1 */
static int same_op(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_ADD_v:
        case SNX_A64_OP_ADD_s: return T_ADD;
        case SNX_A64_OP_SUB_v:
        case SNX_A64_OP_SUB_s: return T_SUB;
        case SNX_A64_OP_AND_v: return T_AND;
        case SNX_A64_OP_BIC_v: return T_BIC;
        case SNX_A64_OP_ORR_v: return T_ORR;
        case SNX_A64_OP_ORN_v: return T_ORN;
        case SNX_A64_OP_EOR_v: return T_EOR;
        case SNX_A64_OP_BSL_v: return T_BSL;
        case SNX_A64_OP_BIT_v: return T_BIT;
        case SNX_A64_OP_BIF_v: return T_BIF;
        case SNX_A64_OP_SQADD_v:
        case SNX_A64_OP_SQADD_s: return T_SQADD;
        case SNX_A64_OP_UQADD_v:
        case SNX_A64_OP_UQADD_s: return T_UQADD;
        case SNX_A64_OP_SQSUB_v:
        case SNX_A64_OP_SQSUB_s: return T_SQSUB;
        case SNX_A64_OP_UQSUB_v:
        case SNX_A64_OP_UQSUB_s: return T_UQSUB;
        case SNX_A64_OP_SUQADD_v:
        case SNX_A64_OP_SUQADD_s: return T_SUQADD;
        case SNX_A64_OP_USQADD_v:
        case SNX_A64_OP_USQADD_s: return T_USQADD;
        case SNX_A64_OP_SSHL_v:
        case SNX_A64_OP_SSHL_s: return T_SSHL;
        case SNX_A64_OP_USHL_v:
        case SNX_A64_OP_USHL_s: return T_USHL;
        case SNX_A64_OP_CMGT_v:
        case SNX_A64_OP_CMGT_s: return T_CMGT;
        case SNX_A64_OP_CMHI_v:
        case SNX_A64_OP_CMHI_s: return T_CMHI;
        case SNX_A64_OP_CMGE_v:
        case SNX_A64_OP_CMGE_s: return T_CMGE;
        case SNX_A64_OP_CMHS_v:
        case SNX_A64_OP_CMHS_s: return T_CMHS;
        case SNX_A64_OP_CMTST_v:
        case SNX_A64_OP_CMTST_s: return T_CMTST;
        case SNX_A64_OP_CMEQ_v:
        case SNX_A64_OP_CMEQ_s: return T_CMEQ;
        case SNX_A64_OP_SHADD_v: return T_SHADD;
        case SNX_A64_OP_UHADD_v: return T_UHADD;
        case SNX_A64_OP_SHSUB_v: return T_SHSUB;
        case SNX_A64_OP_UHSUB_v: return T_UHSUB;
        case SNX_A64_OP_SRHADD_v: return T_SRHADD;
        case SNX_A64_OP_URHADD_v: return T_URHADD;
        case SNX_A64_OP_SMAX_v: return T_SMAX;
        case SNX_A64_OP_UMAX_v: return T_UMAX;
        case SNX_A64_OP_SMIN_v: return T_SMIN;
        case SNX_A64_OP_UMIN_v: return T_UMIN;
        case SNX_A64_OP_SABD_v: return T_SABD;
        case SNX_A64_OP_UABD_v: return T_UABD;
        case SNX_A64_OP_SABA_v: return T_SABA;
        case SNX_A64_OP_UABA_v: return T_UABA;
        case SNX_A64_OP_MUL_v: return T_MUL;
        case SNX_A64_OP_PMUL_v: return T_PMUL;
        case SNX_A64_OP_MLA_v: return T_MLA;
        case SNX_A64_OP_MLS_v: return T_MLS;
        case SNX_A64_OP_SQDMULH_v:
        case SNX_A64_OP_SQDMULH_s: return T_SQDMULH;
        case SNX_A64_OP_SQRDMULH_v:
        case SNX_A64_OP_SQRDMULH_s: return T_SQRDMULH;
        case SNX_A64_OP_SQRDMLAH_v:
        case SNX_A64_OP_SQRDMLAH_s: return T_SQRDMLAH;
        case SNX_A64_OP_SQRDMLSH_v:
        case SNX_A64_OP_SQRDMLSH_s: return T_SQRDMLSH;
        case SNX_A64_OP_ADDP_v: return T_ADDP;
        case SNX_A64_OP_SMAXP_v: return T_SMAXP;
        case SNX_A64_OP_SMINP_v: return T_SMINP;
        case SNX_A64_OP_UMAXP_v: return T_UMAXP;
        case SNX_A64_OP_UMINP_v: return T_UMINP;
        default: return -1;
    }
}
static int scalar_same(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_ADD_s:
        case SNX_A64_OP_SUB_s:
        case SNX_A64_OP_SQADD_s:
        case SNX_A64_OP_UQADD_s:
        case SNX_A64_OP_SQSUB_s:
        case SNX_A64_OP_UQSUB_s:
        case SNX_A64_OP_SUQADD_s:
        case SNX_A64_OP_USQADD_s:
        case SNX_A64_OP_SSHL_s:
        case SNX_A64_OP_USHL_s:
        case SNX_A64_OP_CMGT_s:
        case SNX_A64_OP_CMHI_s:
        case SNX_A64_OP_CMGE_s:
        case SNX_A64_OP_CMHS_s:
        case SNX_A64_OP_CMTST_s:
        case SNX_A64_OP_CMEQ_s:
        case SNX_A64_OP_SQDMULH_s:
        case SNX_A64_OP_SQRDMULH_s:
        case SNX_A64_OP_SQRDMLAH_s:
        case SNX_A64_OP_SQRDMLSH_s: return 1;
        default: return 0;
    }
}

/* the by-element integer ops: the three-same table's index, or -1 */
static int element_op(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_MUL_vi: return T_MUL;
        case SNX_A64_OP_MLA_vi: return T_MLA;
        case SNX_A64_OP_MLS_vi: return T_MLS;
        case SNX_A64_OP_SQDMULH_vi:
        case SNX_A64_OP_SQDMULH_si: return T_SQDMULH;
        case SNX_A64_OP_SQRDMULH_vi:
        case SNX_A64_OP_SQRDMULH_si: return T_SQRDMULH;
        case SNX_A64_OP_SQRDMLAH_vi:
        case SNX_A64_OP_SQRDMLAH_si: return T_SQRDMLAH;
        case SNX_A64_OP_SQRDMLSH_vi:
        case SNX_A64_OP_SQRDMLSH_si: return T_SQRDMLSH;
        default: return -1;
    }
}

/* the long ops, by register and by element: the table's index (sign in *sign), or -1 */
static int long_of(SnxA64Op op, int* sign) {
    *sign = 1;
    switch (op) {
        case SNX_A64_OP_UMULL_v:
        case SNX_A64_OP_UMULL_vi: *sign = 0; /* fall through */
        case SNX_A64_OP_SMULL_v:
        case SNX_A64_OP_SMULL_vi: return L_MULL;
        case SNX_A64_OP_UMLAL_v:
        case SNX_A64_OP_UMLAL_vi: *sign = 0; /* fall through */
        case SNX_A64_OP_SMLAL_v:
        case SNX_A64_OP_SMLAL_vi: return L_MLAL;
        case SNX_A64_OP_UMLSL_v:
        case SNX_A64_OP_UMLSL_vi: *sign = 0; /* fall through */
        case SNX_A64_OP_SMLSL_v:
        case SNX_A64_OP_SMLSL_vi: return L_MLSL;
        case SNX_A64_OP_UADDL_v: *sign = 0; /* fall through */
        case SNX_A64_OP_SADDL_v: return L_ADDL;
        case SNX_A64_OP_USUBL_v: *sign = 0; /* fall through */
        case SNX_A64_OP_SSUBL_v: return L_SUBL;
        case SNX_A64_OP_UABAL_v: *sign = 0; /* fall through */
        case SNX_A64_OP_SABAL_v: return L_ABAL;
        case SNX_A64_OP_UABDL_v: *sign = 0; /* fall through */
        case SNX_A64_OP_SABDL_v: return L_ABDL;
        case SNX_A64_OP_SQDMULL_v:
        case SNX_A64_OP_SQDMULL_vi: return L_QDMULL;
        case SNX_A64_OP_SQDMLAL_v:
        case SNX_A64_OP_SQDMLAL_vi: return L_QDMLAL;
        case SNX_A64_OP_SQDMLSL_v:
        case SNX_A64_OP_SQDMLSL_vi: return L_QDMLSL;
        default: return -1;
    }
}

static int misc_of(SnxA64Op op, int* scalar) {
    *scalar = 1;
    switch (op) {
        case SNX_A64_OP_ABS_s: return M_ABS;
        case SNX_A64_OP_NEG_s: return M_NEG;
        case SNX_A64_OP_SQABS_s: return M_SQABS;
        case SNX_A64_OP_SQNEG_s: return M_SQNEG;
        case SNX_A64_OP_CMGT0_s: return M_CMGT0;
        case SNX_A64_OP_CMGE0_s: return M_CMGE0;
        case SNX_A64_OP_CMEQ0_s: return M_CMEQ0;
        case SNX_A64_OP_CMLE0_s: return M_CMLE0;
        case SNX_A64_OP_CMLT0_s: return M_CMLT0;
        default: break;
    }
    *scalar = 0;
    switch (op) {
        case SNX_A64_OP_ABS_v: return M_ABS;
        case SNX_A64_OP_NEG_v: return M_NEG;
        case SNX_A64_OP_SQABS_v: return M_SQABS;
        case SNX_A64_OP_SQNEG_v: return M_SQNEG;
        case SNX_A64_OP_CLS_v: return M_CLS;
        case SNX_A64_OP_CLZ_v: return M_CLZ;
        case SNX_A64_OP_CNT_v: return M_CNT;
        case SNX_A64_OP_NOT_v: return M_NOT;
        case SNX_A64_OP_RBIT_v: return M_RBIT;
        case SNX_A64_OP_CMGT0_v: return M_CMGT0;
        case SNX_A64_OP_CMGE0_v: return M_CMGE0;
        case SNX_A64_OP_CMEQ0_v: return M_CMEQ0;
        case SNX_A64_OP_CMLE0_v: return M_CMLE0;
        case SNX_A64_OP_CMLT0_v: return M_CMLT0;
        default: return -1;
    }
}

static int shift_of(SnxA64Op op, int* scalar) {
    *scalar = 1;
    switch (op) {
        case SNX_A64_OP_SSHR_s: return S_SSHR;
        case SNX_A64_OP_USHR_s: return S_USHR;
        case SNX_A64_OP_SSRA_s: return S_SSRA;
        case SNX_A64_OP_USRA_s: return S_USRA;
        case SNX_A64_OP_SRSHR_s: return S_SRSHR;
        case SNX_A64_OP_URSHR_s: return S_URSHR;
        case SNX_A64_OP_SRSRA_s: return S_SRSRA;
        case SNX_A64_OP_URSRA_s: return S_URSRA;
        case SNX_A64_OP_SRI_s: return S_SRI;
        case SNX_A64_OP_SHL_s: return S_SHL;
        case SNX_A64_OP_SLI_s: return S_SLI;
        case SNX_A64_OP_SQSHL_si: return S_SQSHL;
        case SNX_A64_OP_UQSHL_si: return S_UQSHL;
        case SNX_A64_OP_SQSHLU_si: return S_SQSHLU;
        default: break;
    }
    *scalar = 0;
    switch (op) {
        case SNX_A64_OP_SSHR_v: return S_SSHR;
        case SNX_A64_OP_USHR_v: return S_USHR;
        case SNX_A64_OP_SSRA_v: return S_SSRA;
        case SNX_A64_OP_USRA_v: return S_USRA;
        case SNX_A64_OP_SRSHR_v: return S_SRSHR;
        case SNX_A64_OP_URSHR_v: return S_URSHR;
        case SNX_A64_OP_SRSRA_v: return S_SRSRA;
        case SNX_A64_OP_URSRA_v: return S_URSRA;
        case SNX_A64_OP_SRI_v: return S_SRI;
        case SNX_A64_OP_SHL_v: return S_SHL;
        case SNX_A64_OP_SLI_v: return S_SLI;
        case SNX_A64_OP_SQSHL_vi: return S_SQSHL;
        case SNX_A64_OP_UQSHL_vi: return S_UQSHL;
        case SNX_A64_OP_SQSHLU_vi: return S_SQSHLU;
        default: return -1;
    }
}

/* the rounding and saturating shifts by register: helper flags, or -1 */
static int shl_flags(SnxA64Op op, int* scalar) {
    *scalar = 0;
    switch (op) {
        case SNX_A64_OP_SRSHL_s: *scalar = 1; /* fall through */
        case SNX_A64_OP_SRSHL_v: return SHL_ROUND;
        case SNX_A64_OP_URSHL_s: *scalar = 1; /* fall through */
        case SNX_A64_OP_URSHL_v: return SHL_ROUND | SHL_UNS;
        case SNX_A64_OP_SQSHL_s: *scalar = 1; /* fall through */
        case SNX_A64_OP_SQSHL_v: return SHL_SAT;
        case SNX_A64_OP_UQSHL_s: *scalar = 1; /* fall through */
        case SNX_A64_OP_UQSHL_v: return SHL_SAT | SHL_UNS;
        case SNX_A64_OP_SQRSHL_s: *scalar = 1; /* fall through */
        case SNX_A64_OP_SQRSHL_v: return SHL_SAT | SHL_ROUND;
        case SNX_A64_OP_UQRSHL_s: *scalar = 1; /* fall through */
        case SNX_A64_OP_UQRSHL_v: return SHL_SAT | SHL_ROUND | SHL_UNS;
        default: return -1;
    }
}

/* 1 for the ops the switch in int_body emits beyond the tables */
static int int_other(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_SMULL_v:
        case SNX_A64_OP_SQDMULL_si:
        case SNX_A64_OP_SQDMLAL_si:
        case SNX_A64_OP_SQDMLSL_si:
        case SNX_A64_OP_SADDW:
        case SNX_A64_OP_UADDW:
        case SNX_A64_OP_SSUBW:
        case SNX_A64_OP_USUBW:
        case SNX_A64_OP_ADDHN:
        case SNX_A64_OP_RADDHN:
        case SNX_A64_OP_SUBHN:
        case SNX_A64_OP_RSUBHN:
        case SNX_A64_OP_PMULL_p8:
        case SNX_A64_OP_ADDP_s:
        case SNX_A64_OP_REV16_v:
        case SNX_A64_OP_REV32_v:
        case SNX_A64_OP_REV64_v:
        case SNX_A64_OP_SADDLP_v:
        case SNX_A64_OP_UADDLP_v:
        case SNX_A64_OP_SADALP_v:
        case SNX_A64_OP_UADALP_v:
        case SNX_A64_OP_XTN:
        case SNX_A64_OP_SQXTN_v:
        case SNX_A64_OP_UQXTN_v:
        case SNX_A64_OP_SQXTUN_v:
        case SNX_A64_OP_SQXTN_s:
        case SNX_A64_OP_UQXTN_s:
        case SNX_A64_OP_SQXTUN_s:
        case SNX_A64_OP_SHLL_v:
        case SNX_A64_OP_ADDV:
        case SNX_A64_OP_SADDLV:
        case SNX_A64_OP_UADDLV:
        case SNX_A64_OP_SMAXV:
        case SNX_A64_OP_UMAXV:
        case SNX_A64_OP_SMINV:
        case SNX_A64_OP_UMINV:
        case SNX_A64_OP_DUP_element_s:
        case SNX_A64_OP_DUP_element_v:
        case SNX_A64_OP_DUP_general:
        case SNX_A64_OP_INS_general:
        case SNX_A64_OP_SMOV:
        case SNX_A64_OP_UMOV:
        case SNX_A64_OP_INS_element:
        case SNX_A64_OP_Vimm:
        case SNX_A64_OP_FMOVI_v_h:
        case SNX_A64_OP_SSHLL_v:
        case SNX_A64_OP_USHLL_v:
        case SNX_A64_OP_SHRN_v:
        case SNX_A64_OP_RSHRN_v:
        case SNX_A64_OP_SQSHRN_v:
        case SNX_A64_OP_UQSHRN_v:
        case SNX_A64_OP_SQSHRUN_v:
        case SNX_A64_OP_SQRSHRN_v:
        case SNX_A64_OP_UQRSHRN_v:
        case SNX_A64_OP_SQRSHRUN_v:
        case SNX_A64_OP_SQSHRN_si:
        case SNX_A64_OP_UQSHRN_si:
        case SNX_A64_OP_SQSHRUN_si:
        case SNX_A64_OP_SQRSHRN_si:
        case SNX_A64_OP_UQRSHRN_si:
        case SNX_A64_OP_SQRSHRUN_si:
        case SNX_A64_OP_UZP1:
        case SNX_A64_OP_UZP2:
        case SNX_A64_OP_TRN1:
        case SNX_A64_OP_TRN2:
        case SNX_A64_OP_ZIP1:
        case SNX_A64_OP_ZIP2:
        case SNX_A64_OP_EXT_d:
        case SNX_A64_OP_EXT_q:
        case SNX_A64_OP_TBL_TBX:
        case SNX_A64_OP_SDOT_v:
        case SNX_A64_OP_UDOT_v:
        case SNX_A64_OP_USDOT_v:
        case SNX_A64_OP_SDOT_vi:
        case SNX_A64_OP_UDOT_vi:
        case SNX_A64_OP_SUDOT_vi:
        case SNX_A64_OP_USDOT_vi:
        case SNX_A64_OP_SMMLA:
        case SNX_A64_OP_UMMLA:
        case SNX_A64_OP_USMMLA: return 1;
        default: return 0;
    }
}

static int int_handles(SnxA64Op op) {
    int s;
    return same_op(op) >= 0 || element_op(op) >= 0 || long_of(op, &s) >= 0 || misc_of(op, &s) >= 0 ||
           shift_of(op, &s) >= 0 || shl_flags(op, &s) >= 0 || int_other(op);
}

/* the by-element index: H:L:M for halves, H:L for words, H for doublewords (the decoder's idx) */
static void int_body(SnxIr* ir, const SnxA64Insn* insn) {
    const SnxA64Args* a = &insn->args;
    SnxA64Op op = insn->op;
    int t, scalar, sign;
    unsigned esz = (unsigned)a->esz;
    if ((t = same_op(op)) >= 0) {
        three_same(ir, a, (unsigned)t, scalar_same(op) ? 1 : ir_vec_lanes(a->q, esz), -1);
        return;
    }
    if ((t = element_op(op)) >= 0) {
        int scalar_form = op == SNX_A64_OP_SQDMULH_si || op == SNX_A64_OP_SQRDMULH_si || op == SNX_A64_OP_SQRDMLAH_si ||
                          op == SNX_A64_OP_SQRDMLSH_si;
        three_same(ir, a, (unsigned)t, scalar_form ? 1 : ir_vec_lanes(a->q, esz), a->idx);
        return;
    }
    if ((t = long_of(op, &sign)) >= 0) {
        int by_element = op == SNX_A64_OP_SMULL_vi || op == SNX_A64_OP_UMULL_vi || op == SNX_A64_OP_SMLAL_vi ||
                         op == SNX_A64_OP_UMLAL_vi || op == SNX_A64_OP_SMLSL_vi || op == SNX_A64_OP_UMLSL_vi ||
                         op == SNX_A64_OP_SQDMULL_vi || op == SNX_A64_OP_SQDMLAL_vi || op == SNX_A64_OP_SQDMLSL_vi;
        three_long(ir, a, (unsigned)t, sign, by_element ? a->idx : -1);
        return;
    }
    if ((t = misc_of(op, &scalar)) >= 0) {
        two_misc(ir, a, (unsigned)t, scalar && op != SNX_A64_OP_SQABS_s && op != SNX_A64_OP_SQNEG_s ? 3 : esz,
                 scalar ? 1 : ir_vec_lanes(a->q, esz));
        return;
    }
    if ((t = shift_of(op, &scalar)) >= 0) {
        shift_imm_form(ir, a, (unsigned)t, scalar ? 1 : ir_vec_lanes(a->q, esz));
        return;
    }
    if ((t = shl_flags(op, &scalar)) >= 0) {
        shl_helper(ir, a, scalar ? 1 : ir_vec_lanes(a->q, esz), (unsigned)t);
        return;
    }
    switch (op) {
        case SNX_A64_OP_SQDMULL_si: scalar_long(ir, a, L_QDMULL, (unsigned)a->idx); break;
        case SNX_A64_OP_SQDMLAL_si: scalar_long(ir, a, L_QDMLAL, (unsigned)a->idx); break;
        case SNX_A64_OP_SQDMLSL_si: scalar_long(ir, a, L_QDMLSL, (unsigned)a->idx); break;
        case SNX_A64_OP_SADDW: three_wide(ir, a, 1, 0); break;
        case SNX_A64_OP_UADDW: three_wide(ir, a, 0, 0); break;
        case SNX_A64_OP_SSUBW: three_wide(ir, a, 1, 1); break;
        case SNX_A64_OP_USUBW: three_wide(ir, a, 0, 1); break;
        case SNX_A64_OP_ADDHN: three_narrow(ir, a, 0, 0); break;
        case SNX_A64_OP_RADDHN: three_narrow(ir, a, 0, 1); break;
        case SNX_A64_OP_SUBHN: three_narrow(ir, a, 1, 0); break;
        case SNX_A64_OP_RSUBHN: three_narrow(ir, a, 1, 1); break;
        case SNX_A64_OP_PMULL_p8: pmull8(ir, a); break;
        case SNX_A64_OP_ADDP_s: {
            V n = reg(ir, (unsigned)a->rn, 3, 2);
            V r = LLVMBuildAdd(B, LLVMBuildExtractElement(B, n, ir_i32(ir, 0), ""),
                               LLVMBuildExtractElement(B, n, ir_i32(ir, 1), ""), "");
            ir_vec_write(ir, (unsigned)a->rd, ir_vec_splat(ir, r, 1));
            break;
        }
        case SNX_A64_OP_REV16_v: rev(ir, a, 1); break;
        case SNX_A64_OP_REV32_v: rev(ir, a, 2); break;
        case SNX_A64_OP_REV64_v: rev(ir, a, 3); break;
        case SNX_A64_OP_SADDLP_v: addlp(ir, a, 1, 0); break;
        case SNX_A64_OP_UADDLP_v: addlp(ir, a, 0, 0); break;
        case SNX_A64_OP_SADALP_v: addlp(ir, a, 1, 1); break;
        case SNX_A64_OP_UADALP_v: addlp(ir, a, 0, 1); break;
        case SNX_A64_OP_XTN: extract_narrow(ir, a, 0, 0); break;
        case SNX_A64_OP_SQXTN_v: extract_narrow(ir, a, 1, 0); break;
        case SNX_A64_OP_UQXTN_v: extract_narrow(ir, a, 2, 0); break;
        case SNX_A64_OP_SQXTUN_v: extract_narrow(ir, a, 3, 0); break;
        case SNX_A64_OP_SQXTN_s: extract_narrow(ir, a, 1, 1); break;
        case SNX_A64_OP_UQXTN_s: extract_narrow(ir, a, 2, 1); break;
        case SNX_A64_OP_SQXTUN_s: extract_narrow(ir, a, 3, 1); break;
        case SNX_A64_OP_SHLL_v: shll(ir, a); break;
        case SNX_A64_OP_ADDV: across(ir, a, "llvm.vector.reduce.add", -1); break;
        case SNX_A64_OP_SADDLV: across(ir, a, "llvm.vector.reduce.add", 1); break;
        case SNX_A64_OP_UADDLV: across(ir, a, "llvm.vector.reduce.add", 0); break;
        case SNX_A64_OP_SMAXV: across(ir, a, "llvm.vector.reduce.smax", -1); break;
        case SNX_A64_OP_UMAXV: across(ir, a, "llvm.vector.reduce.umax", -1); break;
        case SNX_A64_OP_SMINV: across(ir, a, "llvm.vector.reduce.smin", -1); break;
        case SNX_A64_OP_UMINV: across(ir, a, "llvm.vector.reduce.umin", -1); break;
        case SNX_A64_OP_DUP_element_s:
        case SNX_A64_OP_DUP_element_v:
        case SNX_A64_OP_DUP_general:
        case SNX_A64_OP_INS_general:
        case SNX_A64_OP_SMOV:
        case SNX_A64_OP_UMOV:
        case SNX_A64_OP_INS_element: copy(ir, insn); break;
        case SNX_A64_OP_Vimm: vimm(ir, a); break;
        case SNX_A64_OP_FMOVI_v_h: fmov_h(ir, a); break;
        case SNX_A64_OP_SSHLL_v: shll_imm(ir, a, 1); break;
        case SNX_A64_OP_USHLL_v: shll_imm(ir, a, 0); break;
        case SNX_A64_OP_SHRN_v: shift_narrow(ir, a, 0, 0, 0); break;
        case SNX_A64_OP_RSHRN_v: shift_narrow(ir, a, 0, 1, 0); break;
        case SNX_A64_OP_SQSHRN_v: shift_narrow(ir, a, 1, 0, 0); break;
        case SNX_A64_OP_UQSHRN_v: shift_narrow(ir, a, 2, 0, 0); break;
        case SNX_A64_OP_SQSHRUN_v: shift_narrow(ir, a, 3, 0, 0); break;
        case SNX_A64_OP_SQRSHRN_v: shift_narrow(ir, a, 1, 1, 0); break;
        case SNX_A64_OP_UQRSHRN_v: shift_narrow(ir, a, 2, 1, 0); break;
        case SNX_A64_OP_SQRSHRUN_v: shift_narrow(ir, a, 3, 1, 0); break;
        case SNX_A64_OP_SQSHRN_si: shift_narrow(ir, a, 1, 0, 1); break;
        case SNX_A64_OP_UQSHRN_si: shift_narrow(ir, a, 2, 0, 1); break;
        case SNX_A64_OP_SQSHRUN_si: shift_narrow(ir, a, 3, 0, 1); break;
        case SNX_A64_OP_SQRSHRN_si: shift_narrow(ir, a, 1, 1, 1); break;
        case SNX_A64_OP_UQRSHRN_si: shift_narrow(ir, a, 2, 1, 1); break;
        case SNX_A64_OP_SQRSHRUN_si: shift_narrow(ir, a, 3, 1, 1); break;
        case SNX_A64_OP_UZP1:
        case SNX_A64_OP_UZP2:
        case SNX_A64_OP_TRN1:
        case SNX_A64_OP_TRN2:
        case SNX_A64_OP_ZIP1:
        case SNX_A64_OP_ZIP2: permute(ir, a, op); break;
        case SNX_A64_OP_EXT_d: ext(ir, a, 8); break;
        case SNX_A64_OP_EXT_q: ext(ir, a, 16); break;
        case SNX_A64_OP_TBL_TBX: tbl(ir, a); break;
        case SNX_A64_OP_SDOT_v: dot(ir, a, 1, 1, -1); break;
        case SNX_A64_OP_UDOT_v: dot(ir, a, 0, 0, -1); break;
        case SNX_A64_OP_USDOT_v: dot(ir, a, 0, 1, -1); break;
        case SNX_A64_OP_SDOT_vi: dot(ir, a, 1, 1, a->idx); break;
        case SNX_A64_OP_UDOT_vi: dot(ir, a, 0, 0, a->idx); break;
        case SNX_A64_OP_SUDOT_vi: dot(ir, a, 1, 0, a->idx); break;
        case SNX_A64_OP_USDOT_vi: dot(ir, a, 0, 1, a->idx); break;
        case SNX_A64_OP_SMMLA: mmla(ir, a, 1, 1); break;
        case SNX_A64_OP_UMMLA: mmla(ir, a, 0, 0); break;
        case SNX_A64_OP_USMMLA: mmla(ir, a, 0, 1); break;
        default: break;
    }
}

int snx_ir_vec_handles(SnxA64Op op) { return int_handles(op) || ir_vec_fp_handles(op) || ir_vec_crypto_handles(op); }

void snx_ir_vec(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc) {
    (void)word;
    (void)pc;
    if (int_handles(insn->op))
        int_body(ir, insn);
    else if (ir_vec_fp_handles(insn->op))
        ir_vec_fp(ir, insn);
    else
        ir_vec_crypto(ir, insn);
}
