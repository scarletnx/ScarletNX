/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions, and its
   logic_imm_decode_wmask), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The integer data-processing, branch-operand, hint, barrier and system instructions as LLVM IR, keyed on the
   generated decoder's ops and arguments. The system registers and operations EL0 reaches follow the Arm ARM's EL0
   access rules under Horizon's EL1 controls (SCTLR_EL1.UCT, DZE and UCI set, UMA clear; CNTKCTL_EL1 lets EL0 read the
   counters and their frequency): every other one raises UNDEF, as on the console. */
#include "common/cpu/recompiler/llvm/ir_int.h"
#include "common/cpu/aarch64/cpu.h"

typedef LLVMValueRef V;
#define B ir->b

/* the values a Switch's Cortex-A57 and its kernel give EL0 (the A57 TRM; helpers/integer.c has the same) */
#define SWITCH_CTR_EL0   0x8444C004u /* 64-byte cache lines */
#define SWITCH_DCZID_EL0 4u          /* DC ZVA zeroes 64 bytes */
#define SWITCH_CNTFRQ    19200000u
/* the Switch 2's counter: its SDK builds 31.25 MHz into its tick conversions and never reads CNTFRQ_EL0 */
#define SWITCH2_CNTFRQ 31250000u

/* ---- operands ---- */

static LLVMTypeRef width(SnxIr* ir, int sf) { return sf ? ir->t_i64 : ir->t_i32; }
static unsigned bits_of(int sf) { return sf ? 64 : 32; }
static V konst(SnxIr* ir, int sf, SnxU64 v) { return LLVMConstInt(width(ir, sf), sf ? v : (SnxU32)v, 0); }
static SnxU64 ones(unsigned n) { return n >= 64 ? ~0ull : (1ull << n) - 1; }

/* Xn or Wn, n = 31 being the zero register (QEMU's cpu_reg) */
static V reg(SnxIr* ir, unsigned n, int sf) {
    V x = ir_rx(ir, n);
    return sf ? x : ir_u32(ir, x);
}
/* the same with n = 31 the stack pointer (cpu_reg_sp) */
static V reg_sp(SnxIr* ir, unsigned n, int sf) {
    V x = ir_x(ir, n);
    return sf ? x : ir_u32(ir, x);
}
/* a W result is zero-extended into its X register; writes to the zero register are dropped */
static void set_reg(SnxIr* ir, unsigned n, V v) { ir_setx(ir, n, ir_u64(ir, v)); }
static void set_reg_sp(SnxIr* ir, unsigned n, V v) { ir_set_x(ir, n, ir_u64(ir, v)); }

static V call_intrinsic(SnxIr* ir, const char* name, LLVMTypeRef type, V a, V b, V c) {
    V args[3];
    unsigned n = 0;
    args[n++] = a;
    if (b) args[n++] = b;
    if (c) args[n++] = c;
    return ir_intrinsic(ir, name, type, args, n);
}
/* (hi:lo) >> amount, the low half: a rotate when hi and lo are one value */
static V funnel_right(SnxIr* ir, V hi, V lo, V amount) {
    return call_intrinsic(ir, "llvm.fshr", LLVMTypeOf(hi), hi, lo, amount);
}
static V count_leading_zeros(SnxIr* ir, V v) {
    return call_intrinsic(ir, "llvm.ctlz", LLVMTypeOf(v), v, LLVMConstInt(ir->t_i1, 0, 0), NULL);
}
static V nonzero(SnxIr* ir, V v) { return LLVMBuildICmp(B, LLVMIntNE, v, LLVMConstNull(LLVMTypeOf(v)), ""); }

/* a register operand shifted by a constant below its width: LSL, LSR, ASR, ROR (shift_reg_imm) */
static V shift_imm(SnxIr* ir, V v, unsigned type, unsigned amount, int sf) {
    V n = konst(ir, sf, amount);
    if (!amount) return v;
    switch (type) {
        case 0: return LLVMBuildShl(B, v, n, "");
        case 1: return LLVMBuildLShr(B, v, n, "");
        case 2: return LLVMBuildAShr(B, v, n, "");
        default: return funnel_right(ir, v, v, n);
    }
}

/* the N, Z, C and V an addition or subtraction a - b sets, as a nibble with N in bit 3 */
static V nzcv_of(SnxIr* ir, int sf, V a, V b, int subtract) {
    V r = subtract ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, ""), sign = konst(ir, sf, bits_of(sf) - 1);
    V n = ir_u32(ir, LLVMBuildLShr(B, r, sign, ""));
    V z = ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, r, konst(ir, sf, 0), ""));
    V c = ir_bool32(ir, subtract ? LLVMBuildICmp(B, LLVMIntUGE, a, b, "") : LLVMBuildICmp(B, LLVMIntULT, r, a, ""));
    V differ = LLVMBuildXor(B, a, b, ""), changed = LLVMBuildXor(B, a, r, "");
    V overflow = LLVMBuildAnd(B, subtract ? differ : LLVMBuildNot(B, differ, ""), changed, "");
    V v = ir_u32(ir, LLVMBuildLShr(B, overflow, sign, ""));
    V high = LLVMBuildOr(B, LLVMBuildShl(B, n, ir_i32(ir, 3), ""), LLVMBuildShl(B, z, ir_i32(ir, 2), ""), "");
    return LLVMBuildOr(B, high, LLVMBuildOr(B, LLVMBuildShl(B, c, ir_i32(ir, 1), ""), v, ""), "");
}

/* a + b with flags (ADDS) or a - b (SUBS) when setflags, else plain */
static V add_sub(SnxIr* ir, int sf, V a, V b, int subtract, int setflags) {
    if (setflags)
        return subtract ? ir_flags_subs(ir, bits_of(sf), a, b) : ir_flags_adds(ir, bits_of(sf), a, b, ir_i32(ir, 0));
    return subtract ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, "");
}

/* ---- data processing (immediate) ---- */

static void addsub_imm(SnxIr* ir, const SnxA64Args* a, int subtract, int setflags) {
    V r = add_sub(ir, a->sf, reg_sp(ir, (unsigned)a->rn, a->sf), konst(ir, a->sf, (SnxU64)(unsigned)a->imm), subtract,
                  setflags);
    if (setflags)
        set_reg(ir, (unsigned)a->rd, r);
    else
        set_reg_sp(ir, (unsigned)a->rd, r);
}

