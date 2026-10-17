/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c, helper-a64.c, vec_helper.c, vfp_helper.c,
   crypto_helper.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
   Copyright (c) 2003 Fabrice Bellard */
/* AdvSIMD: snx_a64_fast_simd's shortcuts (a64.c), emitted inline as the C it prints, and the SIMD patterns, whose
   template hands the word to snx_simd (cpu_helpers.c, in the runtime library) */
#include "common/cpu/recompiler/llvm/ir.h"
#include <string.h>

typedef LLVMValueRef V;
#define B ir->b

static LLVMTypeRef lane_type(SnxIr* ir, unsigned size) {
    return size == 0 ? ir->t_i8 : size == 1 ? ir->t_i16 : size == 2 ? ir->t_i32 : ir->t_i64;
}

/* lane index of 1<<size bytes in a V128 at pointer base (a register or a local) */
static V lane_pointer(SnxIr* ir, V base, unsigned size, unsigned index) {
    V offset = ir_i64(ir, (SnxU64)index << size);
    return LLVMBuildInBoundsGEP2(B, ir->t_i8, base, &offset, 1, "");
}
static V get(SnxIr* ir, V base, unsigned size, unsigned index) {
    return ir_load(ir, lane_type(ir, size), lane_pointer(ir, base, size, index));
}
static void set(SnxIr* ir, V base, unsigned size, unsigned index, V value) {
    ir_store(ir, ir_cast(ir, value, lane_type(ir, size), 0), lane_pointer(ir, base, size, index));
}
static V vreg(SnxIr* ir, unsigned n) { return ir_cpu(ir, ir_off_v(n)); }
/* V128 x = <source>: a local copy */
static V copy_of(SnxIr* ir, V source) {
    V local = ir_local(ir, ir_v128(ir));
    LLVMBuildMemCpy(B, local, 1, source, 1, ir_i64(ir, 16));
    return local;
}
static void assign(SnxIr* ir, V destination, V source) {
    LLVMBuildMemCpy(B, destination, 1, source, 1, ir_i64(ir, 16));
}

static V fp_fma(SnxIr* ir, V add, V a, V b, unsigned width) {
    LLVMTypeRef p[5];
    V args[5];
    p[0] = ir->t_ptr;
    p[1] = p[2] = p[3] = ir->t_i64;
    p[4] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir_u64(ir, add);
    args[2] = ir_u64(ir, a);
    args[3] = ir_u64(ir, b);
    args[4] = ir_i32(ir, width);
    return ir_call(ir, ir_extern(ir, "snx_fp_fma", ir->t_i64, p, 5), args, 5);
}
/* x ^ sign bit of the lane, or x */
static V negate(SnxIr* ir, V x, unsigned sz, int on) {
    if (!on) return x;
    return LLVMBuildXor(B, x, sz ? ir_i64(ir, (SnxU64)1 << 63) : ir_i32(ir, 0x80000000UL), "");
}

/* lanes 0..n-1 of 1<<size bytes at base as one value: iN for one lane, <n x iN> otherwise */
static LLVMTypeRef lanes_type(SnxIr* ir, unsigned size, unsigned n) {
    return n == 1 ? lane_type(ir, size) : LLVMVectorType(lane_type(ir, size), n);
}
static V get_lanes(SnxIr* ir, V base, unsigned size, unsigned n) {
    V load = ir_load(ir, lanes_type(ir, size, n), base);
    LLVMSetAlignment(load, 1);
    return load;
}
static void set_lanes(SnxIr* ir, V base, V value) {
    V store = LLVMBuildStore(B, value, base);
    LLVMSetAlignment(store, 1);
}
static V broadcast(SnxIr* ir, V scalar, unsigned n) {
    V v = LLVMGetUndef(LLVMVectorType(LLVMTypeOf(scalar), n));
    unsigned i;
    if (n == 1) return scalar;
    for (i = 0; i < n; ++i) v = LLVMBuildInsertElement(B, v, scalar, ir_i32(ir, i), "");
    return v;
}
static V negate_lanes(SnxIr* ir, V x, unsigned sz, unsigned n) {
    return LLVMBuildXor(B, x, broadcast(ir, sz ? ir_i64(ir, (SnxU64)1 << 63) : ir_i32(ir, 0x80000000UL), n), "");
}

int snx_ir_fast_simd(SnxIr* ir, SnxU32 w) {
    unsigned q, u, size, rd, rn, rm, op, hi, sz, lanes, i, scalar, l, h, mb, index, m, imm5, imm4, lane;
    q = (w >> 30) & 1;
    u = (w >> 29) & 1;
    size = (w >> 22) & 3;
    rd = w & 31;
    rn = (w >> 5) & 31;
    rm = (w >> 16) & 31;
    if ((w & 0x9F200400UL) == 0x0E200400UL) {
        op = (w >> 11) & 31;
        if (op == 3) {
            V a0, a1, b0, b1;
            if (!u && size == 2 && rn == rm) { /* MOV (ORR Vd,Vn,Vn) */
                ir_set_vd(ir, rd, 0, ir_vd(ir, rn, 0));
                ir_set_vd(ir, rd, 1, q ? ir_vd(ir, rn, 1) : ir_i64(ir, 0));
                return 1;
            }
            if (u && size) return 0;
            a0 = ir_vd(ir, rn, 0);
            a1 = ir_vd(ir, rn, 1);
            b0 = ir_vd(ir, rm, 0);
            b1 = ir_vd(ir, rm, 1);
            if (!u && (size & 1)) { /* &~, |~ */
                b0 = LLVMBuildNot(B, b0, "");
                b1 = LLVMBuildNot(B, b1, "");
            }
            if (u) {
                ir_set_vd(ir, rd, 0, LLVMBuildXor(B, a0, b0, ""));
                ir_set_vd(ir, rd, 1, q ? LLVMBuildXor(B, a1, b1, "") : ir_i64(ir, 0));
            } else if (size < 2) {
                ir_set_vd(ir, rd, 0, LLVMBuildAnd(B, a0, b0, ""));
                ir_set_vd(ir, rd, 1, q ? LLVMBuildAnd(B, a1, b1, "") : ir_i64(ir, 0));
            } else {
                ir_set_vd(ir, rd, 0, LLVMBuildOr(B, a0, b0, ""));
                ir_set_vd(ir, rd, 1, q ? LLVMBuildOr(B, a1, b1, "") : ir_i64(ir, 0));
            }
            return 1;
        }
        if (op >= 0x18) {
            int operation = -1, acc = 0, negated = 0;
            V a, b, r;
            sz = size & 1;
            hi = size >> 1;
            if (sz && !q) return 0;
            lanes = sz ? 2 : (q ? 4 : 2);
            if (!u && !hi && op == 0x1A)
                operation = 0; /* SNX_FP_ADD */
            else if (!u && hi && op == 0x1A)
                operation = 1; /* SNX_FP_SUB */
            else if (u && !hi && op == 0x1B)
                operation = 2; /* SNX_FP_MUL */
            else if (u && !hi && op == 0x1F)
                operation = 3; /* SNX_FP_DIV */
            else if (!u && op == 0x19) {
                acc = 1;
                negated = (int)hi;
            }
            if (operation < 0 && !acc) return 0;
            /* { V128 a=c->v[rn],b=c->v[rm],r[=c->v[rd]]; r.T[i]=...; if(!q) r.d[1]=0; c->v[rd]=r; } */
            a = copy_of(ir, vreg(ir, rn));
            b = copy_of(ir, vreg(ir, rm));
            r = acc ? copy_of(ir, vreg(ir, rd)) : ir_local(ir, ir_v128(ir));
            {
                /* all lanes at once on the host's vector unit when every lane is exact (ir_fp_fast) */
                unsigned s = sz ? 3 : 2;
                V va = get_lanes(ir, a, s, lanes), vb = get_lanes(ir, b, s, lanes);
                LLVMBasicBlockRef slow, join;
                SnxFpFast f;
                if (acc)
                    ir_fp_fast(ir, SNX_IR_FP_FMA, sz ? 64 : 32, lanes, negated ? negate_lanes(ir, va, sz, lanes) : va,
                               vb, get_lanes(ir, r, s, lanes), &f);
                else
                    ir_fp_fast(ir, (unsigned)operation, sz ? 64 : 32, lanes, va, vb, NULL, &f);
                join = ir_fp_branch(ir, f.ok, &slow);
                set_lanes(ir, r, f.value);
                if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
                assign(ir, vreg(ir, rd), r);
                ir_fp_else(ir, join, slow, f.inexact);
                for (i = 0; i < lanes; ++i) {
                    V value;
                    if (acc)
                        value = fp_fma(ir, get(ir, r, s, i), negate(ir, get(ir, a, s, i), sz, negated),
                                       get(ir, b, s, i), sz ? 64 : 32);
                    else
                        value = ir_fp_binary(ir, get(ir, a, s, i), get(ir, b, s, i), sz ? 64 : 32, (unsigned)operation);
                    set(ir, r, s, i, value);
                }
                if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
                assign(ir, vreg(ir, rd), r);
                ir_fp_join(ir, join);
                return 1;
            }
        }
        if (op == 0x10 && (q || size < 3)) { /* ADD, SUB (vector) */
            V a, b, r;
            lanes = (q ? 16 : 8) >> size;
            a = copy_of(ir, vreg(ir, rn));
            b = copy_of(ir, vreg(ir, rm));
            r = ir_local(ir, ir_v128(ir));
            for (i = 0; i < lanes; ++i) {
                V x = get(ir, a, size, i), y = get(ir, b, size, i);
                set(ir, r, size, i, u ? LLVMBuildSub(B, x, y, "") : LLVMBuildAdd(B, x, y, ""));
            }
            if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
            assign(ir, vreg(ir, rd), r);
            return 1;
        }
        return 0;
    }
    if ((w & 0x9F000400UL) == 0x0F000000UL || (w & 0xDF000400UL) == 0x5F000000UL) { /* FMLA/FMLS/FMUL by element */
        V a, r, d0 = NULL, e;
        unsigned s;
        scalar = (w >> 28) & 1;
        op = (w >> 12) & 15;
        if (u || (op != 1 && op != 5 && op != 9) || size < 2) return 0;
        sz = size & 1;
        l = (w >> 21) & 1;
        h = (w >> 11) & 1;
        mb = (w >> 20) & 1;
        if (sz && l) return 0;
        index = sz ? h : ((h << 1) | l);
        m = (mb << 4) | ((w >> 16) & 15);
        if (!scalar && sz && !q) return 0;
        lanes = scalar ? 1 : (sz ? 2 : (q ? 4 : 2));
        s = sz ? 3 : 2;
        a = copy_of(ir, vreg(ir, rn));
        r = !scalar && op != 9 ? copy_of(ir, vreg(ir, rd)) : ir_local(ir, ir_v128(ir));
        if (scalar && op != 9) d0 = get(ir, vreg(ir, rd), s, 0);
        if (scalar || op == 9) {
            set(ir, r, 3, 0, ir_i64(ir, 0));
            set(ir, r, 3, 1, ir_i64(ir, 0));
        }
        e = get(ir, vreg(ir, m), s, index);
        {
            V va = get_lanes(ir, a, s, lanes), ve = broadcast(ir, e, lanes);
            LLVMBasicBlockRef slow, join;
            SnxFpFast f;
            if (op == 9)
                ir_fp_fast(ir, SNX_IR_FP_MUL, sz ? 64 : 32, lanes, va, ve, NULL, &f);
            else
                ir_fp_fast(ir, SNX_IR_FP_FMA, sz ? 64 : 32, lanes, op == 5 ? negate_lanes(ir, va, sz, lanes) : va, ve,
                           scalar ? d0 : get_lanes(ir, r, s, lanes), &f);
            join = ir_fp_branch(ir, f.ok, &slow);
            set_lanes(ir, r, f.value);
            if (!q && !scalar) set(ir, r, 3, 1, ir_i64(ir, 0));
            assign(ir, vreg(ir, rd), r);
            ir_fp_else(ir, join, slow, f.inexact);
            for (i = 0; i < lanes; ++i) {
                V value;
                if (op == 9)
                    value = ir_fp_binary(ir, get(ir, a, s, i), e, sz ? 64 : 32, 2);
                else
                    value = fp_fma(ir, scalar ? d0 : get(ir, r, s, i), negate(ir, get(ir, a, s, i), sz, op == 5), e,
                                   sz ? 64 : 32);
                set(ir, r, s, i, value);
            }
            if (!q && !scalar) set(ir, r, 3, 1, ir_i64(ir, 0));
            assign(ir, vreg(ir, rd), r);
            ir_fp_join(ir, join);
            return 1;
        }
    }
    if ((w & 0x9FE08400UL) == 0x0E000400UL) { /* DUP, INS, UMOV */
        imm5 = (w >> 16) & 31;
        imm4 = (w >> 11) & 15;
        if (!imm5) return 0;
        sz = 0;
        while (!(imm5 & (1U << sz))) ++sz;
        if (sz > 3) return 0;
        index = imm5 >> (sz + 1);
        lane = sz;
        lanes = (q ? 16 : 8) >> sz;
        if (!u && imm4 == 7 && ((sz == 3) == !!q)) {
            if (rd != 31) ir_set_x(ir, rd, ir_u64(ir, ir_ve(ir, rn, lane, index)));
            return 1;
        }
        if (!u && imm4 == 3 && q) {
            ir_set_ve(ir, rd, lane, index, rn == 31 ? ir_i64(ir, 0) : ir_x(ir, rn));
            return 1;
        }
        if (u && q) {
            ir_set_ve(ir, rd, lane, index, ir_ve(ir, rn, lane, imm4 >> sz));
            return 1;
        }
        if (!u && (imm4 == 0 || imm4 == 1) && !(sz == 3 && !q)) {
            V r = ir_local(ir, ir_v128(ir)), e;
            if (!imm4)
                e = ir_ve(ir, rn, lane, index);
            else
                e = ir_cast(ir, rn == 31 ? ir_i64(ir, 0) : ir_x(ir, rn), lane_type(ir, lane), 0);
            for (i = 0; i < lanes; ++i) set(ir, r, lane, i, e);
            if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
            assign(ir, vreg(ir, rd), r);
            return 1;
        }
        return 0;
    }
    if ((w & 0xFFFFFFE0UL) == 0x2F00E400UL || (w & 0xFFFFFFE0UL) == 0x6F00E400UL) { /* MOVI Vd, #0 */
        ir_set_vd(ir, rd, 0, ir_i64(ir, 0));
        ir_set_vd(ir, rd, 1, ir_i64(ir, 0));
        return 1;
    }
    return 0;
}

