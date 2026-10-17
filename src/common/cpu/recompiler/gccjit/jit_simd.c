/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SIMD: snx_a64_fast_simd's shortcuts, the encodings of snx_simd that ir_simd.c emits inline (with snx_simd's
   semantics), and snx_simd itself for the rest. Written a lane at a time, as the templates' C is: every source lane is
   read before the first result is written (Vd may be Vn), and GCC's vectorizer turns the lanes into the host's
   vector instructions. ir_simd.c's x86-only forms (AES-NI, PCLMULQDQ, GFNI, VPTERNLOG) have no counterpart here: their
   encodings take the generic lanes (PMULL 1Q its portable product, AES its table lookups). */
#include "common/cpu/recompiler/gccjit/jit.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/aes_tables.h"
#include <string.h>

typedef gcc_jit_rvalue* V;
typedef gcc_jit_type* T;

#define ADD(a, b) jit_op(j, GCC_JIT_BINARY_OP_PLUS, a, b)
#define SUB(a, b) jit_op(j, GCC_JIT_BINARY_OP_MINUS, a, b)
#define MUL(a, b) jit_op(j, GCC_JIT_BINARY_OP_MULT, a, b)
#define AND(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_AND, a, b)
#define OR(a, b)  jit_op(j, GCC_JIT_BINARY_OP_BITWISE_OR, a, b)
#define XOR(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_XOR, a, b)
#define SHL(a, b) jit_op(j, GCC_JIT_BINARY_OP_LSHIFT, a, b)
#define SHR(a, b) jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, a, b)
#define NOT(a)    jit_unary(j, GCC_JIT_UNARY_OP_BITWISE_NEGATE, a)
#define EQ(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_EQ, a, b)
#define NE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_NE, a, b)
#define LT(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_LT, a, b)
#define GT(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_GT, a, b)
#define GE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_GE, a, b)
#define K32(v)    jit_u32(j, v)
#define K64(v)    jit_u64(j, v)

/* lanes of 1<<size bytes */
static T lane_type(SnxJit* j, unsigned size) { return jit_uint(j, 8u << size); }
static V klane(SnxJit* j, unsigned size, SnxU64 v) { return jit_k(j, lane_type(j, size), v); }
static void read_lanes(SnxJit* j, unsigned n, unsigned size, unsigned count, V* out) {
    unsigned i;
    for (i = 0; i < count; ++i) out[i] = jit_ve(j, n, size, i);
}
static void write_lanes(SnxJit* j, unsigned n, unsigned size, unsigned count, V const* values) {
    unsigned i;
    for (i = 0; i < count; ++i) jit_set_ve(j, n, size, i, values[i]);
}
static void zero_high(SnxJit* j, unsigned n) { jit_set_vd(j, n, 1, K64(0)); }
/* the lanes of Vd, the upper half cleared when Q is 0 (simd_perm, EXT, ... write r from zero) */
static void write_q(SnxJit* j, unsigned rd, unsigned size, unsigned count, V const* values, unsigned q) {
    write_lanes(j, rd, size, count, values);
    if (!q) zero_high(j, rd);
}
/* (uN)((iN)x >> s), x a lane of eb bits */
static V asr(SnxJit* j, V x, unsigned eb, unsigned s) {
    T i = jit_sint(j, eb);
    return jit_cast(j, jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, jit_cast(j, x, i), jit_k(j, i, s)), jit_uint(j, eb));
}
static V as_signed(SnxJit* j, V x, unsigned eb) { return jit_cast(j, x, jit_sint(j, eb)); }
/* all ones or zero, as the compares write a lane */
static V mask_of(SnxJit* j, V condition, unsigned size) {
    return jit_sel(j, condition, klane(j, size, ~(SnxU64)0), klane(j, size, 0));
}
static V negate(SnxJit* j, V x, unsigned sz, int on) {
    return on ? XOR(x, sz ? K64((SnxU64)1 << 63) : K32(0x80000000UL)) : x;
}

/* ---- snx_a64_fast_simd ---- */

/* FMLA/FMLS/FMUL by element: r starts as Vd for the vector FMLA/FMLS, from zero for FMUL and the scalar forms, so
   only the vector FMLA/FMLS with Q keeps anything of Vd beyond the written lanes */
static void by_element_store(SnxJit* j, unsigned rd, unsigned s, unsigned lanes, V const* values, unsigned scalar,
                             unsigned q) {
    write_lanes(j, rd, s, lanes, values);
    if (scalar) {
        if (s == 2) jit_st(j, jit_off_v(rd) + 4, j->t_u32, K32(0));
        zero_high(j, rd);
    } else if (!q)
        zero_high(j, rd);
}

int snx_jit_fast_simd(SnxJit* j, SnxU32 w) {
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
                a0 = jit_vd(j, rn, 0);
                a1 = q ? jit_vd(j, rn, 1) : K64(0);
                jit_set_vd(j, rd, 0, a0);
                jit_set_vd(j, rd, 1, a1);
                return 1;
            }
            if (u && size) return 0;
            a0 = jit_vd(j, rn, 0);
            a1 = jit_vd(j, rn, 1);
            b0 = jit_vd(j, rm, 0);
            b1 = jit_vd(j, rm, 1);
            if (!u && (size & 1)) { /* &~, |~ */
                b0 = NOT(b0);
                b1 = NOT(b1);
            }
            if (u) {
                a0 = XOR(a0, b0);
                a1 = XOR(a1, b1);
            } else if (size < 2) {
                a0 = AND(a0, b0);
                a1 = AND(a1, b1);
            } else {
                a0 = OR(a0, b0);
                a1 = OR(a1, b1);
            }
            jit_set_vd(j, rd, 0, a0);
            jit_set_vd(j, rd, 1, q ? a1 : K64(0));
            return 1;
        }
        if (op >= 0x18) {
            int operation = -1, acc = 0, negated = 0;
            V a[4], b[4], r[4], na[4];
            unsigned s, width;
            gcc_jit_block *slow, *join;
            SnxJitFpFast f;
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
            s = sz ? 3 : 2;
            width = sz ? 64 : 32;
            read_lanes(j, rn, s, lanes, a);
            read_lanes(j, rm, s, lanes, b);
            if (acc) read_lanes(j, rd, s, lanes, r);
            for (i = 0; i < lanes; ++i) na[i] = negated ? jit_val(j, negate(j, a[i], sz, 1)) : a[i];
            /* every lane at once when every lane is exact (jit_fp_fast) */
            if (acc)
                jit_fp_fast(j, SNX_JIT_FP_FMA, width, lanes, na, b, r, &f);
            else
                jit_fp_fast(j, (unsigned)operation, width, lanes, a, b, NULL, &f);
            join = jit_fp_branch(j, f.ok, &slow);
            write_q(j, rd, s, lanes, f.value, q);
            jit_fp_else(j, join, slow, f.inexact);
            for (i = 0; i < lanes; ++i) {
                V value = acc ? jit_fp_fma(j, r[i], na[i], b[i], width)
                              : jit_fp_binary(j, a[i], b[i], width, (unsigned)operation);
                jit_set_ve(j, rd, s, i, value);
            }
            if (!q) zero_high(j, rd);
            jit_fp_join(j, join);
            return 1;
        }
        if (op == 0x10 && (q || size < 3)) { /* ADD, SUB (vector) */
            V a[16], b[16], r[16];
            lanes = (q ? 16 : 8) >> size;
            read_lanes(j, rn, size, lanes, a);
            read_lanes(j, rm, size, lanes, b);
            for (i = 0; i < lanes; ++i) r[i] = u ? SUB(a[i], b[i]) : ADD(a[i], b[i]);
            write_q(j, rd, size, lanes, r, q);
            return 1;
        }
        return 0;
    }
    if ((w & 0x9F000400UL) == 0x0F000000UL || (w & 0xDF000400UL) == 0x5F000000UL) { /* FMLA/FMLS/FMUL by element */
        V a[4], na[4], acc[4], e[4];
        unsigned s, width;
        gcc_jit_block *slow, *join;
        SnxJitFpFast f;
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
        width = sz ? 64 : 32;
        read_lanes(j, rn, s, lanes, a);
        if (op != 9) read_lanes(j, rd, s, lanes, acc); /* scalar: d0 */
        e[0] = jit_ve(j, m, s, index);
        for (i = 0; i < lanes; ++i) {
            e[i] = e[0];
            na[i] = op == 5 ? jit_val(j, negate(j, a[i], sz, 1)) : a[i];
        }
        if (op == 9)
            jit_fp_fast(j, SNX_JIT_FP_MUL, width, lanes, a, e, NULL, &f);
        else
            jit_fp_fast(j, SNX_JIT_FP_FMA, width, lanes, na, e, acc, &f);
        join = jit_fp_branch(j, f.ok, &slow);
        by_element_store(j, rd, s, lanes, f.value, scalar, q);
        jit_fp_else(j, join, slow, f.inexact);
        for (i = 0; i < lanes; ++i)
            f.value[i] = op == 9 ? jit_fp_binary(j, a[i], e[i], width, 2) : jit_fp_fma(j, acc[i], na[i], e[i], width);
        by_element_store(j, rd, s, lanes, f.value, scalar, q);
        jit_fp_join(j, join);
        return 1;
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
            if (rd != 31) jit_set_x(j, rd, jit_ve(j, rn, lane, index));
            return 1;
        }
        if (!u && imm4 == 3 && q) {
            jit_set_ve(j, rd, lane, index, jit_rx(j, rn));
            return 1;
        }
        if (u && q) {
            jit_set_ve(j, rd, lane, index, jit_ve(j, rn, lane, imm4 >> sz));
            return 1;
        }
        if (!u && (imm4 == 0 || imm4 == 1) && !(sz == 3 && !q)) {
            V e = !imm4 ? jit_ve(j, rn, lane, index) : jit_val(j, jit_cast(j, jit_rx(j, rn), lane_type(j, lane))),
              r[16];
            for (i = 0; i < lanes; ++i) r[i] = e;
            write_q(j, rd, lane, lanes, r, q);
            return 1;
        }
        return 0;
    }
    if ((w & 0xFFFFFFE0UL) == 0x2F00E400UL || (w & 0xFFFFFFE0UL) == 0x6F00E400UL) { /* MOVI Vd, #0 */
        jit_set_vd(j, rd, 0, K64(0));
        jit_set_vd(j, rd, 1, K64(0));
        return 1;
    }
    return 0;
}

/* SIMD_LDST, SIMD_LDST1, SIMD_DP, SIMD_DP2: if(!snx_simd(c,M,w)) c->rt->undef(c,pc,w); */
int snx_jit_simd_plain(SnxJit* j, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index) {
    gcc_jit_type* p[3];
    V args[3], handled;
    gcc_jit_block *undefined, *done;
    (void)index;
    if (strcmp(pattern->name, "SIMD_LDST") && strcmp(pattern->name, "SIMD_LDST1") && strcmp(pattern->name, "SIMD_DP") &&
        strcmp(pattern->name, "SIMD_DP2"))
        return 0;
    p[0] = j->t_ptr;
    p[1] = j->t_ptr;
    p[2] = j->t_u32;
    args[0] = j->c;
    args[1] = j->M;
    args[2] = K32(word);
    handled = jit_call(j, jit_extern(j, "snx_simd", j->t_int, 3, p), 3, args);
    undefined = jit_block(j);
    done = jit_block(j);
    jit_branch(j, jit_expect(j, EQ(handled, jit_int(j, 0)), 0), undefined, done);
    jit_enter(j, undefined);
    jit_undef(j, pc, word);
    jit_goto(j, done);
    jit_enter(j, done);
    return 1;
}

/* ---- beyond snx_a64_fast_simd: encodings snx_simd handles, emitted inline with its semantics ---- */

