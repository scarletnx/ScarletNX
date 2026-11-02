/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions, gen_load_exclusive,
   gen_store_exclusive, gen_compare_and_swap, gen_compare_and_swap_pair, do_atomic_ld, do_STG,
   ext_and_shift_reg and the op_addr_* address helpers), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The loads and stores as LLVM IR, keyed on the generated decoder's ops and arguments: the general and FP/SIMD
   register forms of every addressing mode, the pairs, literals, exclusives, load-acquire and store-release, the
   AdvSIMD structure loads and stores, the LSE and RCpc atomics, and the memory-tagging loads and stores as
   FEAT_MTE has them (instructions only: no allocation tags are stored, so a tag reads as 0). Guest memory uses ir_state.c's accessors
   (c->mem plus the guest address, unaligned and volatile only with volatile_memory enabled;
   seq_cst host atomics for the ordered and atomic accesses),
   so a page the GPU owns (memory/own.c) traps on the host access as it does for every other one. The exclusives keep
   cpu.h's value-based monitor through its ldx/stx/ldxp/stxp helpers. */
#include "common/cpu/recompiler/llvm/ir_ldst.h"
#include "common/cpu/aarch64/cpu.h"

typedef LLVMValueRef V;
#define B ir->b

/* ---- operands ---- */

static LLVMTypeRef bytes_type(SnxIr* ir, unsigned n) { return LLVMVectorType(ir->t_i8, n); }

/* Xn, n = 31 being SP (cpu_reg_sp), plus a constant offset */
static V base_plus(SnxIr* ir, unsigned n, SnxU64 offset) {
    V x = ir_x(ir, n);
    return offset ? LLVMBuildAdd(B, x, ir_i64(ir, offset), "") : x;
}

/* the low 8 << sz bits of a load, sign- or zero-extended to 64 bits, and a sign-extended one cut back to 32 when the
   destination is a W register (do_gpr_ld's extend) */
static V extend_load(SnxIr* ir, V v, int sign, int ext) {
    v = ir_cast(ir, v, ir->t_i64, sign);
    return sign && ext ? ir_u64(ir, ir_u32(ir, v)) : v;
}

/* the whole of Vn as 16 bytes, in the guest's order on any host (the CPU structure aligns V128 to 8) */
static V vreg(SnxIr* ir, unsigned n) {
    V load = LLVMBuildLoad2(B, bytes_type(ir, 16), ir_cpu(ir, ir_off_v(n)), "");
    LLVMSetAlignment(load, 8);
    return ir_tag_cpu(ir, load);
}
/* Vn = a 16-byte vector of any lane type */
static void set_vreg(SnxIr* ir, unsigned n, V value) {
    ir_vectors_invalidate(ir);
    V store = LLVMBuildStore(B, value, ir_cpu(ir, ir_off_v(n)));
    LLVMSetAlignment(store, 8);
    ir_tag_cpu(ir, store);
}

/* shufflevector(a, b, mask) with a mask of n indices */
static V shuffle(SnxIr* ir, V a, V b, const unsigned* mask, unsigned n) {
    V m[64];
    for (unsigned i = 0; i < n; ++i) m[i] = ir_i32(ir, mask[i]);
    return LLVMBuildShuffleVector(B, a, b, LLVMConstVector(m, n), "");
}

/* <16 x i8> of the count bytes of v (<total x i8>) from first, the rest zero (do_fp_ld's clear_vec_high) */
static V widen_bytes(SnxIr* ir, V v, unsigned total, unsigned first, unsigned count) {
    unsigned mask[16];
    if (total == 16 && first == 0 && count == 16) return v;
    for (unsigned i = 0; i < 16; ++i) mask[i] = i < count ? first + i : total;
    return shuffle(ir, v, LLVMConstNull(bytes_type(ir, total)), mask, 16);
}

/* ---- general registers: immediate, register and literal offsets ---- */

/* LDR (and its signed and unprivileged forms) of 1 << sz bytes to Rt, or STR from it */
static void gpr_access(SnxIr* ir, V address, unsigned rt, unsigned sz, int sign, int ext, int store) {
    if (store)
        ir_wr(ir, address, 8u << sz, ir_rx(ir, rt));
    else
        ir_setx(ir, rt, extend_load(ir, ir_rd(ir, address, 8u << sz), sign, ext));
}