/* SIMD_LDST, SIMD_LDST1, SIMD_DP, SIMD_DP2: if(!snx_simd(c,M,w)) c->rt->undef(c,pc,w); */
int snx_ir_simd_plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index) {
    LLVMTypeRef p[3];
    V args[3], handled;
    LLVMBasicBlockRef undefined, done;
    (void)index;
    if (strcmp(pattern->name, "SIMD_LDST") && strcmp(pattern->name, "SIMD_LDST1") && strcmp(pattern->name, "SIMD_DP") &&
        strcmp(pattern->name, "SIMD_DP2"))
        return 0;
    p[0] = ir->t_ptr;
    p[1] = ir->t_ptr;
    p[2] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir->M;
    args[2] = ir_i32(ir, word);
    handled = ir_call(ir, ir_extern(ir, "snx_simd", ir->t_i32, p, 3), args, 3);
    undefined = ir_block(ir, "simd_undef");
    done = ir_block(ir, "simd_done");
    ir_unlikely(ir, LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, handled, ir_i32(ir, 0), ""), undefined, done));
    LLVMPositionBuilderAtEnd(B, undefined);
    ir_undef(ir, pc, word);
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
    return 1;
}

/* ---- beyond snx_a64_fast_simd: encodings snx_simd handles, emitted inline with its semantics ---- */

static V v128_type_load(SnxIr* ir, V base) { /* the 16 bytes at base as <16 x i8> */
    V load = ir_load(ir, LLVMVectorType(ir->t_i8, 16), base);
    LLVMSetAlignment(load, 1);
    return load;
}
static void zero_high(SnxIr* ir, unsigned n) { ir_set_vd(ir, n, 1, ir_i64(ir, 0)); }

/* LD1/ST1 {Vt..Vt+n-1}: simd_ldst stores each element in turn; for consecutive registers that is one copy per
   register. Post-index adds the size, or Xm */
static int ld1_multiple(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, op = (w >> 12) & 15, regs, bytes = q ? 16 : 8, r;
    V address;
    regs = op == 7 ? 1 : op == 10 ? 2 : op == 6 ? 3 : op == 2 ? 4 : 0;
    if (!regs) return 0;
    address = ir_x(ir, rn);
    for (r = 0; r < regs; ++r) {
        V a = LLVMBuildAdd(B, address, ir_i64(ir, (SnxU64)r * bytes), "");
        if (l)
            set_lanes(ir, vreg(ir, (rt + r) & 31), ir_rd_bytes(ir, a, bytes));
        else
            ir_wr_bytes(ir, a, get_lanes(ir, vreg(ir, (rt + r) & 31), 0, bytes));
    }
    if (l && !q)
        for (r = 0; r < regs; ++r) zero_high(ir, (rt + r) & 31);
    if (post)
        ir_set_x(ir, rn, LLVMBuildAdd(B, address, rm == 31 ? ir_i64(ir, (SnxU64)regs * bytes) : ir_x(ir, rm), ""));
    return 1;
}

/* LD1R-LD4R and LD1-4/ST1-4 of one lane: sel consecutive elements, one per register (Vt..Vt+sel-1, wrapping),
   accessed one at a time in simd_ldst's order */
static int ld1_single(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, r = (w >> 21) & 1, op = (w >> 13) & 7, s = (w >> 12) & 1, size = (w >> 10) & 3;
    unsigned sc = op >> 1, sel = (((op & 1) << 1) | r) + 1, esz, idx, i, n, bytes, k;
    V address;
    address = ir_x(ir, rn);
    if (sc == 3) { /* LDnR: each element in every lane of its register, the rest zero */
        V value, local;
        if (!l || s) return 0;
        n = (q ? 128u : 64u) >> (3 + size);
        for (k = 0; k < sel; ++k) {
            value = ir_rd(ir, LLVMBuildAdd(B, address, ir_i64(ir, (SnxU64)k << size), ""), 8u << size);
            local = ir_local(ir, ir_v128(ir));
            set(ir, local, 3, 0, ir_i64(ir, 0));
            set(ir, local, 3, 1, ir_i64(ir, 0));
            for (i = 0; i < n; ++i) set(ir, local, size, i, value);
            assign(ir, vreg(ir, (rt + k) & 31), local);
        }
        bytes = sel << size;
    } else {
        if (sc == 0) {
            esz = 0;
            idx = (q << 3) | (s << 2) | size;
        } else if (sc == 1) {
            if (size & 1) return 0;
            esz = 1;
            idx = (q << 2) | (s << 1) | (size >> 1);
        } else {
            if (size & 2) return 0;
            if (size & 1) {
                if (s) return 0;
                esz = 3;
                idx = q;
            } else {
                esz = 2;
                idx = (q << 1) | s;
            }
        }
        for (k = 0; k < sel; ++k) {
            V a = LLVMBuildAdd(B, address, ir_i64(ir, (SnxU64)k << esz), "");
            if (l)
                ir_set_ve(ir, (rt + k) & 31, esz, idx, ir_rd(ir, a, 8u << esz));
            else
                ir_wr(ir, a, 8u << esz, ir_ve(ir, (rt + k) & 31, esz, idx));
        }
        bytes = sel << esz;
    }
    if (post) ir_set_x(ir, rn, LLVMBuildAdd(B, address, rm == 31 ? ir_i64(ir, bytes) : ir_x(ir, rm), ""));
    return 1;
}

/* store <lanes> into Vd with the upper half cleared when Q is 0 (simd_perm and EXT write r from zero) */
static void write_lanes(SnxIr* ir, unsigned rd, V value, unsigned q) {
    V local = ir_local(ir, ir_v128(ir));
    set(ir, local, 3, 0, ir_i64(ir, 0)); /* a scalar lane narrower than 64 bits leaves the rest of d[0] */
    set(ir, local, 3, 1, ir_i64(ir, 0));
    set_lanes(ir, local, value);
    if (!q) set(ir, local, 3, 1, ir_i64(ir, 0));
    assign(ir, vreg(ir, rd), local);
}
static V shuffle(SnxIr* ir, V a, V b, const unsigned* indices, unsigned n) {
    V mask[16];
    unsigned i;
    for (i = 0; i < n; ++i) mask[i] = ir_i32(ir, indices[i]);
    return LLVMBuildShuffleVector(B, a, b, LLVMConstVector(mask, n), "");
}
/* count vectors of n lanes end to end, as one of count*n lanes (at most 64) */
static V concat(SnxIr* ir, V const* parts, unsigned count, unsigned n) {
    V mask[64], all = parts[0], part;
    unsigned have = n, k, i;
    for (k = 1; k < count; ++k) {
        part = parts[k];
        if (have > n) { /* a shuffle's operands share a type: the part widened to have lanes (the extra unused) */
            for (i = 0; i < have; ++i) mask[i] = ir_i32(ir, i < n ? i : 0);
            part = LLVMBuildShuffleVector(B, part, part, LLVMConstVector(mask, have), "");
        }
        for (i = 0; i < have + n; ++i) mask[i] = ir_i32(ir, i); /* all's lanes, then the part's first n */
        all = LLVMBuildShuffleVector(B, all, part, LLVMConstVector(mask, have + n), "");
        have += n;
    }
    return all;
}

/* UZP1/2, TRN1/2, ZIP1/2: simd_perm's element formulas, as one shuffle */
static int permute(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 7, rn = (w >> 5) & 31,
             rd = w & 31, n, h, part, i, indices[16];
    if ((size == 3 && !q) || !(op & 3)) return 0;
    n = (q ? 128u : 64u) >> (3 + size);
    h = n / 2;
    part = (op >> 2) & 1;
    for (i = 0; i < n; ++i) {
        unsigned k;
        if ((op & 3) == 1) /* UZP: k < n ? a[k] : b[k-n] */
            k = 2 * i + part;
        else if ((op & 3) == 2) /* TRN */
            k = (i & 1) ? n + i / 2 * 2 + part : i / 2 * 2 + part;
        else /* ZIP */
            k = (i & 1) ? n + part * h + i / 2 : part * h + i / 2;
        indices[i] = k;
    }
    write_lanes(ir, rd,
                shuffle(ir, get_lanes(ir, vreg(ir, rn), size, n), get_lanes(ir, vreg(ir, rm), size, n), indices, n), q);
    return 1;
}

/* EXT: bytes imm4.. of Vn:Vm */
static int extract(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, imm4 = (w >> 11) & 15, rn = (w >> 5) & 31, rd = w & 31,
             len = q ? 16 : 8, i, indices[16];
    if (!q && (imm4 & 8)) return 0;
    for (i = 0; i < len; ++i) {
        unsigned source = imm4 + i;
        indices[i] = source < len ? source : 16 + (source - len);
    }
    write_lanes(ir, rd, shuffle(ir, v128_type_load(ir, vreg(ir, rn)), v128_type_load(ir, vreg(ir, rm)), indices, len),
                q);
    return 1;
}

/* ---- GFNI: GF2P8AFFINEQB as a per-byte bit matrix ----
   (wunkolo, "gf2p8affineqb: int8 shifting", 2020.) Byte 7-i of the 64-bit matrix builds result bit i, its bit j
   selecting source bit j: the identity is 0x0102040810204080. Shifting the identity right by 8n bytes is a logical
   shift left by n, left by 8n a logical shift right; an arithmetic shift right also ORs n bytes of 0x80 back in,
   replicating bit 7. x86 has no 8-bit shifts, so each of these replaces a shift-and-mask sequence. */
#define GF_IDENTITY ((SnxU64)0x01020408UL << 32 | 0x10204080UL)
static SnxU64 gf_shl(unsigned n) { return n >= 8 ? 0 : GF_IDENTITY >> (8 * n); }
static SnxU64 gf_lshr(unsigned n) { return n >= 8 ? 0 : GF_IDENTITY << (8 * n); }
static SnxU64 gf_ashr(unsigned n) {
    SnxU64 sign = (SnxU64)0x80808080UL << 32 | 0x80808080UL;
    if (n > 7) n = 7;
    return n ? (GF_IDENTITY << (8 * n)) | (~(~(SnxU64)0 << (8 * n)) & sign) : GF_IDENTITY;
}
/* result bit 0 = source bit k, the rest 0 */
static SnxU64 gf_bit(unsigned k) { return ((SnxU64)1 << k) << 56; }

/* the host's GF2P8AFFINEQB applies, from CPUID (snx_ir_host): an x86-64 target with GFNI */
static int host_gfni(SnxIr* ir) { return ir->features->x86 && ir->features->gfni; }

/* GF2P8AFFINEQB x, matrix, 0 on 16 bytes */
static V gf_affine(SnxIr* ir, V x, SnxU64 matrix) {
    V bytes[16], args[3];
    unsigned i;
    for (i = 0; i < 16; ++i) bytes[i] = ir_i8(ir, (unsigned)(matrix >> (8 * (i & 7))));
    args[0] = x;
    args[1] = LLVMConstVector(bytes, 16);
    args[2] = ir_i8(ir, 0);
    return ir_intrinsic(ir, "llvm.x86.vgf2p8affineqb.128", NULL, args, 3);
}

/* the low n lanes of a <16 x i8> */
static V low_lanes(SnxIr* ir, V x, unsigned n) {
    unsigned indices[16], i;
    if (n == 16) return x;
    for (i = 0; i < n; ++i) indices[i] = i;
    return shuffle(ir, x, x, indices, n);
}

/* CNT, NOT, RBIT (byte lanes): llvm.ctpop and llvm.bitreverse become VPOPCNTB and GF2P8AFFINEQB on x86 hosts with
   AVX512_BITALG and GFNI, a shuffle-table sequence otherwise, CNT and RBIT on AArch64 */
static int byte_misc(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, n = q ? 16 : 8;
    V a, r;
    LLVMTypeRef t;
    if (op != 5 || !(u ? size < 2 : size == 0)) return 0;
    t = LLVMVectorType(ir->t_i8, n);
    a = get_lanes(ir, vreg(ir, rn), 0, n);
    if (!u)
        r = ir_intrinsic(ir, "llvm.ctpop", t, &a, 1);
    else if (!size)
        r = LLVMBuildNot(B, a, "");
    else if (host_gfni(ir)) /* wunkolo, "gf2p8affineqb: Bit reversal": the matrix 0x8040201008040201 */
        r = low_lanes(ir, gf_affine(ir, v128_type_load(ir, vreg(ir, rn)), ((SnxU64)0x80402010UL << 32) | 0x08040201UL),
                      n);
    else
        r = ir_intrinsic(ir, "llvm.bitreverse", t, &a, 1);
    write_lanes(ir, rd, r, q);
    return 1;
}

/* v_expand_imm (helpers/simd.c), evaluated here */
static SnxU64 expand_imm(unsigned op, unsigned cmode, unsigned imm8) {
    SnxU64 i = imm8, r = 0;
    unsigned k;
    switch (cmode >> 1) {
        case 0: r = i | (i << 32); break;
        case 1: r = (i << 8) | (i << 40); break;
        case 2: r = (i << 16) | (i << 48); break;
        case 3: r = (i << 24) | (i << 56); break;
        case 4:
            r = i | (i << 16);
            r |= r << 32;
            break;
        case 5:
            r = (i << 8) | (i << 24);
            r |= r << 32;
            break;
        case 6:
            r = !(cmode & 1) ? (i << 8) | 0xFF : (i << 16) | 0xFFFF;
            r |= r << 32;
            break;
        default:
            if (!(cmode & 1) && !op) {
                for (k = 0; k < 8; k++) r |= i << (k * 8);
            } else if (!(cmode & 1) && op) {
                for (k = 0; k < 8; k++)
                    if ((i >> k) & 1) r |= (SnxU64)0xFF << (k * 8);
            } else if ((cmode & 1) && !op) {
                SnxU32 b =
                    (SnxU32)((i & 0x80) << 24) | ((i & 0x40) ? 0x3E000000u : 0x40000000u) | ((SnxU32)(i & 0x3F) << 19);
                r = b | ((SnxU64)b << 32);
            } else {
                r = ((i & 0x80) << 56) | ((i & 0x40) ? ((SnxU64)0x3FC00000UL << 32) : ((SnxU64)0x40000000UL << 32)) |
                    ((i & 0x3F) << 48);
            }
            break;
    }
    return r;
}

/* MOVI, MVNI, ORR, BIC (vector, immediate): simd_modimm with the immediate a constant */
static int modified_immediate(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, op = (w >> 29) & 1, cmode = (w >> 12) & 15, rd = w & 31, o2 = (w >> 11) & 1;
    unsigned imm8 = (((w >> 16) & 7) << 5) | ((w >> 5) & 31);
    SnxU64 imm;
    if (o2 || (cmode == 15 && op && !q)) return 0;
    imm = expand_imm(op, cmode, imm8);
    if (cmode < 12 && (cmode & 1)) { /* ORR, BIC */
        V k = ir_i64(ir, op ? ~imm : imm);
        if (!op) {
            ir_set_vd(ir, rd, 0, LLVMBuildOr(B, ir_vd(ir, rd, 0), k, ""));
            ir_set_vd(ir, rd, 1, q ? LLVMBuildOr(B, ir_vd(ir, rd, 1), k, "") : ir_i64(ir, 0));
        } else {
            ir_set_vd(ir, rd, 0, LLVMBuildAnd(B, ir_vd(ir, rd, 0), k, ""));
            ir_set_vd(ir, rd, 1, q ? LLVMBuildAnd(B, ir_vd(ir, rd, 1), k, "") : ir_i64(ir, 0));
        }
        return 1;
    }
    if (op && cmode != 14 && cmode != 15) imm = ~imm; /* MVNI */
    ir_set_vd(ir, rd, 0, ir_i64(ir, imm));
    ir_set_vd(ir, rd, 1, ir_i64(ir, q ? imm : 0));
    return 1;
}