/* LD1/ST1 {Vt..Vt+n-1}: one copy per register, as 8-byte guest accesses. Post-index adds the size, or Xm */
static int ld1_multiple(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, op = (w >> 12) & 15, regs, bytes = q ? 16 : 8, r, k;
    V address;
    regs = op == 7 ? 1 : op == 10 ? 2 : op == 6 ? 3 : op == 2 ? 4 : 0;
    if (!regs) return 0;
    address = jit_x(j, rn);
    for (r = 0; r < regs; ++r)
        for (k = 0; k < bytes / 8; ++k) {
            V a = ADD(address, K64((SnxU64)r * bytes + 8 * k));
            if (l)
                jit_set_vd(j, (rt + r) & 31, k, jit_rd(j, a, 64));
            else
                jit_wr(j, a, 64, jit_vd(j, (rt + r) & 31, k));
        }
    if (l && !q)
        for (r = 0; r < regs; ++r) zero_high(j, (rt + r) & 31);
    if (post) jit_set_x(j, rn, ADD(address, rm == 31 ? K64((SnxU64)regs * bytes) : jit_x(j, rm)));
    return 1;
}

/* LD1R-LD4R and LD1-4/ST1-4 of one lane: sel consecutive elements, one per register (Vt..Vt+sel-1, wrapping),
   accessed one at a time in simd_ldst's order */
static int ld1_single(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, r = (w >> 21) & 1, op = (w >> 13) & 7, s = (w >> 12) & 1, size = (w >> 10) & 3;
    unsigned sc = op >> 1, sel = (((op & 1) << 1) | r) + 1, esz, idx, i, n, bytes, k;
    V address;
    address = jit_x(j, rn);
    if (sc == 3) { /* LDnR: each element in every lane of its register, the rest zero */
        V value, lanes[16];
        if (!l || s) return 0;
        n = (q ? 128u : 64u) >> (3 + size);
        for (k = 0; k < sel; ++k) {
            value = jit_val(j, jit_rd(j, ADD(address, K64((SnxU64)k << size)), 8u << size));
            for (i = 0; i < n; ++i) lanes[i] = value;
            write_q(j, (rt + k) & 31, size, n, lanes, q);
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
            V a = ADD(address, K64((SnxU64)k << esz));
            if (l)
                jit_set_ve(j, (rt + k) & 31, esz, idx, jit_rd(j, a, 8u << esz));
            else
                jit_wr(j, a, 8u << esz, jit_ve(j, (rt + k) & 31, esz, idx));
        }
        bytes = sel << esz;
    }
    if (post) jit_set_x(j, rn, ADD(address, rm == 31 ? K64(bytes) : jit_x(j, rm)));
    return 1;
}

/* UZP1/2, TRN1/2, ZIP1/2: simd_perm's element formulas */
static int permute(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 7, rn = (w >> 5) & 31,
             rd = w & 31, n, h, part, i;
    V ab[32], r[16];
    if ((size == 3 && !q) || !(op & 3)) return 0;
    n = (q ? 128u : 64u) >> (3 + size);
    h = n / 2;
    part = (op >> 2) & 1;
    read_lanes(j, rn, size, n, ab);
    read_lanes(j, rm, size, n, ab + n);
    for (i = 0; i < n; ++i) {
        unsigned k;
        if ((op & 3) == 1) /* UZP: k < n ? a[k] : b[k-n] */
            k = 2 * i + part;
        else if ((op & 3) == 2) /* TRN */
            k = (i & 1) ? n + i / 2 * 2 + part : i / 2 * 2 + part;
        else /* ZIP */
            k = (i & 1) ? n + part * h + i / 2 : part * h + i / 2;
        r[i] = ab[k];
    }
    write_q(j, rd, size, n, r, q);
    return 1;
}

/* EXT: bytes imm4.. of Vn:Vm */
static int extract(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, imm4 = (w >> 11) & 15, rn = (w >> 5) & 31, rd = w & 31,
             len = q ? 16 : 8, i;
    V a[16], b[16], r[16];
    if (!q && (imm4 & 8)) return 0;
    read_lanes(j, rn, 0, len, a);
    read_lanes(j, rm, 0, len, b);
    for (i = 0; i < len; ++i) {
        unsigned source = imm4 + i;
        r[i] = source < len ? a[source] : b[source - len];
    }
    write_q(j, rd, 0, len, r, q);
    return 1;
}

static SnxU64 bytes_of(unsigned b) { return 0x0101010101010101ULL * (b & 0xFF); }
static V swap_bits(SnxJit* j, V x, unsigned shift, SnxU64 mask) {
    V m = K64(mask), s = K64(shift);
    return OR(AND(SHR(x, s), m), SHL(AND(x, m), s));
}

/* CNT, NOT, RBIT (byte lanes), on the 64-bit halves: a population count and a bit reversal per byte without crossing
   bytes (SWAR), which GCC's vectorizer has nothing to add to */