/* the same for the FP/SIMD registers: the low 1 << sz bytes of Vt, a load clearing the rest (do_fp_ld, do_fp_st) */
static void fp_access(SnxIr* ir, V address, unsigned rt, unsigned sz, int store) {
    unsigned n = 1u << sz;
    if (store && sz < 4)
        /* A scalar store needs only its lane, not a whole V register and a byte shuffle. Keep the access width,
           unaligned guest access and endian conversion of ir_wr; this also avoids vector temporaries at O0. */
        ir_wr(ir, address, 8u << sz, ir_ve(ir, rt, sz, 0));
    else if (store)
        ir_wr_bytes(ir, address, vreg(ir, rt));
    else
        set_vreg(ir, rt, widen_bytes(ir, ir_rd_bytes(ir, address, n), n, 0, n));
}

/* the immediate forms: the address is Xn + imm unless post-indexed (p), and the writeback (w) puts Xn + imm in Xn
   after the access (op_addr_ldst_imm_pre/post). With writeback and Rn == Rt a load's register takes the written-back
   address and a store stores Rt's value from before it, as QEMU orders them */
static void ldst_imm(SnxIr* ir, const SnxA64Args* a, int vector, int store) {
    unsigned rn = (unsigned)a->rn;
    SnxU64 offset = (SnxU64)(int64_t)a->imm;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    if (vector)
        fp_access(ir, address, (unsigned)a->rt, (unsigned)a->sz, store);
    else
        gpr_access(ir, address, (unsigned)a->rt, (unsigned)a->sz, a->sign, a->ext, store);
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* the register offset: Xn + Rm extended by option (UXTW, LSL, SXTW, SXTX) and shifted by the size when s
   (op_addr_ldst_pre, ext_and_shift_reg) */
static void ldst_reg(SnxIr* ir, const SnxA64Args* a, int vector, int store) {
    static const unsigned widths[4] = {8, 16, 32, 64};
    unsigned option = (unsigned)a->opt, sz = (unsigned)a->sz;
    V m = ir_rx(ir, (unsigned)a->rm);
    if ((option & 3) != 3)
        m = ir_cast(ir, ir_cast(ir, m, ir_int_type(ir, widths[option & 3]), 0), ir->t_i64, option & 4);
    if (a->s) m = LLVMBuildShl(B, m, ir_i64(ir, sz), "");
    V address = LLVMBuildAdd(B, ir_x(ir, (unsigned)a->rn), m, "");
    if (vector)
        fp_access(ir, address, (unsigned)a->rt, sz, store);
    else
        gpr_access(ir, address, (unsigned)a->rt, sz, a->sign, a->ext, store);
}

/* ---- pairs ---- */

/* STP, LDP, LDPSW and the non-temporal STNP and LDNP, which take the same rows: the offset is imm scaled by the size,
   and the addressing and writeback are the immediate forms'. A pair of words is one 64-bit access (Rt in the low
   word), a pair of doublewords two. With Rt == Rt2 a load leaves Rt2's value, the second, as QEMU's extracts do;
   with writeback the written-back address wins over a loaded Rn */
static void ldst_pair(SnxIr* ir, const SnxA64Args* a, int vector, int store) {
    unsigned rn = (unsigned)a->rn, rt = (unsigned)a->rt, rt2 = (unsigned)a->rt2, sz = (unsigned)a->sz;
    SnxU64 offset = (SnxU64)(int64_t)a->imm << sz;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    if (vector) { /* one access of both registers' low bytes (trans_LDP_v, trans_STP_v make two adjacent ones) */
        unsigned n = 1u << sz, mask[32];
        if (store) {
            for (unsigned i = 0; i < 2 * n; ++i) mask[i] = i < n ? i : 16 + i - n;
            ir_wr_bytes(ir, address, shuffle(ir, vreg(ir, rt), vreg(ir, rt2), mask, 2 * n));
        } else {
            V both = ir_rd_bytes(ir, address, 2 * n);
            set_vreg(ir, rt, widen_bytes(ir, both, 2 * n, 0, n));
            set_vreg(ir, rt2, widen_bytes(ir, both, 2 * n, n, n));
        }
    } else if (sz == 2) {
        if (store) {
            V low = ir_u64(ir, ir_u32(ir, ir_rx(ir, rt))), high = ir_rx(ir, rt2);
            ir_wr(ir, address, 64, LLVMBuildOr(B, low, LLVMBuildShl(B, high, ir_i64(ir, 32), ""), ""));
        } else {
            V both = ir_rd(ir, address, 64);
            V first = ir_u32(ir, both), second = ir_u32(ir, LLVMBuildLShr(B, both, ir_i64(ir, 32), ""));
            ir_setx(ir, rt, ir_cast(ir, first, ir->t_i64, a->sign));
            ir_setx(ir, rt2, ir_cast(ir, second, ir->t_i64, a->sign));
        }
    } else {
        V next = LLVMBuildAdd(B, address, ir_i64(ir, 8), "");
        if (store) {
            V first = ir_rx(ir, rt), second = ir_rx(ir, rt2);
            ir_wr(ir, address, 64, first);
            ir_wr(ir, next, 64, second);
        } else {
            V first = ir_rd(ir, address, 64), second = ir_rd(ir, next, 64);
            ir_setx(ir, rt, first);
            ir_setx(ir, rt2, second);
        }
    }
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* ---- exclusives, acquire and release ----
   cpu.h's model: the loads are seq_cst host atomics that arm the value-based monitor, a store-exclusive is a
   compare-and-swap from the value they read when the monitor holds the same address and size, and every
   store-exclusive clears it. The acquire and release forms need nothing more: seq_cst orders both ways. A store's Rs
   gets the status after Rt and Rn are read, whatever registers they share; LDXP with Rt == Rt2 leaves Rt2's value */

static void load_exclusive(SnxIr* ir, const SnxA64Args* a, int pair) {
    V address = ir_x(ir, (unsigned)a->rn), size = ir_i32(ir, (unsigned)a->sz);
    if (pair) {
        V first = ir_local(ir, ir->t_i64), second = ir_local(ir, ir->t_i64);
        ir_call_helper(ir, SNX_IR_LDXP, (V[]){ir->c, ir->M, address, size, first, second}, 6);
        ir_setx(ir, (unsigned)a->rt, ir_load(ir, ir->t_i64, first));
        ir_setx(ir, (unsigned)a->rt2, ir_load(ir, ir->t_i64, second));
    } else
        ir_setx(ir, (unsigned)a->rt, ir_call_helper(ir, SNX_IR_LDX, (V[]){ir->c, ir->M, address, size}, 4));
}

static void store_exclusive(SnxIr* ir, const SnxA64Args* a, int pair) {
    V address = ir_x(ir, (unsigned)a->rn), size = ir_i32(ir, (unsigned)a->sz), status;
    if (pair)
        status = ir_call_helper(
            ir, SNX_IR_STXP,
            (V[]){ir->c, ir->M, address, size, ir_rx(ir, (unsigned)a->rt), ir_rx(ir, (unsigned)a->rt2)}, 6);
    else
        status = ir_call_helper(ir, SNX_IR_STX, (V[]){ir->c, ir->M, address, size, ir_rx(ir, (unsigned)a->rt)}, 5);
    ir_setx(ir, (unsigned)a->rs, ir_u64(ir, status));
}

/* LDAR, LDAPR (load-acquire, the RCpc one implemented as the stronger), LDLAR; with an immediate offset LDAPUR and
   its signed forms. STLR, STLLR, STLUR */
static void load_acquire(SnxIr* ir, const SnxA64Args* a, SnxU64 offset, int sign, int ext) {
    V v = ir_ldacq(ir, base_plus(ir, (unsigned)a->rn, offset), 8u << (unsigned)a->sz);
    ir_setx(ir, (unsigned)a->rt, extend_load(ir, v, sign, ext));
}
static void store_release(SnxIr* ir, const SnxA64Args* a, SnxU64 offset) {
    ir_strel(ir, base_plus(ir, (unsigned)a->rn, offset), 8u << (unsigned)a->sz, ir_rx(ir, (unsigned)a->rt));
}

/* ---- the LSE atomics ----
   QEMU's atomic primitives are full barriers, so the acquire and release bits change nothing: these are seq_cst host
   atomics of the access size, the old value zero-extended into Rt (do_atomic_ld). Rs is read before Rt is written */

static V atomic_tag(SnxIr* ir, V access) { return ir_tag_guest(ir, access); }

/* a binary operation of the guest's values, for the big-endian loop */
static V rmw_value(SnxIr* ir, LLVMAtomicRMWBinOp op, V old, V operand) {
    switch (op) {
        case LLVMAtomicRMWBinOpAdd: return LLVMBuildAdd(B, old, operand, "");
        case LLVMAtomicRMWBinOpMax:
            return LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSGT, old, operand, ""), old, operand, "");
        case LLVMAtomicRMWBinOpMin:
            return LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, old, operand, ""), old, operand, "");
        case LLVMAtomicRMWBinOpUMax:
            return LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntUGT, old, operand, ""), old, operand, "");
        default: return LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntULT, old, operand, ""), old, operand, "");
    }
}