/* AESE/AESD/AESMC/AESIMC with AES-NI (x86, CPUID aes). Arm's AESE is ShiftRows(SubBytes(d^k)), x86's AESENCLAST
   ShiftRows(SubBytes(s))^rk: AESENCLAST(d^k, 0). MixColumns alone is AESENC(AESDECLAST(x, 0), 0), the inverse
   steps cancelling */
static int aes(SnxIr* ir, SnxU32 w) {
    unsigned op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31;
    LLVMTypeRef t = LLVMVectorType(ir->t_i64, 2);
    V args[2], zero = LLVMConstNull(t), x, r;
    if (!ir->features->x86 || !ir->features->aes || op < 4 || op > 7) return 0;
    x = ir_load(ir, t, vreg(ir, rn));
    LLVMSetAlignment(x, 8);
    if (op < 6) {
        V d = ir_load(ir, t, vreg(ir, rd));
        LLVMSetAlignment(d, 8);
        args[0] = LLVMBuildXor(B, d, x, "");
        args[1] = zero;
        r = ir_intrinsic(ir, op == 4 ? "llvm.x86.aesni.aesenclast" : "llvm.x86.aesni.aesdeclast", NULL, args, 2);
    } else if (op == 6) {
        args[0] = x;
        args[1] = zero;
        args[0] = ir_intrinsic(ir, "llvm.x86.aesni.aesdeclast", NULL, args, 2);
        r = ir_intrinsic(ir, "llvm.x86.aesni.aesenc", NULL, args, 2);
    } else
        r = ir_intrinsic(ir, "llvm.x86.aesni.aesimc", NULL, &x, 1);
    LLVMSetAlignment(LLVMBuildStore(B, r, vreg(ir, rd)), 8);
    return 1;
}

/* the 128-bit carry-less product of x and y (i64) from integer multiplies, for hosts without PCLMULQDQ (the
   masked-multiply technique of BearSSL's ghash_ctmul, Thomas Pornin): with both operands masked to every fourth bit,
   each bit of the integer product counts the pairs of set bits whose positions sum to it, and the count's parity is
   the carry-less bit. Taking x 32 bits at a time keeps every count at 8 or below (8 bits of a half in each residue
   class), so it never carries into the next position of its class: XORing the products of the class pairs and
   masking to the class gives that class of the product. 32 multiplies of 64 by 64 bits to 128 */
static V carryless_multiply(SnxIr* ir, V x, V y) {
    static const SnxU64 m = (SnxU64)0x11111111UL << 32 | 0x11111111UL;
    V yb[4], xa[4], r = NULL, half, part, z, k;
    unsigned h, a, c;
    for (a = 0; a < 4; ++a) yb[a] = LLVMBuildZExt(B, LLVMBuildAnd(B, y, ir_i64(ir, m << a), ""), ir->t_i128, "");
    for (h = 0; h < 2; ++h) {
        half = h ? LLVMBuildLShr(B, x, ir_i64(ir, 32), "") : LLVMBuildAnd(B, x, ir_i64(ir, 0xFFFFFFFFUL), "");
        for (a = 0; a < 4; ++a) xa[a] = LLVMBuildZExt(B, LLVMBuildAnd(B, half, ir_i64(ir, m << a), ""), ir->t_i128, "");
        part = NULL;
        for (c = 0; c < 4; ++c) {
            z = LLVMBuildMul(B, xa[0], yb[c], "");
            for (a = 1; a < 4; ++a) z = LLVMBuildXor(B, z, LLVMBuildMul(B, xa[a], yb[(c - a) & 3], ""), "");
            k = LLVMConstInt(ir->t_i128, m << c, 0);
            k = LLVMBuildOr(B, k, LLVMBuildShl(B, k, LLVMConstInt(ir->t_i128, 64, 0), ""), "");
            z = LLVMBuildAnd(B, z, k, "");
            part = part ? LLVMBuildOr(B, part, z, "") : z;
        }
        r = h ? LLVMBuildXor(B, r, LLVMBuildShl(B, part, LLVMConstInt(ir->t_i128, 32, 0), ""), "") : part;
    }
    return r;
}

/* PMULL/PMULL2 Vd.1Q (64x64 carry-less): PCLMULQDQ where CPUID has it (x86), carryless_multiply otherwise */
static int pmull_1q(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31;
    LLVMTypeRef t = LLVMVectorType(ir->t_i64, 2);
    V args[3], r;
    if (!ir->features->x86 || !ir->features->pclmul) {
        r = carryless_multiply(ir, ir_vd(ir, rn, q), ir_vd(ir, rm, q));
        ir_set_vd(ir, rd, 0, LLVMBuildTrunc(B, r, ir->t_i64, ""));
        ir_set_vd(ir, rd, 1,
                  LLVMBuildTrunc(B, LLVMBuildLShr(B, r, LLVMConstInt(ir->t_i128, 64, 0), ""), ir->t_i64, ""));
        return 1;
    }
    args[0] = ir_load(ir, t, vreg(ir, rn));
    args[1] = ir_load(ir, t, vreg(ir, rm));
    LLVMSetAlignment(args[0], 8);
    LLVMSetAlignment(args[1], 8);
    args[2] = ir_i8(ir, q ? 0x11 : 0x00);
    r = ir_intrinsic(ir, "llvm.x86.pclmulqdq", NULL, args, 3);
    LLVMSetAlignment(LLVMBuildStore(B, r, vreg(ir, rd)), 8);
    return 1;
}

/* ---- SHA1 and SHA256: crypto.c's v_sha1 and v_sha256 (the Arm ARM's SHA1C/P/M, SHA1SU0/SU1, SHA1H, SHA256H/H2,
   SHA256SU0/SU1) on 32-bit words; every operand is read before Vd is written ---- */

static V rol32(SnxIr* ir, V x, unsigned n) {
    return LLVMBuildOr(B, LLVMBuildShl(B, x, ir_i32(ir, n), ""), LLVMBuildLShr(B, x, ir_i32(ir, 32 - n), ""), "");
}
/* SHA256's sigmas: ror(x, a) ^ ror(x, b) ^ (c ? ror(x, c) : x >> d) */
static V sigma(SnxIr* ir, V x, unsigned a, unsigned b, unsigned c, unsigned d) {
    V r = LLVMBuildXor(B, rol32(ir, x, 32 - a), rol32(ir, x, 32 - b), "");
    return LLVMBuildXor(B, r, c ? rol32(ir, x, 32 - c) : LLVMBuildLShr(B, x, ir_i32(ir, d), ""), "");
}
static V add3(SnxIr* ir, V a, V b, V c) { return LLVMBuildAdd(B, LLVMBuildAdd(B, a, b, ""), c, ""); }

/* snx_simd's SHA ops: three registers (0-6, (w >> 12) & 7) and two (0x10-0x12) */
static int sha(SnxIr* ir, SnxU32 w, unsigned op) {
    unsigned rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, i;
    V x[4], y[4], m[4], r[4], t;
    for (i = 0; i < 4; ++i) {
        x[i] = get(ir, vreg(ir, rd), 2, i);
        y[i] = get(ir, vreg(ir, rn), 2, i);
        m[i] = op < 0x10 ? get(ir, vreg(ir, rm), 2, i) : NULL;
    }
    switch (op) {
        case 0: /* SHA1C, SHA1P, SHA1M: four rounds on the hash in Vd, E in Sn, the words in Vm */
        case 1:
        case 2: {
            V e = y[0], f, next;
            for (i = 0; i < 4; ++i) {
                if (op == 0) /* Choose */
                    f = LLVMBuildOr(B, LLVMBuildAnd(B, x[1], x[2], ""),
                                    LLVMBuildAnd(B, LLVMBuildNot(B, x[1], ""), x[3], ""), "");
                else if (op == 1) /* Parity */
                    f = LLVMBuildXor(B, LLVMBuildXor(B, x[1], x[2], ""), x[3], "");
                else /* Majority */
                    f = LLVMBuildOr(
                        B, LLVMBuildOr(B, LLVMBuildAnd(B, x[1], x[2], ""), LLVMBuildAnd(B, x[1], x[3], ""), ""),
                        LLVMBuildAnd(B, x[2], x[3], ""), "");
                e = LLVMBuildAdd(B, add3(ir, e, rol32(ir, x[0], 5), f), m[i], "");
                next = x[3];
                x[3] = x[2];
                x[2] = rol32(ir, x[1], 30);
                x[1] = x[0];
                x[0] = e;
                e = next;
            }
            memcpy(r, x, sizeof(r));
            break;
        }
        case 3: /* SHA1SU0: Vn's low half above Vd's upper half, ^ Vd ^ Vm */
            for (i = 0; i < 4; ++i)
                r[i] = LLVMBuildXor(B, LLVMBuildXor(B, i < 2 ? x[i + 2] : y[i - 2], x[i], ""), m[i], "");
            break;
        case 4: /* SHA256H (the hash's first half in Vd, the second in Vn), SHA256H2 (the other way round) */
        case 5: {
            V *a = op == 4 ? x : y, *b = op == 4 ? y : x, ch, maj, a3;
            for (i = 0; i < 4; ++i) {
                ch = LLVMBuildXor(B, LLVMBuildAnd(B, b[0], b[1], ""),
                                  LLVMBuildAnd(B, LLVMBuildNot(B, b[0], ""), b[2], ""), "");
                maj = LLVMBuildXor(
                    B, LLVMBuildXor(B, LLVMBuildAnd(B, a[0], a[1], ""), LLVMBuildAnd(B, a[0], a[2], ""), ""),
                    LLVMBuildAnd(B, a[1], a[2], ""), "");
                t = LLVMBuildAdd(B, add3(ir, b[3], sigma(ir, b[0], 6, 11, 25, 0), ch), m[i], "");
                a3 = LLVMBuildAdd(B, a[3], t, "");
                t = add3(ir, t, sigma(ir, a[0], 2, 13, 22, 0), maj);
                /* the eight words rotate up one: a = (t, a0, a1, a2), b = (a3 + t1, b0, b1, b2) */
                b[3] = b[2];
                b[2] = b[1];
                b[1] = b[0];
                b[0] = a3;
                a[3] = a[2];
                a[2] = a[1];
                a[1] = a[0];
                a[0] = t;
            }
            memcpy(r, op == 4 ? a : b, sizeof(r));
            break;
        }
        case 6: /* SHA256SU1: the next schedule words; the upper two use the lower two just computed */
            for (i = 0; i < 4; ++i)
                r[i] = add3(ir, x[i], sigma(ir, i < 2 ? m[i + 2] : r[i - 2], 17, 19, 0, 10), i < 3 ? y[i + 1] : m[0]);
            break;
        case 0x10: /* SHA1H: ROL(Sn, 30) alone */ write_lanes(ir, rd, rol32(ir, y[0], 30), 0); return 1;
        case 0x11: /* SHA1SU1: (Vd ^ Vn >> 32) rotated by one, the top word also ^ the first rotated by two */
            for (i = 0; i < 4; ++i) r[i] = i < 3 ? LLVMBuildXor(B, x[i], y[i + 1], "") : x[3];
            t = rol32(ir, r[0], 2);
            for (i = 0; i < 4; ++i) r[i] = rol32(ir, r[i], 1);
            r[3] = LLVMBuildXor(B, r[3], t, "");
            break;
        case 0x12: /* SHA256SU0 */
            for (i = 0; i < 4; ++i) r[i] = LLVMBuildAdd(B, x[i], sigma(ir, i < 3 ? x[i + 1] : y[0], 7, 18, 0, 3), "");
            break;
        default: return 0;
    }
    for (i = 0; i < 4; ++i) set(ir, vreg(ir, rd), 2, i, r[i]);
    return 1;
}

/* ---- vpternlog: any function of three bit vectors in one instruction (AVX512F+VL, from CPUID) ----
   (wunkolo, "vpternlog: Signed Saturation", 2025.) The 8-bit table is the function itself applied to the constants
   A=0xF0, B=0xCC, C=0xAA: each bit position of those is one combination of the three inputs */
enum { TERN_A = 0xF0, TERN_B = 0xCC, TERN_C = 0xAA };
static int host_ternlog(SnxIr* ir) { return ir->features->x86 && ir->features->avx512vl; }
/* VPTERNLOG{D,Q} on 128 bits: lanes of 32 or 64 bits (only the masking granularity differs) */
static V ternlog(SnxIr* ir, V a, V b, V c, unsigned table, unsigned bits) {
    V args[4];
    LLVMTypeRef t = LLVMVectorType(bits == 64 ? ir->t_i64 : ir->t_i32, bits == 64 ? 2 : 4);
    args[0] = LLVMBuildBitCast(B, a, t, "");
    args[1] = LLVMBuildBitCast(B, b, t, "");
    args[2] = LLVMBuildBitCast(B, c, t, "");
    args[3] = ir_i32(ir, table & 0xFF);
    return ir_intrinsic(ir, bits == 64 ? "llvm.x86.avx512.pternlog.q.128" : "llvm.x86.avx512.pternlog.d.128", NULL,
                        args, 4);
}

/* FPSR.QC (bit 27) |= any lane of an i1 vector (or an i1, for the scalar forms) */
static void set_qc(SnxIr* ir, V lanes) {
    V any = LLVMGetTypeKind(LLVMTypeOf(lanes)) == LLVMVectorTypeKind
                ? ir_intrinsic(ir, "llvm.vector.reduce.or", LLVMTypeOf(lanes), &lanes, 1)
                : lanes;
    size_t off = ir_off_fpcr() + 4;
    V fpsr = ir_cpu_load(ir, ir->t_i32, off);
    ir_cpu_store(ir, LLVMBuildOr(B, fpsr, LLVMBuildShl(B, ir_bool32(ir, any), ir_i32(ir, 27), ""), ""), off);
}

/* SQADD, UQADD, SQSUB, UQSUB (vector): saturating, QC when a lane saturates (sqadd_ and friends). For 32/64-bit
   signed lanes on AVX-512 hosts, wunkolo's vpternlog test: a lane overflowed when the inputs' sign bits agree and the
   sum's differs (add, 0x42) or the inputs' differ and the difference's differs from the first (sub, 0x18); the
   saturated value is the result's sign spread and its top bit flipped */