/* DecodeBitMasks' wmask (logic_imm_decode_wmask): 0 for a reserved immediate */
static int bitmask_imm(SnxU64* result, unsigned immn, unsigned imms, unsigned immr) {
    unsigned v = (immn << 6) | (~imms & 0x3Fu), len = 0;
    if (!v) return 0;
    while (v >> (len + 1)) ++len;
    if (len < 1) return 0; /* immn 0, imms 11111x */
    unsigned e = 1u << len;
    unsigned levels = e - 1;
    unsigned s = imms & levels;
    unsigned r = immr & levels;
    if (s == levels) return 0;
    SnxU64 mask = ones(s + 1);
    if (r) mask = ((mask >> r) | (mask << (e - r))) & ones(e);
    for (; e < 64; e *= 2) mask |= mask << e;
    *result = mask;
    return 1;
}

static V logic(SnxIr* ir, SnxA64Op op, V a, V b) {
    switch (op) {
        case SNX_A64_OP_ORR_i:
        case SNX_A64_OP_ORR_r: return LLVMBuildOr(B, a, b, "");
        case SNX_A64_OP_EOR_i:
        case SNX_A64_OP_EOR_r: return LLVMBuildXor(B, a, b, "");
        default: return LLVMBuildAnd(B, a, b, ""); /* AND, ANDS */
    }
}

static int logic_imm(SnxIr* ir, SnxA64Op op, const SnxA64Args* a) {
    SnxU64 imm;
    if (!bitmask_imm(&imm, ((unsigned)a->dbm >> 12) & 1, (unsigned)a->dbm & 0x3F, ((unsigned)a->dbm >> 6) & 0x3F))
        return 0;
    V r = logic(ir, op, reg(ir, (unsigned)a->rn, a->sf), konst(ir, a->sf, imm));
    if (op == SNX_A64_OP_ANDS_i) {
        ir_flags_logic(ir, r, bits_of(a->sf));
        set_reg(ir, (unsigned)a->rd, r);
    } else
        set_reg_sp(ir, (unsigned)a->rd, r);
    return 1;
}

static void movw(SnxIr* ir, SnxA64Op op, const SnxA64Args* a) {
    unsigned pos = (unsigned)a->hw << 4;
    SnxU64 imm = (SnxU64)(unsigned)a->imm << pos;
    if (op == SNX_A64_OP_MOVK) {
        V old = reg(ir, (unsigned)a->rd, a->sf);
        set_reg(ir, (unsigned)a->rd,
                LLVMBuildOr(B, LLVMBuildAnd(B, old, konst(ir, a->sf, ~(0xFFFFull << pos)), ""), konst(ir, a->sf, imm),
                            ""));
        return;
    }
    if (op == SNX_A64_OP_MOVN) imm = ~imm;
    set_reg(ir, (unsigned)a->rd, konst(ir, a->sf, imm));
}

/* bits [pos, pos+len) of a 64-bit value, sign-extended */
static V sextract(SnxIr* ir, V v, unsigned pos, unsigned len) {
    return LLVMBuildAShr(B, LLVMBuildShl(B, v, ir_i64(ir, 64 - pos - len), ""), ir_i64(ir, 64 - len), "");
}

static void bitfield(SnxIr* ir, SnxA64Op op, const SnxA64Args* a) {
    unsigned size = bits_of(a->sf), r = (unsigned)a->immr, s = (unsigned)a->imms, pos, len;
    V src = ir_rx(ir, (unsigned)a->rn), v;
    if (s >= r) { /* Wd<s-r:0> = Wn<s:r> */
        pos = 0;
        len = s - r + 1;
    } else { /* Wd<32+s-r,32-r> = Wn<s:0> */
        pos = (size - r) & (size - 1);
        len = s + 1;
    }
    if (op == SNX_A64_OP_SBFM) {
        if (s >= r)
            v = sextract(ir, src, r, len);
        else {
            if (len < r) { /* the deposit below carries the field's sign up to bit pos + r */
                src = sextract(ir, src, 0, len);
                len = r;
            }
            v = LLVMBuildShl(B, LLVMBuildAnd(B, src, ir_i64(ir, ones(len)), ""), ir_i64(ir, pos), "");
        }
    } else {
        V field = LLVMBuildAnd(B, s >= r ? LLVMBuildLShr(B, src, ir_i64(ir, r), "") : src, ir_i64(ir, ones(len)), "");
        v = LLVMBuildShl(B, field, ir_i64(ir, pos), "");
        if (op == SNX_A64_OP_BFM)
            v = LLVMBuildOr(B, LLVMBuildAnd(B, ir_rx(ir, (unsigned)a->rd), ir_i64(ir, ~(ones(len) << pos)), ""), v, "");
    }
    set_reg(ir, (unsigned)a->rd, a->sf ? v : ir_u32(ir, v));
}

static void extr(SnxIr* ir, const SnxA64Args* a) {
    set_reg(ir, (unsigned)a->rd,
            funnel_right(ir, reg(ir, (unsigned)a->rn, a->sf), reg(ir, (unsigned)a->rm, a->sf),
                         konst(ir, a->sf, (SnxU64)(unsigned)a->imm)));
}

/* ---- data processing (register) ---- */

static void logic_reg(SnxIr* ir, SnxA64Op op, const SnxA64Args* a) {
    V b = shift_imm(ir, reg(ir, (unsigned)a->rm, a->sf), (unsigned)a->st, (unsigned)a->sa, a->sf);
    if (a->n) b = LLVMBuildNot(B, b, ""); /* BIC, ORN, EON, BICS */
    V r = logic(ir, op, reg(ir, (unsigned)a->rn, a->sf), b);
    if (op == SNX_A64_OP_ANDS_r) ir_flags_logic(ir, r, bits_of(a->sf));
    set_reg(ir, (unsigned)a->rd, r);
}