/* the old value of an atomic read-modify-write of operand (iN) at address. Bitwise operations and exchanges commute
   with the byte order; on a big-endian host the arithmetic ones are a compare-and-swap loop on the guest's value */
static V atomic_rmw(SnxIr* ir, LLVMAtomicRMWBinOp op, V address, V operand) {
    LLVMTypeRef type = LLVMTypeOf(operand);
    V pointer = ir_mp(ir, address);
    int bitwise = op == LLVMAtomicRMWBinOpXchg || op == LLVMAtomicRMWBinOpAnd || op == LLVMAtomicRMWBinOpOr ||
                  op == LLVMAtomicRMWBinOpXor;
    if (!ir->big_endian || bitwise) {
        V rmw = LLVMBuildAtomicRMW(B, op, pointer, ir_guest_order(ir, operand), LLVMAtomicOrderingSequentiallyConsistent,
                                   0);
        return ir_guest_order(ir, atomic_tag(ir, rmw));
    }
    V first = LLVMBuildLoad2(B, type, pointer, "");
    LLVMSetOrdering(first, LLVMAtomicOrderingSequentiallyConsistent);
    LLVMSetAlignment(first, LLVMGetIntTypeWidth(type) / 8);
    atomic_tag(ir, first);
    LLVMBasicBlockRef entry = LLVMGetInsertBlock(B);
    LLVMBasicBlockRef loop = ir_block(ir, "rmw");
    LLVMBasicBlockRef done = ir_block(ir, "rmw_done");
    LLVMBuildBr(B, loop);
    LLVMPositionBuilderAtEnd(B, loop);
    V old = LLVMBuildPhi(B, type, "");
    V pair = LLVMBuildAtomicCmpXchg(B, pointer, old,
                                    ir_guest_order(ir, rmw_value(ir, op, ir_guest_order(ir, old), operand)),
                                    LLVMAtomicOrderingSequentiallyConsistent, LLVMAtomicOrderingSequentiallyConsistent,
                                    0);
    atomic_tag(ir, pair);
    V seen = LLVMBuildExtractValue(B, pair, 0, "");
    LLVMAddIncoming(old, &first, &entry, 1);
    LLVMAddIncoming(old, &seen, &loop, 1);
    LLVMBuildCondBr(B, LLVMBuildExtractValue(B, pair, 1, ""), done, loop);
    LLVMPositionBuilderAtEnd(B, done);
    return ir_guest_order(ir, old);
}