static V saturating(SnxIr* ir, unsigned u, unsigned sub, unsigned eb, unsigned n, V a, V b) {
    static const char* const names[4] = {"llvm.sadd.sat", "llvm.uadd.sat", "llvm.ssub.sat", "llvm.usub.sat"};
    LLVMTypeRef t = LLVMTypeOf(a);
    V wrap = sub ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, ""), args[2], r;
    if (!u && eb >= 32 && n * eb == 128 && host_ternlog(ir)) {
        unsigned table = sub ? (TERN_A ^ TERN_B) & (TERN_A ^ TERN_C) : ~(TERN_A ^ TERN_B) & (TERN_A ^ TERN_C);
        V check = LLVMBuildBitCast(B, ternlog(ir, a, b, wrap, table, eb), t, "");
        V overflow = LLVMBuildICmp(B, LLVMIntSLT, check, LLVMConstNull(t), "");
        V spread = LLVMBuildAShr(B, wrap, broadcast(ir, LLVMConstInt(LLVMGetElementType(t), eb - 1, 0), n), "");
        V value = LLVMBuildXor(B, spread,
                               broadcast(ir, LLVMConstInt(LLVMGetElementType(t), (SnxU64)1 << (eb - 1), 0), n), "");
        set_qc(ir, overflow);
        return LLVMBuildSelect(B, overflow, value, wrap, "");
    }
    args[0] = a;
    args[1] = b;
    r = ir_intrinsic(ir, names[sub * 2 + u], t, args, 2); /* PADDS/PADDUS/PSUBS/PSUBUS for 8/16-bit lanes on x86 */
    set_qc(ir, LLVMBuildICmp(B, LLVMIntNE, r, wrap, ""));
    return r;
}

/* SHADD, UHADD, SRHADD, URHADD: (a + b [+ 1]) >> 1 without overflow. Unsigned 8/16-bit rounding is PAVGB/PAVGW on
   x86; signed goes through it too, biased by the lanes' sign bit (wunkolo, "pavgb: most-significant-bit constant"):
   (a ^ 0x80) and (b ^ 0x80) are a + 128 and b + 128, so their average is the signed one plus 128 */
static V halving(SnxIr* ir, unsigned u, unsigned rounding, unsigned size, unsigned n, V a, V b) {
    LLVMTypeRef t = LLVMTypeOf(a), wide = LLVMVectorType(lane_type(ir, size + 1), n);
    V bias = NULL, x, y, r;
    if (!u && size < 2) {
        bias = broadcast(ir, LLVMConstInt(lane_type(ir, size), (SnxU64)1 << ((8u << size) - 1), 0), n);
        a = LLVMBuildXor(B, a, bias, "");
        b = LLVMBuildXor(B, b, bias, "");
        u = 1;
    }
    x = u ? LLVMBuildZExt(B, a, wide, "") : LLVMBuildSExt(B, a, wide, "");
    y = u ? LLVMBuildZExt(B, b, wide, "") : LLVMBuildSExt(B, b, wide, "");
    r = LLVMBuildAdd(B, x, y, "");
    if (rounding) r = LLVMBuildAdd(B, r, broadcast(ir, LLVMConstInt(lane_type(ir, size + 1), 1, 0), n), "");
    r = u ? LLVMBuildLShr(B, r, broadcast(ir, LLVMConstInt(lane_type(ir, size + 1), 1, 0), n), "")
          : LLVMBuildAShr(B, r, broadcast(ir, LLVMConstInt(lane_type(ir, size + 1), 1, 0), n), "");
    r = LLVMBuildTrunc(B, r, t, "");
    return bias ? LLVMBuildXor(B, r, bias, "") : r;
}

/* <n x iN> all-ones or zero per lane, as the compares write them */
static V mask_of(SnxIr* ir, V condition, LLVMTypeRef lanes) { return LLVMBuildSExt(B, condition, lanes, ""); }

/* ---- integer lanes as vectors: the helpers' sx/zx arithmetic in a wider lane where it needs the room ---- */

/* n lanes of 1<<size bytes at base, always a vector (<1 x iN> for the scalar forms, so one emitter serves both) */
static V vlanes(SnxIr* ir, V base, unsigned size, unsigned n) {
    V load = ir_load(ir, LLVMVectorType(lane_type(ir, size), n), base);
    LLVMSetAlignment(load, 1);
    return load;
}
/* the 64-bit half of Vn that Q selects (the "2" forms read the upper one) */
static V half_lanes(SnxIr* ir, unsigned rn, unsigned size, unsigned upper) {
    return vlanes(ir, ir_cpu(ir, ir_off_v(rn) + (upper ? 8 : 0)), size, 8u >> size);
}
static unsigned lanes_of(V x) { return LLVMGetVectorSize(LLVMTypeOf(x)); }
static unsigned bits_of(V x) { return LLVMGetIntTypeWidth(LLVMGetElementType(LLVMTypeOf(x))); }
static V vsplat(SnxIr* ir, V scalar, unsigned n) { /* broadcast, but a vector for one lane too */
    return n == 1 ? LLVMBuildInsertElement(B, LLVMGetUndef(LLVMVectorType(LLVMTypeOf(scalar), 1)), scalar,
                                           ir_i32(ir, 0), "")
                  : broadcast(ir, scalar, n);
}
/* v in every lane of a vector shaped like x */
static V splat_like(SnxIr* ir, V x, SnxU64 v) {
    return vsplat(ir, LLVMConstInt(LLVMGetElementType(LLVMTypeOf(x)), v, 0), lanes_of(x));
}
/* lanes zero- or sign-extended to bits, or truncated to them (LLVM's casts refuse the same width: x itself) */
static V extend(SnxIr* ir, V x, unsigned u, unsigned bits) {
    LLVMTypeRef t = LLVMVectorType(LLVMIntTypeInContext(ir->ctx, bits), lanes_of(x));
    if (bits == bits_of(x)) return x;
    if (bits < bits_of(x)) return LLVMBuildTrunc(B, x, t, "");
    return u ? LLVMBuildZExt(B, x, t, "") : LLVMBuildSExt(B, x, t, "");
}
static V widen(SnxIr* ir, V x, unsigned u) { return extend(ir, x, u, 2 * bits_of(x)); }
static V narrow(SnxIr* ir, V x) {
    return LLVMBuildTrunc(B, x, LLVMVectorType(LLVMIntTypeInContext(ir->ctx, bits_of(x) / 2), lanes_of(x)), "");
}
static SnxU64 int_min(unsigned eb) { return (SnxU64)1 << (eb - 1); }
static SnxU64 int_max(unsigned eb) { return int_min(eb) - 1; }
static SnxU64 uint_max(unsigned eb) { return eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1; }

/* the narrowing forms' result: Vd's low half with the upper half zero, or (the "2" forms) its upper half with the
   low half kept (simd_2misc's and simd_shimm's o = Q ? d : r) */
static void write_narrow(SnxIr* ir, unsigned rd, V value, unsigned upper) {
    if (upper)
        set_lanes(ir, ir_cpu(ir, ir_off_v(rd) + 8), value);
    else
        write_lanes(ir, rd, value, 0);
}

/* wide lanes clamped to half their width, QC for any lane that changed: signed to signed (ssat, SQXTN), signed to
   unsigned (usat, SQXTUN) or unsigned to unsigned (UQXTN) */
static V saturate_narrow(SnxIr* ir, V x, unsigned from_signed, unsigned to_signed) {
    unsigned eb = bits_of(x) / 2;
    V hi = splat_like(ir, x, to_signed ? int_max(eb) : uint_max(eb)), over, under, r;
    over = LLVMBuildICmp(B, from_signed ? LLVMIntSGT : LLVMIntUGT, x, hi, "");
    r = LLVMBuildSelect(B, over, hi, x, "");
    if (from_signed) { /* INT_MIN sign-extended, for lanes of 128 bits too (SUQADD's) */
        V lo = to_signed ? vsplat(ir, LLVMConstInt(LLVMGetElementType(LLVMTypeOf(x)), (SnxU64)0 - int_min(eb), 1),
                                  lanes_of(x))
                         : splat_like(ir, x, 0);
        under = LLVMBuildICmp(B, LLVMIntSLT, x, lo, "");
        r = LLVMBuildSelect(B, under, lo, r, "");
        over = LLVMBuildOr(B, over, under, "");
    }
    set_qc(ir, over);
    return narrow(ir, r);
}

/* SQDMULL and the doubling in SQDMLAL/SQDMLSL: 2*a*b in twice the width. Only INT_MIN * INT_MIN overflows it, and
   saturates with QC */
static V doubling_long(SnxIr* ir, V a, V b) {
    unsigned eb = bits_of(a);
    V x = widen(ir, a, 0), p = LLVMBuildShl(B, LLVMBuildMul(B, x, widen(ir, b, 0), ""), splat_like(ir, x, 1), "");
    V min = splat_like(ir, a, int_min(eb));
    V both = LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntEQ, a, min, ""), LLVMBuildICmp(B, LLVMIntEQ, b, min, ""), "");
    set_qc(ir, both);
    return LLVMBuildSelect(B, both, splat_like(ir, p, int_max(2 * eb)), p, "");
}
/* SQDMULH, SQRDMULH: the high half of 2*a*b (plus 1<<(eb-1) rounding), computed in twice the width where it cannot
   overflow but for INT_MIN * INT_MIN, which saturates with QC */
static V doubling_high(SnxIr* ir, V a, V b, unsigned round) {
    unsigned eb = bits_of(a);
    V x = widen(ir, a, 0), p, min, both;
    p = LLVMBuildShl(B, LLVMBuildMul(B, x, widen(ir, b, 0), ""), splat_like(ir, x, 1), "");
    if (round) p = LLVMBuildAdd(B, p, splat_like(ir, x, int_min(eb)), "");
    p = narrow(ir, LLVMBuildAShr(B, p, splat_like(ir, x, eb), ""));
    min = splat_like(ir, a, int_min(eb));
    both = LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntEQ, a, min, ""), LLVMBuildICmp(B, LLVMIntEQ, b, min, ""), "");
    set_qc(ir, both);
    return LLVMBuildSelect(B, both, splat_like(ir, a, int_max(eb)), p, "");
}

/* PMUL, PMULL (bytes): carry-less products of byte lanes, 16 bits wide: b shifted by each set bit of a, xored */
static V poly_multiply(SnxIr* ir, V a, V b) {
    V x = widen(ir, a, 1), y = widen(ir, b, 1), p = splat_like(ir, x, 0), bit;
    unsigned k;
    for (k = 0; k < 8; ++k) {
        bit = LLVMBuildAnd(B, LLVMBuildLShr(B, x, splat_like(ir, x, k), ""), splat_like(ir, x, 1), "");
        p = LLVMBuildXor(
            B, p, LLVMBuildAnd(B, LLVMBuildNeg(B, bit, ""), LLVMBuildShl(B, y, splat_like(ir, y, k), ""), ""), "");
    }
    return p;
}

/* the long forms on narrow lanes a and b, by three-different's opcode: [SU]ADDL (0), [SU]SUBL (2), [SU]ABAL (5),
   [SU]ABDL (7), [SU]MLAL (8), SQDMLAL (9), [SU]MLSL (10), SQDMLSL (11), [SU]MULL (12), SQDMULL (13); z the
   accumulator's wide lanes. Every result fits the wide lane, so the helper's 64-bit sums truncate to the same bits */
static V long_op(SnxIr* ir, unsigned op, unsigned u, V a, V b, V z) {
    V x, y, r;
    if (op == 9 || op == 11 || op == 13) {
        r = doubling_long(ir, a, b);
        return op == 13 ? r : saturating(ir, 0, op == 11, 2 * bits_of(a), lanes_of(a), z, r);
    }
    x = widen(ir, a, u);
    y = widen(ir, b, u);
    switch (op) {
        case 0: return LLVMBuildAdd(B, x, y, "");
        case 2: return LLVMBuildSub(B, x, y, "");
        case 5:
        case 7:
            r = LLVMBuildSelect(B, LLVMBuildICmp(B, u ? LLVMIntUGT : LLVMIntSGT, a, b, ""), LLVMBuildSub(B, x, y, ""),
                                LLVMBuildSub(B, y, x, ""), "");
            return op == 5 ? LLVMBuildAdd(B, z, r, "") : r;
        case 8: return LLVMBuildAdd(B, z, LLVMBuildMul(B, x, y, ""), "");
        case 10: return LLVMBuildSub(B, z, LLVMBuildMul(B, x, y, ""), "");
        default: return LLVMBuildMul(B, x, y, "");
    }
}

/* SSHL, USHL, SRSHL, URSHL, SQSHL, UQSHL, SQRSHL, UQRSHL (register): vshl (helpers/simd.c) on 64-bit copies of the
   lanes, the shift the signed low byte of b's lane. Every shift amount is clamped below 64 and the cases it would
   change are selected away: LLVM's shifts by the width or more are poison. QC lanes go to *saturated */
static V register_shift(SnxIr* ir, V a, V b, unsigned u, unsigned round, unsigned sat) {
    unsigned eb = bits_of(a);
    V x = extend(ir, a, u, 64), sh = extend(ir, extend(ir, b, 1, 8), 0, 64);
    V zero = splat_like(ir, x, 0), k63 = splat_like(ir, x, 63), keb = splat_like(ir, x, eb), left, right, r;
    V amount, shifted, fits, negative, back;
    /* left: sh >= 0 */
    amount = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntUGT, sh, k63, ""), k63, sh, "");
    shifted = LLVMBuildShl(B, x, amount, "");
    if (!sat)
        left = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSGE, sh, keb, ""), zero, shifted, "");
    else {
        /* no bit lost: shifting back gives x, and the result fits the lane (vshl's checks) */
        back = u ? LLVMBuildLShr(B, shifted, amount, "") : LLVMBuildAShr(B, shifted, amount, "");
        fits = LLVMBuildICmp(B, LLVMIntEQ, extend(ir, extend(ir, shifted, u, eb), u, 64), shifted, "");
        fits = LLVMBuildAnd(B, fits, LLVMBuildICmp(B, LLVMIntEQ, back, x, ""), "");
        fits = LLVMBuildAnd(B, fits, LLVMBuildICmp(B, LLVMIntSLT, sh, keb, ""), "");
        fits = LLVMBuildOr(B, fits, LLVMBuildICmp(B, LLVMIntEQ, x, zero, ""), "");
        left = LLVMBuildSelect(B, fits, shifted,
                               u ? splat_like(ir, x, uint_max(eb))
                                 : LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, x, zero, ""),
                                                   splat_like(ir, x, int_min(eb)), splat_like(ir, x, int_max(eb)), ""),
                               "");
        set_qc(ir, LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntSGE, sh, zero, ""), LLVMBuildNot(B, fits, ""), ""));
    }
    /* right: n = -sh, 1..128; a shift by 64 or more leaves the sign (signed) or 0 */
    negative = LLVMBuildSub(B, zero, sh, "");
    amount = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntUGT, negative, k63, ""), k63, negative, "");
    if (u)
        right = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntUGT, negative, k63, ""), zero,
                                LLVMBuildLShr(B, x, amount, ""), "");
    else
        right = LLVMBuildAShr(B, x, amount, "");
    if (round) { /* + the last bit shifted out, bit n-1; past a shift of 64 there is nothing left */
        V last = LLVMBuildSub(B, negative, splat_like(ir, x, 1), "");
        V bit;
        last = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntUGT, last, k63, ""), k63, last, "");
        bit = LLVMBuildAnd(B, LLVMBuildLShr(B, x, last, ""), splat_like(ir, x, 1), "");
        right = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntUGT, negative, splat_like(ir, x, 64), ""), zero,
                                LLVMBuildAdd(B, right, bit, ""), "");
    }
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, sh, zero, ""), right, left, "");
    return extend(ir, r, 1, eb);
}