static void addsub_reg(SnxIr* ir, const SnxA64Args* a, int subtract, int setflags) {
    V b = shift_imm(ir, reg(ir, (unsigned)a->rm, a->sf), (unsigned)a->st, (unsigned)a->sa, a->sf);
    set_reg(ir, (unsigned)a->rd, add_sub(ir, a->sf, reg(ir, (unsigned)a->rn, a->sf), b, subtract, setflags));
}

/* option: UXTB, UXTH, UXTW, UXTX, then the signed four; shifted left by 0-4 (ext_and_shift_reg) */
static void addsub_ext(SnxIr* ir, const SnxA64Args* a, int subtract, int setflags) {
    static const unsigned sizes[3] = {8, 16, 32};
    unsigned option = (unsigned)a->st;
    V m = ir_rx(ir, (unsigned)a->rm);
    if ((option & 3) != 3) {
        LLVMTypeRef narrow = LLVMIntTypeInContext(ir->ctx, sizes[option & 3]);
        m = ir_cast(ir, LLVMBuildTrunc(B, m, narrow, ""), ir->t_i64, (option & 4) != 0);
    }
    m = LLVMBuildShl(B, m, ir_i64(ir, (unsigned)a->sa), "");
    V r = add_sub(ir, a->sf, reg_sp(ir, (unsigned)a->rn, a->sf), a->sf ? m : ir_u32(ir, m), subtract, setflags);
    if (setflags)
        set_reg(ir, (unsigned)a->rd, r);
    else
        set_reg_sp(ir, (unsigned)a->rd, r);
}

/* Rn + Rm + C, or Rn + NOT(Rm) + C for the subtractions */
static void adc(SnxIr* ir, const SnxA64Args* a, int subtract, int setflags) {
    V n = reg(ir, (unsigned)a->rn, a->sf), y = reg(ir, (unsigned)a->rm, a->sf), carry = ir_flag_read(ir, 2), r;
    if (subtract) y = LLVMBuildNot(B, y, "");
    if (setflags)
        r = ir_flags_adds(ir, bits_of(a->sf), n, y, carry);
    else
        r = LLVMBuildAdd(B, LLVMBuildAdd(B, n, y, ""), ir_cast(ir, carry, width(ir, a->sf), 0), "");
    set_reg(ir, (unsigned)a->rd, r);
}

/* CCMP (op 1) and CCMN: the comparison's flags when cond holds, else the immediate nzcv */
static void ccmp(SnxIr* ir, const SnxA64Args* a) {
    V holds = nonzero(ir, ir_flags_cond(ir, (unsigned)a->cond));
    V y = a->imm ? konst(ir, a->sf, (SnxU64)(unsigned)a->y) : reg(ir, (unsigned)a->y, a->sf);
    V flags = nzcv_of(ir, a->sf, reg(ir, (unsigned)a->rn, a->sf), y, a->op);
    ir_flags_set_nzcv(ir, LLVMBuildSelect(B, holds, flags, ir_i32(ir, (unsigned)a->nzcv), ""));
}

/* CSEL, CSINC (else_inc), CSINV (else_inv), CSNEG (both) */
static void csel(SnxIr* ir, const SnxA64Args* a) {
    V holds = nonzero(ir, ir_flags_cond(ir, (unsigned)a->cond)), other = reg(ir, (unsigned)a->rm, a->sf);
    if (a->else_inv && a->else_inc)
        other = LLVMBuildNeg(B, other, "");
    else if (a->else_inv)
        other = LLVMBuildNot(B, other, "");
    else if (a->else_inc)
        other = LLVMBuildAdd(B, other, konst(ir, a->sf, 1), "");
    set_reg(ir, (unsigned)a->rd, LLVMBuildSelect(B, holds, reg(ir, (unsigned)a->rn, a->sf), other, ""));
}

static void dp1(SnxIr* ir, SnxA64Op op, const SnxA64Args* a) {
    int sf = op == SNX_A64_OP_REV64 ? 1 : a->sf;
    LLVMTypeRef type = width(ir, sf);
    V x = reg(ir, (unsigned)a->rn, sf), r;
    switch (op) {
        case SNX_A64_OP_RBIT: r = call_intrinsic(ir, "llvm.bitreverse", type, x, NULL, NULL); break;
        case SNX_A64_OP_REV16: {
            V mask = konst(ir, sf, 0x00FF00FF00FF00FFull), eight = konst(ir, sf, 8);
            r = LLVMBuildOr(B, LLVMBuildShl(B, LLVMBuildAnd(B, x, mask, ""), eight, ""),
                            LLVMBuildAnd(B, LLVMBuildLShr(B, x, eight, ""), mask, ""), "");
            break;
        }
        case SNX_A64_OP_REV32:
            r = call_intrinsic(ir, "llvm.bswap", type, x, NULL, NULL);
            if (sf) r = funnel_right(ir, r, r, ir_i64(ir, 32)); /* each word reversed in place */
            break;
        case SNX_A64_OP_REV64: r = call_intrinsic(ir, "llvm.bswap", type, x, NULL, NULL); break;
        case SNX_A64_OP_CLZ: r = count_leading_zeros(ir, x); break;
        default: /* CLS: the leading zeros of x XOR (x ASR 1), less the sign bit itself */
            r = count_leading_zeros(ir, LLVMBuildXor(B, x, LLVMBuildAShr(B, x, konst(ir, sf, 1), ""), ""));
            r = LLVMBuildSub(B, r, konst(ir, sf, 1), "");
            break;
    }
    set_reg(ir, (unsigned)a->rd, r);
}

/* UDIV and SDIV: x / 0 is 0, and the most negative value / -1 is itself (do_div) */
static void divide(SnxIr* ir, const SnxA64Args* a, int is_signed) {
    V n = reg(ir, (unsigned)a->rn, a->sf), m = reg(ir, (unsigned)a->rm, a->sf), one = konst(ir, a->sf, 1);
    V by_zero = LLVMBuildICmp(B, LLVMIntEQ, m, konst(ir, a->sf, 0), ""), avoid = by_zero;
    V zero = konst(ir, a->sf, 0);
    if (is_signed) {
        V minimum = LLVMBuildICmp(B, LLVMIntEQ, n, konst(ir, a->sf, 1ull << (bits_of(a->sf) - 1)), "");
        avoid = LLVMBuildOr(B, avoid,
                            LLVMBuildAnd(B, minimum, LLVMBuildICmp(B, LLVMIntEQ, m, konst(ir, a->sf, ~0ull), ""), ""),
                            "");
    }
    m = LLVMBuildSelect(B, avoid, one, m, "");
    V r = is_signed ? LLVMBuildSDiv(B, n, m, "") : LLVMBuildUDiv(B, n, m, "");
    set_reg(ir, (unsigned)a->rd, LLVMBuildSelect(B, by_zero, zero, r, ""));
}