static int byte_misc(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, k;
    V x[2];
    if (op != 5 || !(u ? size < 2 : size == 0)) return 0;
    for (k = 0; k <= q; ++k) x[k] = jit_vd(j, rn, k);
    for (k = 0; k <= q; ++k) {
        V v = x[k];
        if (!u) { /* popcount per byte */
            v = jit_val(j, SUB(v, AND(SHR(v, K64(1)), K64(bytes_of(0x55)))));
            v = jit_val(j, ADD(AND(v, K64(bytes_of(0x33))), AND(SHR(v, K64(2)), K64(bytes_of(0x33)))));
            v = AND(ADD(v, SHR(v, K64(4))), K64(bytes_of(0x0F)));
        } else if (!size)
            v = NOT(v);
        else { /* bit reversal within each byte */
            v = jit_val(j, swap_bits(j, v, 1, bytes_of(0x55)));
            v = jit_val(j, swap_bits(j, v, 2, bytes_of(0x33)));
            v = swap_bits(j, v, 4, bytes_of(0x0F));
        }
        jit_set_vd(j, rd, k, v);
    }
    if (!q) zero_high(j, rd);
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
static int modified_immediate(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, op = (w >> 29) & 1, cmode = (w >> 12) & 15, rd = w & 31, o2 = (w >> 11) & 1;
    unsigned imm8 = (((w >> 16) & 7) << 5) | ((w >> 5) & 31);
    SnxU64 imm;
    if (o2 || (cmode == 15 && op && !q)) return 0;
    imm = expand_imm(op, cmode, imm8);
    if (cmode < 12 && (cmode & 1)) { /* ORR, BIC */
        V k = K64(op ? ~imm : imm), lo = jit_vd(j, rd, 0), hi = jit_vd(j, rd, 1);
        jit_set_vd(j, rd, 0, op ? AND(lo, k) : OR(lo, k));
        jit_set_vd(j, rd, 1, q ? (op ? AND(hi, k) : OR(hi, k)) : K64(0));
        return 1;
    }
    if (op && cmode != 14 && cmode != 15) imm = ~imm; /* MVNI */
    jit_set_vd(j, rd, 0, K64(imm));
    jit_set_vd(j, rd, 1, K64(q ? imm : 0));
    return 1;
}

/* FPSR.QC (bit 27) |= any saturated lane */
static void set_qc(SnxJit* j, V any) {
    V fpsr = jit_ld(j, offsetof(CPU, fpsr), j->t_u32);
    jit_st(j, offsetof(CPU, fpsr), j->t_u32,
           OR(fpsr, jit_op(j, GCC_JIT_BINARY_OP_LSHIFT, jit_cast(j, any, j->t_u32), K32(27))));
}

/* SQADD, UQADD, SQSUB, UQSUB (vector): the exact result in twice the lane's width (128 bits for 64-bit lanes),
   clamped to the lane's range; QC when a lane saturates (sqadd_ and friends) */
static V saturating(SnxJit* j, unsigned u, unsigned sub, unsigned eb, V a, V b, V* saturated) {
    unsigned wb = eb == 64 ? 128 : 2 * eb;
    T wide = u ? jit_uint(j, wb) : jit_sint(j, wb), lane = jit_uint(j, eb);
    V x = jit_cast(j, u ? a : as_signed(j, a, eb), wide), y = jit_cast(j, u ? b : as_signed(j, b, eb), wide), r, lo, hi;
    if (u) {
        lo = jit_k(j, wide, 0);
        hi = jit_k(j, wide, eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1); /* jit_k zero-extends to 128 bits */
        if (sub) {                                                          /* a < b: 0 */
            *saturated = LT(x, y);
            return jit_val(j, jit_sel(j, *saturated, jit_k(j, lane, 0), jit_cast(j, SUB(x, y), lane)));
        }
        r = jit_val(j, ADD(x, y));
        *saturated = GT(r, hi);
        return jit_val(j, jit_cast(j, jit_sel(j, *saturated, hi, r), lane));
    }
    hi = jit_cast(j, jit_k(j, jit_sint(j, eb), ((SnxU64)1 << (eb - 1)) - 1), wide);
    lo = jit_cast(j, jit_k(j, jit_sint(j, eb), (SnxU64)1 << (eb - 1)), wide);
    r = jit_val(j, sub ? SUB(x, y) : ADD(x, y));
    *saturated = jit_val(j, jit_lor(j, GT(r, hi), LT(r, lo)));
    return jit_val(j, jit_cast(j, jit_sel(j, GT(r, hi), hi, jit_sel(j, LT(r, lo), lo, r)), lane));
}

/* SHADD, UHADD, SRHADD, URHADD: (a + b [+ 1]) >> 1 in twice the lane's width */
static V halving(SnxJit* j, unsigned u, unsigned rounding, unsigned eb, V a, V b) {
    T wide = u ? jit_uint(j, 2 * eb) : jit_sint(j, 2 * eb);
    V r = ADD(jit_cast(j, u ? a : as_signed(j, a, eb), wide), jit_cast(j, u ? b : as_signed(j, b, eb), wide));
    if (rounding) r = ADD(r, jit_k(j, wide, 1));
    return jit_cast(j, SHR(r, jit_k(j, wide, 1)), jit_uint(j, eb));
}

/* a > b (or >=, <), signed or unsigned lanes */
static V compare(SnxJit* j, enum gcc_jit_comparison op, unsigned u, unsigned eb, V a, V b) {
    return u ? jit_cmp(j, op, a, b) : jit_cmp(j, op, as_signed(j, a, eb), as_signed(j, b, eb));
}

/* ---- integer lanes (ir_simd.c's vector forms, a lane at a time): the helpers' sx/zx arithmetic in a wider type
   where it needs the room, unsigned wherever it may wrap ---- */

static SnxU64 int_min(unsigned eb) { return (SnxU64)1 << (eb - 1); }
static SnxU64 int_max(unsigned eb) { return int_min(eb) - 1; }
static SnxU64 uint_max(unsigned eb) { return eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1; }
/* x, a lane of eb bits, zero- or sign-extended (or truncated) to an unsigned of bits */
static V ext(SnxJit* j, V x, unsigned eb, unsigned u, unsigned bits) {
    V v = u ? jit_cast(j, x, jit_uint(j, eb)) : jit_cast(j, as_signed(j, x, eb), jit_sint(j, bits));
    return jit_cast(j, v, jit_uint(j, bits));
}
/* QC's condition so far, or'ed with c */
static V any_of(SnxJit* j, V so_far, V c) { return so_far ? jit_lor(j, so_far, c) : c; }
/* a scalar result, zero-extended: the rest of Vd is zero */
static void write_scalar(SnxJit* j, unsigned rd, V value) {
    jit_set_vd(j, rd, 0, jit_val(j, jit_cast(j, value, j->t_u64)));
    zero_high(j, rd);
}
/* the narrowing forms' m lanes: Vd's low half with the upper half zero, or (the "2" forms) its upper half with the
   low half kept (simd_2misc's and simd_shimm's o = Q ? d : r) */
static void write_narrow(SnxJit* j, unsigned rd, unsigned size, unsigned m, V const* values, unsigned upper) {
    unsigned i;
    if (!upper)
        write_q(j, rd, size, m, values, 0);
    else
        for (i = 0; i < m; ++i) jit_set_ve(j, rd, size, m + i, values[i]);
}
/* x, a lane of 2*eb bits, clamped to eb bits: signed to signed (ssat), signed to unsigned (usat) or unsigned to
   unsigned (UQXTN); *saturated or'ed with whether it changed */
static V saturate_lane(SnxJit* j, V x, unsigned eb, unsigned from_signed, unsigned to_signed, V* saturated) {
    unsigned wb = 2 * eb;
    V over, r;
    if (from_signed) {
        T s = jit_sint(j, wb);
        V v = jit_val(j, as_signed(j, x, wb)), hi = jit_k(j, s, to_signed ? int_max(eb) : uint_max(eb)), under;
        /* INT_MIN cast up, so it sign-extends into 128 bits too (jit_k zero-extends a 128-bit constant) */
        V lo = to_signed ? jit_cast(j, jit_k(j, jit_sint(j, eb), int_min(eb)), s) : jit_k(j, s, 0);
        over = jit_val(j, GT(v, hi));
        under = jit_val(j, LT(v, lo));
        r = jit_sel(j, over, hi, jit_sel(j, under, lo, v));
        over = jit_lor(j, over, under);
    } else {
        V v = jit_val(j, jit_cast(j, x, jit_uint(j, wb))), hi = jit_k(j, jit_uint(j, wb), uint_max(eb));
        over = jit_val(j, GT(v, hi));
        r = jit_sel(j, over, hi, v);
    }
    *saturated = any_of(j, *saturated, over);
    return jit_val(j, jit_cast(j, r, jit_uint(j, eb)));
}
/* a == b == INT_MIN, the one product that overflows the doubling multiplies */
static V both_min(SnxJit* j, V a, V b, unsigned eb) {
    V min = jit_k(j, jit_uint(j, eb), int_min(eb));
    return jit_val(j, jit_land(j, EQ(jit_cast(j, a, jit_uint(j, eb)), min), EQ(jit_cast(j, b, jit_uint(j, eb)), min)));
}
/* SQDMULL and the doubling in SQDMLAL/SQDMLSL: 2*a*b in twice the width, INT_MIN * INT_MIN saturating */
static V doubling_long(SnxJit* j, V a, V b, unsigned eb, V* saturated) {
    T wide = jit_uint(j, 2 * eb);
    V p = SHL(MUL(ext(j, a, eb, 0, 2 * eb), ext(j, b, eb, 0, 2 * eb)), jit_k(j, wide, 1)), both = both_min(j, a, b, eb);
    *saturated = any_of(j, *saturated, both);
    return jit_val(j, jit_sel(j, both, jit_k(j, wide, int_max(2 * eb)), p));
}
/* SQDMULH, SQRDMULH: the high half of 2*a*b (plus 1<<(eb-1) rounding) in twice the width, INT_MIN * INT_MIN
   saturating */
static V doubling_high(SnxJit* j, V a, V b, unsigned eb, unsigned round, V* saturated) {
    unsigned wb = 2 * eb;
    T wide = jit_uint(j, wb);
    V p = SHL(MUL(ext(j, a, eb, 0, wb), ext(j, b, eb, 0, wb)), jit_k(j, wide, 1)), both = both_min(j, a, b, eb);
    if (round) p = ADD(p, jit_k(j, wide, int_min(eb)));
    p = jit_cast(j, asr(j, p, wb, eb), jit_uint(j, eb));
    *saturated = any_of(j, *saturated, both);
    return jit_val(j, jit_sel(j, both, jit_k(j, jit_uint(j, eb), int_max(eb)), p));
}
/* PMUL, PMULL (bytes): the carry-less product of byte lanes a and b, 16 bits wide */
static V poly_multiply(SnxJit* j, V a, V b) {
    V x = jit_val(j, jit_cast(j, a, j->t_u16)), y = jit_val(j, jit_cast(j, b, j->t_u16)), p = jit_k(j, j->t_u16, 0);
    unsigned k;
    for (k = 0; k < 8; ++k)
        p = XOR(p, AND(SUB(jit_k(j, j->t_u16, 0), AND(SHR(x, jit_k(j, j->t_u16, k)), jit_k(j, j->t_u16, 1))),
                       SHL(y, jit_k(j, j->t_u16, k))));
    return jit_val(j, p);
}

/* the long forms on narrow lanes a, b by three-different's opcode (ir_simd.c's long_op): [SU]ADDL 0, [SU]SUBL 2,
   [SU]ABAL 5, [SU]ABDL 7, [SU]MLAL 8, SQDMLAL 9, [SU]MLSL 10, SQDMLSL 11, [SU]MULL 12, SQDMULL 13; z the accumulator */
static V long_lane(SnxJit* j, unsigned op, unsigned u, unsigned eb, V a, V b, V z, V* saturated) {
    unsigned wb = 2 * eb;
    V x, y, r;
    if (op == 9 || op == 11 || op == 13) {
        V added = NULL;
        r = doubling_long(j, a, b, eb, saturated);
        if (op == 13) return r;
        r = saturating(j, 0, op == 11, wb, z, r, &added);
        *saturated = any_of(j, *saturated, added);
        return r;
    }
    x = ext(j, a, eb, u, wb);
    y = ext(j, b, eb, u, wb);
    switch (op) {
        case 0: r = ADD(x, y); break;
        case 2: r = SUB(x, y); break;
        case 5:
        case 7:
            r = jit_sel(j, compare(j, GCC_JIT_COMPARISON_GT, u, eb, a, b), SUB(x, y), SUB(y, x));
            if (op == 5) r = ADD(jit_cast(j, z, jit_uint(j, wb)), r);
            break;
        case 8: r = ADD(jit_cast(j, z, jit_uint(j, wb)), MUL(x, y)); break;
        case 10: r = SUB(jit_cast(j, z, jit_uint(j, wb)), MUL(x, y)); break;
        default: r = MUL(x, y); break;
    }
    return jit_val(j, jit_cast(j, r, jit_uint(j, wb)));
}
/* the register shifts (SSHL, USHL, SRSHL, URSHL, SQSHL, UQSHL, SQRSHL, UQRSHL): vshl (helpers/simd.c) on a 64-bit copy
   of lane a, the shift the signed low byte of lane b. Amounts are clamped below 64 (C's shifts by the width are
   undefined) and the cases that clamping changes are selected away, as ir_simd.c's register_shift does */
static V shift_lane(SnxJit* j, V a, V b, unsigned eb, unsigned u, unsigned round, unsigned sat, V* saturated) {
    T s64 = j->t_s64, lane = jit_uint(j, eb);
    V x = jit_val(j, ext(j, a, eb, u, 64)), sh = jit_val(j, jit_cast(j, as_signed(j, b, 8), s64));
    V shu = jit_val(j, jit_cast(j, sh, j->t_u64)), k63 = K64(63), amount, shifted, left, right, negative;
    amount = jit_val(j, jit_sel(j, GT(shu, k63), k63, shu));
    shifted = jit_val(j, SHL(x, amount));
    if (!sat)
        left = jit_sel(j, GE(sh, jit_k(j, s64, eb)), K64(0), shifted);
    else {
        V back = u ? SHR(shifted, amount)
                   : jit_cast(j, jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, jit_cast(j, shifted, s64), amount), j->t_u64);
        V fits = jit_land(j, EQ(ext(j, shifted, eb, u, 64), shifted), EQ(back, x)), top;
        fits = jit_val(j, jit_lor(j, jit_land(j, fits, LT(sh, jit_k(j, s64, eb))), EQ(x, K64(0))));
        top = u ? K64(uint_max(eb))
                : jit_sel(j, LT(jit_cast(j, x, s64), jit_k(j, s64, 0)), K64(int_min(eb)), K64(int_max(eb)));
        left = jit_sel(j, fits, shifted, top);
        *saturated = any_of(j, *saturated, jit_land(j, GE(sh, jit_k(j, s64, 0)), jit_lnot(j, fits)));
    }
    /* right: n = -sh, 1..128; 64 or more leaves the sign (signed) or 0 */
    negative = jit_val(j, jit_cast(j, SUB(jit_k(j, s64, 0), sh), j->t_u64));
    amount = jit_val(j, jit_sel(j, GT(negative, k63), k63, negative));
    if (u)
        right = jit_sel(j, GT(negative, k63), K64(0), SHR(x, amount));
    else
        right = jit_cast(j, jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, jit_cast(j, x, s64), amount), j->t_u64);
    if (round) { /* + bit n-1, the last shifted out; past a shift of 64 nothing is left */
        V last = jit_val(j, SUB(negative, K64(1)));
        last = jit_sel(j, GT(last, k63), k63, last);
        right = jit_sel(j, GT(negative, K64(64)), K64(0), ADD(right, AND(SHR(x, last), K64(1))));
    }
    return jit_val(j, jit_cast(j, jit_sel(j, LT(sh, jit_k(j, s64, 0)), right, left), lane));
}

/* three-same integer (vector): simd_3same for BSL/BIT/BIF, compares, MIN/MAX, ABD/ABA, MLA/MLS, MUL, pairwise
   ADDP/MAXP/MINP, and the saturating and halving adds */