/* three-same integer (vector): simd_3same for BSL/BIT/BIF, compares, MIN/MAX, ABD/ABA, MLA/MLS, MUL, pairwise
   ADDP/MAXP/MINP */
static int three_same(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 11) & 31,
             rn = (w >> 5) & 31, rd = w & 31, eb = 8u << size, n, i;
    LLVMTypeRef t;
    V a, b, z, r;
    if (op == 3) { /* BSL, BIT, BIF (fast_simd has the others): one VPTERNLOGQ on AVX-512 hosts */
        LLVMTypeRef t = LLVMVectorType(ir->t_i64, 2);
        V x, y, d, r;
        if (!u || !size) return 0;
        x = ir_load(ir, t, vreg(ir, rn));
        y = ir_load(ir, t, vreg(ir, rm));
        d = ir_load(ir, t, vreg(ir, rd));
        LLVMSetAlignment(x, 8); /* V128 is 8-aligned in the CPU structure */
        LLVMSetAlignment(y, 8);
        LLVMSetAlignment(d, 8);
        if (host_ternlog(ir)) {
            unsigned table = size == 1   ? (TERN_C & TERN_A) | (~TERN_C & TERN_B)  /* BSL: (d & n) | (~d & m) */
                             : size == 2 ? (TERN_C & ~TERN_B) | (TERN_A & TERN_B)  /* BIT: (d & ~m) | (n & m) */
                                         : (TERN_C & TERN_B) | (TERN_A & ~TERN_B); /* BIF: (d & m) | (n & ~m) */
            r = LLVMBuildBitCast(B, ternlog(ir, x, y, d, table, 64), t, "");
        } else if (size == 1)
            r = LLVMBuildOr(B, LLVMBuildAnd(B, d, x, ""), LLVMBuildAnd(B, LLVMBuildNot(B, d, ""), y, ""), "");
        else if (size == 2)
            r = LLVMBuildOr(B, LLVMBuildAnd(B, d, LLVMBuildNot(B, y, ""), ""), LLVMBuildAnd(B, x, y, ""), "");
        else
            r = LLVMBuildOr(B, LLVMBuildAnd(B, d, y, ""), LLVMBuildAnd(B, x, LLVMBuildNot(B, y, ""), ""), "");
        if (!q) r = LLVMBuildInsertElement(B, r, ir_i64(ir, 0), ir_i32(ir, 1), "");
        LLVMSetAlignment(LLVMBuildStore(B, r, vreg(ir, rd)), 8);
        return 1;
    }
    if (op >= 0x18 || (size == 3 && !q)) return 0;
    n = (q ? 128u : 64u) / eb;
    t = LLVMVectorType(lane_type(ir, size), n);
    a = get_lanes(ir, vreg(ir, rn), size, n);
    b = get_lanes(ir, vreg(ir, rm), size, n);
    switch (op) {
        case 0x00:
        case 0x02:
            if (eb == 64) return 0;
            r = halving(ir, u, op == 0x02, size, n, a, b);
            break;
        case 0x01:
        case 0x05: r = saturating(ir, u, op == 0x05, eb, n, a, b); break;
        case 0x04: /* SHSUB, UHSUB: the difference in twice the width, halved arithmetically */
            if (eb == 64) return 0;
            r = narrow(ir, LLVMBuildAShr(B, LLVMBuildSub(B, widen(ir, a, u), widen(ir, b, u), ""),
                                         splat_like(ir, widen(ir, a, u), 1), ""));
            break;
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: r = register_shift(ir, a, b, u, op == 0x0A || op == 0x0B, op == 0x09 || op == 0x0B); break;
        case 0x16:
            if (size != 1 && size != 2) return 0;
            r = doubling_high(ir, a, b, u);
            break;
        case 0x06: r = mask_of(ir, LLVMBuildICmp(B, u ? LLVMIntUGT : LLVMIntSGT, a, b, ""), t); break;
        case 0x07: r = mask_of(ir, LLVMBuildICmp(B, u ? LLVMIntUGE : LLVMIntSGE, a, b, ""), t); break;
        case 0x11:
            r = u ? mask_of(ir, LLVMBuildICmp(B, LLVMIntEQ, a, b, ""), t)
                  : mask_of(ir, LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, a, b, ""), LLVMConstNull(t), ""), t);
            break;
        case 0x0C:
        case 0x0D:
            if (eb == 64) return 0;
            r = LLVMBuildSelect(
                B,
                LLVMBuildICmp(B, op == 0x0C ? (u ? LLVMIntUGT : LLVMIntSGT) : (u ? LLVMIntULT : LLVMIntSLT), a, b, ""),
                a, b, "");
            break;
        case 0x0E:
        case 0x0F:
            if (eb == 64) return 0;
            r = LLVMBuildSelect(B, LLVMBuildICmp(B, u ? LLVMIntUGT : LLVMIntSGT, a, b, ""), LLVMBuildSub(B, a, b, ""),
                                LLVMBuildSub(B, b, a, ""), "");
            if (op == 0x0F) r = LLVMBuildAdd(B, get_lanes(ir, vreg(ir, rd), size, n), r, "");
            break;
        case 0x12:
            if (eb == 64) return 0;
            z = get_lanes(ir, vreg(ir, rd), size, n);
            r = u ? LLVMBuildSub(B, z, LLVMBuildMul(B, a, b, ""), "")
                  : LLVMBuildAdd(B, z, LLVMBuildMul(B, a, b, ""), "");
            break;
        case 0x13: /* MUL, PMUL (bytes) */
            if (eb == 64 || (u && size)) return 0;
            r = u ? narrow(ir, poly_multiply(ir, a, b)) : LLVMBuildMul(B, a, b, "");
            break;
        case 0x14:
        case 0x15:
        case 0x17: {
            /* pairwise: lanes 0..n/2-1 from pairs of a, the rest from pairs of b */
            unsigned even[16], odd[16];
            V x, y;
            if ((op != 0x17 && eb == 64) || (op == 0x17 && u)) return 0;
            for (i = 0; i < n; ++i) {
                even[i] = i < n / 2 ? 2 * i : n + 2 * (i - n / 2);
                odd[i] = even[i] + 1;
            }
            x = shuffle(ir, a, b, even, n);
            y = shuffle(ir, a, b, odd, n);
            if (op == 0x17)
                r = LLVMBuildAdd(B, x, y, "");
            else
                r = LLVMBuildSelect(
                    B,
                    LLVMBuildICmp(B, op == 0x14 ? (u ? LLVMIntUGT : LLVMIntSGT) : (u ? LLVMIntULT : LLVMIntSLT), x, y,
                                  ""),
                    x, y, "");
            break;
        }
        default: return 0;
    }
    write_lanes(ir, rd, r, q);
    return 1;
}

/* byte-lane shifts by immediate through GF2P8AFFINEQB: simd_shimm's semantics for eb 8 */
static int shift_bytes_gfni(SnxIr* ir, unsigned q, unsigned u, unsigned op, unsigned shr, unsigned shl, unsigned rn,
                            unsigned rd) {
    unsigned n = q ? 16 : 8;
    V a = v128_type_load(ir, vreg(ir, rn)), r, z;
    switch (op) {
        case 0x00:
        case 0x02:
        case 0x04:
        case 0x06: /* [SU][R]SHR, [SU][R]SRA */
            r = gf_affine(ir, a, u ? gf_lshr(shr) : gf_ashr(shr));
            if (op & 4) /* + the last bit shifted out (signed or not, it is source bit shr-1) */
                r = LLVMBuildAdd(B, r, gf_affine(ir, a, gf_bit(shr - 1)), "");
            if (op & 2) r = LLVMBuildAdd(B, v128_type_load(ir, vreg(ir, rd)), r, "");
            break;
        case 0x08: /* SRI */
            if (!u) return 0;
            z = v128_type_load(ir, vreg(ir, rd));
            if (shr >= 8)
                r = z;
            else
                r = LLVMBuildOr(B, LLVMBuildAnd(B, z, broadcast(ir, ir_i8(ir, ~(0xFFu >> shr)), 16), ""),
                                gf_affine(ir, a, gf_lshr(shr)), "");
            break;
        case 0x0A:
            r = gf_affine(ir, a, gf_shl(shl));
            if (u) { /* SLI */
                z = v128_type_load(ir, vreg(ir, rd));
                r = LLVMBuildOr(B, LLVMBuildAnd(B, z, broadcast(ir, ir_i8(ir, ~(0xFFu << shl)), 16), ""), r, "");
            }
            break;
        default: return 0;
    }
    write_lanes(ir, rd, low_lanes(ir, r, n), q);
    return 1;
}

/* the narrowing shifts right on the wide lanes of Vn, shr 1..eb: SHRN and RSHRN wrap, the saturating ones clamp
   with QC (and have scalar forms: one lane, the rest of Vd zero). Rounding adds the last bit shifted out (simd_shimm:
   the constant first would overflow 64 bits) */
static int shift_narrow(SnxIr* ir, unsigned q, unsigned u, unsigned op, unsigned size, unsigned shr, unsigned rn,
                        unsigned rd, unsigned scalar) {
    unsigned rnd = op & 1;
    V x, v, bit;
    if (scalar && !u && op < 0x12) return 0;
    x = vlanes(ir, vreg(ir, rn), size + 1, scalar ? 1 : 8u >> size);
    if (!u && op < 0x12) { /* SHRN, RSHRN: the bits kept end below the wide lane's top, so its carry is not needed */
        if (rnd) x = LLVMBuildAdd(B, x, splat_like(ir, x, (SnxU64)1 << (shr - 1)), "");
        write_narrow(ir, rd, narrow(ir, LLVMBuildLShr(B, x, splat_like(ir, x, shr), "")), q);
        return 1;
    }
    /* U with op 0x12/0x13: UQSHRN, UQRSHRN (unsigned); else SQSHRN, SQRSHRN, SQSHRUN, SQRSHRUN (signed source) */
    v = u && op >= 0x12 ? LLVMBuildLShr(B, x, splat_like(ir, x, shr), "")
                        : LLVMBuildAShr(B, x, splat_like(ir, x, shr), "");
    if (rnd) {
        bit = LLVMBuildAnd(B, LLVMBuildLShr(B, x, splat_like(ir, x, shr - 1), ""), splat_like(ir, x, 1), "");
        v = LLVMBuildAdd(B, v, bit, "");
    }
    write_narrow(ir, rd, saturate_narrow(ir, v, !(u && op >= 0x12), !u), q);
    return 1;
}

/* shift by immediate: simd_shimm's SSHR/USHR, SSRA/USRA, SRSHR/URSHR, SRSRA/URSRA, SHL, SLI, SRI, SQSHL/UQSHL,
   SQSHLU, SSHLL/USHLL and the narrowing shifts; scalar, the 64-bit forms and the saturating left shifts of any size
   (as simd_shimm allows) */
static int shift_immediate(SnxIr* ir, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, immh = (w >> 19) & 15, immb = (w >> 16) & 7,
             op = (w >> 11) & 31, rn = (w >> 5) & 31, rd = w & 31, size = 3, eb, imm, shr, shl, n;
    LLVMTypeRef t;
    V a, r;
    if (!immh) return 0;
    while (!((immh >> size) & 1)) size--;
    eb = 8u << size;
    imm = (immh << 3) | immb;
    shr = 2 * eb - imm;
    shl = imm - eb;
    if (op == 0x14 || (op >= 0x10 && op <= 0x13)) {
        if (size == 3) return 0;
        if (op != 0x14) return shift_narrow(ir, q, u, op, size, shr, rn, rd, scalar);
        if (scalar) return 0;
        /* SSHLL, USHLL: the low or high half widened, shifted left */
        r = widen(ir, half_lanes(ir, rn, size, q), u);
        set_lanes(ir, vreg(ir, rd), LLVMBuildShl(B, r, splat_like(ir, r, shl), ""));
        return 1;
    }
    if (size == 3 && !q && !scalar) return 0;
    if (scalar && size != 3 && op != 0x0C && op != 0x0E) return 0;
    if (!size && !scalar && host_gfni(ir) && shift_bytes_gfni(ir, q, u, op, shr, shl, rn, rd)) return 1;
    n = scalar ? 1 : (q ? 128u : 64u) / eb;
    t = lanes_type(ir, size, n);
    a = get_lanes(ir, vreg(ir, rn), size, n);
#define SPLAT(v) broadcast(ir, LLVMConstInt(lane_type(ir, size), (v), 0), n)
    switch (op) {
        case 0x00:
        case 0x02:
        case 0x04:
        case 0x06: /* [SU][R]SHR, [SU][R]SRA: a shift by the element size gives 0 (unsigned) or the sign */
            if (u)
                r = shr >= eb ? LLVMConstNull(t) : LLVMBuildLShr(B, a, SPLAT(shr), "");
            else
                r = LLVMBuildAShr(B, a, SPLAT(shr >= eb ? eb - 1 : shr), "");
            if (op & 4) { /* rounding: + bit shr-1 of the source (for shr = eb, 0 signed, the top bit unsigned) */
                V bit = LLVMBuildAnd(B, LLVMBuildLShr(B, a, SPLAT(shr - 1), ""), SPLAT(1), "");
                r = shr >= eb && !u ? LLVMConstNull(t) : LLVMBuildAdd(B, r, bit, "");
            }
            if (op & 2) r = LLVMBuildAdd(B, get_lanes(ir, vreg(ir, rd), size, n), r, "");
            break;
        case 0x08: /* SRI */
            if (!u) return 0;
            if (shr >= eb)
                r = get_lanes(ir, vreg(ir, rd), size, n);
            else {
                SnxU64 m = (eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1) >> shr;
                r = LLVMBuildOr(B, LLVMBuildAnd(B, get_lanes(ir, vreg(ir, rd), size, n), SPLAT(~m), ""),
                                LLVMBuildLShr(B, a, SPLAT(shr), ""), "");
            }
            break;
        case 0x0A:
            if (!u) /* SHL */
                r = LLVMBuildShl(B, a, SPLAT(shl), "");
            else { /* SLI */
                SnxU64 m = (eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1) << shl;
                r = LLVMBuildOr(B, LLVMBuildAnd(B, get_lanes(ir, vreg(ir, rd), size, n), SPLAT(~m), ""),
                                LLVMBuildAnd(B, LLVMBuildShl(B, a, SPLAT(shl), ""), SPLAT(m), ""), "");
            }
            break;
        case 0x0C:   /* SQSHLU: a negative lane gives 0, then as UQSHL */
        case 0x0E: { /* SQSHL, UQSHL: saturated when shifting back does not give the lane (no bit lost, no sign
                        change), QC */
            V shifted = LLVMBuildShl(B, a, SPLAT(shl), ""), lost, negative = NULL, top;
            if (op == 0x0C && !u) return 0;
            if (!u) {
                lost = LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAShr(B, shifted, SPLAT(shl), ""), a, "");
                top = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, a, LLVMConstNull(t), ""), SPLAT(int_min(eb)),
                                      SPLAT(int_max(eb)), "");
            } else {
                lost = LLVMBuildICmp(B, LLVMIntNE, LLVMBuildLShr(B, shifted, SPLAT(shl), ""), a, "");
                top = SPLAT(uint_max(eb));
            }
            r = LLVMBuildSelect(B, lost, top, shifted, "");
            if (op == 0x0C) {
                negative = LLVMBuildICmp(B, LLVMIntSLT, a, LLVMConstNull(t), "");
                r = LLVMBuildSelect(B, negative, LLVMConstNull(t), r, "");
                lost = LLVMBuildOr(B, lost, negative, "");
            }
            set_qc(ir, lost);
            break;
        }
        default: return 0;
    }