static void atomic_op(SnxIr* ir, const SnxA64Args* a, LLVMAtomicRMWBinOp op, int invert) {
    unsigned bits = 8u << (unsigned)a->sz;
    V address = ir_x(ir, (unsigned)a->rn);
    V operand = ir_cast(ir, ir_rx(ir, (unsigned)a->rs), ir_int_type(ir, bits), 0);
    if (invert) operand = LLVMBuildNot(B, operand, ""); /* LDCLR: AND NOT */
    ir_setx(ir, (unsigned)a->rt, ir_u64(ir, atomic_rmw(ir, op, address, operand)));
}

/* the old value of a compare-and-swap of iN values at address, seq_cst both ways */
static V compare_swap(SnxIr* ir, V address, V expected, V desired) {
    V pair = LLVMBuildAtomicCmpXchg(B, ir_mp(ir, address), ir_guest_order(ir, expected), ir_guest_order(ir, desired),
                                    LLVMAtomicOrderingSequentiallyConsistent, LLVMAtomicOrderingSequentiallyConsistent,
                                    0);
    atomic_tag(ir, pair);
    return ir_guest_order(ir, LLVMBuildExtractValue(B, pair, 0, ""));
}

/* CAS, CASA, CASAL, CASL: Rs = the old value, Rt stored where it equalled Rs (gen_compare_and_swap) */
static void cas(SnxIr* ir, const SnxA64Args* a) {
    LLVMTypeRef type = ir_int_type(ir, 8u << (unsigned)a->sz);
    V expected = ir_cast(ir, ir_rx(ir, (unsigned)a->rs), type, 0), desired = ir_cast(ir, ir_rx(ir, (unsigned)a->rt), type, 0);
    ir_setx(ir, (unsigned)a->rs, ir_u64(ir, compare_swap(ir, ir_x(ir, (unsigned)a->rn), expected, desired)));
}

