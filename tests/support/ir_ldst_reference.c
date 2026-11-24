/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions, gen_load_exclusive,
   gen_store_exclusive, gen_compare_and_swap, gen_compare_and_swap_pair, do_atomic_ld, ext_and_shift_reg and the
   op_addr_* address helpers), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The general-purpose registers' load and store emitters written by hand that the ones tools/sailgen generates from
   the Sail took the place of (ir_ldst.c, ir_sail.c), kept as they were for tests/llvm_sail_ir.c, which holds the
   generated IR to theirs word by word */
#include "support/ir_ldst_reference.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"

typedef LLVMValueRef V;
#define B ir->b

/* ---- the general registers ---- */

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

/* LDR (and its signed and unprivileged forms) of 1 << sz bytes to Rt, or STR from it */
static void gpr_access(SnxIr* ir, V address, unsigned rt, unsigned sz, int sign, int ext, int store) {
    if (store)
        ir_wr(ir, address, 8u << sz, ir_rx(ir, rt));
    else
        ir_setx(ir, rt, extend_load(ir, ir_rd(ir, address, 8u << sz), sign, ext));
}

/* the immediate forms: the address is Xn + imm unless post-indexed (p), and the writeback (w) puts Xn + imm in Xn
   after the access (op_addr_ldst_imm_pre/post). With writeback and Rn == Rt a load's register takes the written-back
   address and a store stores Rt's value from before it, as QEMU orders them */
static void ldst_imm(SnxIr* ir, const SnxA64Args* a, int store) {
    unsigned rn = (unsigned)a->rn;
    SnxU64 offset = (SnxU64)(int64_t)a->imm;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    gpr_access(ir, address, (unsigned)a->rt, (unsigned)a->sz, a->sign, a->ext, store);
    if (a->w) ir_set_x(ir, rn, a->p ? LLVMBuildAdd(B, base, ir_i64(ir, offset), "") : address);
}

/* LDRAA, LDRAB, their keys disabled: LDR of a doubleword, the offset before it, written back where w */
static void load_authenticated(SnxIr* ir, const SnxA64Args* a) {
    unsigned rn = (unsigned)a->rn;
    V address = LLVMBuildAdd(B, ir_x(ir, rn), ir_i64(ir, (SnxU64)(int64_t)a->imm), "");
    gpr_access(ir, address, (unsigned)a->rt, 3, 0, 0, 0);
    if (a->w) ir_set_x(ir, rn, address);
}

/* the register offset: Xn + Rm extended by option (UXTW, LSL, SXTW, SXTX) and shifted by the size when s
   (op_addr_ldst_pre, ext_and_shift_reg) */
static void ldst_reg(SnxIr* ir, const SnxA64Args* a, int store) {
    static const unsigned widths[4] = {8, 16, 32, 64};
    unsigned option = (unsigned)a->opt, sz = (unsigned)a->sz;
    V m = ir_rx(ir, (unsigned)a->rm);
    if ((option & 3) != 3)
        m = ir_cast(ir, ir_cast(ir, m, ir_int_type(ir, widths[option & 3]), 0), ir->t_i64, option & 4);
    if (a->s) m = LLVMBuildShl(B, m, ir_i64(ir, sz), "");
    V address = LLVMBuildAdd(B, ir_x(ir, (unsigned)a->rn), m, "");
    gpr_access(ir, address, (unsigned)a->rt, sz, a->sign, a->ext, store);
}

/* STP, LDP, LDPSW and the non-temporal STNP and LDNP, which take the same rows: the offset is imm scaled by the size,
   and the addressing and writeback are the immediate forms'. A pair of words is one 64-bit access (Rt in the low
   word), a pair of doublewords two. With Rt == Rt2 a load leaves Rt2's value, the second, as QEMU's extracts do;
   with writeback the written-back address wins over a loaded Rn */