static int three_same(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 11) & 31,
             rn = (w >> 5) & 31, rd = w & 31, eb = 8u << size, n, i;
    V a[16], b[16], z[16], r[16], sat = NULL;
    if (op == 3) { /* BSL, BIT, BIF (fast_simd has the others) */
        unsigned k;
        V x[2], y[2], d[2];
        if (!u || !size) return 0;
        for (k = 0; k < 2; ++k) {
            x[k] = jit_vd(j, rn, k);
            y[k] = jit_vd(j, rm, k);
            d[k] = jit_vd(j, rd, k);
        }
        for (k = 0; k < 2; ++k) {
            if (size == 1) /* BSL: (d & n) | (~d & m) */
                r[k] = OR(AND(d[k], x[k]), AND(NOT(d[k]), y[k]));
            else if (size == 2) /* BIT: (d & ~m) | (n & m) */
                r[k] = OR(AND(d[k], NOT(y[k])), AND(x[k], y[k]));
            else /* BIF: (d & m) | (n & ~m) */
                r[k] = OR(AND(d[k], y[k]), AND(x[k], NOT(y[k])));
        }
        jit_set_vd(j, rd, 0, r[0]);
        jit_set_vd(j, rd, 1, q ? r[1] : K64(0));
        return 1;
    }
    if (op >= 0x18 || (size == 3 && !q)) return 0;
    n = (q ? 128u : 64u) / eb;
    switch (op) { /* the encodings handled here, before reading anything */
        case 0x00:
        case 0x02:
        case 0x04:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
        case 0x12:
        case 0x14:
        case 0x15:
            if (eb == 64) return 0;
            break;
        case 0x13: /* MUL, PMUL (bytes) */
            if (eb == 64 || (u && size)) return 0;
            break;
        case 0x16:
            if (size != 1 && size != 2) return 0;
            break;
        case 0x17:
            if (u) return 0;
            break;
        case 0x01:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x11: break;
        default: return 0;
    }
    read_lanes(j, rn, size, n, a);
    read_lanes(j, rm, size, n, b);
    if (op == 0x0F || op == 0x12) read_lanes(j, rd, size, n, z);
    for (i = 0; i < n; ++i) {
        V x = a[i], y = b[i];
        switch (op) {
            case 0x00:
            case 0x02: r[i] = halving(j, u, op == 0x02, eb, x, y); break;
            case 0x01:
            case 0x05: {
                V lane_sat;
                r[i] = saturating(j, u, op == 0x05, eb, x, y, &lane_sat);
                sat = sat ? jit_lor(j, sat, lane_sat) : lane_sat;
                break;
            }
            case 0x04: /* SHSUB, UHSUB: the difference in twice the width, halved arithmetically */
                r[i] = asr(j, SUB(ext(j, x, eb, u, 2 * eb), ext(j, y, eb, u, 2 * eb)), 2 * eb, 1);
                break;
            case 0x08:
            case 0x09:
            case 0x0A:
            case 0x0B:
                r[i] = shift_lane(j, x, y, eb, u, op == 0x0A || op == 0x0B, op == 0x09 || op == 0x0B, &sat);
                break;
            case 0x16: r[i] = doubling_high(j, x, y, eb, u, &sat); break;
            case 0x06: r[i] = mask_of(j, compare(j, GCC_JIT_COMPARISON_GT, u, eb, x, y), size); break;
            case 0x07: r[i] = mask_of(j, compare(j, GCC_JIT_COMPARISON_GE, u, eb, x, y), size); break;
            case 0x11: r[i] = mask_of(j, u ? EQ(x, y) : NE(AND(x, y), klane(j, size, 0)), size); break;
            case 0x0C:
            case 0x0D:
                r[i] = jit_sel(j, compare(j, op == 0x0C ? GCC_JIT_COMPARISON_GT : GCC_JIT_COMPARISON_LT, u, eb, x, y),
                               x, y);
                break;
            case 0x0E:
            case 0x0F:
                r[i] = jit_sel(j, compare(j, GCC_JIT_COMPARISON_GT, u, eb, x, y), SUB(x, y), SUB(y, x));
                if (op == 0x0F) r[i] = ADD(z[i], r[i]);
                break;
            case 0x12: r[i] = u ? SUB(z[i], MUL(x, y)) : ADD(z[i], MUL(x, y)); break;
            case 0x13: r[i] = u ? poly_multiply(j, x, y) : MUL(x, y); break;
            default: { /* pairwise: lanes 0..n/2-1 from pairs of a, the rest from pairs of b */
                V* src = i < n / 2 ? a : b;
                unsigned k = 2 * (i < n / 2 ? i : i - n / 2);
                x = src[k];
                y = src[k + 1];
                if (op == 0x17)
                    r[i] = ADD(x, y);
                else
                    r[i] = jit_sel(
                        j, compare(j, op == 0x14 ? GCC_JIT_COMPARISON_GT : GCC_JIT_COMPARISON_LT, u, eb, x, y), x, y);
                break;
            }
        }
        r[i] = jit_val(j, jit_cast(j, r[i], lane_type(j, size)));
    }
    if (sat) set_qc(j, sat);
    write_q(j, rd, size, n, r, q);
    return 1;
}

/* the narrowing shifts right on the wide lanes of Vn, shr 1..eb (ir_simd.c's shift_narrow): SHRN and RSHRN wrap,
   the saturating ones clamp with QC (and have scalar forms); rounding adds the last bit shifted out */
static int shift_narrow(SnxJit* j, unsigned q, unsigned u, unsigned op, unsigned size, unsigned shr, unsigned rn,
                        unsigned rd, unsigned scalar) {
    unsigned eb = 8u << size, m = scalar ? 1 : 64 / eb, rnd = op & 1, i;
    V a[8], r[8], sat = NULL;
    if (scalar && !u && op < 0x12) return 0;
    read_lanes(j, rn, size + 1, m, a);
    for (i = 0; i < m; ++i) {
        V x = a[i], v;
        if (!u && op < 0x12) { /* SHRN, RSHRN */
            if (rnd) x = ADD(x, klane(j, size + 1, (SnxU64)1 << (shr - 1)));
            r[i] = jit_val(j, jit_cast(j, SHR(x, klane(j, size + 1, shr)), lane_type(j, size)));
            continue;
        }
        /* UQSHRN, UQRSHRN (unsigned source); SQSHRN, SQRSHRN, SQSHRUN, SQRSHRUN (signed) */
        v = u && op >= 0x12 ? SHR(x, klane(j, size + 1, shr)) : asr(j, x, 2 * eb, shr);
        if (rnd) v = ADD(v, AND(SHR(x, klane(j, size + 1, shr - 1)), klane(j, size + 1, 1)));
        r[i] = saturate_lane(j, v, eb, !(u && op >= 0x12), !u, &sat);
    }
    if (sat) set_qc(j, sat);
    if (scalar)
        write_scalar(j, rd, r[0]);
    else
        write_narrow(j, rd, size, m, r, q);
    return 1;
}

/* shift by immediate: simd_shimm's SSHR/USHR, SSRA/USRA, SRSHR/URSHR, SRSRA/URSRA, SHL, SLI, SRI, SQSHL/UQSHL,
   SQSHLU, SSHLL/USHLL and the narrowing shifts; scalar, the 64-bit forms and the saturating left shifts of any size */
static int shift_immediate(SnxJit* j, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, immh = (w >> 19) & 15, immb = (w >> 16) & 7,
             op = (w >> 11) & 31, rn = (w >> 5) & 31, rd = w & 31, size = 3, eb, imm, shr, shl, n, i;
    V a[16], z[16], r[16], sat = NULL;
    if (!immh) return 0;
    while (!((immh >> size) & 1)) size--;
    eb = 8u << size;
    imm = (immh << 3) | immb;
    shr = 2 * eb - imm;
    shl = imm - eb;
    if (op >= 0x10 && op <= 0x13) return size == 3 ? 0 : shift_narrow(j, q, u, op, size, shr, rn, rd, scalar);
    if (op == 0x14) { /* SSHLL, USHLL: the low or high half widened, shifted left */
        unsigned m;
        if (size == 3 || scalar) return 0;
        m = 64 / eb;
        for (i = 0; i < m; ++i) a[i] = jit_ve(j, rn, size, (q ? m : 0) + i);
        for (i = 0; i < m; ++i) {
            V x = u ? jit_cast(j, a[i], lane_type(j, size + 1))
                    : jit_cast(j, as_signed(j, a[i], eb), jit_sint(j, 2 * eb));
            r[i] = SHL(jit_cast(j, x, lane_type(j, size + 1)), klane(j, size + 1, shl));
        }
        write_lanes(j, rd, size + 1, m, r);
        return 1;
    }
    if (size == 3 && !q && !scalar) return 0;
    if (scalar && size != 3 && op != 0x0C && op != 0x0E) return 0;
    if (((op == 0x08 || op == 0x0C) && !u) ||
        (op != 0x00 && op != 0x02 && op != 0x04 && op != 0x06 && op != 0x08 && op != 0x0A && op != 0x0C && op != 0x0E))
        return 0;
    n = scalar ? 1 : (q ? 128u : 64u) / eb;
    read_lanes(j, rn, size, n, a);
    if (op == 0x02 || op == 0x06 || op == 0x08 || (op == 0x0A && u)) read_lanes(j, rd, size, n, z);
    for (i = 0; i < n; ++i) {
        V x = a[i], v;
        SnxU64 all = eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1, m;
        switch (op) {
            case 0x00:
            case 0x02:
            case 0x04:
            case 0x06: /* [SU][R]SHR, [SU][R]SRA: a shift by the element size gives 0 (unsigned) or the sign */
                if (u)
                    v = shr >= eb ? klane(j, size, 0) : SHR(x, klane(j, size, shr));
                else
                    v = asr(j, x, eb, shr >= eb ? eb - 1 : shr);
                if (op & 4) { /* rounding: + bit shr-1 of the source (for shr = eb, 0 signed, the top bit unsigned) */
                    V bit = AND(SHR(x, klane(j, size, shr - 1)), klane(j, size, 1));
                    v = shr >= eb && !u ? klane(j, size, 0) : ADD(v, bit);
                }
                if (op & 2) v = ADD(z[i], v);
                break;
            case 0x08: /* SRI */
                if (shr >= eb)
                    v = z[i];
                else {
                    m = all >> shr;
                    v = OR(AND(z[i], klane(j, size, ~m)), SHR(x, klane(j, size, shr)));
                }
                break;
            case 0x0C:   /* SQSHLU: a negative lane gives 0, then as UQSHL */
            case 0x0E: { /* SQSHL, UQSHL: saturated when shifting back does not give the lane, QC */
                V shifted = jit_val(j, SHL(x, klane(j, size, shl))), lost, top;
                if (!u) {
                    lost = NE(asr(j, shifted, eb, shl), x);
                    top = jit_sel(j, LT(as_signed(j, x, eb), jit_k(j, jit_sint(j, eb), 0)), klane(j, size, int_min(eb)),
                                  klane(j, size, int_max(eb)));
                } else {
                    lost = NE(SHR(shifted, klane(j, size, shl)), x);
                    top = klane(j, size, uint_max(eb));
                }
                lost = jit_val(j, lost);
                v = jit_sel(j, lost, top, shifted);
                if (op == 0x0C) {
                    V negative = jit_val(j, LT(as_signed(j, x, eb), jit_k(j, jit_sint(j, eb), 0)));
                    v = jit_sel(j, negative, klane(j, size, 0), v);
                    lost = jit_lor(j, lost, negative);
                }
                sat = any_of(j, sat, lost);
                break;
            }
            default:
                if (!u) /* SHL */
                    v = SHL(x, klane(j, size, shl));
                else { /* SLI */
                    m = (all << shl) & all;
                    v = OR(AND(z[i], klane(j, size, ~m)), AND(SHL(x, klane(j, size, shl)), klane(j, size, m)));
                }
                break;
        }
        r[i] = jit_val(j, jit_cast(j, v, lane_type(j, size)));
    }
    if (sat) set_qc(j, sat);
    if (scalar)
        write_scalar(j, rd, r[0]);
    else
        write_q(j, rd, size, n, r, q);
    return 1;
}