/* LSLV, LSRV, ASRV, RORV: the amount is Rm modulo the width (do_shift_reg) */
static void shift_reg(SnxIr* ir, SnxA64Op op, const SnxA64Args* a) {
    V n = reg(ir, (unsigned)a->rn, a->sf), r;
    V amount = LLVMBuildAnd(B, reg(ir, (unsigned)a->rm, a->sf), konst(ir, a->sf, bits_of(a->sf) - 1), "");
    switch (op) {
        case SNX_A64_OP_LSLV: r = LLVMBuildShl(B, n, amount, ""); break;
        case SNX_A64_OP_LSRV: r = LLVMBuildLShr(B, n, amount, ""); break;
        case SNX_A64_OP_ASRV: r = LLVMBuildAShr(B, n, amount, ""); break;
        default: r = funnel_right(ir, n, n, amount); break;
    }
    set_reg(ir, (unsigned)a->rd, r);
}

/* ---- CRC32 and CRC32C ----
   The architecture's CRC32 is the bit-reflected CRC update without the pre- and post-inversion (QEMU's crc32_64 and
   crc32c_64 helpers undo zlib's). On a host without the instruction it is computed by slicing: table k holds the CRC
   of a byte followed by k zero bytes, so the n bytes of (acc XOR value) are n independent lookups */