/* CASP's 16-byte compare-and-swap through snx_cas128, the one the exclusives use (cpu.h): the two halves read, the
   new value Rt:Rt2 if they equal Rs:Rs+1 and else themselves, retried until the compare-and-swap of what was read
   succeeds, so the old value returned is one the location held */
static void compare_swap128(SnxIr* ir, V address, V cmp_low, V cmp_high, V new_low, V new_high, V* low, V* high) {
    LLVMTypeRef params[6];
    LLVMBasicBlockRef loop = ir_block(ir, "cas128"), done = ir_block(ir, "cas128_done");
    V next = LLVMBuildAdd(B, address, ir_i64(ir, 8), ""), args[6];
    params[0] = ir->t_ptr;
    params[1] = ir->t_ptr;
    params[2] = params[3] = params[4] = params[5] = ir->t_i64;
    LLVMBuildBr(B, loop);
    LLVMPositionBuilderAtEnd(B, loop);
    V lo = ir_ldacq(ir, address, 64);
    V hi = ir_ldacq(ir, next, 64);
    V equal = LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntEQ, lo, cmp_low, ""),
                           LLVMBuildICmp(B, LLVMIntEQ, hi, cmp_high, ""), "");
    V n0 = LLVMBuildSelect(B, equal, new_low, lo, "");
    V n1 = LLVMBuildSelect(B, equal, new_high, hi, "");
    /* snx_cas128 takes the host's reads of the two doublewords */
    args[0] = ir->c;
    args[1] = ir_mp(ir, address);
    args[2] = ir_guest_order(ir, lo);
    args[3] = ir_guest_order(ir, hi);
    args[4] = ir_guest_order(ir, n0);
    args[5] = ir_guest_order(ir, n1);
    V ok = ir_call(ir, ir_extern(ir, "snx_cas128", ir->t_i32, params, 6), args, 6);
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntNE, ok, ir_i32(ir, 0), ""), done, loop);
    LLVMPositionBuilderAtEnd(B, done);
    *low = lo;
    *high = hi;
}

/* CASP and its ordered forms: Rs:Rs+1 against the pair at Xn, Rt:Rt+1 stored, the old pair to Rs:Rs+1 (even
   registers, so Rs+1 is the zero register for Rs 30). A pair of words is one 64-bit compare-and-swap, Rs the low
   word (gen_compare_and_swap_pair) */
static void casp(SnxIr* ir, const SnxA64Args* a) {
    unsigned rs = (unsigned)a->rs, rt = (unsigned)a->rt;
    V address = ir_x(ir, (unsigned)a->rn);
    if (a->sz == 2) {
        V thirty_two = ir_i64(ir, 32);
        V expected = LLVMBuildOr(B, ir_u64(ir, ir_u32(ir, ir_rx(ir, rs))),
                                 LLVMBuildShl(B, ir_rx(ir, rs + 1), thirty_two, ""), "");
        V desired = LLVMBuildOr(B, ir_u64(ir, ir_u32(ir, ir_rx(ir, rt))),
                                LLVMBuildShl(B, ir_rx(ir, rt + 1), thirty_two, ""), "");
        V old = compare_swap(ir, address, expected, desired);
        ir_setx(ir, rs, ir_u64(ir, ir_u32(ir, old)));
        ir_setx(ir, rs + 1, LLVMBuildLShr(B, old, thirty_two, ""));
    } else {
        V low, high;
        compare_swap128(ir, address, ir_rx(ir, rs), ir_rx(ir, rs + 1), ir_rx(ir, rt), ir_rx(ir, rt + 1), &low, &high);
        ir_setx(ir, rs, low);
        ir_setx(ir, rs + 1, high);
    }
}

/* ---- the AdvSIMD structures ---- */

/* the post-index writeback of the structure forms: Xn += Xm, or the bytes transferred for Rm 31 */
static void structure_writeback(SnxIr* ir, const SnxA64Args* a, V base, unsigned total) {
    unsigned rm = (unsigned)a->rm;
    if (!a->p) return;
    ir_set_x(ir, (unsigned)a->rn, LLVMBuildAdd(B, base, rm == 31 ? ir_i64(ir, total) : ir_x(ir, rm), ""));
}

/* LD1-LD4 and ST1-ST4 (multiple structures): rpt registers of selem-element structures, interleaved in memory
   (element e of register r + x at ((r * elements + e) * selem + x) << sz), the register list wrapping at V31. One
   access of all the bytes, the interleaving done as a byte shuffle; a 64-bit load clears its registers' high halves
   (trans_LD_mult, trans_ST_mult) */