/* FABS, FNEG (vector): sign bits only */
static int fp_sign_misc(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, sz = size & 1;
    SnxU64 sign = sz ? (SnxU64)1 << 63 : ((SnxU64)0x80000000UL << 32) | 0x80000000UL;
    V lo, hi;
    if (op != 0x0F || !(size >> 1) || (sz && !q)) return 0;
    lo = jit_vd(j, rn, 0);
    hi = jit_vd(j, rn, 1);
    if (u) {
        lo = XOR(lo, K64(sign));
        hi = XOR(hi, K64(sign));
    } else {
        lo = AND(lo, K64(~sign));
        hi = AND(hi, K64(~sign));
    }
    jit_set_vd(j, rd, 0, lo);
    jit_set_vd(j, rd, 1, q ? hi : K64(0));
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
/* what fp_lanes computes (ir_simd.c's FpOp). arg: the minimum flag (MINMAX), the comparison (COMPARE), rsqrt (STEP,
   ESTIMATE) or snx_fp_to_int's rounding mode (TO_INT). flag: numeric (MINMAX) or unsigned (the conversions). fbits:
   the conversions' fixed point */
typedef struct {
    unsigned kind, arg, flag, fbits;
} FpOp;

/* one lane through the helper simd_3same's and simd_2misc's loops call (helpers/simd.c) */
static V fp_lane(SnxJit* j, const FpOp* op, V a, V b, unsigned width) {
    switch (op->kind) {
        case FP_LANES_ADD: return jit_fp_binary(j, a, b, width, SNX_FP_ADD);
        case FP_LANES_ABD: return jit_fp_binary(j, a, b, width, SNX_FP_ABD);
        case FP_LANES_MULX: return jit_fp_binary(j, a, b, width, SNX_FP_MULX);
        case FP_LANES_STEP: return jit_fp_binary(j, a, b, width, op->arg ? SNX_FP_RSQRTS : SNX_FP_RECPS);
        case FP_LANES_MINMAX: return jit_fp_minmax(j, a, b, width, op->arg, op->flag);
        case FP_LANES_COMPARE: return jit_fp_compare_mask(j, op->arg, a, b, width);
        case FP_LANES_SQRT: return jit_fp_unary(j, a, width, SNX_FP_SQRT);
        case FP_LANES_ESTIMATE: return jit_fp_unary(j, a, width, op->arg ? SNX_FP_RSQRTE : SNX_FP_RECPE);
        case FP_LANES_TO_INT: return jit_fp_to_int(j, a, width, width, op->flag, op->fbits, op->arg);
        default: return jit_fp_from_int(j, a, width, width, op->flag, op->fbits);
    }
}

/* the operation on n lanes of 1<<s bytes (x, and y for two operands) into Vd: inline when the fast path's guard holds
   for every lane, otherwise the helper a lane at a time (ir_simd.c's fp_lanes). One lane is a scalar form: the rest
   of Vd is zero */
static void fp_lanes(SnxJit* j, const FpOp* op, unsigned s, unsigned n, V const* x, V const* y, unsigned rd,
                     unsigned q) {
    unsigned width = 8u << s, i;
    gcc_jit_block *slow, *join = NULL;
    SnxJitFpFast f;
    V r[4];
    switch (op->kind) {
        case FP_LANES_ADD: jit_fp_fast(j, SNX_JIT_FP_ADD, width, n, x, y, NULL, &f); break;
        case FP_LANES_ABD: /* |x - y|: snx_fp_binary clears the difference's sign */
            jit_fp_fast(j, SNX_JIT_FP_SUB, width, n, x, y, NULL, &f);
            for (i = 0; i < n; ++i) f.value[i] = AND(f.value[i], klane(j, s, ((SnxU64)1 << (width - 1)) - 1));
            break;
        case FP_LANES_MULX: /* FMUL's result but for infinity times zero, which the guard keeps out */
            jit_fp_fast(j, SNX_JIT_FP_MUL, width, n, x, y, NULL, &f);
            break;
        case FP_LANES_STEP: jit_fp_step_fast(j, op->arg, width, n, x, y, &f); break;
        case FP_LANES_MINMAX: jit_fp_minmax_fast(j, width, n, x, y, op->arg, &f); break;
        case FP_LANES_COMPARE: jit_fp_compare_fast(j, op->arg, width, n, x, y, &f); break;
        case FP_LANES_SQRT: jit_fp_fast(j, SNX_JIT_FP_SQRT, width, n, x, NULL, NULL, &f); break;
        case FP_LANES_ESTIMATE: jit_fp_estimate_fast(j, op->arg, width, n, x, &f); break;
        case FP_LANES_TO_INT:
            if (!jit_fp_to_int_fast(j, width, n, x, width, op->flag, op->arg, op->fbits, &f)) f.ok = NULL;
            break;
        default: jit_fp_from_int_fast(j, width, n, x, width, op->flag, op->fbits, &f); break;
    }
    if (f.ok) { /* else no fast path: the helper alone */
        join = jit_fp_branch(j, f.ok, &slow);
        by_element_store(j, rd, s, n, f.value, n == 1, q);
        jit_fp_else(j, join, slow, f.inexact);
    }
    for (i = 0; i < n; ++i) r[i] = fp_lane(j, op, x[i], y ? y[i] : NULL, width);
    by_element_store(j, rd, s, n, r, n == 1, q);
    if (join) jit_fp_join(j, join);
}

/* FP three-same beyond fast_simd's arithmetic (ir_simd.c's fp_three_same): FADDP, FABD, FMULX, FRECPS, FRSQRTS,
   FMAX/FMIN/FMAXNM/FMINNM and their pairwise forms, FCMEQ/FCMGE/FCMGT and FACGE/FACGT; the scalar FABD, FMULX,
   FRECPS, FRSQRTS and compares */
static int fp_three_same(SnxJit* j, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, sz = (w >> 22) & 1, hi = (w >> 23) & 1,
             rm = (w >> 16) & 31, op = (w >> 11) & 31, rn = (w >> 5) & 31, rd = w & 31, s = sz ? 3 : 2,
             n = scalar ? 1
                 : sz   ? 2
                        : (q ? 4 : 2),
             i;
    unsigned pair = 0, absolute = 0;
    FpOp o;
    V a[4], b[4], x[4], y[4];
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
            o.arg = !u ? SNX_JIT_CMP_EQ : hi ? SNX_JIT_CMP_GT : SNX_JIT_CMP_GE;
            break;
        case 0x1D: /* FACGE, FACGT: the comparison of the magnitudes */
            if (!u) return 0;
            o.kind = FP_LANES_COMPARE;
            o.arg = hi ? SNX_JIT_CMP_GT : SNX_JIT_CMP_GE;
            absolute = 1;
            break;
        default: return 0;
    }
    if (scalar && o.kind != FP_LANES_ABD && o.kind != FP_LANES_MULX && o.kind != FP_LANES_STEP &&
        o.kind != FP_LANES_COMPARE)
        return 0;
    read_lanes(j, rn, s, n, a);
    read_lanes(j, rm, s, n, b);
    for (i = 0; i < n; ++i) {
        if (pair) { /* lanes 0..n/2-1 from pairs of Vn, the rest from pairs of Vm */
            const V* from = i < n / 2 ? a : b;
            unsigned k = i < n / 2 ? 2 * i : 2 * (i - n / 2);
            x[i] = from[k];
            y[i] = from[k + 1];
        } else {
            x[i] = a[i];
            y[i] = b[i];
        }
        if (absolute) {
            x[i] = jit_val(j, AND(x[i], klane(j, s, sz ? ~((SnxU64)1 << 63) : 0x7FFFFFFFUL)));
            y[i] = jit_val(j, AND(y[i], klane(j, s, sz ? ~((SnxU64)1 << 63) : 0x7FFFFFFFUL)));
        }
    }
    fp_lanes(j, &o, s, n, x, y, rd, q);
    return 1;
}

/* FP two-register misc (ir_simd.c's fp_misc): FCMGT, FCMGE, FCMEQ, FCMLE, FCMLT #0.0, FCVT[NPMZA][SU], SCVTF, UCVTF,
   FRECPE and FRSQRTE (vector and scalar) and FSQRT (vector) */