static V crc_table(SnxIr* ir, int castagnoli) {
    const char* name = castagnoli ? "snx_crc32c_slices" : "snx_crc32_slices";
    V table = LLVMGetNamedGlobal(ir->mod, name);
    if (!table) {
        SnxU32 entries[8 * 256], poly = castagnoli ? 0x82F63B78u : 0xEDB88320u, c;
        V values[8 * 256];
        LLVMTypeRef type = LLVMArrayType(ir->t_i32, 8 * 256);
        for (unsigned i = 0; i < 256; ++i) {
            c = i;
            for (unsigned k = 0; k < 8; ++k) c = (c >> 1) ^ (poly & (0u - (c & 1)));
            entries[i] = c;
        }
        for (unsigned i = 256; i < 8 * 256; ++i)
            entries[i] = (entries[i - 256] >> 8) ^ entries[entries[i - 256] & 0xFF];
        for (unsigned i = 0; i < 8 * 256; ++i) values[i] = ir_i32(ir, entries[i]);
        table = LLVMAddGlobal(ir->mod, type, name);
        LLVMSetLinkage(table, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(table, LLVMGlobalUnnamedAddr);
        LLVMSetGlobalConstant(table, 1);
        LLVMSetInitializer(table, LLVMConstArray(ir->t_i32, values, 8 * 256));
    }
    return table;
}

static V crc_sliced(SnxIr* ir, V acc, V value, unsigned bytes, int castagnoli) {
    V table = crc_table(ir, castagnoli);
    LLVMTypeRef type = LLVMArrayType(ir->t_i32, 8 * 256);
    V m = LLVMBuildXor(B, LLVMBuildAnd(B, value, ir_i64(ir, ones(8 * bytes)), ""), ir_u64(ir, acc), "");
    V r = bytes < 4 ? LLVMBuildLShr(B, acc, ir_i32(ir, 8 * bytes), "") : ir_i32(ir, 0);
    for (unsigned k = 0; k < bytes; ++k) {
        V byte = LLVMBuildAnd(B, LLVMBuildLShr(B, m, ir_i64(ir, 8 * k), ""), ir_i64(ir, 0xFF), "");
        V index[2];
        index[0] = ir_i64(ir, 0);
        index[1] = LLVMBuildAdd(B, byte, ir_i64(ir, 256 * (bytes - 1 - k)), "");
        V entry = LLVMBuildLoad2(B, ir->t_i32, LLVMBuildInBoundsGEP2(B, type, table, index, 2, ""), "");
        r = LLVMBuildXor(B, r, entry, "");
    }
    return r;
}

static V crc32(SnxIr* ir, V acc, V value, unsigned esz, int castagnoli) {
    static const char* const arm[2][4] = {
        {"llvm.aarch64.crc32b", "llvm.aarch64.crc32h", "llvm.aarch64.crc32w", "llvm.aarch64.crc32x"},
        {"llvm.aarch64.crc32cb", "llvm.aarch64.crc32ch", "llvm.aarch64.crc32cw", "llvm.aarch64.crc32cx"}};
    static const char* const x86[4] = {"llvm.x86.sse42.crc32.32.8", "llvm.x86.sse42.crc32.32.16",
                                       "llvm.x86.sse42.crc32.32.32", "llvm.x86.sse42.crc32.64.64"};
    if (ir->features->aarch64 && ir->features->arm_crc)
        return call_intrinsic(ir, arm[castagnoli][esz], NULL, acc, esz == 3 ? value : ir_u32(ir, value), NULL);
    if (castagnoli && ir->features->x86 && ir->features->crc32) {
        if (esz == 3) return ir_u32(ir, call_intrinsic(ir, x86[3], NULL, ir_u64(ir, acc), value, NULL));
        return call_intrinsic(ir, x86[esz], NULL, acc,
                              LLVMBuildTrunc(B, value, LLVMIntTypeInContext(ir->ctx, 8u << esz), ""), NULL);
    }
    return crc_sliced(ir, acc, value, 1u << esz, castagnoli);
}

/* ---- data processing (3 source) ---- */

static void dp3(SnxIr* ir, SnxA64Op op, const SnxA64Args* a) {
    unsigned rn = (unsigned)a->rn, rm = (unsigned)a->rm, ra = (unsigned)a->ra;
    V r;
    switch (op) {
        case SNX_A64_OP_MADD_w:
        case SNX_A64_OP_MSUB_w:
        case SNX_A64_OP_MADD_x:
        case SNX_A64_OP_MSUB_x: {
            int sf = op == SNX_A64_OP_MADD_x || op == SNX_A64_OP_MSUB_x;
            V product = LLVMBuildMul(B, reg(ir, rn, sf), reg(ir, rm, sf), "");
            r = op == SNX_A64_OP_MSUB_w || op == SNX_A64_OP_MSUB_x ? LLVMBuildSub(B, reg(ir, ra, sf), product, "")
                                                                     : LLVMBuildAdd(B, reg(ir, ra, sf), product, "");
            break;
        }
        case SNX_A64_OP_SMULH:
        case SNX_A64_OP_UMULH: {
            int is_signed = op == SNX_A64_OP_SMULH;
            V n = ir_cast(ir, ir_rx(ir, rn), ir->t_i128, is_signed), m = ir_cast(ir, ir_rx(ir, rm), ir->t_i128, is_signed);
            V wide = LLVMBuildMul(B, n, m, "");
            r = LLVMBuildTrunc(B, LLVMBuildLShr(B, wide, LLVMConstInt(ir->t_i128, 64, 0), ""), ir->t_i64, "");
            break;
        }
        default: { /* SMADDL, SMSUBL, UMADDL, UMSUBL: 32 x 32 to 64 bits */
            int is_signed = op == SNX_A64_OP_SMADDL || op == SNX_A64_OP_SMSUBL;
            V n = ir_cast(ir, reg(ir, rn, 0), ir->t_i64, is_signed), m = ir_cast(ir, reg(ir, rm, 0), ir->t_i64, is_signed);
            V product = LLVMBuildMul(B, n, m, "");
            r = op == SNX_A64_OP_SMSUBL || op == SNX_A64_OP_UMSUBL ? LLVMBuildSub(B, ir_rx(ir, ra), product, "")
                                                                   : LLVMBuildAdd(B, ir_rx(ir, ra), product, "");
            break;
        }
    }
    set_reg(ir, (unsigned)a->rd, r);
}

/* ---- hints, barriers and the system instructions ---- */

static void undef(SnxIr* ir, SnxU64 pc, SnxU32 word) {
    ir_set_pc(ir, ir_i64(ir, pc));
    ir_undef(ir, pc, word);
}

/* DMB and DSB: loads before loads and stores (MBReqTypes_Reads) is an acquire fence, stores before stores a release
   one (which also orders loads before stores), anything else a full one (trans_DSB_DMB) */
static void barrier(SnxIr* ir, unsigned types) {
    if (types == 1)
        LLVMBuildFence(B, LLVMAtomicOrderingAcquire, 0, "");
    else if (types == 2)
        LLVMBuildFence(B, LLVMAtomicOrderingRelease, 0, "");
    else
        ir_fence(ir);
}

/* op0:op1:CRn:CRm:op2 as helpers/integer.c's snx_mrs and snx_msr take them (op0 3) */
#define SYSREG(op1, crn, crm, op2) ((1u << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

static V mrs_helper(SnxIr* ir, SnxU32 key) {
    LLVMTypeRef params[2];
    V args[2];
    params[0] = ir->t_ptr;
    params[1] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir_i32(ir, key);
    return ir_call(ir, ir_extern(ir, "snx_mrs", ir->t_i64, params, 2), args, 2);
}
static void msr_helper(SnxIr* ir, SnxU32 key, V value) {
    LLVMTypeRef params[3];
    V args[3];
    params[0] = ir->t_ptr;
    params[1] = ir->t_i32;
    params[2] = ir->t_i64;
    args[0] = ir->c;
    args[1] = ir_i32(ir, key);
    args[2] = value;
    ir_call(ir, ir_extern(ir, "snx_msr", ir->t_void, params, 3), args, 3);
}

/* MRS of the registers EL0 reads on the Switch: NULL for the rest, which are UNDEF there (the EL1 registers, the
   ID registers MIDR_EL1 included, which Horizon doesn't emulate as Linux does, DAIF without SCTLR_EL1.UMA, the
   timers' control registers) */
static V read_sysreg(SnxIr* ir, SnxU32 key) {
    switch (key) {
        case SYSREG(3, 4, 2, 0): { /* NZCV */
            V v = ir_i32(ir, 0);
            for (unsigned i = 0; i < 4; ++i)
                v = LLVMBuildOr(B, v, LLVMBuildShl(B, ir_flag_read(ir, i), ir_i32(ir, 31 - i), ""), "");
            return ir_u64(ir, v);
        }
        case SYSREG(3, 4, 4, 0): return ir_u64(ir, ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpcr)));
        case SYSREG(3, 4, 4, 1): return ir_u64(ir, ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpsr)));
        case SYSREG(3, 13, 0, 2): return ir_cpu_load(ir, ir->t_i64, offsetof(CPU, tpidr));   /* TPIDR_EL0 */
        case SYSREG(3, 13, 0, 3): return ir_cpu_load(ir, ir->t_i64, offsetof(CPU, tpidrro)); /* TPIDRRO_EL0 */
        case SYSREG(3, 0, 0, 1): return ir_i64(ir, SWITCH_CTR_EL0);                          /* SCTLR_EL1.UCT */
        case SYSREG(3, 0, 0, 7): return ir_i64(ir, SWITCH_DCZID_EL0);
        case SYSREG(3, 14, 0, 0):
            return ir_i64(ir, ir->guest_features == SNX_A64_TARGET_SWITCH2 ? SWITCH2_CNTFRQ : SWITCH_CNTFRQ);
        case SYSREG(3, 14, 0, 1): /* CNTPCT_EL0, the SDK's system tick */
        case SYSREG(3, 14, 0, 2): /* CNTVCT_EL0 */
            return mrs_helper(ir, key);
        default: return NULL;
    }
}