static void ldst_mult(SnxIr* ir, const SnxA64Args* a, int store) {
    unsigned size = (unsigned)a->sz, esize = 1u << size, vbytes = a->q ? 16 : 8, elements = vbytes >> size;
    unsigned selem = (unsigned)a->selem, rpt = (unsigned)a->rpt, regs = rpt * selem, total = regs * vbytes;
    unsigned rt = (unsigned)a->rt, mask[64];
    V base = ir_x(ir, (unsigned)a->rn);
    /* where byte o of the memory comes from, as register j's byte */
    for (unsigned i = 0; i < total; ++i) {
        unsigned k = i >> size, x = k % selem, e = (k / selem) % elements, r = k / (selem * elements);
        mask[i] = (r + x) * 16 + e * esize + (i & (esize - 1));
    }
    if (store) {
        V v[4], wide;
        for (unsigned j = 0; j < regs; ++j) v[j] = vreg(ir, (rt + j) % 32);
        if (regs == 1)
            wide = v[0];
        else {
            unsigned concat[64];
            for (unsigned i = 0; i < 64; ++i) concat[i] = i;
            wide = shuffle(ir, v[0], v[1], concat, 32);
            if (regs > 2)
                wide = shuffle(ir, wide, shuffle(ir, v[2], regs > 3 ? v[3] : LLVMConstNull(bytes_type(ir, 16)), concat, 32),
                               concat, 64);
        }
        ir_wr_bytes(ir, base, shuffle(ir, wide, wide, mask, total));
    } else {
        V memory = ir_rd_bytes(ir, base, total), zero = LLVMConstNull(bytes_type(ir, total));
        for (unsigned j = 0; j < regs; ++j) {
            unsigned lanes[16];
            for (unsigned i = 0; i < 16; ++i) lanes[i] = total; /* zero */
            for (unsigned i = 0; i < total; ++i)
                if (mask[i] / 16 == j) lanes[mask[i] % 16] = i;
            set_vreg(ir, (rt + j) % 32, shuffle(ir, memory, zero, lanes, 16));
        }
    }
    structure_writeback(ir, a, base, total);
}

/* LD1-LD4 and ST1-ST4 (single structure): element index of selem consecutive registers, the other elements kept
   (trans_LD_single, trans_ST_single) */
static void ldst_single(SnxIr* ir, const SnxA64Args* a, int store) {
    unsigned scale = (unsigned)a->scale, esize = 1u << scale, selem = (unsigned)a->selem;
    V base = ir_x(ir, (unsigned)a->rn);
    for (unsigned x = 0; x < selem; ++x) {
        unsigned t = ((unsigned)a->rt + x) % 32;
        V address = x ? LLVMBuildAdd(B, base, ir_i64(ir, x * esize), "") : base;
        if (store)
            ir_wr(ir, address, 8 * esize, ir_ve(ir, t, scale, (unsigned)a->index));
        else
            ir_set_ve(ir, t, scale, (unsigned)a->index, ir_rd(ir, address, 8 * esize));
    }
    structure_writeback(ir, a, base, selem * esize);
}

/* LD1R-LD4R: one element to every element of selem consecutive registers, the high half cleared for a 64-bit
   arrangement (trans_LD_single_repl) */
static void ld_single_repl(SnxIr* ir, const SnxA64Args* a) {
    unsigned scale = (unsigned)a->scale, esize = 1u << scale, selem = (unsigned)a->selem, lanes = 16 >> scale;
    LLVMTypeRef type = ir_int_type(ir, 8 * esize), vector = LLVMVectorType(type, lanes);
    V base = ir_x(ir, (unsigned)a->rn);
    for (unsigned x = 0; x < selem; ++x) {
        unsigned mask[16];
        V address = x ? LLVMBuildAdd(B, base, ir_i64(ir, x * esize), "") : base;
        V value = ir_rd(ir, address, 8 * esize);
        V splat = LLVMBuildInsertElement(B, LLVMConstNull(vector), value, ir_i32(ir, 0), "");
        for (unsigned i = 0; i < lanes; ++i) mask[i] = a->q || i < lanes / 2 ? 0 : lanes;
        splat = shuffle(ir, splat, LLVMConstNull(vector), mask, lanes);
        set_vreg(ir, ((unsigned)a->rt + x) % 32, ir_guest_order(ir, splat));
    }
    structure_writeback(ir, a, base, selem * esize);
}