#undef SPLAT
    write_lanes(ir, rd, r, q);
    return 1;
}

/* FABS, FNEG (vector): sign bits only */
static int fp_sign_misc(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, sz = size & 1;
    SnxU64 sign = sz ? (SnxU64)1 << 63 : ((SnxU64)0x80000000UL << 32) | 0x80000000UL;
    V lo, hi;
    if (op != 0x0F || !(size >> 1) || (sz && !q)) return 0;
    lo = ir_vd(ir, rn, 0);
    hi = ir_vd(ir, rn, 1);
    if (u) {
        lo = LLVMBuildXor(B, lo, ir_i64(ir, sign), "");
        hi = LLVMBuildXor(B, hi, ir_i64(ir, sign), "");
    } else {
        lo = LLVMBuildAnd(B, lo, ir_i64(ir, ~sign), "");
        hi = LLVMBuildAnd(B, hi, ir_i64(ir, ~sign), "");
    }
    ir_set_vd(ir, rd, 0, lo);
    ir_set_vd(ir, rd, 1, q ? hi : ir_i64(ir, 0));
    return 1;
}

enum {
    FP_LANES_ADD,
    FP_LANES_ABD,
    FP_LANES_MULX,
    FP_LANES_STEP,
    FP_LANES_MINMAX,
    FP_LANES_COMPARE,
    FP_LANES_SQRT,
    FP_LANES_ESTIMATE,
    FP_LANES_TO_INT,
    FP_LANES_FROM_INT
};
/* what fp_lanes computes. arg: the minimum flag (MINMAX), the comparison (COMPARE), rsqrt (STEP: FRSQRTS rather
   than FRECPS, ESTIMATE: FRSQRTE rather than FRECPE) or snx_fp_to_int's rounding mode (TO_INT). flag: numeric
   (MINMAX) or unsigned (the conversions). fbits: the conversions' fixed point */
typedef struct {
    unsigned kind, arg, flag, fbits;
} FpOp;

/* one lane through the helper simd_3same's and simd_2misc's loops call (helpers/simd.c), as iN */
static V fp_lane(SnxIr* ir, const FpOp* op, V a, V b, unsigned width) {
    switch (op->kind) {
        case FP_LANES_ADD: return ir_fp_binary(ir, a, b, width, 0);                /* SNX_FP_ADD */
        case FP_LANES_ABD: return ir_fp_binary(ir, a, b, width, 5);                /* SNX_FP_ABD */
        case FP_LANES_MULX: return ir_fp_binary(ir, a, b, width, 4);               /* SNX_FP_MULX */
        case FP_LANES_STEP: return ir_fp_binary(ir, a, b, width, op->arg ? 7 : 6); /* SNX_FP_RSQRTS, SNX_FP_RECPS */
        case FP_LANES_MINMAX: return ir_fp_minmax(ir, a, b, width, op->arg, op->flag);
        case FP_LANES_COMPARE: return ir_fp_compare_mask(ir, op->arg, a, b, width);
        case FP_LANES_SQRT: return ir_fp_unary(ir, a, width, 0);                   /* SNX_FP_SQRT */
        case FP_LANES_ESTIMATE: return ir_fp_unary(ir, a, width, op->arg ? 2 : 1); /* SNX_FP_RSQRTE, SNX_FP_RECPE */
        case FP_LANES_TO_INT: return ir_fp_to_int(ir, a, width, width, op->flag, op->fbits, op->arg);
        default: return ir_fp_from_int(ir, a, width, width, op->flag, op->fbits);
    }
}

/* the operation on n lanes of 1<<s bytes (x, and y for two operands) into Vd: every lane at once on the host's
   vector unit when the fast path's guard holds for all of them (ir_fp.c's fast paths), otherwise the helper a lane
   at a time. One lane is a scalar form: the rest of Vd is zero */
static void fp_lanes(SnxIr* ir, const FpOp* op, unsigned s, unsigned n, V x, V y, unsigned rd, unsigned q) {
    unsigned width = 8u << s, i;
    LLVMBasicBlockRef slow, join = NULL;
    SnxFpFast f;
    V r;
    switch (op->kind) {
        case FP_LANES_ADD: ir_fp_fast(ir, SNX_IR_FP_ADD, width, n, x, y, NULL, &f); break;
        case FP_LANES_ABD: /* |x - y|: snx_fp_binary clears the difference's sign */
            ir_fp_fast(ir, SNX_IR_FP_SUB, width, n, x, y, NULL, &f);
            f.value = LLVMBuildAnd(
                B, f.value, broadcast(ir, LLVMConstInt(lane_type(ir, s), ((SnxU64)1 << (width - 1)) - 1, 0), n), "");
            break;
        case FP_LANES_MULX: /* FMUL's result but for infinity times zero, which the guard keeps out */
            ir_fp_fast(ir, SNX_IR_FP_MUL, width, n, x, y, NULL, &f);
            break;
        case FP_LANES_STEP: ir_fp_step_fast(ir, op->arg, width, n, x, y, &f); break;
        case FP_LANES_MINMAX: ir_fp_minmax_fast(ir, width, n, x, y, op->arg, &f); break;
        case FP_LANES_COMPARE: ir_fp_compare_fast(ir, op->arg, width, n, x, y, &f); break;
        case FP_LANES_SQRT: ir_fp_fast(ir, SNX_IR_FP_SQRT, width, n, x, NULL, NULL, &f); break;
        case FP_LANES_ESTIMATE: ir_fp_estimate_fast(ir, op->arg, width, n, x, &f); break;
        case FP_LANES_TO_INT:
            if (!ir_fp_to_int_fast(ir, width, n, x, width, op->flag, op->arg, op->fbits, &f)) f.ok = NULL;
            break;
        default: ir_fp_from_int_fast(ir, width, n, x, op->flag, op->fbits, &f); break;
    }
    if (f.ok) { /* else no fast path: the helper alone */
        join = ir_fp_branch(ir, f.ok, &slow);
        write_lanes(ir, rd, f.value, q);
        ir_fp_else(ir, join, slow, f.inexact);
    }
    r = n == 1 ? NULL : LLVMGetUndef(LLVMTypeOf(x));
    for (i = 0; i < n; ++i) {
        V a = n == 1 ? x : LLVMBuildExtractElement(B, x, ir_i32(ir, i), ""), b = NULL, v;
        if (y) b = n == 1 ? y : LLVMBuildExtractElement(B, y, ir_i32(ir, i), "");
        v = ir_cast(ir, fp_lane(ir, op, a, b, width), lane_type(ir, s), 0);
        r = n == 1 ? v : LLVMBuildInsertElement(B, r, v, ir_i32(ir, i), "");
    }
    write_lanes(ir, rd, r, q);
    if (join) ir_fp_join(ir, join);
}

/* FP three-same beyond fast_simd's arithmetic: simd_3same's FADDP, FABD, FMULX, FRECPS, FRSQRTS, FMAX/FMIN/FMAXNM/
   FMINNM and their pairwise forms, FCMEQ/FCMGE/FCMGT and FACGE/FACGT; the scalar forms of FABD, FMULX, FRECPS,
   FRSQRTS and the compares (simd_3same refuses the scalar pairwise, accumulating and min/max ones) */
static int fp_three_same(SnxIr* ir, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, sz = (w >> 22) & 1, hi = (w >> 23) & 1,
             rm = (w >> 16) & 31, op = (w >> 11) & 31, rn = (w >> 5) & 31, rd = w & 31, s = sz ? 3 : 2,
             n = scalar ? 1
                 : sz   ? 2
                        : (q ? 4 : 2),
             i;
    unsigned pair = 0, absolute = 0;
    FpOp o;
    V x, y;
    if (!scalar && sz && !q) return 0;
    o.arg = hi;
    o.flag = op == 0x18;
    o.fbits = 0;
    switch (op) {
        case 0x18: /* FMAXNM, FMINNM */
        case 0x1E: /* FMAX, FMIN; with U, the pairwise FMAXNMP, FMINNMP, FMAXP, FMINP */
            o.kind = FP_LANES_MINMAX;
            pair = u;
            break;
        case 0x1A: /* FADDP, FABD (FADD and FSUB are fast_simd's) */
            if (!u) return 0;
            o.kind = hi ? FP_LANES_ABD : FP_LANES_ADD;
            pair = !hi;
            break;
        case 0x1B: /* FMULX (FMUL is fast_simd's) */
            if (u || hi) return 0;
            o.kind = FP_LANES_MULX;
            break;
        case 0x1F: /* FRECPS, FRSQRTS (FDIV is fast_simd's) */
            if (u) return 0;
            o.kind = FP_LANES_STEP;
            break;
        case 0x1C:
            if (!u && hi) return 0;
            o.kind = FP_LANES_COMPARE;
            o.arg = !u ? SNX_IR_CMP_EQ : hi ? SNX_IR_CMP_GT : SNX_IR_CMP_GE;
            break;
        case 0x1D: /* FACGE, FACGT: the comparison of the magnitudes */
            if (!u) return 0;
            o.kind = FP_LANES_COMPARE;
            o.arg = hi ? SNX_IR_CMP_GT : SNX_IR_CMP_GE;
            absolute = 1;
            break;
        default: return 0;
    }
    if (scalar && o.kind != FP_LANES_ABD && o.kind != FP_LANES_MULX && o.kind != FP_LANES_STEP &&
        o.kind != FP_LANES_COMPARE)
        return 0;
    x = get_lanes(ir, vreg(ir, rn), s, n);
    y = get_lanes(ir, vreg(ir, rm), s, n);
    if (pair) { /* lanes 0..n/2-1 from pairs of Vn, the rest from pairs of Vm: in Vn:Vm, elements 2i and 2i+1 */
        unsigned even[4], odd[4];
        V a = x;
        for (i = 0; i < n; ++i) {
            even[i] = 2 * i;
            odd[i] = 2 * i + 1;
        }
        x = shuffle(ir, a, y, even, n);
        y = shuffle(ir, a, y, odd, n);
    }
    if (absolute) {
        V magnitude = broadcast(ir, LLVMConstInt(lane_type(ir, s), sz ? ~((SnxU64)1 << 63) : 0x7FFFFFFFUL, 0), n);
        x = LLVMBuildAnd(B, x, magnitude, "");
        y = LLVMBuildAnd(B, y, magnitude, "");
    }
    fp_lanes(ir, &o, s, n, x, y, rd, q);
    return 1;
}

/* FP two-register misc: simd_2misc's FCMGT, FCMGE, FCMEQ, FCMLE, FCMLT #0.0, FCVT[NPMZA][SU], SCVTF, UCVTF, FRECPE
   and FRSQRTE (vector and scalar) and FSQRT (vector) */
static int fp_misc(SnxIr* ir, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31,
             rn = (w >> 5) & 31, rd = w & 31, sz = size & 1, s = sz ? 3 : 2,
             n = scalar ? 1
                 : sz   ? 2
                        : (q ? 4 : 2);
    unsigned k = u << 6 | (size >> 1) << 5 | op;
    FpOp o;
    V x;
    if (!scalar && sz && !q) return 0;
    o.arg = 0;
    o.flag = u;
    o.fbits = 0;
    if (op >= 0x0C && op <= 0x0E && (size >> 1)) {
        if (op == 0x0C)
            o.arg = u ? SNX_IR_CMP_GE : SNX_IR_CMP_GT;
        else if (op == 0x0D)
            o.arg = u ? SNX_IR_CMP_LE : SNX_IR_CMP_EQ;
        else if (!u)
            o.arg = SNX_IR_CMP_LT;
        else
            return 0;
        o.kind = FP_LANES_COMPARE;
        fp_lanes(ir, &o, s, n, get_lanes(ir, vreg(ir, rn), s, n), LLVMConstNull(lanes_type(ir, s, n)), rd, q);
        return 1;
    }
    switch (k) {
        case 0x1A: /* FCVTNS, FCVTPS, FCVTMS, FCVTZS, FCVTAS and the unsigned ones: snx_fp_to_int's mode */
        case 0x3A:
        case 0x1B:
        case 0x3B:
        case 0x1C:
        case 0x5A:
        case 0x7A:
        case 0x5B:
        case 0x7B:
        case 0x5C:
            o.kind = FP_LANES_TO_INT;
            o.arg = (k & 0x1F) == 0x1C ? 4 : (k & 0x1F) == 0x1A ? (size >> 1 ? 1 : 0) : (size >> 1 ? 3 : 2);
            break;
        case 0x1D: /* SCVTF, UCVTF */
        case 0x5D: o.kind = FP_LANES_FROM_INT; break;
        case 0x3D: /* FRECPE, FRSQRTE */
        case 0x7D:
            o.kind = FP_LANES_ESTIMATE;
            o.arg = u;
            break;
        case 0x7F:
            if (scalar) return 0; /* FSQRT has no scalar AdvSIMD form */
            o.kind = FP_LANES_SQRT;
            break;
        default: return 0;
    }
    x = get_lanes(ir, vreg(ir, rn), s, n);
    fp_lanes(ir, &o, s, n, x, NULL, rd, q);
    return 1;
}