/* MSR of the registers EL0 writes: 0 for the rest (read-only at EL0, or EL1's) */
static int write_sysreg(SnxIr* ir, SnxU32 key, V value) {
    switch (key) {
        case SYSREG(3, 4, 2, 0): /* NZCV from bits 31:28 */
            ir_flags_set_nzcv(ir, ir_u32(ir, LLVMBuildLShr(B, value, ir_i64(ir, 28), "")));
            return 1;
        case SYSREG(3, 4, 4, 0): /* FPCR: the bits the guest's features implement (FZ16 with FEAT_FP16), the
                                    runtime's FPCR hook */
            if (!(ir->guest_features & SNX_A64_FEAT_FP16))
                value = LLVMBuildAnd(B, value, ir_i64(ir, ~(SnxU64)(1u << 19)), "");
            msr_helper(ir, key, value);
            return 1;
        case SYSREG(3, 4, 4, 1): msr_helper(ir, key, value); return 1; /* FPSR: the bits the A57 keeps */
        case SYSREG(3, 13, 0, 2): ir_cpu_store(ir, value, offsetof(CPU, tpidr)); return 1;
        default: return 0;
    }
}

/* SYS, SYSL, MRS and MSR (register) */
static void sys(SnxIr* ir, const SnxA64Args* a, SnxU32 word, SnxU64 pc) {
    unsigned rt = (unsigned)a->rt;
    if (a->op0 == 3) {
        SnxU32 key = SYSREG((unsigned)a->op1, (unsigned)a->crn, (unsigned)a->crm, (unsigned)a->op2);
        if (a->l) {
            V v = read_sysreg(ir, key);
            if (v) {
                set_reg(ir, rt, v);
                return;
            }
        } else if (write_sysreg(ir, key, ir_rx(ir, rt)))
            return;
    } else if (a->op0 == 1 && !a->l && a->op1 == 3 && a->crn == 7 && a->op2 == 1) {
        switch (a->crm) {
            case 4: /* DC ZVA (SCTLR_EL1.DZE): the 64-byte block holding Xt */
                ir_call_helper(ir, SNX_IR_DC_ZVA, (V[]){ir->M, ir_rx(ir, rt)}, 2);
                return;
            case 5: { /* IC IVAU: what was compiled from the line Xt is in is stale (helpers/integer.c's snx_ic_ivau) */
                LLVMTypeRef params[2];
                V args[2];
                params[0] = ir->t_ptr;
                params[1] = ir->t_i64;
                args[0] = ir->c;
                args[1] = ir_rx(ir, rt);
                ir_call(ir, ir_extern(ir, "snx_ic_ivau", ir->t_void, params, 2), args, 2);
                return;
            }
            case 10: /* DC CVAC */
            case 11: /* DC CVAU */
            case 14: /* DC CIVAC (SCTLR_EL1.UCI): the host keeps instruction and data coherent */
                return;
            default: break;
        }
    }
    undef(ir, pc, word);
}
#undef SYSREG

/* ---- pointer authentication (FEAT_PAuth) ----
   As with the address keys off (SCTLR_EL1.EnIA, EnIB, EnDA and EnDB clear), which is how eden-ns2 runs Switch 2 programs:
   the Arm ARM's AddPAC and Auth give the pointer back unchanged when the key is off, so PAC* and AUT* leave their
   register as it is, and the combined branches and returns go to the register (snx_ir_int_operand). The guest signs
   and checks with the same identity, so every pointer it signed is the one it gets back */

/* XPACI, XPACD and XPACLRI: the Arm ARM's Strip with TBI off, bit 55 over the bits above the address. 48 bits is the
   widest address without FEAT_LVA. A Horizon process's 39-bit addresses have zeroes from bit 39 up, as every pointer
   does that PAC* here signed, so the narrower field of the console strips them alike */
static void xpac(SnxIr* ir, unsigned rd) {
    const SnxU64 address = ones(48);
    V x = ir_rx(ir, rd);
    V bit55 = LLVMBuildAShr(B, LLVMBuildShl(B, x, ir_i64(ir, 8), ""), ir_i64(ir, 63), "");
    set_reg(ir, rd,
            LLVMBuildOr(B, LLVMBuildAnd(B, x, ir_i64(ir, address), ""), LLVMBuildAnd(B, bit55, ir_i64(ir, ~address), ""),
                        ""));
}

/* MurmurHash3's 64-bit finalizer (Austin Appleby, public domain) */
static V fmix64(SnxIr* ir, V h) {
    h = LLVMBuildXor(B, h, LLVMBuildLShr(B, h, ir_i64(ir, 33), ""), "");
    h = LLVMBuildMul(B, h, ir_i64(ir, 0xFF51AFD7ED558CCDull), "");
    h = LLVMBuildXor(B, h, LLVMBuildLShr(B, h, ir_i64(ir, 33), ""), "");
    h = LLVMBuildMul(B, h, ir_i64(ir, 0xC4CEB9FE1A85EC53ull), "");
    return LLVMBuildXor(B, h, LLVMBuildLShr(B, h, ir_i64(ir, 33), ""), "");
}
/* PACGA Xd, Xn, Xm|SP: the generic key's PAC of Xn with the modifier in the upper half, zero in the lower. No enable
   bit turns it off. EL0 cannot read the key, and the Switch 2 SDK compares a PACGA only with another one of the same
   run (it signs a saved context's registers and checks them when it restores it), so a keyed function that gives one
   value for one input serves as the key's QARMA: two rounds of fmix64 under a fixed key */
#define PACGA_KEY 0x5C4E1E7A11D2B3F9ull
static void pacga(SnxIr* ir, const SnxA64Args* a) {
    V h = fmix64(ir, LLVMBuildXor(B, ir_x(ir, (unsigned)a->rm), ir_i64(ir, PACGA_KEY), ""));
    h = fmix64(ir, LLVMBuildXor(B, h, ir_rx(ir, (unsigned)a->rn), ""));
    set_reg(ir, (unsigned)a->rd, LLVMBuildAnd(B, h, ir_i64(ir, 0xFFFFFFFF00000000ull), ""));
}

/* ---- the entry points ---- */