/* ---- memory tagging (FEAT_MTE, instructions only) ----
   No allocation tags are stored: STG and ST2G only address their granules, the zeroing forms clear them, LDG gives
   Rt a 0 tag, and STGP is the store of its pair, as QEMU has them with tag access off (do_STG, trans_LDG,
   trans_STGP). The granule is 16 bytes, and an immediate counts granules (the decoder scales it) */

/* STG, STZG (zero), ST2G, STZ2G (pair: two granules) */
static void store_tag(SnxIr* ir, const SnxA64Args* a, int zero, int pair) {
    unsigned rn = (unsigned)a->rn;
    SnxU64 offset = (SnxU64)(int64_t)a->imm;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    if (zero) ir_wr_bytes(ir, address, LLVMConstNull(bytes_type(ir, pair ? 32 : 16)));
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* LDG: Rt with its tag bits 59:56 cleared, the tag of a granule holding none */
static void load_tag(SnxIr* ir, const SnxA64Args* a) {
    unsigned rt = (unsigned)a->rt;
    ir_setx(ir, rt, LLVMBuildAnd(B, ir_rx(ir, rt), ir_i64(ir, ~(0xFull << 56)), ""));
}

/* STGP: Xt and Xt2 to the 16 bytes at the address, the offset scaled by the granule */
static void store_tag_pair(SnxIr* ir, const SnxA64Args* a) {
    unsigned rn = (unsigned)a->rn;
    SnxU64 offset = (SnxU64)(int64_t)a->imm << 4;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    V first = ir_rx(ir, (unsigned)a->rt), second = ir_rx(ir, (unsigned)a->rt2);
    ir_wr(ir, address, 64, first);
    ir_wr(ir, LLVMBuildAdd(B, address, ir_i64(ir, 8), ""), 64, second);
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* ---- the entry points ---- */

int snx_ir_ldst_handles(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_STR_i:
        case SNX_A64_OP_LDR_i:
        case SNX_A64_OP_STR_v_i:
        case SNX_A64_OP_LDR_v_i:
        case SNX_A64_OP_STR:
        case SNX_A64_OP_LDR:
        case SNX_A64_OP_STR_v:
        case SNX_A64_OP_LDR_v:
        case SNX_A64_OP_LD_lit:
        case SNX_A64_OP_LD_lit_v:
        case SNX_A64_OP_STP:
        case SNX_A64_OP_LDP:
        case SNX_A64_OP_STP_v:
        case SNX_A64_OP_LDP_v:
        case SNX_A64_OP_STXR:
        case SNX_A64_OP_LDXR:
        case SNX_A64_OP_STXP:
        case SNX_A64_OP_LDXP:
        case SNX_A64_OP_STLR:
        case SNX_A64_OP_LDAR:
        case SNX_A64_OP_LDAPR:
        case SNX_A64_OP_LDAPR_i:
        case SNX_A64_OP_STLR_i:
        case SNX_A64_OP_CAS:
        case SNX_A64_OP_CASP:
        case SNX_A64_OP_LDADD:
        case SNX_A64_OP_LDCLR:
        case SNX_A64_OP_LDEOR:
        case SNX_A64_OP_LDSET:
        case SNX_A64_OP_LDSMAX:
        case SNX_A64_OP_LDSMIN:
        case SNX_A64_OP_LDUMAX:
        case SNX_A64_OP_LDUMIN:
        case SNX_A64_OP_SWP:
        case SNX_A64_OP_ST_mult:
        case SNX_A64_OP_LD_mult:
        case SNX_A64_OP_ST_single:
        case SNX_A64_OP_LD_single:
        case SNX_A64_OP_LD_single_repl:
        case SNX_A64_OP_STGP:
        case SNX_A64_OP_STG:
        case SNX_A64_OP_STZG:
        case SNX_A64_OP_ST2G:
        case SNX_A64_OP_STZ2G:
        case SNX_A64_OP_LDG: return 1;
        default: return 0;
    }
}

void snx_ir_ldst(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc) {
    const SnxA64Args* a = &insn->args;
    (void)word;
    switch (insn->op) {
        case SNX_A64_OP_STR_i: ldst_imm(ir, a, 0, 1); break;
        case SNX_A64_OP_LDR_i: ldst_imm(ir, a, 0, 0); break;
        case SNX_A64_OP_STR_v_i: ldst_imm(ir, a, 1, 1); break;
        case SNX_A64_OP_LDR_v_i: ldst_imm(ir, a, 1, 0); break;
        case SNX_A64_OP_STR: ldst_reg(ir, a, 0, 1); break;
        case SNX_A64_OP_LDR: ldst_reg(ir, a, 0, 0); break;
        case SNX_A64_OP_STR_v: ldst_reg(ir, a, 1, 1); break;
        case SNX_A64_OP_LDR_v: ldst_reg(ir, a, 1, 0); break;
        case SNX_A64_OP_LD_lit: /* PC-relative, LDRSW (sign) included */
            gpr_access(ir, ir_i64(ir, pc + (SnxU64)(int64_t)a->imm), (unsigned)a->rt, (unsigned)a->sz, a->sign, 0, 0);
            break;
        case SNX_A64_OP_LD_lit_v:
            fp_access(ir, ir_i64(ir, pc + (SnxU64)(int64_t)a->imm), (unsigned)a->rt, (unsigned)a->sz, 0);
            break;
        case SNX_A64_OP_STP: ldst_pair(ir, a, 0, 1); break;
        case SNX_A64_OP_LDP: ldst_pair(ir, a, 0, 0); break;
        case SNX_A64_OP_STP_v: ldst_pair(ir, a, 1, 1); break;
        case SNX_A64_OP_LDP_v: ldst_pair(ir, a, 1, 0); break;
        case SNX_A64_OP_STXR: store_exclusive(ir, a, 0); break;
        case SNX_A64_OP_LDXR: load_exclusive(ir, a, 0); break;
        case SNX_A64_OP_STXP: store_exclusive(ir, a, 1); break;
        case SNX_A64_OP_LDXP: load_exclusive(ir, a, 1); break;
        case SNX_A64_OP_STLR: store_release(ir, a, 0); break;
        case SNX_A64_OP_LDAR:
        case SNX_A64_OP_LDAPR: load_acquire(ir, a, 0, 0, 0); break;
        case SNX_A64_OP_LDAPR_i: load_acquire(ir, a, (SnxU64)(int64_t)a->imm, a->sign, a->ext); break;
        case SNX_A64_OP_STLR_i: store_release(ir, a, (SnxU64)(int64_t)a->imm); break;
        case SNX_A64_OP_CAS: cas(ir, a); break;
        case SNX_A64_OP_CASP: casp(ir, a); break;
        case SNX_A64_OP_LDADD: atomic_op(ir, a, LLVMAtomicRMWBinOpAdd, 0); break;
        case SNX_A64_OP_LDCLR: atomic_op(ir, a, LLVMAtomicRMWBinOpAnd, 1); break;
        case SNX_A64_OP_LDEOR: atomic_op(ir, a, LLVMAtomicRMWBinOpXor, 0); break;
        case SNX_A64_OP_LDSET: atomic_op(ir, a, LLVMAtomicRMWBinOpOr, 0); break;
        case SNX_A64_OP_LDSMAX: atomic_op(ir, a, LLVMAtomicRMWBinOpMax, 0); break;
        case SNX_A64_OP_LDSMIN: atomic_op(ir, a, LLVMAtomicRMWBinOpMin, 0); break;
        case SNX_A64_OP_LDUMAX: atomic_op(ir, a, LLVMAtomicRMWBinOpUMax, 0); break;
        case SNX_A64_OP_LDUMIN: atomic_op(ir, a, LLVMAtomicRMWBinOpUMin, 0); break;
        case SNX_A64_OP_SWP: atomic_op(ir, a, LLVMAtomicRMWBinOpXchg, 0); break;
        case SNX_A64_OP_ST_mult: ldst_mult(ir, a, 1); break;
        case SNX_A64_OP_LD_mult: ldst_mult(ir, a, 0); break;
        case SNX_A64_OP_ST_single: ldst_single(ir, a, 1); break;
        case SNX_A64_OP_LD_single: ldst_single(ir, a, 0); break;
        case SNX_A64_OP_LD_single_repl: ld_single_repl(ir, a); break;
        case SNX_A64_OP_STGP: store_tag_pair(ir, a); break;
        case SNX_A64_OP_STG: store_tag(ir, a, 0, 0); break;
        case SNX_A64_OP_STZG: store_tag(ir, a, 1, 0); break;
        case SNX_A64_OP_ST2G: store_tag(ir, a, 0, 1); break;
        case SNX_A64_OP_STZ2G: store_tag(ir, a, 1, 1); break;
        case SNX_A64_OP_LDG: load_tag(ir, a); break;
        default: break;
    }
}