static void ldst_pair(SnxIr* ir, const SnxA64Args* a, int store) {
    unsigned rn = (unsigned)a->rn, rt = (unsigned)a->rt, rt2 = (unsigned)a->rt2, sz = (unsigned)a->sz;
    SnxU64 offset = (SnxU64)(int64_t)a->imm << sz;
    V base = ir_x(ir, rn), address = a->p ? base : LLVMBuildAdd(B, base, ir_i64(ir, offset), "");
    if (sz == 2) {
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
   with the byte order; on a big-endian host the arithmetic ones are a compare-and-swap loop on the guest's value. So
   are PowerPC64's byte and halfword UMAX and UMIN: its LLVM compares the operand in all 32 bits of its register
   (cmplw after lbarx or lharx, the operand never zero-extended), which bits above the byte or halfword change */
static V atomic_rmw(SnxIr* ir, LLVMAtomicRMWBinOp op, V address, V operand) {
    LLVMTypeRef type = LLVMTypeOf(operand);
    V pointer = ir_mp(ir, address);
    int bitwise = op == LLVMAtomicRMWBinOpXchg || op == LLVMAtomicRMWBinOpAnd || op == LLVMAtomicRMWBinOpOr ||
                  op == LLVMAtomicRMWBinOpXor;
    int partword_unsigned =
        ir->ppc && LLVMGetIntTypeWidth(type) < 32 && (op == LLVMAtomicRMWBinOpUMax || op == LLVMAtomicRMWBinOpUMin);
    if ((!ir->big_endian || bitwise) && !partword_unsigned) {
        V rmw = LLVMBuildAtomicRMW(B, op, pointer, ir_guest_order(ir, operand),
                                   LLVMAtomicOrderingSequentiallyConsistent, 0);
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
    V pair =
        LLVMBuildAtomicCmpXchg(B, pointer, old, ir_guest_order(ir, rmw_value(ir, op, ir_guest_order(ir, old), operand)),
                               LLVMAtomicOrderingSequentiallyConsistent, LLVMAtomicOrderingSequentiallyConsistent, 0);
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
    V pair =
        LLVMBuildAtomicCmpXchg(B, ir_mp(ir, address), ir_guest_order(ir, expected), ir_guest_order(ir, desired),
                               LLVMAtomicOrderingSequentiallyConsistent, LLVMAtomicOrderingSequentiallyConsistent, 0);
    atomic_tag(ir, pair);
    return ir_guest_order(ir, LLVMBuildExtractValue(B, pair, 0, ""));
}

/* CAS, CASA, CASAL, CASL: Rs = the old value, Rt stored where it equalled Rs (gen_compare_and_swap) */
static void cas(SnxIr* ir, const SnxA64Args* a) {
    LLVMTypeRef type = ir_int_type(ir, 8u << (unsigned)a->sz);
    V expected = ir_cast(ir, ir_rx(ir, (unsigned)a->rs), type, 0),
      desired = ir_cast(ir, ir_rx(ir, (unsigned)a->rt), type, 0);
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
    /* snx_cas128 takes the doublewords' values, as STXP's (cpu.h), and a big-endian host's puts them in memory order */
    args[0] = ir->c;
    args[1] = ir_mp(ir, address);
    args[2] = lo;
    args[3] = hi;
    args[4] = n0;
    args[5] = n1;
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

void snx_ir_ldst_reference(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc) {
    const SnxA64Insn* insn = (const SnxA64Insn*)decoded;
    const SnxA64Args* a = &insn->args;
    (void)word;
    switch (insn->op) {
        case SNX_A64_OP_STR_i: ldst_imm(ir, a, 1); break;
        case SNX_A64_OP_LDR_i: ldst_imm(ir, a, 0); break;
        case SNX_A64_OP_STR: ldst_reg(ir, a, 1); break;
        case SNX_A64_OP_LDR: ldst_reg(ir, a, 0); break;
        case SNX_A64_OP_LDRA: load_authenticated(ir, a); break;
        case SNX_A64_OP_LD_lit: /* PC-relative, LDRSW (sign) included */
            gpr_access(ir, ir_i64(ir, pc + (SnxU64)(int64_t)a->imm), (unsigned)a->rt, (unsigned)a->sz, a->sign, 0, 0);
            break;
        case SNX_A64_OP_STP: ldst_pair(ir, a, 1); break;
        case SNX_A64_OP_LDP: ldst_pair(ir, a, 0); break;
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
        default: break;
    }
}