int snx_ir_int_handles(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_ADR:
        case SNX_A64_OP_ADRP:
        case SNX_A64_OP_ADD_i:
        case SNX_A64_OP_ADDS_i:
        case SNX_A64_OP_SUB_i:
        case SNX_A64_OP_SUBS_i:
        case SNX_A64_OP_AND_i:
        case SNX_A64_OP_ORR_i:
        case SNX_A64_OP_EOR_i:
        case SNX_A64_OP_ANDS_i:
        case SNX_A64_OP_MOVN:
        case SNX_A64_OP_MOVZ:
        case SNX_A64_OP_MOVK:
        case SNX_A64_OP_SBFM:
        case SNX_A64_OP_BFM:
        case SNX_A64_OP_UBFM:
        case SNX_A64_OP_EXTR:
        case SNX_A64_OP_AND_r:
        case SNX_A64_OP_ORR_r:
        case SNX_A64_OP_EOR_r:
        case SNX_A64_OP_ANDS_r:
        case SNX_A64_OP_ADD_r:
        case SNX_A64_OP_SUB_r:
        case SNX_A64_OP_ADDS_r:
        case SNX_A64_OP_SUBS_r:
        case SNX_A64_OP_ADD_ext:
        case SNX_A64_OP_SUB_ext:
        case SNX_A64_OP_ADDS_ext:
        case SNX_A64_OP_SUBS_ext:
        case SNX_A64_OP_ADC:
        case SNX_A64_OP_ADCS:
        case SNX_A64_OP_SBC:
        case SNX_A64_OP_SBCS:
        case SNX_A64_OP_CCMP:
        case SNX_A64_OP_CSEL:
        case SNX_A64_OP_RBIT:
        case SNX_A64_OP_REV16:
        case SNX_A64_OP_REV32:
        case SNX_A64_OP_REV64:
        case SNX_A64_OP_CLZ:
        case SNX_A64_OP_CLS:
        case SNX_A64_OP_UDIV:
        case SNX_A64_OP_SDIV:
        case SNX_A64_OP_LSLV:
        case SNX_A64_OP_LSRV:
        case SNX_A64_OP_ASRV:
        case SNX_A64_OP_RORV:
        case SNX_A64_OP_CRC32:
        case SNX_A64_OP_CRC32C:
        case SNX_A64_OP_MADD_w:
        case SNX_A64_OP_MSUB_w:
        case SNX_A64_OP_MADD_x:
        case SNX_A64_OP_MSUB_x:
        case SNX_A64_OP_SMADDL:
        case SNX_A64_OP_SMSUBL:
        case SNX_A64_OP_UMADDL:
        case SNX_A64_OP_UMSUBL:
        case SNX_A64_OP_SMULH:
        case SNX_A64_OP_UMULH:
        case SNX_A64_OP_B_cond:
        case SNX_A64_OP_CBZ:
        case SNX_A64_OP_TBZ:
        case SNX_A64_OP_BR:
        case SNX_A64_OP_BLR:
        case SNX_A64_OP_RET:
        case SNX_A64_OP_SVC:
        case SNX_A64_OP_BRK:
        case SNX_A64_OP_NOP:
        case SNX_A64_OP_YIELD:
        case SNX_A64_OP_WFE:
        case SNX_A64_OP_WFI:
        case SNX_A64_OP_SEV:
        case SNX_A64_OP_SEVL:
        case SNX_A64_OP_CLREX:
        case SNX_A64_OP_DSB_DMB:
        case SNX_A64_OP_ISB:
        case SNX_A64_OP_SYS:
        case SNX_A64_OP_XPACLRI:
        case SNX_A64_OP_PACIA1716:
        case SNX_A64_OP_PACIB1716:
        case SNX_A64_OP_AUTIA1716:
        case SNX_A64_OP_AUTIB1716:
        case SNX_A64_OP_PACIAZ:
        case SNX_A64_OP_PACIASP:
        case SNX_A64_OP_PACIBZ:
        case SNX_A64_OP_PACIBSP:
        case SNX_A64_OP_AUTIAZ:
        case SNX_A64_OP_AUTIASP:
        case SNX_A64_OP_AUTIBZ:
        case SNX_A64_OP_AUTIBSP:
        case SNX_A64_OP_PACIA:
        case SNX_A64_OP_PACIB:
        case SNX_A64_OP_PACDA:
        case SNX_A64_OP_PACDB:
        case SNX_A64_OP_AUTIA:
        case SNX_A64_OP_AUTIB:
        case SNX_A64_OP_AUTDA:
        case SNX_A64_OP_AUTDB:
        case SNX_A64_OP_XPACI:
        case SNX_A64_OP_XPACD:
        case SNX_A64_OP_PACGA:
        case SNX_A64_OP_BRAZ:
        case SNX_A64_OP_BLRAZ:
        case SNX_A64_OP_RETA:
        case SNX_A64_OP_BRA:
        case SNX_A64_OP_BLRA: return 1;
        default: return 0;
    }
}