static int fp_misc(SnxJit* j, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31,
             rn = (w >> 5) & 31, rd = w & 31, sz = size & 1, s = sz ? 3 : 2,
             n = scalar ? 1
                 : sz   ? 2
                        : (q ? 4 : 2),
             i;
    unsigned k = u << 6 | (size >> 1) << 5 | op;
    FpOp o;
    V a[4], zero[4];
    if (!scalar && sz && !q) return 0;
    o.arg = 0;
    o.flag = u;
    o.fbits = 0;
    read_lanes(j, rn, s, n, a);
    if (op >= 0x0C && op <= 0x0E && (size >> 1)) {
        if (op == 0x0C)
            o.arg = u ? SNX_JIT_CMP_GE : SNX_JIT_CMP_GT;
        else if (op == 0x0D)
            o.arg = u ? SNX_JIT_CMP_LE : SNX_JIT_CMP_EQ;
        else if (!u)
            o.arg = SNX_JIT_CMP_LT;
        else
            return 0;
        o.kind = FP_LANES_COMPARE;
        for (i = 0; i < n; ++i) zero[i] = klane(j, s, 0);
        fp_lanes(j, &o, s, n, a, zero, rd, q);
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
    fp_lanes(j, &o, s, n, a, NULL, rd, q);
    return 1;
}

/* SCVTF, UCVTF, FCVTZS, FCVTZU (vector and scalar, fixed point): simd_shimm's conversions with fbits = 2*esize - imm */
static int fp_fixed(SnxJit* j, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, immh = (w >> 19) & 15, immb = (w >> 16) & 7,
             op = (w >> 11) & 31, rn = (w >> 5) & 31, rd = w & 31, s, n;
    FpOp o;
    V a[4];
    if ((op != 0x1C && op != 0x1F) || immh < 4) return 0; /* 32 and 64-bit lanes only */
    s = immh & 8 ? 3 : 2;
    if (s == 3 && !q && !scalar) return 0;
    n = scalar ? 1 : (q ? 128u : 64u) >> (3 + s);
    o.kind = op == 0x1C ? FP_LANES_FROM_INT : FP_LANES_TO_INT;
    o.arg = 3; /* toward zero */
    o.flag = u;
    o.fbits = 2 * (8u << s) - ((immh << 3) | immb);
    read_lanes(j, rn, s, n, a);
    fp_lanes(j, &o, s, n, a, NULL, rd, q);
    return 1;
}

/* FCVTN, FCVTN2, FCVTXN, FCVTXN2 and the scalar FCVTXN (ir_simd.c's fp_narrow): snx_fp_convert's lanes into Vd's low
   half (upper zero), its upper half (low kept) or, scalar, its low 32 bits */
static void narrow_store(SnxJit* j, unsigned rd, unsigned size, unsigned n, V const* values, unsigned q) {
    if (n == 1)
        write_scalar(j, rd, values[0]);
    else
        write_narrow(j, rd, size, n, values, q);
}
static int fp_narrow(SnxJit* j, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31,
             rn = (w >> 5) & 31, rd = w & 31, from = size & 1 ? 64 : 32, s = size & 1 ? 3 : 2,
             n = scalar ? 1 : 128 / from, i;
    gcc_jit_block *slow, *join;
    SnxJitFpFast f;
    V a[4], r[4];
    if (op != 0x16 || (size >> 1) || (scalar && !u) || (u && from != 64)) return 0;
    read_lanes(j, rn, s, n, a);
    jit_fp_narrow_fast(j, from, n, a, u, &f);
    join = jit_fp_branch(j, f.ok, &slow);
    narrow_store(j, rd, s - 1, n, f.value, q);
    jit_fp_else(j, join, slow, f.inexact);
    for (i = 0; i < n; ++i) r[i] = jit_val(j, jit_cast(j, jit_fp_narrow(j, a[i], from, u), lane_type(j, s - 1)));
    narrow_store(j, rd, s - 1, n, r, q);
    jit_fp_join(j, join);
    return 1;
}

/* ---- the integer and data-movement rest of snx_simd (ir_simd.c's emitters, a lane at a time) ---- */

/* DUP (element, scalar), the MOV Bd/Hd/Sd/Dd, Vn.T[i] alias: the element alone in Vd */
static int scalar_dup(SnxJit* j, SnxU32 w) {
    unsigned imm5 = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, size = 0;
    if (!imm5) return 0;
    while (!((imm5 >> size) & 1)) size++;
    if (size > 3) return 0;
    write_scalar(j, rd, jit_ve(j, rn, size, imm5 >> (size + 1)));
    return 1;
}

/* SMOV: the element sign-extended into Xd, or Wd zero-extended above it (fast_simd has UMOV) */
static int smov(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, imm5 = (w >> 16) & 31, imm4 = (w >> 11) & 15, rn = (w >> 5) & 31,
             rd = w & 31, size = 0;
    V v;
    if (u || imm4 != 5 || !imm5) return 0;
    while (!((imm5 >> size) & 1)) size++;
    if (size >= 3 || (size == 2 && !q)) return 0;
    v = ext(j, jit_ve(j, rn, size, imm5 >> (size + 1)), 8u << size, 0, q ? 64 : 32);
    if (rd != 31) jit_set_x(j, rd, jit_cast(j, v, j->t_u64));
    return 1;
}

/* TBL, TBX: bytes of the 1-4 table registers Vn.. (wrapping) by Vm's indexes, from a local copy of the table; an
   index past it gives 0 (TBL) or keeps Vd's byte (TBX), and reads byte 0 instead (the select evaluates both) */
static int table_lookup(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, len = (w >> 13) & 3, tbx = (w >> 12) & 1, rn = (w >> 5) & 31,
             rd = w & 31, n = q ? 16 : 8, k, i;
    /* the table as 64-bit halves, read back a byte at a time through an unsigned char pointer (which may alias) */
    gcc_jit_lvalue* table = jit_local(j, gcc_jit_context_new_array_type(j->ctx, NULL, j->t_u64, 8));
    V bytes = gcc_jit_context_new_cast(j->ctx, NULL, gcc_jit_lvalue_get_address(table, NULL),
                                       gcc_jit_type_get_pointer(j->t_u8));
    V idx[16], d[16], r[16];
    for (k = 0; k <= len; ++k)
        for (i = 0; i < 2; ++i)
            gcc_jit_block_add_assignment(j->block, NULL,
                                         gcc_jit_context_new_array_access(j->ctx, NULL, gcc_jit_lvalue_as_rvalue(table),
                                                                          jit_int(j, (int)(2 * k + i))),
                                         jit_vd(j, (rn + k) & 31, i));
    read_lanes(j, rm, 0, n, idx);
    if (tbx) read_lanes(j, rd, 0, n, d);
    for (i = 0; i < n; ++i) {
        V in = jit_val(j, LT(idx[i], klane(j, 0, 16 * (len + 1)))), e;
        e = gcc_jit_lvalue_as_rvalue(gcc_jit_context_new_array_access(
            j->ctx, NULL, bytes, jit_cast(j, jit_sel(j, in, idx[i], klane(j, 0, 0)), j->t_int)));
        r[i] = jit_val(j, jit_sel(j, in, e, tbx ? d[i] : klane(j, 0, 0)));
    }
    write_q(j, rd, 0, n, r, q);
    return 1;
}

/* the S-box (inverse 0) or its inverse (1) as unsigned char[256]: an internal constant global of the context, or,
   with a libgccjit older than global initializers (GCC 10), a local copy written a byte at a time */
static V aes_table(SnxJit* j, unsigned inverse) {
    const unsigned char* bytes = inverse ? AES_ISBOX : AES_SBOX;
    T type = gcc_jit_context_new_array_type(j->ctx, NULL, j->t_u8, 256);
#ifdef LIBGCCJIT_HAVE_gcc_jit_global_set_initializer
    if (!j->aes_tables[inverse]) {
        /* not const: libgccjit (GCC 16.2) gets a const array's size wrong (65 bytes for unsigned char[256]) and
           refuses the initializer */
        j->aes_tables[inverse] = gcc_jit_context_new_global(j->ctx, NULL, GCC_JIT_GLOBAL_INTERNAL, type,
                                                            inverse ? "snx_aes_isbox" : "snx_aes_sbox");
        gcc_jit_global_set_initializer(j->aes_tables[inverse], bytes, 256);
    }
    return gcc_jit_lvalue_as_rvalue(j->aes_tables[inverse]);
#else
    gcc_jit_lvalue* table = jit_local(j, type);
    unsigned i;
    for (i = 0; i < 256; ++i)
        gcc_jit_block_add_assignment(
            j->block, NULL,
            gcc_jit_context_new_array_access(j->ctx, NULL, gcc_jit_lvalue_as_rvalue(table), jit_int(j, (int)i)),
            klane(j, 0, bytes[i]));
    return gcc_jit_lvalue_as_rvalue(table);
#endif
}
/* x * 2 in GF(2^8) */
static V aes_xtime(SnxJit* j, V x) {
    return jit_val(j, XOR(SHL(x, klane(j, 0, 1)), jit_sel(j, NE(AND(x, klane(j, 0, 0x80)), klane(j, 0, 0)),
                                                          klane(j, 0, 0x1b), klane(j, 0, 0))));
}
/* crypto.c's v_aes and v_aesmc on the bytes, as ir_simd.c's aes_portable: AESE/AESD (op 4, 5) Vd =
   ShiftRows(SubBytes(Vd ^ Vn)) and the inverse, AESMC/AESIMC (6, 7) Vd = MixColumns(Vn) and the inverse. Byte
   (col, row) of the state is byte col*4 + row */
static int aes(SnxJit* j, SnxU32 w) {
    unsigned op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31, col, row, i;
    V n[16], d[16], x[16], r[16];
    if (op < 4 || op > 7) return 0;
    read_lanes(j, rn, 0, 16, n);
    if (op < 6) {
        V table = aes_table(j, op == 5);
        read_lanes(j, rd, 0, 16, d);
        for (i = 0; i < 16; ++i) x[i] = jit_val(j, XOR(d[i], n[i]));
        for (col = 0; col < 4; ++col)
            for (row = 0; row < 4; ++row) {
                /* AESE's ShiftRows takes column (col + row) % 4, AESD's (col - row) % 4 */
                unsigned from = (op == 4 ? (col + row) % 4 : (col + 4 - row) % 4) * 4 + row;
                r[col * 4 + row] = jit_val(j, gcc_jit_lvalue_as_rvalue(gcc_jit_context_new_array_access(
                                                  j->ctx, NULL, table, jit_cast(j, x[from], j->t_int))));
            }
    } else {
        V x2[16], x4[16], x8[16];
        for (i = 0; i < 16; ++i) {
            x2[i] = aes_xtime(j, n[i]);
            if (op == 7) {
                x4[i] = aes_xtime(j, x2[i]);
                x8[i] = aes_xtime(j, x4[i]);
            }
        }
        for (col = 0; col < 4; ++col)
            for (row = 0; row < 4; ++row) {
                unsigned a = col * 4 + row, b = col * 4 + (row + 1) % 4, c = col * 4 + (row + 2) % 4,
                         e = col * 4 + (row + 3) % 4;
                if (op == 6) /* 2a ^ 3b ^ c ^ e */
                    r[a] = jit_val(j, XOR(XOR(x2[a], XOR(x2[b], n[b])), XOR(n[c], n[e])));
                else /* 14a ^ 11b ^ 13c ^ 9e */
                    r[a] = jit_val(j, XOR(XOR(XOR(XOR(x8[a], x4[a]), x2[a]), XOR(XOR(x8[b], x2[b]), n[b])),
                                          XOR(XOR(XOR(x8[c], x4[c]), n[c]), XOR(x8[e], n[e]))));
            }
    }
    write_q(j, rd, 0, 16, r, 1);
    return 1;
}

/* LD2-4/ST2-4 (multiple structures): element e of register s is at byte (e*sel + s)*esize. As LD1 here, guest memory
   moves as 8-byte accesses in address order, each assembled from or split into its elements */
static int ld_structures(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, op = (w >> 12) & 15, size = (w >> 10) & 3, sel, bytes, eby, per, count, m, c;
    V address, chunk[8], lanes[64];
    sel = op == 0 ? 4 : op == 4 ? 3 : op == 8 ? 2 : 0;
    if (!sel || (size == 3 && !q)) return 0;
    bytes = q ? 16 : 8;
    eby = 1u << size;
    per = 8 / eby;             /* elements per 8-byte access */
    count = sel * bytes / eby; /* elements, in memory order: element m is lane m/sel of register m%sel */
    address = jit_x(j, rn);
    if (l) {
        for (c = 0; c < count / per; ++c) chunk[c] = jit_val(j, jit_rd(j, ADD(address, K64(8 * c)), 64));
        for (m = 0; m < count; ++m)
            jit_set_ve(j, (rt + m % sel) & 31, size, m / sel,
                       jit_cast(j, SHR(chunk[m / per], K64(8 * eby * (m % per))), lane_type(j, size)));
        if (!q)
            for (m = 0; m < sel; ++m) zero_high(j, (rt + m) & 31);
    } else {
        for (m = 0; m < count; ++m) lanes[m] = jit_ve(j, (rt + m % sel) & 31, size, m / sel);
        for (c = 0; c < count / per; ++c) {
            V v = K64(0);
            for (m = 0; m < per; ++m) v = OR(v, SHL(jit_cast(j, lanes[c * per + m], j->t_u64), K64(8 * eby * m)));
            jit_wr(j, ADD(address, K64(8 * c)), 64, v);
        }
    }
    if (post) jit_set_x(j, rn, ADD(address, rm == 31 ? K64((SnxU64)sel * bytes) : jit_x(j, rm)));
    return 1;
}

/* three-different (vector): simd_3diff's long, wide and narrowing-high forms (ir_simd.c's three_different) */
static int three_different(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 15,
             rn = (w >> 5) & 31, rd = w & 31, eb = 8u << size, m = 64 / eb, off = q ? 64 / eb : 0, i;
    V a[8], b[8], z[8], r[8], sat = NULL;
    if (size == 3) return 0;
    switch (op) {
        case 4:
        case 6: /* ADDHN, SUBHN, RADDHN, RSUBHN: the high half of each wide sum or difference */
            read_lanes(j, rn, size + 1, m, a);
            read_lanes(j, rm, size + 1, m, b);
            for (i = 0; i < m; ++i) {
                V s = op == 4 ? ADD(a[i], b[i]) : SUB(a[i], b[i]);
                if (u) s = ADD(s, klane(j, size + 1, int_min(eb)));
                r[i] = jit_val(j, jit_cast(j, SHR(s, klane(j, size + 1, eb)), lane_type(j, size)));
            }
            write_narrow(j, rd, size, m, r, q);
            return 1;
        case 1:
        case 3: /* [SU]ADDW, [SU]SUBW: Vn's wide lanes and the half of Vm widened */
            read_lanes(j, rn, size + 1, m, a);
            for (i = 0; i < m; ++i) b[i] = jit_ve(j, rm, size, off + i);
            for (i = 0; i < m; ++i) {
                V y = ext(j, b[i], eb, u, 2 * eb);
                r[i] = jit_val(j, jit_cast(j, op == 1 ? ADD(a[i], y) : SUB(a[i], y), lane_type(j, size + 1)));
            }
            write_lanes(j, rd, size + 1, m, r);
            return 1;
        case 14: /* PMULL (bytes; pmull_1q has the 1Q form) */
            if (u || size) return 0;
            for (i = 0; i < 8; ++i) {
                a[i] = jit_ve(j, rn, 0, off + i);
                b[i] = jit_ve(j, rm, 0, off + i);
            }
            for (i = 0; i < 8; ++i) r[i] = poly_multiply(j, a[i], b[i]);
            write_lanes(j, rd, 1, 8, r);
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
    for (i = 0; i < m; ++i) {
        a[i] = jit_ve(j, rn, size, off + i);
        b[i] = jit_ve(j, rm, size, off + i);
        z[i] = jit_ve(j, rd, size + 1, i);
    }
    for (i = 0; i < m; ++i) r[i] = long_lane(j, op, u, eb, a[i], b[i], z[i], &sat);
    if (sat) set_qc(j, sat);
    write_lanes(j, rd, size + 1, m, r);
    return 1;
}

/* three-different (scalar): SQDMLAL, SQDMLSL, SQDMULL on lane 0, the rest of Vd zero */
static int scalar_three_different(SnxJit* j, SnxU32 w) {
    unsigned u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 15, rn = (w >> 5) & 31,
             rd = w & 31;
    V r, sat = NULL;
    if (u || (op != 9 && op != 11 && op != 13) || (size != 1 && size != 2)) return 0;
    r = long_lane(j, op, 0, 8u << size, jit_ve(j, rn, size, 0), jit_ve(j, rm, size, 0), jit_ve(j, rd, size + 1, 0),
                  &sat);
    if (sat) set_qc(j, sat);
    write_scalar(j, rd, r);
    return 1;
}

/* the leading zeros of x, a lane of eb < 64 bits: a marker bit just below the lane keeps __builtin_clzll's argument
   nonzero (it is undefined at 0) and makes a zero lane count eb */
static V lane_clz(SnxJit* j, V x, unsigned eb) {
    V arg = OR(SHL(jit_cast(j, x, j->t_u64), K64(64 - eb)), K64((SnxU64)1 << (63 - eb)));
    return jit_call(j, jit_builtin(j, "__builtin_clzll"), 1, &arg);
}

/* two-register misc, integer: simd_2misc's REV16/32/64, CLS, CLZ, ABS, NEG, SQABS, SQNEG, SUQADD, USQADD, the
   compares against zero, [SU]ADDLP, [SU]ADALP, XTN, SQXTN, UQXTN, SQXTUN and SHLL, and the scalar forms
   simd_2misc allows (ir_simd.c's int_misc) */
static int int_misc(SnxJit* j, SnxU32 w, unsigned scalar) {
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31,
             rn = (w >> 5) & 31, rd = w & 31, eb = 8u << size, n = scalar ? 1 : (q ? 128u : 64u) / eb,
             m = scalar ? 1 : 64 / eb, i, k = (u << 5) | op;
    V a[16], z[16], r[16], sat = NULL;
    if (scalar && op != 0x12 && op != 0x14 && size != 3 && op != 0x03 && op != 0x07) return 0;
    switch (op) {
        case 0x12:
        case 0x14:                                            /* XTN (wraps), SQXTUN, SQXTN, UQXTN */
            if (size == 3 || (scalar && k == 0x12)) return 0; /* XTN has no scalar form */
            read_lanes(j, rn, size + 1, m, a);
            for (i = 0; i < m; ++i)
                r[i] = k == 0x12 ? jit_val(j, jit_cast(j, a[i], lane_type(j, size)))
                                 : saturate_lane(j, a[i], eb, k != 0x34, k == 0x14, &sat);
            if (sat) set_qc(j, sat);
            if (scalar)
                write_scalar(j, rd, r[0]);
            else
                write_narrow(j, rd, size, m, r, q);
            return 1;
        case 0x03: /* SUQADD (signed Vd + unsigned Vn), USQADD (unsigned Vd + signed Vn): exact in twice the width */
            if (size == 3 && !q && !scalar) return 0;
            read_lanes(j, rn, size, n, a);
            read_lanes(j, rd, size, n, z);
            for (i = 0; i < n; ++i)
                r[i] =
                    saturate_lane(j, ADD(ext(j, z[i], eb, u, 2 * eb), ext(j, a[i], eb, !u, 2 * eb)), eb, 1, !u, &sat);
            if (sat) set_qc(j, sat);
            if (scalar)
                write_scalar(j, rd, r[0]);
            else
                write_q(j, rd, size, n, r, q);
            return 1;
        case 0x13: /* SHLL: the half widened, shifted by the element size */
            if (!u || size == 3 || scalar) return 0;
            for (i = 0; i < m; ++i) a[i] = jit_ve(j, rn, size, (q ? m : 0) + i);
            for (i = 0; i < m; ++i)
                r[i] = jit_val(j, SHL(jit_cast(j, a[i], lane_type(j, size + 1)), klane(j, size + 1, eb)));
            write_lanes(j, rd, size + 1, m, r);
            return 1;
        case 0x02:
        case 0x06: /* [SU]ADDLP, [SU]ADALP: adjacent pairs summed in twice the width */
            if (size == 3 || scalar) return 0;
            read_lanes(j, rn, size, n, a);
            if (op == 6) read_lanes(j, rd, size + 1, n / 2, z);
            for (i = 0; i < n / 2; ++i) {
                V s = ADD(ext(j, a[2 * i], eb, u, 2 * eb), ext(j, a[2 * i + 1], eb, u, 2 * eb));
                if (op == 6) s = ADD(s, z[i]);
                r[i] = jit_val(j, jit_cast(j, s, lane_type(j, size + 1)));
            }
            write_q(j, rd, size + 1, n / 2, r, q);
            return 1;
        default: break;
    }
    if (size == 3 && !q && !scalar) return 0;
    switch (k) { /* the encodings handled here, before reading anything */
        case 0x00:
        case 0x20:
        case 0x01:
            if (eb >= (op == 1 ? 16u : u ? 32u : 64u) || scalar) return 0;
            break;
        case 0x04:
        case 0x24:
            if (size == 3) return 0;
            break;
        case 0x07:
        case 0x27:
        case 0x08:
        case 0x28:
        case 0x09:
        case 0x29:
        case 0x0A:
        case 0x0B:
        case 0x2B: break;
        default: return 0;
    }
    read_lanes(j, rn, size, n, a);
    for (i = 0; i < n; ++i) {
        V x = a[i], s = as_signed(j, x, eb), zero = jit_k(j, jit_sint(j, eb), 0), negated = SUB(klane(j, size, 0), x),
          v;
        switch (k) {
            case 0x00:
            case 0x20:
            case 0x01: { /* REV64, REV32, REV16: the elements of each container reversed */
                unsigned per = (op == 1 ? 16u : u ? 32u : 64u) / eb;
                v = a[i - i % per + per - 1 - i % per];
                break;
            }
            case 0x04: /* CLS: the leading zeros of x ^ (x >> 1), less one */
                v = SUB(jit_cast(j, lane_clz(j, XOR(x, asr(j, x, eb, 1)), eb), lane_type(j, size)), klane(j, size, 1));
                break;
            case 0x24: v = jit_cast(j, lane_clz(j, x, eb), lane_type(j, size)); break;
            case 0x07: /* SQABS, SQNEG: INT_MIN saturates to INT_MAX with QC */
            case 0x27: {
                V min = jit_val(j, EQ(x, klane(j, size, int_min(eb))));
                v = k == 0x27 ? negated : jit_sel(j, LT(s, zero), negated, x);
                v = jit_sel(j, min, klane(j, size, int_max(eb)), v);
                sat = any_of(j, sat, min);
                break;
            }
            case 0x08: v = mask_of(j, GT(s, zero), size); break;
            case 0x28: v = mask_of(j, GE(s, zero), size); break;
            case 0x09: v = mask_of(j, EQ(s, zero), size); break;
            case 0x29: v = mask_of(j, jit_cmp(j, GCC_JIT_COMPARISON_LE, s, zero), size); break;
            case 0x0A: v = mask_of(j, LT(s, zero), size); break;
            case 0x0B: v = jit_sel(j, LT(s, zero), negated, x); break;
            default: v = negated; break;
        }
        r[i] = jit_val(j, jit_cast(j, v, lane_type(j, size)));
    }
    if (sat) set_qc(j, sat);
    if (scalar)
        write_scalar(j, rd, r[0]);
    else
        write_q(j, rd, size, n, r, q);
    return 1;
}

/* across lanes: simd_across's ADDV, [SU]MAXV, [SU]MINV, [SU]ADDLV, and FMAXV, FMINV, FMAXNMV, FMINNMV through the
   helper in its reduction order (ir_simd.c's across) */
static int across(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, eb = 8u << size, n, i;
    V a[16], r;
    if (op == 0x0C || op == 0x0F) {
        unsigned mn = size >> 1, numeric = op == 0x0C;
        V p0, p1;
        if (!u || !q || (size & 1)) return 0;
        read_lanes(j, rn, 2, 4, a);
        p0 = jit_val(j, jit_fp_minmax(j, a[0], a[1], 32, mn, numeric));
        p1 = jit_val(j, jit_fp_minmax(j, a[2], a[3], 32, mn, numeric));
        write_scalar(j, rd, jit_cast(j, jit_fp_minmax(j, p0, p1, 32, mn, numeric), j->t_u32));
        return 1;
    }
    if (size == 3 || (size == 2 && !q)) return 0;
    if (op != 0x03 && op != 0x0A && op != 0x1A && !(op == 0x1B && !u)) return 0;
    n = (q ? 128u : 64u) / eb;
    read_lanes(j, rn, size, n, a);
    if (op == 0x03) { /* the long sum fits twice the width */
        r = ext(j, a[0], eb, u, 2 * eb);
        for (i = 1; i < n; ++i) r = jit_val(j, ADD(r, ext(j, a[i], eb, u, 2 * eb)));
    } else {
        r = a[0];
        for (i = 1; i < n; ++i) {
            if (op == 0x1B)
                r = ADD(r, a[i]);
            else
                r = jit_sel(j, compare(j, op == 0x0A ? GCC_JIT_COMPARISON_GT : GCC_JIT_COMPARISON_LT, u, eb, a[i], r),
                            a[i], r);
            r = jit_val(j, jit_cast(j, r, lane_type(j, size)));
        }
    }
    write_scalar(j, rd, r);
    return 1;
}

/* scalar pairwise: ADDP Dd, Vn.2D, and FADDP, FMAXP, FMINP, FMAXNMP, FMINNMP of the two lanes */
static int scalar_pairwise(SnxJit* j, SnxU32 w) {
    unsigned u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31,
             s = size & 1 ? 3 : 2;
    FpOp o;
    V x[1], y[1];
    if (!u) {
        if (op != 0x1B || size != 3) return 0;
        write_scalar(j, rd, ADD(jit_vd(j, rn, 0), jit_vd(j, rn, 1)));
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
    x[0] = jit_ve(j, rn, s, 0);
    y[0] = jit_ve(j, rn, s, 1);
    fp_lanes(j, &o, s, 1, x, y, rd, 0);
    return 1;
}

/* by element, beyond fast_simd's FMLA/FMLS/FMUL: FMULX and simd_elem's integer forms (ir_simd.c's by_element) */
static int by_element(SnxJit* j, SnxU32 w, unsigned scalar) {
    static const unsigned char long_ops[16] = {0, 0, 8, 9, 0, 0, 10, 11, 0, 0, 12, 13, 0, 0, 0, 0};
    unsigned q = scalar ? 0 : (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, l = (w >> 21) & 1,
             mb = (w >> 20) & 1, rm = (w >> 16) & 15, op = (w >> 12) & 15, h = (w >> 11) & 1, rn = (w >> 5) & 31,
             rd = w & 31, idx, m, n, eb = 8u << size, i;
    V a[16], z[16], r[16], b, sat = NULL;
    if (u ? op == 9 : (op == 1 || op == 5 || op == 9)) { /* FP */
        unsigned sz = size & 1, s = sz ? 3 : 2;
        FpOp o;
        V y[4];
        if (!u || size < 2 || (sz && l) || (sz && !q && !scalar)) return 0; /* (fast_simd has FMLA/FMLS/FMUL) */
        n = scalar ? 1 : (q ? 16u : 8u) >> s;
        memset(&o, 0, sizeof(o));
        o.kind = FP_LANES_MULX;
        b = jit_ve(j, (mb << 4) | rm, s, sz ? h : (h << 1) | l);
        read_lanes(j, rn, s, n, a);
        for (i = 0; i < n; ++i) y[i] = b;
        fp_lanes(j, &o, s, n, a, y, rd, q);
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
    if (op == 2 || op == 3 || op == 6 || op == 7 || op == 10 || op == 11) {
        unsigned saturating_op = op == 3 || op == 7 || op == 11;
        if ((u && saturating_op) || (scalar && !saturating_op)) return 0;
        n = scalar ? 1 : 64 / eb;
        b = jit_ve(j, m, size, idx);
        for (i = 0; i < n; ++i) {
            a[i] = jit_ve(j, rn, size, (q ? n : 0) + i);
            z[i] = jit_ve(j, rd, size + 1, i);
        }
        for (i = 0; i < n; ++i) r[i] = long_lane(j, long_ops[op], u, eb, a[i], b, z[i], &sat);
        if (sat) set_qc(j, sat);
        if (scalar)
            write_scalar(j, rd, r[0]);
        else
            write_lanes(j, rd, size + 1, n, r);
        return 1;
    }
    if (u ? op != 0 && op != 4 : op != 8 && op != 12 && op != 13) return 0;
    if (scalar && op < 12) return 0;
    n = scalar ? 1 : (q ? 128u : 64u) / eb;
    b = jit_ve(j, m, size, idx);
    read_lanes(j, rn, size, n, a);
    if (op == 0 || op == 4) read_lanes(j, rd, size, n, z);
    for (i = 0; i < n; ++i) {
        if (op >= 12)
            r[i] = doubling_high(j, a[i], b, eb, op == 13, &sat);
        else {
            V p = MUL(a[i], b);
            r[i] = jit_val(j, jit_cast(j, op == 8 ? p : op == 0 ? ADD(z[i], p) : SUB(z[i], p), lane_type(j, size)));
        }
    }
    if (sat) set_qc(j, sat);
    if (scalar)
        write_scalar(j, rd, r[0]);
    else
        write_q(j, rd, size, n, r, q);
    return 1;
}

/* three-same integer (scalar): simd_3same's SQADD, UQADD, SQSUB, UQSUB, SQSHL, UQSHL, SQRSHL, UQRSHL, SQDMULH and
   SQRDMULH, and on D registers ADD, SUB, the compares and SSHL/USHL/SRSHL/URSHL (ir_simd.c's scalar_three_same) */
static int scalar_three_same(SnxJit* j, SnxU32 w) {
    unsigned u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 11) & 31, rn = (w >> 5) & 31,
             rd = w & 31, eb = 8u << size;
    V x, y, r, sat = NULL;
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
    x = jit_ve(j, rn, size, 0);
    y = jit_ve(j, rm, size, 0);
    switch (op) {
        case 0x01:
        case 0x05: r = saturating(j, u, op == 0x05, eb, x, y, &sat); break;
        case 0x16: r = doubling_high(j, x, y, eb, u, &sat); break;
        case 0x06: r = mask_of(j, compare(j, GCC_JIT_COMPARISON_GT, u, eb, x, y), size); break;
        case 0x07: r = mask_of(j, compare(j, GCC_JIT_COMPARISON_GE, u, eb, x, y), size); break;
        case 0x10: r = u ? SUB(x, y) : ADD(x, y); break;
        case 0x11: r = mask_of(j, u ? EQ(x, y) : NE(AND(x, y), klane(j, size, 0)), size); break;
        default: r = shift_lane(j, x, y, eb, u, op == 0x0A || op == 0x0B, op == 0x09 || op == 0x0B, &sat); break;
    }
    r = jit_val(j, jit_cast(j, r, lane_type(j, size)));
    if (sat) set_qc(j, sat);
    write_scalar(j, rd, r);
    return 1;
}

/* PMULL/PMULL2 Vd.1Q: ir_simd.c's carryless_multiply (which says why the masked products are carry-free) in u128 */
static int pmull_1q(SnxJit* j, SnxU32 w) {
    static const SnxU64 m = (SnxU64)0x11111111UL << 32 | 0x11111111UL;
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, h, a, c;
    T t = j->t_u128;
    V x = jit_vd(j, rn, q), y = jit_vd(j, rm, q), yb[4], xa[4], r = NULL, half, part, z, k;
    for (a = 0; a < 4; ++a) yb[a] = jit_val(j, jit_cast(j, AND(y, K64(m << a)), t));
    for (h = 0; h < 2; ++h) {
        half = h ? SHR(x, K64(32)) : AND(x, K64(0xFFFFFFFFUL));
        for (a = 0; a < 4; ++a) xa[a] = jit_val(j, jit_cast(j, AND(half, K64(m << a)), t));
        part = NULL;
        for (c = 0; c < 4; ++c) {
            z = MUL(xa[0], yb[c]);
            for (a = 1; a < 4; ++a) z = XOR(z, MUL(xa[a], yb[(c - a) & 3]));
            k = jit_k(j, t, m << c);
            z = AND(z, OR(k, SHL(k, jit_k(j, t, 64))));
            part = part ? OR(part, z) : z;
        }
        part = jit_val(j, part);
        r = h ? jit_val(j, XOR(r, SHL(part, jit_k(j, t, 32)))) : part;
    }
    jit_set_vd(j, rd, 0, jit_cast(j, r, j->t_u64));
    jit_set_vd(j, rd, 1, jit_cast(j, SHR(r, jit_k(j, t, 64)), j->t_u64));
    return 1;
}

/* SHA1 and SHA256 (ir_simd.c's sha): crypto.c's v_sha1 and v_sha256 on u32 words, every operand read first */
static V rol32(SnxJit* j, V x, unsigned n) { return OR(SHL(x, K32(n)), SHR(x, K32(32 - n))); }
/* ror(x, a) ^ ror(x, b) ^ (c ? ror(x, c) : x >> d) */
static V sigma(SnxJit* j, V x, unsigned a, unsigned b, unsigned c, unsigned d) {
    return XOR(XOR(rol32(j, x, 32 - a), rol32(j, x, 32 - b)), c ? rol32(j, x, 32 - c) : SHR(x, K32(d)));
}
static int sha(SnxJit* j, SnxU32 w, unsigned op) {
    unsigned rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, i;
    V x[4], y[4], m[4], r[4], t;
    read_lanes(j, rd, 2, 4, x);
    read_lanes(j, rn, 2, 4, y);
    if (op < 0x10) read_lanes(j, rm, 2, 4, m);
    switch (op) {
        case 0: /* SHA1C, SHA1P, SHA1M */
        case 1:
        case 2: {
            V e = y[0], f, next;
            for (i = 0; i < 4; ++i) {
                if (op == 0)
                    f = OR(AND(x[1], x[2]), AND(NOT(x[1]), x[3]));
                else if (op == 1)
                    f = XOR(XOR(x[1], x[2]), x[3]);
                else
                    f = OR(OR(AND(x[1], x[2]), AND(x[1], x[3])), AND(x[2], x[3]));
                e = jit_val(j, ADD(ADD(ADD(e, rol32(j, x[0], 5)), f), m[i]));
                next = x[3];
                x[3] = x[2];
                x[2] = jit_val(j, rol32(j, x[1], 30));
                x[1] = x[0];
                x[0] = e;
                e = next;
            }
            memcpy(r, x, sizeof(r));
            break;
        }
        case 3: /* SHA1SU0 */
            for (i = 0; i < 4; ++i) r[i] = jit_val(j, XOR(XOR(i < 2 ? x[i + 2] : y[i - 2], x[i]), m[i]));
            break;
        case 4: /* SHA256H, SHA256H2 */
        case 5: {
            V *a = op == 4 ? x : y, *b = op == 4 ? y : x, ch, maj, a3;
            for (i = 0; i < 4; ++i) {
                ch = XOR(AND(b[0], b[1]), AND(NOT(b[0]), b[2]));
                maj = XOR(XOR(AND(a[0], a[1]), AND(a[0], a[2])), AND(a[1], a[2]));
                t = jit_val(j, ADD(ADD(ADD(b[3], sigma(j, b[0], 6, 11, 25, 0)), ch), m[i]));
                a3 = jit_val(j, ADD(a[3], t));
                t = jit_val(j, ADD(ADD(t, sigma(j, a[0], 2, 13, 22, 0)), maj));
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
        case 6: /* SHA256SU1 */
            for (i = 0; i < 4; ++i)
                r[i] = jit_val(
                    j, ADD(ADD(x[i], sigma(j, i < 2 ? m[i + 2] : r[i - 2], 17, 19, 0, 10)), i < 3 ? y[i + 1] : m[0]));
            break;
        case 0x10: /* SHA1H */ write_scalar(j, rd, rol32(j, y[0], 30)); return 1;
        case 0x11: /* SHA1SU1 */
            for (i = 0; i < 4; ++i) r[i] = jit_val(j, i < 3 ? XOR(x[i], y[i + 1]) : x[3]);
            t = jit_val(j, rol32(j, r[0], 2));
            for (i = 0; i < 4; ++i) r[i] = jit_val(j, rol32(j, r[i], 1));
            r[3] = XOR(r[3], t);
            break;
        case 0x12: /* SHA256SU0 */
            for (i = 0; i < 4; ++i) r[i] = ADD(x[i], sigma(j, i < 3 ? x[i + 1] : y[0], 7, 18, 0, 3));
            break;
        default: return 0;
    }
    write_lanes(j, rd, 2, 4, r);
    return 1;
}

/* in snx_simd's order (cpu_helpers.c), each group's emitters in turn; 0 leaves the word to snx_simd */
int snx_jit_more_simd(SnxJit* j, SnxU32 w) {
    if ((w & 0xBF800000UL) == 0x0C000000UL || (w & 0xBF800000UL) == 0x0C800000UL)
        return ld1_multiple(j, w) || ld_structures(j, w);
    if ((w & 0xBF000000UL) == 0x0D000000UL) return ld1_single(j, w);
    if ((w & 0x9F200400UL) == 0x0E200400UL) return three_same(j, w) || fp_three_same(j, w, 0);
    if ((w & 0xDF200400UL) == 0x5E200400UL) return scalar_three_same(j, w) || fp_three_same(j, w, 1);
    if ((w & 0xBFE0FC00UL) == 0x0EE0E000UL) return pmull_1q(j, w);
    if ((w & 0x9F200C00UL) == 0x0E200000UL) return three_different(j, w);
    if ((w & 0xDF200C00UL) == 0x5E200000UL) return scalar_three_different(j, w);
    if ((w & 0x9F3E0C00UL) == 0x0E200800UL)
        return byte_misc(j, w) || fp_sign_misc(j, w) || fp_misc(j, w, 0) || fp_narrow(j, w, 0) || int_misc(j, w, 0);
    if ((w & 0xDF3E0C00UL) == 0x5E200800UL) return fp_misc(j, w, 1) || fp_narrow(j, w, 1) || int_misc(j, w, 1);
    if ((w & 0x9F3E0C00UL) == 0x0E300800UL) return across(j, w);
    if ((w & 0xDF3E0C00UL) == 0x5E300800UL) return scalar_pairwise(j, w);
    if ((w & 0x9FE08400UL) == 0x0E000400UL) return smov(j, w);
    if ((w & 0xFFE0FC00UL) == 0x5E000400UL) return scalar_dup(j, w);
    if ((w & 0x9FF80400UL) == 0x0F000400UL) return modified_immediate(j, w);
    if ((w & 0x9F800400UL) == 0x0F000400UL) return fp_fixed(j, w, 0) || shift_immediate(j, w, 0);
    if ((w & 0xDF800400UL) == 0x5F000400UL) return fp_fixed(j, w, 1) || shift_immediate(j, w, 1);
    if ((w & 0x9F000400UL) == 0x0F000000UL) return by_element(j, w, 0);
    if ((w & 0xDF000400UL) == 0x5F000000UL) return by_element(j, w, 1);
    if ((w & 0xBF208C00UL) == 0x0E000800UL) return permute(j, w);
    if ((w & 0xBFE08400UL) == 0x2E000000UL) return extract(j, w);
    if ((w & 0xBFE08C00UL) == 0x0E000000UL) return table_lookup(j, w);
    if ((w & 0xFFFF0C00UL) == 0x4E280800UL) return aes(j, w);
    if ((w & 0xFFE08C00UL) == 0x5E000000UL) return sha(j, w, (w >> 12) & 7);
    if ((w & 0xFFFF0C00UL) == 0x5E280800UL) return ((w >> 12) & 31) < 3 && sha(j, w, 0x10 + ((w >> 12) & 31));
    return 0;
}