/* SCVTF, UCVTF, FCVTZS, FCVTZU (vector and scalar, fixed point): simd_shimm's conversions with fbits = 2*esize - imm */
static int fp_fixed(SnxIr* ir, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, immh = (w >> 19) & 15, immb = (w >> 16) & 7,
             op = (w >> 11) & 31, rn = (w >> 5) & 31, rd = w & 31, s, n;
    FpOp o;
    if ((op != 0x1C && op != 0x1F) || immh < 4) return 0; /* 32 and 64-bit lanes only */
    s = immh & 8 ? 3 : 2;
    if (s == 3 && !q && !scalar) return 0;
    n = scalar ? 1 : (q ? 128u : 64u) >> (3 + s);
    o.kind = op == 0x1C ? FP_LANES_FROM_INT : FP_LANES_TO_INT;
    o.arg = 3; /* toward zero */
    o.flag = u;
    o.fbits = 2 * (8u << s) - ((immh << 3) | immb);
    fp_lanes(ir, &o, s, n, get_lanes(ir, vreg(ir, rn), s, n), NULL, rd, q);
    return 1;
}

/* FCVTN, FCVTN2 (doubles to floats, floats to halves, FPCR's rounding) and FCVTXN, FCVTXN2 (doubles to floats,
   round to odd, also scalar): simd_2misc's snx_fp_convert lanes, into Vd's low half with the upper half zero, its
   upper half with the low half kept (the "2" forms), or for the scalar FCVTXN its low 32 bits */
static int fp_narrow(SnxIr* ir, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31,
             rn = (w >> 5) & 31, rd = w & 31, from = size & 1 ? 64 : 32, s = size & 1 ? 3 : 2,
             n = scalar ? 1 : 128 / from, i;
    LLVMTypeRef t = lane_type(ir, s - 1);
    LLVMBasicBlockRef slow, join;
    SnxFpFast f;
    V x, r;
    if (op != 0x16 || (size >> 1) || (scalar && !u) || (u && from != 64)) return 0;
    x = get_lanes(ir, vreg(ir, rn), s, n);
    ir_fp_narrow_fast(ir, from, n, x, u, &f);
    join = ir_fp_branch(ir, f.ok, &slow);
    write_narrow(ir, rd, f.value, q);
    ir_fp_else(ir, join, slow, f.inexact);
    r = n == 1 ? NULL : LLVMGetUndef(LLVMVectorType(t, n));
    for (i = 0; i < n; ++i) {
        V lane =
            ir_cast(ir, ir_fp_narrow(ir, n == 1 ? x : LLVMBuildExtractElement(B, x, ir_i32(ir, i), ""), from, u), t, 0);
        r = n == 1 ? lane : LLVMBuildInsertElement(B, r, lane, ir_i32(ir, i), "");
    }
    write_narrow(ir, rd, r, q);
    ir_fp_join(ir, join);
    return 1;
}

/* ---- the integer and data-movement rest of snx_simd ---- */

/* DUP (element, scalar), the MOV Bd/Hd/Sd/Dd, Vn.T[i] alias: the element alone in Vd */
static int scalar_dup(SnxIr* ir, SnxU32 w) {
    unsigned imm5 = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, size = 0;
    if (!imm5) return 0;
    while (!((imm5 >> size) & 1)) size++;
    if (size > 3) return 0;
    write_lanes(ir, rd, get(ir, vreg(ir, rn), size, imm5 >> (size + 1)), 0);
    return 1;
}

/* SMOV: the element sign-extended into Xd, or Wd zero-extended above it (fast_simd has UMOV) */
static int smov(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, imm5 = (w >> 16) & 31, imm4 = (w >> 11) & 15, rn = (w >> 5) & 31,
             rd = w & 31, size = 0;
    V v;
    if (u || imm4 != 5 || !imm5) return 0;
    while (!((imm5 >> size) & 1)) size++;
    if (size >= 3 || (size == 2 && !q)) return 0;
    v = LLVMBuildSExt(B, get(ir, vreg(ir, rn), size, imm5 >> (size + 1)), ir->t_i64, "");
    if (!q) v = LLVMBuildZExt(B, LLVMBuildTrunc(B, v, ir->t_i32, ""), ir->t_i64, "");
    if (rd != 31) ir_set_x(ir, rd, v);
    return 1;
}

/* TBL, TBX: bytes of the 1-4 table registers Vn.. (wrapping) by Vm's indexes; an index past the table gives 0 (TBL)
   or keeps Vd's byte (TBX). On x86 one PSHUFB per table register, kept where the index falls in its 16 bytes (an
   in-range index is below 64, so PSHUFB's zeroing bit 7 is clear and its low 4 bits pick the byte); elsewhere one
   variable extract per byte from the table laid end to end */
static int table_lookup(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, len = (w >> 13) & 3, tbx = (w >> 12) & 1, rn = (w >> 5) & 31,
             rd = w & 31, n = q ? 16 : 8, k, i;
    LLVMTypeRef t = LLVMVectorType(ir->t_i8, 16);
    V idx = v128_type_load(ir, vreg(ir, rm)), tables[4], r;
    for (k = 0; k <= len; ++k) tables[k] = v128_type_load(ir, vreg(ir, (rn + k) & 31));
    r = tbx ? v128_type_load(ir, vreg(ir, rd)) : LLVMConstNull(t);
    if (ir->features->x86 && ir->features->sse42) { /* SSE4.2 implies SSSE3's PSHUFB */
        for (k = 0; k <= len; ++k) {
            V args[2], in;
            args[0] = tables[k];
            args[1] = idx;
            in = LLVMBuildICmp(B, LLVMIntULT, LLVMBuildSub(B, idx, broadcast(ir, ir_i8(ir, 16 * k), 16), ""),
                               broadcast(ir, ir_i8(ir, 16), 16), "");
            r = LLVMBuildSelect(B, in, ir_intrinsic(ir, "llvm.x86.ssse3.pshuf.b.128", NULL, args, 2), r, "");
        }
    } else {
        V all = concat(ir, tables, len + 1, 16);
        for (i = 0; i < n; ++i) {
            V x = LLVMBuildExtractElement(B, idx, ir_i32(ir, i), ""), e;
            V in = LLVMBuildICmp(B, LLVMIntULT, x, ir_i8(ir, 16 * (len + 1)), "");
            /* an index past the table reads byte 0 instead: LLVM's extract past the end is poison */
            e = LLVMBuildExtractElement(
                B, all, LLVMBuildZExt(B, LLVMBuildSelect(B, in, x, ir_i8(ir, 0), ""), ir->t_i32, ""), "");
            r = LLVMBuildInsertElement(B, r,
                                       LLVMBuildSelect(B, in, e, LLVMBuildExtractElement(B, r, ir_i32(ir, i), ""), ""),
                                       ir_i32(ir, i), "");
        }
    }
    write_lanes(ir, rd, low_lanes(ir, r, n), q);
    return 1;
}

/* LD2-4/ST2-4 (multiple structures): element e of register s is at byte (e*sel + s)*esize. simd_ldst moves an
   element at a time in address order; here, as LD1 does, the guest memory moves a register's size at a time, in
   address order, interleaved or deinterleaved by shuffles */
static int ld_structures(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, op = (w >> 12) & 15, size = (w >> 10) & 3, sel, bytes, eby, s, i, k, map[64];
    V address, parts[4], all;
    sel = op == 0 ? 4 : op == 4 ? 3 : op == 8 ? 2 : 0;
    if (!sel || (size == 3 && !q)) return 0;
    bytes = q ? 16 : 8;
    eby = 1u << size;
    address = ir_x(ir, rn);
    if (l) {
        for (k = 0; k < sel; ++k)
            parts[k] = ir_rd_bytes(ir, LLVMBuildAdd(B, address, ir_i64(ir, (SnxU64)k * bytes), ""), bytes);
        all = concat(ir, parts, sel, bytes);
        for (s = 0; s < sel; ++s) { /* byte i of register s: element i/eby, byte i%eby of it */
            for (i = 0; i < bytes; ++i) map[i] = (i / eby * sel + s) * eby + i % eby;
            set_lanes(ir, vreg(ir, (rt + s) & 31), shuffle(ir, all, all, map, bytes));
        }
        if (!q)
            for (s = 0; s < sel; ++s) zero_high(ir, (rt + s) & 31);
    } else {
        for (s = 0; s < sel; ++s) parts[s] = get_lanes(ir, vreg(ir, (rt + s) & 31), 0, bytes);
        all = concat(ir, parts, sel, bytes);
        for (i = 0; i < sel * bytes; ++i) { /* memory byte i: element m = i/eby, of register m%sel, lane m/sel */
            unsigned m = i / eby;
            map[i] = m % sel * bytes + m / sel * eby + i % eby;
        }
        for (k = 0; k < sel; ++k)
            ir_wr_bytes(ir, LLVMBuildAdd(B, address, ir_i64(ir, (SnxU64)k * bytes), ""),
                        shuffle(ir, all, all, map + k * bytes, bytes));
    }
    if (post) ir_set_x(ir, rn, LLVMBuildAdd(B, address, rm == 31 ? ir_i64(ir, (SnxU64)sel * bytes) : ir_x(ir, rm), ""));
    return 1;
}

/* three-different (vector): simd_3diff's long, wide and narrowing-high forms; PMULL stays with pmull_1q and
   snx_simd */
static int three_different(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 15,
             rn = (w >> 5) & 31, rd = w & 31, m;
    V a, b, z = NULL, r;
    if (size == 3) return 0;
    m = 8u >> size;
    switch (op) {
        case 4:
        case 6: /* ADDHN, SUBHN, RADDHN, RSUBHN: the high half of each wide sum or difference */
            a = vlanes(ir, vreg(ir, rn), size + 1, m);
            b = vlanes(ir, vreg(ir, rm), size + 1, m);
            r = op == 4 ? LLVMBuildAdd(B, a, b, "") : LLVMBuildSub(B, a, b, "");
            if (u) r = LLVMBuildAdd(B, r, splat_like(ir, r, int_min(8u << size)), "");
            write_narrow(ir, rd, narrow(ir, LLVMBuildLShr(B, r, splat_like(ir, r, 8u << size), "")), q);
            return 1;
        case 1:
        case 3: /* [SU]ADDW, [SU]SUBW: Vn's wide lanes and the half of Vm widened */
            a = vlanes(ir, vreg(ir, rn), size + 1, m);
            b = widen(ir, half_lanes(ir, rm, size, q), u);
            set_lanes(ir, vreg(ir, rd), op == 1 ? LLVMBuildAdd(B, a, b, "") : LLVMBuildSub(B, a, b, ""));
            return 1;
        case 14: /* PMULL (bytes; pmull_1q has the 1Q form) */
            if (u || size) return 0;
            set_lanes(ir, vreg(ir, rd), poly_multiply(ir, half_lanes(ir, rn, 0, q), half_lanes(ir, rm, 0, q)));
            return 1;
        case 9:
        case 11:
        case 13:
            if (u || !size) return 0;
            break;
        case 0:
        case 2:
        case 5:
        case 7:
        case 8:
        case 10:
        case 12: break;
        default: return 0;
    }
    a = half_lanes(ir, rn, size, q);
    b = half_lanes(ir, rm, size, q);
    if (op == 5 || op == 8 || op == 9 || op == 10 || op == 11) z = vlanes(ir, vreg(ir, rd), size + 1, m);
    set_lanes(ir, vreg(ir, rd), long_op(ir, op, u, a, b, z));
    return 1;
}

/* three-different (scalar): SQDMLAL, SQDMLSL, SQDMULL on lane 0, the rest of Vd zero */
static int scalar_three_different(SnxIr* ir, SnxU32 w) {
    unsigned u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 15, rn = (w >> 5) & 31,
             rd = w & 31;
    if (u || (op != 9 && op != 11 && op != 13) || (size != 1 && size != 2)) return 0;
    write_lanes(ir, rd,
                long_op(ir, op, 0, vlanes(ir, vreg(ir, rn), size, 1), vlanes(ir, vreg(ir, rm), size, 1),
                        op == 13 ? NULL : vlanes(ir, vreg(ir, rd), size + 1, 1)),
                0);
    return 1;
}

/* two-register misc, integer: simd_2misc's REV16/32/64, CLS, CLZ, ABS, NEG, SQABS, SQNEG, SUQADD, USQADD, the
   compares against zero, [SU]ADDLP, [SU]ADALP, XTN, SQXTN, UQXTN, SQXTUN and SHLL (byte_misc has CNT, NOT and RBIT).
   Scalar, as simd_2misc allows: the saturating ones of any size, ABS, NEG and the compares on D registers */