void snx_ir_int(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc) {
    const SnxA64Args* a = &insn->args;
    SnxA64Op op = insn->op;
    switch (op) {
        case SNX_A64_OP_ADR: set_reg(ir, (unsigned)a->rd, ir_i64(ir, pc + (SnxU64)(int64_t)a->imm)); break;
        case SNX_A64_OP_ADRP:
            set_reg(ir, (unsigned)a->rd, ir_i64(ir, (pc & ~0xFFFull) + ((SnxU64)(int64_t)a->imm << 12)));
            break;
        case SNX_A64_OP_ADD_i: addsub_imm(ir, a, 0, 0); break;
        case SNX_A64_OP_ADDS_i: addsub_imm(ir, a, 0, 1); break;
        case SNX_A64_OP_SUB_i: addsub_imm(ir, a, 1, 0); break;
        case SNX_A64_OP_SUBS_i: addsub_imm(ir, a, 1, 1); break;
        case SNX_A64_OP_AND_i:
        case SNX_A64_OP_ORR_i:
        case SNX_A64_OP_EOR_i:
        case SNX_A64_OP_ANDS_i:
            if (!logic_imm(ir, op, a)) undef(ir, pc, word);
            break;
        case SNX_A64_OP_MOVN:
        case SNX_A64_OP_MOVZ:
        case SNX_A64_OP_MOVK: movw(ir, op, a); break;
        case SNX_A64_OP_SBFM:
        case SNX_A64_OP_BFM:
        case SNX_A64_OP_UBFM: bitfield(ir, op, a); break;
        case SNX_A64_OP_EXTR: extr(ir, a); break;
        case SNX_A64_OP_AND_r:
        case SNX_A64_OP_ORR_r:
        case SNX_A64_OP_EOR_r:
        case SNX_A64_OP_ANDS_r: logic_reg(ir, op, a); break;
        case SNX_A64_OP_ADD_r: addsub_reg(ir, a, 0, 0); break;
        case SNX_A64_OP_SUB_r: addsub_reg(ir, a, 1, 0); break;
        case SNX_A64_OP_ADDS_r: addsub_reg(ir, a, 0, 1); break;
        case SNX_A64_OP_SUBS_r: addsub_reg(ir, a, 1, 1); break;
        case SNX_A64_OP_ADD_ext: addsub_ext(ir, a, 0, 0); break;
        case SNX_A64_OP_SUB_ext: addsub_ext(ir, a, 1, 0); break;
        case SNX_A64_OP_ADDS_ext: addsub_ext(ir, a, 0, 1); break;
        case SNX_A64_OP_SUBS_ext: addsub_ext(ir, a, 1, 1); break;
        case SNX_A64_OP_ADC: adc(ir, a, 0, 0); break;
        case SNX_A64_OP_ADCS: adc(ir, a, 0, 1); break;
        case SNX_A64_OP_SBC: adc(ir, a, 1, 0); break;
        case SNX_A64_OP_SBCS: adc(ir, a, 1, 1); break;
        case SNX_A64_OP_CCMP: ccmp(ir, a); break;
        case SNX_A64_OP_CSEL: csel(ir, a); break;
        case SNX_A64_OP_RBIT:
        case SNX_A64_OP_REV16:
        case SNX_A64_OP_REV32:
        case SNX_A64_OP_REV64:
        case SNX_A64_OP_CLZ:
        case SNX_A64_OP_CLS: dp1(ir, op, a); break;
        case SNX_A64_OP_UDIV: divide(ir, a, 0); break;
        case SNX_A64_OP_SDIV: divide(ir, a, 1); break;
        case SNX_A64_OP_LSLV:
        case SNX_A64_OP_LSRV:
        case SNX_A64_OP_ASRV:
        case SNX_A64_OP_RORV: shift_reg(ir, op, a); break;
        case SNX_A64_OP_CRC32:
        case SNX_A64_OP_CRC32C:
            set_reg(ir, (unsigned)a->rd,
                    crc32(ir, ir_u32(ir, ir_rx(ir, (unsigned)a->rn)), ir_rx(ir, (unsigned)a->rm), (unsigned)a->esz,
                          op == SNX_A64_OP_CRC32C));
            break;
        case SNX_A64_OP_MADD_w:
        case SNX_A64_OP_MSUB_w:
        case SNX_A64_OP_MADD_x:
        case SNX_A64_OP_MSUB_x:
        case SNX_A64_OP_SMADDL:
        case SNX_A64_OP_SMSUBL:
        case SNX_A64_OP_UMADDL:
        case SNX_A64_OP_UMSUBL:
        case SNX_A64_OP_SMULH:
        case SNX_A64_OP_UMULH: dp3(ir, op, a); break;
        case SNX_A64_OP_YIELD: ir_yield(ir); break;
        case SNX_A64_OP_WFE: { /* the runtime lets the core's other threads run: the spin waits on one of them */
            LLVMTypeRef param = ir->t_ptr;
            V c = ir->c;
            ir_rt_call(ir, ir_rt_wfe(), ir->t_void, &param, &c, 1);
            break;
        }
        case SNX_A64_OP_SEV:
        case SNX_A64_OP_SEVL: ir_rt_call(ir, ir_rt_sev(), ir->t_void, NULL, NULL, 0); break;
        case SNX_A64_OP_CLREX: ir_cpu_store(ir, ir_i32(ir, 0), ir_off_excl_on()); break;
        case SNX_A64_OP_DSB_DMB: barrier(ir, (unsigned)a->types); break;
        case SNX_A64_OP_ISB: ir_fence(ir); break;
        case SNX_A64_OP_SYS: sys(ir, a, word, pc); break;
        case SNX_A64_OP_XPACLRI: xpac(ir, 30); break;
        case SNX_A64_OP_XPACI:
        case SNX_A64_OP_XPACD: xpac(ir, (unsigned)a->rd); break;
        case SNX_A64_OP_PACGA: pacga(ir, a); break;
        default: break; /* NOP and WFI (SCTLR_EL1.nTWI: no trap), the prefetches, PAC* and AUT* with the keys off; the
                           flow ops have no body */
    }
}

LLVMValueRef snx_ir_int_operand(SnxIr* ir, const SnxA64Insn* insn) {
    const SnxA64Args* a = &insn->args;
    switch (insn->op) {
        case SNX_A64_OP_B_cond: return nonzero(ir, ir_flags_cond(ir, (unsigned)a->cond));
        case SNX_A64_OP_CBZ:
            return LLVMBuildICmp(B, a->nz ? LLVMIntNE : LLVMIntEQ, reg(ir, (unsigned)a->rt, a->sf),
                                 konst(ir, a->sf, 0), "");
        case SNX_A64_OP_TBZ: {
            V bit = LLVMBuildAnd(B, ir_rx(ir, (unsigned)a->rt), ir_i64(ir, 1ull << (unsigned)a->bitpos), "");
            return LLVMBuildICmp(B, a->nz ? LLVMIntNE : LLVMIntEQ, bit, ir_i64(ir, 0), "");
        }
        case SNX_A64_OP_BR:
        case SNX_A64_OP_BLR:
        case SNX_A64_OP_RET:
        case SNX_A64_OP_BRAZ:
        case SNX_A64_OP_BLRAZ:
        case SNX_A64_OP_BRA:
        case SNX_A64_OP_BLRA: return ir_rx(ir, (unsigned)a->rn);
        case SNX_A64_OP_RETA: return ir_rx(ir, 30);
        case SNX_A64_OP_SVC:
        case SNX_A64_OP_BRK: return ir_i32(ir, (SnxU32)a->imm & 0xFFFFu);
        default: return NULL;
    }
}