static int int_misc(SnxIr* ir, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31,
             rn = (w >> 5) & 31, rd = w & 31, eb = 8u << size, n = scalar ? 1 : (q ? 128u : 64u) / eb, i, indices[16],
             k = (u << 5) | op;
    LLVMTypeRef t;
    V a, r, zero;
    if (scalar && op != 0x12 && op != 0x14 && size != 3 && op != 0x03 && op != 0x07) return 0;
    switch (op) {
        case 0x12:
        case 0x14:                                            /* XTN (wraps), SQXTUN, SQXTN, UQXTN */
            if (size == 3 || (scalar && k == 0x12)) return 0; /* XTN has no scalar form */
            a = vlanes(ir, vreg(ir, rn), size + 1, scalar ? 1 : 64 / eb);
            r = k == 0x12 ? narrow(ir, a) : saturate_narrow(ir, a, k != 0x34, k == 0x14);
            write_narrow(ir, rd, r, q);
            return 1;
        case 0x03: { /* SUQADD (signed Vd + unsigned Vn), USQADD (unsigned Vd + signed Vn): exact in twice the width */
            V z;
            if (size == 3 && !q && !scalar) return 0;
            a = vlanes(ir, vreg(ir, rn), size, n);
            z = vlanes(ir, vreg(ir, rd), size, n);
            write_lanes(ir, rd, saturate_narrow(ir, LLVMBuildAdd(B, widen(ir, z, u), widen(ir, a, !u), ""), 1, !u), q);
            return 1;
        }
        case 0x13: /* SHLL: the half widened, shifted by the element size */
            if (!u || size == 3 || scalar) return 0;
            r = widen(ir, half_lanes(ir, rn, size, q), 1);
            set_lanes(ir, vreg(ir, rd), LLVMBuildShl(B, r, splat_like(ir, r, eb), ""));
            return 1;
        case 0x02:
        case 0x06: { /* [SU]ADDLP, [SU]ADALP: adjacent pairs summed in twice the width */
            unsigned even[16], odd[16];
            if (size == 3 || scalar) return 0;
            a = vlanes(ir, vreg(ir, rn), size, n);
            for (i = 0; i < n / 2; ++i) {
                even[i] = 2 * i;
                odd[i] = 2 * i + 1;
            }
            r = LLVMBuildAdd(B, widen(ir, shuffle(ir, a, a, even, n / 2), u),
                             widen(ir, shuffle(ir, a, a, odd, n / 2), u), "");
            if (op == 6) r = LLVMBuildAdd(B, r, vlanes(ir, vreg(ir, rd), size + 1, n / 2), "");
            write_lanes(ir, rd, r, q);
            return 1;
        }
        default: break;
    }
    if (size == 3 && !q && !scalar) return 0;
    a = vlanes(ir, vreg(ir, rn), size, n);
    t = LLVMTypeOf(a);
    zero = LLVMConstNull(t);
    switch (k) {
        case 0x00:
        case 0x20:
        case 0x01: { /* REV64, REV32, REV16: the elements of each container reversed */
            unsigned container = op == 1 ? 16 : u ? 32 : 64, per;
            if (eb >= container || scalar) return 0;
            per = container / eb;
            for (i = 0; i < n; ++i) indices[i] = i - i % per + per - 1 - i % per;
            r = shuffle(ir, a, a, indices, n);
            break;
        }
        case 0x04:   /* CLS: the leading zeros of x ^ (x >> 1), less one */
        case 0x24: { /* CLZ */
            V args[2];
            if (size == 3) return 0;
            args[0] = u ? a : LLVMBuildXor(B, a, LLVMBuildAShr(B, a, splat_like(ir, a, 1), ""), "");
            args[1] = LLVMConstInt(ir->t_i1, 0, 0); /* a zero lane counts the lane's width */
            r = ir_intrinsic(ir, "llvm.ctlz", t, args, 2);
            if (!u) r = LLVMBuildSub(B, r, splat_like(ir, a, 1), "");
            break;
        }
        case 0x07: /* SQABS, SQNEG: INT_MIN saturates to INT_MAX with QC */
        case 0x27: {
            V min = LLVMBuildICmp(B, LLVMIntEQ, a, splat_like(ir, a, int_min(eb)), ""),
              negated = LLVMBuildSub(B, zero, a, "");
            r = u ? negated : LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, a, zero, ""), negated, a, "");
            r = LLVMBuildSelect(B, min, splat_like(ir, a, int_max(eb)), r, "");
            set_qc(ir, min);
            break;
        }
        case 0x08: r = mask_of(ir, LLVMBuildICmp(B, LLVMIntSGT, a, zero, ""), t); break;
        case 0x28: r = mask_of(ir, LLVMBuildICmp(B, LLVMIntSGE, a, zero, ""), t); break;
        case 0x09: r = mask_of(ir, LLVMBuildICmp(B, LLVMIntEQ, a, zero, ""), t); break;
        case 0x29: r = mask_of(ir, LLVMBuildICmp(B, LLVMIntSLE, a, zero, ""), t); break;
        case 0x0A: r = mask_of(ir, LLVMBuildICmp(B, LLVMIntSLT, a, zero, ""), t); break;
        case 0x0B:
            r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, a, zero, ""), LLVMBuildSub(B, zero, a, ""), a, "");
            break;
        case 0x2B: r = LLVMBuildSub(B, zero, a, ""); break;
        default: return 0;
    }
    write_lanes(ir, rd, r, q);
    return 1;
}

/* across lanes: simd_across's ADDV, [SU]MAXV, [SU]MINV, [SU]ADDLV (the long sum fits twice the width), and FMAXV,
   FMINV, FMAXNMV, FMINNMV through the helper in its reduction order, (0,1), (2,3), then the two */
static int across(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, n;
    V a, r;
    if (op == 0x0C || op == 0x0F) {
        unsigned mn = size >> 1, numeric = op == 0x0C;
        V p0, p1;
        if (!u || !q || (size & 1)) return 0;
        p0 = ir_fp_minmax(ir, get(ir, vreg(ir, rn), 2, 0), get(ir, vreg(ir, rn), 2, 1), 32, mn, numeric);
        p1 = ir_fp_minmax(ir, get(ir, vreg(ir, rn), 2, 2), get(ir, vreg(ir, rn), 2, 3), 32, mn, numeric);
        write_lanes(ir, rd, ir_cast(ir, ir_fp_minmax(ir, p0, p1, 32, mn, numeric), ir->t_i32, 0), 0);
        return 1;
    }
    if (size == 3 || (size == 2 && !q)) return 0;
    n = (q ? 128u : 64u) >> (3 + size);
    a = vlanes(ir, vreg(ir, rn), size, n);
    switch (op) {
        case 0x03:
            a = widen(ir, a, u);
            r = ir_intrinsic(ir, "llvm.vector.reduce.add", LLVMTypeOf(a), &a, 1);
            break;
        case 0x0A:
            r = ir_intrinsic(ir, u ? "llvm.vector.reduce.umax" : "llvm.vector.reduce.smax", LLVMTypeOf(a), &a, 1);
            break;
        case 0x1A:
            r = ir_intrinsic(ir, u ? "llvm.vector.reduce.umin" : "llvm.vector.reduce.smin", LLVMTypeOf(a), &a, 1);
            break;
        case 0x1B:
            if (u) return 0;
            r = ir_intrinsic(ir, "llvm.vector.reduce.add", LLVMTypeOf(a), &a, 1);
            break;
        default: return 0;
    }
    write_lanes(ir, rd, r, 0);
    return 1;
}

/* scalar pairwise: ADDP Dd, Vn.2D, and FADDP, FMAXP, FMINP, FMAXNMP, FMINNMP of the two lanes */
static int scalar_pairwise(SnxIr* ir, SnxU32 w) {
    unsigned u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31,
             s = size & 1 ? 3 : 2;
    FpOp o;
    if (!u) {
        if (op != 0x1B || size != 3) return 0;
        write_lanes(ir, rd, LLVMBuildAdd(B, ir_vd(ir, rn, 0), ir_vd(ir, rn, 1), ""), 0);
        return 1;
    }
    memset(&o, 0, sizeof(o));
    if (op == 0x0D && !(size >> 1))
        o.kind = FP_LANES_ADD;
    else if (op == 0x0C || op == 0x0F) {
        o.kind = FP_LANES_MINMAX;
        o.arg = size >> 1;
        o.flag = op == 0x0C;
    } else
        return 0;
    fp_lanes(ir, &o, s, 1, get(ir, vreg(ir, rn), s, 0), get(ir, vreg(ir, rn), s, 1), rd, 0);
    return 1;
}

/* by element, beyond fast_simd's FMLA/FMLS/FMUL: FMULX, and simd_elem's integer MUL, MLA, MLS, SQDMULH, SQRDMULH and
   long forms ([SU]MULL, [SU]MLAL, [SU]MLSL, SQDMULL, SQDMLAL, SQDMLSL), vector and scalar as simd_elem allows */
static int by_element(SnxIr* ir, SnxU32 w, unsigned scalar) {
    /* the by-element long opcodes as three-different numbers them (long_op) */
    static const unsigned char long_ops[16] = {0, 0, 8, 9, 0, 0, 10, 11, 0, 0, 12, 13, 0, 0, 0, 0};
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, l = (w >> 21) & 1,
             mb = (w >> 20) & 1, rm = (w >> 16) & 15, op = (w >> 12) & 15, h = (w >> 11) & 1, rn = (w >> 5) & 31,
             rd = w & 31, idx, m, n, eb = 8u << size;
    V a, b, z = NULL, r;
    if (u ? op == 9 : (op == 1 || op == 5 || op == 9)) { /* FP */
        unsigned sz = size & 1, s = sz ? 3 : 2;
        FpOp o;
        if (!u || size < 2 || (sz && l) || (sz && !q && !scalar)) return 0; /* (fast_simd has FMLA/FMLS/FMUL) */
        n = scalar ? 1 : (q ? 16u : 8u) >> s;
        memset(&o, 0, sizeof(o));
        o.kind = FP_LANES_MULX;
        b = get(ir, vreg(ir, (mb << 4) | rm), s, sz ? h : (h << 1) | l);
        fp_lanes(ir, &o, s, n, get_lanes(ir, vreg(ir, rn), s, n), broadcast(ir, b, n), rd, q);
        return 1;
    }
    if (size != 1 && size != 2) return 0;
    if (size == 1) {
        idx = (h << 2) | (l << 1) | mb;
        m = rm;
    } else {
        idx = (h << 1) | l;
        m = (mb << 4) | rm;
    }
    b = get(ir, vreg(ir, m), size, idx);
    if (op == 2 || op == 3 || op == 6 || op == 7 || op == 10 || op == 11) {
        unsigned saturating_op = op == 3 || op == 7 || op == 11;
        if ((u && saturating_op) || (scalar && !saturating_op)) return 0;
        n = scalar ? 1 : 64 / eb;
        a = scalar ? vlanes(ir, vreg(ir, rn), size, 1) : half_lanes(ir, rn, size, q);
        if (op != 10 && op != 11) z = vlanes(ir, vreg(ir, rd), size + 1, n);
        r = long_op(ir, long_ops[op], u, a, vsplat(ir, b, n), z);
        if (scalar)
            write_lanes(ir, rd, r, 0);
        else
            set_lanes(ir, vreg(ir, rd), r);
        return 1;
    }
    if (u ? op != 0 && op != 4 : op != 8 && op != 12 && op != 13) return 0;
    if (scalar && op < 12) return 0;
    n = scalar ? 1 : (q ? 128u : 64u) / eb;
    a = vlanes(ir, vreg(ir, rn), size, n);
    b = vsplat(ir, b, n);
    if (op >= 12)
        r = doubling_high(ir, a, b, op == 13);
    else {
        r = LLVMBuildMul(B, a, b, "");
        if (op != 8) {
            z = vlanes(ir, vreg(ir, rd), size, n);
            r = op == 0 ? LLVMBuildAdd(B, z, r, "") : LLVMBuildSub(B, z, r, "");
        }
    }
    write_lanes(ir, rd, r, q);
    return 1;
}

/* three-same integer (scalar): simd_3same's SQADD, UQADD, SQSUB, UQSUB, SQSHL, UQSHL, SQRSHL, UQRSHL, SQDMULH and
   SQRDMULH, and on D registers ADD, SUB, the compares and SSHL/USHL/SRSHL/URSHL */
static int scalar_three_same(SnxIr* ir, SnxU32 w) {
    unsigned u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 11) & 31, rn = (w >> 5) & 31,
             rd = w & 31;
    V a, b, r;
    LLVMTypeRef t;
    switch (op) {
        case 0x01:
        case 0x05:
        case 0x09:
        case 0x0B: break;
        case 0x16:
            if (size != 1 && size != 2) return 0;
            break;
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x0A:
        case 0x10:
        case 0x11:
            if (size != 3) return 0;
            break;
        default: return 0;
    }
    a = vlanes(ir, vreg(ir, rn), size, 1);
    b = vlanes(ir, vreg(ir, rm), size, 1);
    t = LLVMTypeOf(a);
    switch (op) {
        case 0x01:
        case 0x05: r = saturating(ir, u, op == 0x05, 8u << size, 1, a, b); break;
        case 0x16: r = doubling_high(ir, a, b, u); break;
        case 0x06: r = mask_of(ir, LLVMBuildICmp(B, u ? LLVMIntUGT : LLVMIntSGT, a, b, ""), t); break;
        case 0x07: r = mask_of(ir, LLVMBuildICmp(B, u ? LLVMIntUGE : LLVMIntSGE, a, b, ""), t); break;
        case 0x10: r = u ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, ""); break;
        case 0x11:
            r = u ? mask_of(ir, LLVMBuildICmp(B, LLVMIntEQ, a, b, ""), t)
                  : mask_of(ir, LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, a, b, ""), LLVMConstNull(t), ""), t);
            break;
        default: r = register_shift(ir, a, b, u, op == 0x0A || op == 0x0B, op == 0x09 || op == 0x0B); break;
    }
    write_lanes(ir, rd, r, 0);
    return 1;
}

/* in snx_simd's order (cpu_helpers.c), each group's emitters in turn; 0 leaves the word to snx_simd */
int snx_ir_more_simd(SnxIr* ir, SnxU32 w) {
    if ((w & 0xBF800000UL) == 0x0C000000UL || (w & 0xBF800000UL) == 0x0C800000UL)
        return ld1_multiple(ir, w) || ld_structures(ir, w);
    if ((w & 0xBF000000UL) == 0x0D000000UL) return ld1_single(ir, w);
    if ((w & 0x9F200400UL) == 0x0E200400UL) return three_same(ir, w) || fp_three_same(ir, w, 0);
    if ((w & 0xDF200400UL) == 0x5E200400UL) return scalar_three_same(ir, w) || fp_three_same(ir, w, 1);
    if ((w & 0xBFE0FC00UL) == 0x0EE0E000UL) return pmull_1q(ir, w);
    if ((w & 0x9F200C00UL) == 0x0E200000UL) return three_different(ir, w);
    if ((w & 0xDF200C00UL) == 0x5E200000UL) return scalar_three_different(ir, w);
    if ((w & 0x9F3E0C00UL) == 0x0E200800UL)
        return byte_misc(ir, w) || fp_sign_misc(ir, w) || fp_misc(ir, w, 0) || fp_narrow(ir, w, 0) ||
               int_misc(ir, w, 0);
    if ((w & 0xDF3E0C00UL) == 0x5E200800UL) return fp_misc(ir, w, 1) || fp_narrow(ir, w, 1) || int_misc(ir, w, 1);
    if ((w & 0x9F3E0C00UL) == 0x0E300800UL) return across(ir, w);
    if ((w & 0xDF3E0C00UL) == 0x5E300800UL) return scalar_pairwise(ir, w);
    if ((w & 0x9FE08400UL) == 0x0E000400UL) return smov(ir, w);
    if ((w & 0xFFE0FC00UL) == 0x5E000400UL) return scalar_dup(ir, w);
    if ((w & 0x9FF80400UL) == 0x0F000400UL) return modified_immediate(ir, w);
    if ((w & 0x9F800400UL) == 0x0F000400UL) return fp_fixed(ir, w, 0) || shift_immediate(ir, w, 0);
    if ((w & 0xDF800400UL) == 0x5F000400UL) return fp_fixed(ir, w, 1) || shift_immediate(ir, w, 1);
    if ((w & 0x9F000400UL) == 0x0F000000UL) return by_element(ir, w, 0);
    if ((w & 0xDF000400UL) == 0x5F000000UL) return by_element(ir, w, 1);
    if ((w & 0xBF208C00UL) == 0x0E000800UL) return permute(ir, w);
    if ((w & 0xBFE08400UL) == 0x2E000000UL) return extract(ir, w);
    if ((w & 0xBFE08C00UL) == 0x0E000000UL) return table_lookup(ir, w);
    if ((w & 0xFFFF0C00UL) == 0x4E280800UL) return aes(ir, w);
    if ((w & 0xFFE08C00UL) == 0x5E000000UL) return sha(ir, w, (w >> 12) & 7);
    if ((w & 0xFFFF0C00UL) == 0x5E280800UL) return ((w >> 12) & 31) < 3 && sha(ir, w, 0x10 + ((w >> 12) & 31));
    return 0;
}
