/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c, helper-a64.c, vec_helper.c, vfp_helper.c,
   crypto_helper.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
   Copyright (c) 2003 Fabrice Bellard */
/* The integer, branch, load/store and system instructions: each emitter builds the C of its a64_templates.inc case,
   as ir_a64.c does for LLVM. Field values are the template's f_ declarations evaluated here, and cpu.h's inline
   helpers are written out for them: their selector arguments (a shift type, an extension, a condition, an access
   size, bitfield positions) are fields of the constant word, so only the selected computation is emitted. */
#include "common/cpu/recompiler/gccjit/jit.h"
#include "common/cpu/aarch64/cpu.h"
#include <string.h>

typedef gcc_jit_rvalue* V;
typedef gcc_jit_type* T;
#define F(lo, n)     ((w >> (lo)) & ((1UL << (n)) - 1))
/* (i64)((i32)(w<<shl)>>shr), the template's signed fields */
#define SF(shl, shr) ((SnxU64)(SnxI64)((SnxI32)(SnxU32)(w << (shl)) >> (shr)))
typedef int32_t SnxI32;
typedef int64_t SnxI64;

#define ADD(a, b) jit_op(j, GCC_JIT_BINARY_OP_PLUS, a, b)
#define SUB(a, b) jit_op(j, GCC_JIT_BINARY_OP_MINUS, a, b)
#define MUL(a, b) jit_op(j, GCC_JIT_BINARY_OP_MULT, a, b)
#define DIV(a, b) jit_op(j, GCC_JIT_BINARY_OP_DIVIDE, a, b)
#define AND(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_AND, a, b)
#define OR(a, b)  jit_op(j, GCC_JIT_BINARY_OP_BITWISE_OR, a, b)
#define XOR(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_XOR, a, b)
#define SHL(a, b) jit_op(j, GCC_JIT_BINARY_OP_LSHIFT, a, b)
#define SHR(a, b) jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, a, b)
#define NOT(a)    jit_unary(j, GCC_JIT_UNARY_OP_BITWISE_NEGATE, a)
#define EQ(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_EQ, a, b)
#define NE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_NE, a, b)
#define LT(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_LT, a, b)
#define LE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_LE, a, b)
#define GE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_GE, a, b)
#define U32(v)    jit_cast(j, v, j->t_u32)
#define U64(v)    jit_cast(j, v, j->t_u64)
#define K32(v)    jit_u32(j, v)
#define K64(v)    jit_u64(j, v)

static T width_type(SnxJit* j, int wide) { return wide ? j->t_u64 : j->t_u32; }
/* (u64)((u32)(...)) */
static V zext32(SnxJit* j, V v) { return U64(U32(v)); }
static V rx32(SnxJit* j, unsigned n) { return U32(jit_rx(j, n)); }
static V x32(SnxJit* j, unsigned n) { return U32(jit_x(j, n)); }
/* (uN)((iN)x >> s) */
static V asr(SnxJit* j, V x, unsigned bits, V s) {
    T u = jit_uint(j, bits), i = jit_sint(j, bits);
    return jit_cast(j, jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, jit_cast(j, x, i), jit_cast(j, s, i)), u);
}
/* (u64)(iN)x: sign extension of an N-bit value */
static V sext64(SnxJit* j, V x, unsigned bits) { return U64(jit_cast(j, jit_cast(j, x, jit_sint(j, bits)), j->t_s64)); }
static SnxU64 low_mask(unsigned n) { return n >= 64 ? ~(SnxU64)0 : ((SnxU64)1 << n) - 1; }

/* ---- cpu.h's helpers ---- */

static void store_flag(SnxJit* j, size_t offset, V value) { jit_st(j, offset, j->t_u32, value); }

/* adds64/adds32(c,a,b,cin); cin NULL is the constant 0 */
static V adds(SnxJit* j, int wide, V a, V b, V cin) {
    T t = width_type(j, wide);
    unsigned top = wide ? 63 : 31;
    V r, cf;
    a = jit_cast(j, a, t);
    b = jit_cast(j, b, t);
    r = jit_val(j, cin ? ADD(ADD(a, b), jit_cast(j, cin, t)) : ADD(a, b));
    store_flag(j, offsetof(CPU, n), SHR(r, jit_k(j, t, top)));
    store_flag(j, offsetof(CPU, z), EQ(r, jit_k(j, t, 0)));
    /* cin ? r <= a : r < a */
    cf = LT(r, a);
    if (cin) cf = jit_lor(j, cf, jit_land(j, NE(cin, K32(0)), EQ(r, a)));
    store_flag(j, offsetof(CPU, cf), cf);
    store_flag(j, offsetof(CPU, vf), SHR(AND(NOT(XOR(a, b)), XOR(a, r)), jit_k(j, t, top)));
    return r;
}
/* subs64/subs32(c,a,b) */
static V subs(SnxJit* j, int wide, V a, V b) {
    T t = width_type(j, wide);
    unsigned top = wide ? 63 : 31;
    V r;
    a = jit_cast(j, a, t);
    b = jit_cast(j, b, t);
    r = jit_val(j, SUB(a, b));
    store_flag(j, offsetof(CPU, n), SHR(r, jit_k(j, t, top)));
    store_flag(j, offsetof(CPU, z), EQ(r, jit_k(j, t, 0)));
    store_flag(j, offsetof(CPU, cf), GE(a, b));
    store_flag(j, offsetof(CPU, vf), SHR(AND(XOR(a, b), XOR(a, r)), jit_k(j, t, top)));
    return r;
}
/* logic64/logic32(c,r) */
static void logic_flags(SnxJit* j, int wide, V r) {
    T t = width_type(j, wide);
    r = jit_val(j, jit_cast(j, r, t));
    store_flag(j, offsetof(CPU, n), SHR(r, jit_k(j, t, wide ? 63 : 31)));
    store_flag(j, offsetof(CPU, z), EQ(r, jit_k(j, t, 0)));
    store_flag(j, offsetof(CPU, cf), K32(0));
    store_flag(j, offsetof(CPU, vf), K32(0));
}
/* cond_ok(c,cond) for a constant condition, as a bool */
static V condition(SnxJit* j, unsigned cond) {
    V zero = K32(0), r, n, z, cf, vf;
    unsigned k = cond >> 1;
    if (k == 7) return jit_true(j);
    switch (k) {
        case 0: r = NE(jit_ld(j, offsetof(CPU, z), j->t_u32), zero); break;
        case 1: r = NE(jit_ld(j, offsetof(CPU, cf), j->t_u32), zero); break;
        case 2: r = NE(jit_ld(j, offsetof(CPU, n), j->t_u32), zero); break;
        case 3: r = NE(jit_ld(j, offsetof(CPU, vf), j->t_u32), zero); break;
        case 4:
            cf = jit_ld(j, offsetof(CPU, cf), j->t_u32);
            z = jit_ld(j, offsetof(CPU, z), j->t_u32);
            r = jit_land(j, NE(cf, zero), EQ(z, zero));
            break;
        case 5:
            n = jit_ld(j, offsetof(CPU, n), j->t_u32);
            vf = jit_ld(j, offsetof(CPU, vf), j->t_u32);
            r = EQ(n, vf);
            break;
        default:
            n = jit_ld(j, offsetof(CPU, n), j->t_u32);
            z = jit_ld(j, offsetof(CPU, z), j->t_u32);
            vf = jit_ld(j, offsetof(CPU, vf), j->t_u32);
            r = jit_land(j, EQ(z, zero), EQ(n, vf));
            break;
    }
    return jit_val(j, cond & 1 ? jit_lnot(j, r) : r);
}
/* set_nzcv(c,f) for constant flags */
static void nzcv_from(SnxJit* j, unsigned f) {
    store_flag(j, offsetof(CPU, n), K32((f >> 3) & 1));
    store_flag(j, offsetof(CPU, z), K32((f >> 2) & 1));
    store_flag(j, offsetof(CPU, cf), K32((f >> 1) & 1));
    store_flag(j, offsetof(CPU, vf), K32(f & 1));
}
/* shf64/shf32(x,t,s) for a constant type and amount */
static V shf(SnxJit* j, V x, unsigned type, unsigned s, unsigned bits) {
    T t = jit_uint(j, bits);
    x = jit_cast(j, x, t);
    switch (type) {
        case 0: return SHL(x, jit_k(j, t, s));
        case 1: return SHR(x, jit_k(j, t, s));
        case 2: return asr(j, x, bits, jit_k(j, t, s));
        default: /* ror: s&=w-1; s ? (x>>s)|(x<<(w-s)) : x */
            s &= bits - 1;
            return s ? OR(SHR(x, jit_k(j, t, s)), SHL(x, jit_k(j, t, bits - s))) : x;
    }
}
/* ror64/ror32(x,s) for a register amount; (w-s)&(w-1) keeps s==0 defined (x|x) */
static V ror(SnxJit* j, V x, V s, unsigned bits) {
    T t = jit_uint(j, bits);
    x = jit_cast(j, x, t);
    s = jit_val(j, AND(jit_cast(j, s, t), jit_k(j, t, bits - 1)));
    return OR(SHR(x, s), SHL(x, AND(SUB(jit_k(j, t, bits), s), jit_k(j, t, bits - 1))));
}
/* ext64(x,opt,sh) for constant opt and shift */
static V extend(SnxJit* j, V x, unsigned opt, unsigned sh) {
    V r;
    switch (opt) {
        case 0: r = U64(jit_cast(j, x, j->t_u8)); break;
        case 1: r = U64(jit_cast(j, x, j->t_u16)); break;
        case 2: r = U64(U32(x)); break;
        case 4: r = sext64(j, x, 8); break;
        case 5: r = sext64(j, x, 16); break;
        case 6: r = sext64(j, x, 32); break;
        default: r = U64(x); break;
    }
    return SHL(r, K64(sh));
}
/* clz64/clz32(x): x ? __builtin_clz(x) : width, as a u32; the builtin is undefined at 0, so it only runs when x isn't */
static V clz(SnxJit* j, V x, unsigned bits) {
    gcc_jit_lvalue* r = jit_local(j, j->t_u32);
    gcc_jit_block *count = jit_block(j), *done = jit_block(j);
    V arg = jit_cast(j, x, jit_uint(j, bits));
    jit_assign(j, r, K32(bits));
    jit_branch(j, NE(arg, jit_k(j, jit_uint(j, bits), 0)), count, done);
    jit_enter(j, count);
    jit_assign(j, r, jit_call(j, jit_builtin(j, bits == 64 ? "__builtin_clzll" : "__builtin_clz"), 1, &arg));
    jit_goto(j, done);
    jit_enter(j, done);
    return jit_val(j, gcc_jit_lvalue_as_rvalue(r));
}
/* cls64/cls32(x): clz(x ^ (x >> 1 arithmetic)) - 1 */
static V cls(SnxJit* j, V x, unsigned bits) {
    T t = jit_uint(j, bits);
    x = jit_cast(j, x, t);
    return SUB(clz(j, XOR(x, asr(j, x, bits, jit_k(j, t, 1))), bits), K32(1));
}
static V bswap(SnxJit* j, V x, unsigned bits) {
    char name[24];
    sprintf(name, "__builtin_bswap%u", bits);
    x = jit_cast(j, x, jit_uint(j, bits));
    return jit_cast(j, jit_call(j, jit_builtin(j, name), 1, &x), jit_uint(j, bits));
}
static V swap_pairs(SnxJit* j, V x, unsigned shift, SnxU64 mask) {
    V m = K64(mask), s = K64(shift);
    return OR(AND(SHR(x, s), m), SHL(AND(x, m), s));
}
/* rbit64(x): swap bits, pairs and nibbles, then bswap64 */
static V bit_reverse(SnxJit* j, V x) {
    x = U64(x);
    x = swap_pairs(j, x, 1, ((SnxU64)0x55555555UL << 32) | 0x55555555UL);
    x = swap_pairs(j, x, 2, ((SnxU64)0x33333333UL << 32) | 0x33333333UL);
    x = swap_pairs(j, x, 4, ((SnxU64)0x0F0F0F0FUL << 32) | 0x0F0F0F0FUL);
    return bswap(j, x, 64);
}
static V rev16(SnxJit* j, V x) { return swap_pairs(j, U64(x), 8, ((SnxU64)0x00FF00FFUL << 32) | 0x00FF00FFUL); }
static V rev32(SnxJit* j, V x) {
    V y = jit_val(j, bswap(j, x, 64));
    return OR(SHR(y, K64(32)), SHL(y, K64(32)));
}
/* udiv(a,b): b ? a/b : 0 */
static V udiv(SnxJit* j, V a, V b, unsigned bits) {
    T t = jit_uint(j, bits);
    gcc_jit_lvalue* r = jit_local(j, t);
    gcc_jit_block *divide = jit_block(j), *done = jit_block(j);
    a = jit_cast(j, a, t);
    b = jit_cast(j, b, t);
    jit_assign(j, r, jit_k(j, t, 0));
    jit_branch(j, NE(b, jit_k(j, t, 0)), divide, done);
    jit_enter(j, divide);
    jit_assign(j, r, DIV(a, b));
    jit_goto(j, done);
    jit_enter(j, done);
    return jit_val(j, gcc_jit_lvalue_as_rvalue(r));
}
/* sdiv(a,b): !b -> 0, b==-1 -> 0-a (no INT_MIN/-1 trap), else a/b */
static V sdiv(SnxJit* j, V a, V b, unsigned bits) {
    T t = jit_uint(j, bits), s = jit_sint(j, bits);
    gcc_jit_lvalue* r = jit_local(j, t);
    gcc_jit_block *test = jit_block(j), *negate = jit_block(j), *divide = jit_block(j), *done = jit_block(j);
    a = jit_cast(j, a, t);
    b = jit_cast(j, b, t);
    jit_assign(j, r, jit_k(j, t, 0));
    jit_branch(j, NE(b, jit_k(j, t, 0)), test, done);
    jit_enter(j, test);
    jit_branch(j, EQ(b, jit_k(j, t, ~(SnxU64)0)), negate, divide);
    jit_enter(j, negate);
    jit_assign(j, r, SUB(jit_k(j, t, 0), a));
    jit_goto(j, done);
    jit_enter(j, divide);
    jit_assign(j, r, jit_cast(j, jit_op(j, GCC_JIT_BINARY_OP_DIVIDE, jit_cast(j, a, s), jit_cast(j, b, s)), t));
    jit_goto(j, done);
    jit_enter(j, done);
    return jit_val(j, gcc_jit_lvalue_as_rvalue(r));
}
/* umulh64(a,b): (u64)(((u128)a * b) >> 64) */
static V umulh(SnxJit* j, V a, V b) {
    V wide = MUL(jit_cast(j, a, j->t_u128), jit_cast(j, b, j->t_u128));
    return U64(SHR(wide, jit_k(j, j->t_u128, 64)));
}
/* smulh64(a,b): umulh64, minus b when a < 0, minus a when b < 0 */
static V smulh(SnxJit* j, V a, V b) {
    V r = umulh(j, a, b), s63 = K64(63);
    r = SUB(r, AND(asr(j, a, 64, s63), b));
    return SUB(r, AND(asr(j, b, 64, s63), a));
}
/* ubfm_/sbfm_/bfm_(..., immr, imms, ds) for constant positions; shifts stay below 64 for every encoding */
static V ubfm(SnxJit* j, V src, unsigned immr, unsigned imms, unsigned ds) {
    if (imms >= immr) return AND(SHR(src, K64(immr & 63)), K64(low_mask(imms - immr + 1)));
    return SHL(AND(src, K64(low_mask(imms + 1))), K64((ds - immr) & 63));
}
static V sbfm(SnxJit* j, V src, unsigned immr, unsigned imms, unsigned ds) {
    unsigned top = imms >= immr ? imms - immr : ds - immr + imms;
    V r = ubfm(j, src, immr, imms, ds);
    /* if((r>>top)&1) r|=~bmask(top+1): r has no bits above top, so this sign-extends from bit top */
    if (top < 63) r = asr(j, SHL(r, K64(63 - top)), 64, K64(63 - top));
    return ds == 32 ? zext32(j, r) : r;
}
static V bfm(SnxJit* j, V dst, V src, unsigned immr, unsigned imms, unsigned ds) {
    V r;
    if (imms >= immr) {
        SnxU64 m = low_mask(imms - immr + 1);
        r = OR(AND(dst, K64(~m)), AND(SHR(src, K64(immr & 63)), K64(m)));
    } else {
        unsigned p = (ds - immr) & 63;
        SnxU64 m = low_mask(imms + 1) << p;
        r = OR(AND(dst, K64(~m)), AND(SHL(src, K64(p)), K64(m)));
    }
    return ds == 32 ? zext32(j, r) : r;
}
/* c->excl_on && c->excl_addr==a && c->excl_size==size */
static V exclusive_match(SnxJit* j, V a, unsigned size) {
    V on = jit_ld(j, offsetof(CPU, excl_on), j->t_u32), address = jit_ld(j, offsetof(CPU, excl_addr), j->t_u64),
      have = jit_ld(j, offsetof(CPU, excl_size), j->t_u32);
    return jit_land(j, jit_land(j, NE(on, K32(0)), EQ(address, a)), EQ(have, K32(size)));
}
/* ldx(c,M,a,sz) */
static V load_exclusive(SnxJit* j, V a, unsigned sz) {
    V v = U64(jit_ldacq(j, a, 8u << sz));
    jit_st(j, offsetof(CPU, excl_addr), j->t_u64, a);
    jit_st(j, offsetof(CPU, excl_val), j->t_u64, v);
    jit_st(j, offsetof(CPU, excl_size), j->t_u32, K32(sz));
    jit_st(j, offsetof(CPU, excl_on), j->t_u32, K32(1));
    return v;
}
/* stx/stxp's tail: the store happened when ok; c->excl_on=0; return !ok */
static V exclusive_done(SnxJit* j, gcc_jit_lvalue* ok) {
    jit_st(j, offsetof(CPU, excl_on), j->t_u32, K32(0));
    return jit_val(j, U32(EQ(gcc_jit_lvalue_as_rvalue(ok), K32(0))));
}
/* stx(c,M,a,sz,v) */
static V store_exclusive(SnxJit* j, V a, unsigned sz, V v) {
    gcc_jit_lvalue* ok = jit_local(j, j->t_u32);
    gcc_jit_block *attempt = jit_block(j), *finish = jit_block(j);
    jit_assign(j, ok, K32(0));
    jit_branch(j, exclusive_match(j, a, sz), attempt, finish);
    jit_enter(j, attempt);
    jit_assign(j, ok, jit_cas(j, a, 8u << sz, jit_ld(j, offsetof(CPU, excl_val), j->t_u64), v));
    jit_goto(j, finish);
    jit_enter(j, finish);
    return exclusive_done(j, ok);
}
/* stxp(c,M,a,sz,v1,v2) */
static V store_exclusive_pair(SnxJit* j, V a, unsigned sz, V v1, V v2) {
    gcc_jit_lvalue* ok = jit_local(j, j->t_u32);
    gcc_jit_block *attempt = jit_block(j), *finish = jit_block(j);
    jit_assign(j, ok, K32(0));
    jit_branch(j, exclusive_match(j, a, sz | 0x10), attempt, finish);
    jit_enter(j, attempt);
    if (sz == 2)
        jit_assign(
            j, ok,
            jit_cas(j, a, 64, jit_ld(j, offsetof(CPU, excl_val), j->t_u64), OR(zext32(j, v1), SHL(U64(v2), K64(32)))));
    else {
        gcc_jit_type* params[5];
        V args[5];
        params[0] = j->t_ptr;
        params[1] = params[2] = params[3] = params[4] = j->t_u64;
        args[0] = jit_mp(j, a);
        args[1] = jit_ld(j, offsetof(CPU, excl_val), j->t_u64);
        args[2] = jit_ld(j, offsetof(CPU, excl_val2), j->t_u64);
        args[3] = v1;
        args[4] = v2;
        jit_assign(j, ok, jit_call(j, jit_extern(j, "snx_cas128", j->t_u32, 5, params), 5, args));
    }
    jit_goto(j, finish);
    jit_enter(j, finish);
    return exclusive_done(j, ok);
}

/* snx_decode_bitmask (helpers/integer.c), evaluated here: its arguments are fields of the constant word */
static SnxU64 decode_bitmask(unsigned n, unsigned imms, unsigned immr, unsigned ds) {
    unsigned v = (n << 6) | (~imms & 0x3F), esize, levels, s, r;
    int len = -1, i;
    SnxU64 welem, result = 0, mask;
    for (i = 6; i >= 0; i--)
        if (v & (1u << i)) {
            len = i;
            break;
        }
    if (len < 1) return 0;
    esize = 1u << len;
    levels = esize - 1;
    s = imms & levels;
    r = immr & levels;
    if (s == levels) return 0;
    welem = s + 1 >= 64 ? ~(SnxU64)0 : ((SnxU64)1 << (s + 1)) - 1;
    mask = esize >= 64 ? ~(SnxU64)0 : ((SnxU64)1 << esize) - 1;
    if (r) welem = ((welem >> r) | (welem << (esize - r))) & mask;
    for (v = 0; v < ds; v += esize) result |= welem << v;
    return ds == 32 ? (SnxU32)result : result;
}

/* ---- data processing, immediate ---- */

static void adr(SnxJit* j, SnxU32 w, SnxU64 pc, int page) {
    SnxU64 immhi = SF(8, 13), immlo = F(29, 2), offset = (immhi << 2) | immlo;
    jit_setx(j, F(0, 5), K64(page ? (pc & ~(SnxU64)0xFFF) + (offset << 12) : pc + offset));
}

static void addsub_imm(SnxJit* j, SnxU32 w, size_t index) {
    unsigned sh = index & 1, s = (index >> 1) & 1, op = (index >> 2) & 1, sf = (unsigned)(index >> 3);
    unsigned rd = F(0, 5), rn = F(5, 5);
    SnxU32 imm = sh ? F(10, 12) << 12 : F(10, 12);
    V a = sf ? jit_x(j, rn) : x32(j, rn), k = sf ? K64(imm) : K32(imm);
    if (!s)
        jit_set_x(j, rd, U64(op ? SUB(a, k) : ADD(a, k)));
    else
        jit_setx(j, rd, U64(op ? subs(j, sf, a, k) : adds(j, sf, a, k, NULL)));
}

static V logic_op(SnxJit* j, unsigned opc, V a, V b) {
    switch (opc) {
        case 1: return OR(a, b);
        case 2: return XOR(a, b);
        default: return AND(a, b);
    }
}

static void logic_imm(SnxJit* j, SnxU32 w, size_t index) {
    unsigned opc = index & 3, sf = (unsigned)(index >> 2), rd = F(0, 5), rn = F(5, 5);
    V mask = K64(decode_bitmask(F(22, 1), F(10, 6), F(16, 6), sf ? 64 : 32)), r;
    /* (u32)RX(rn) & mask: the u32 widens to the u64 mask */
    r = jit_val(j, logic_op(j, opc, sf ? jit_rx(j, rn) : U64(rx32(j, rn)), mask));
    if (!sf) r = zext32(j, r);
    if (opc == 3) {
        logic_flags(j, sf, r);
        jit_setx(j, rd, r);
    } else
        jit_set_x(j, rd, r);
}

static void movw(SnxJit* j, SnxU32 w, size_t index) {
    unsigned opc = index & 3, sf = (unsigned)(index >> 2), rd = F(0, 5), shift = F(21, 2) * 16;
    SnxU64 imm = (SnxU64)F(5, 16) << shift, keep = ~((SnxU64)0xFFFF << shift);
    V r;
    if (opc == 0)
        r = K64(sf ? ~imm : (SnxU32)~imm);
    else if (opc == 2)
        r = K64(sf ? imm : (SnxU32)imm);
    else {
        r = OR(AND(jit_rx(j, rd), K64(keep)), K64(imm));
        if (!sf) r = zext32(j, r);
    }
    jit_setx(j, rd, r);
}

static void bitfield(SnxJit* j, SnxU32 w, size_t index) {
    unsigned opc = index & 3, sf = (unsigned)(index >> 2), rd = F(0, 5), rn = F(5, 5);
    unsigned immr = F(16, 6), imms = F(10, 6), ds = sf ? 64 : 32;
    V src = sf ? jit_rx(j, rn) : U64(rx32(j, rn)), r;
    if (opc == 0)
        r = sbfm(j, src, immr, imms, ds);
    else if (opc == 1)
        r = bfm(j, sf ? jit_rx(j, rd) : U64(rx32(j, rd)), src, immr, imms, ds);
    else
        r = ubfm(j, src, immr, imms, ds);
    jit_setx(j, rd, sf ? r : zext32(j, r));
}

static void extr(SnxJit* j, SnxU32 w, size_t index) {
    unsigned imms = F(10, 6), rd = F(0, 5), rm = F(16, 5), rn = F(5, 5), bits = index ? 64 : 32;
    T t = jit_uint(j, bits);
    V m = jit_cast(j, jit_rx(j, rm), t), r;
    if (imms)
        r = OR(SHR(m, jit_k(j, t, imms)), SHL(jit_cast(j, jit_rx(j, rn), t), jit_k(j, t, (bits - imms) & (bits - 1))));
    else
        r = m;
    jit_setx(j, rd, U64(r));
}

/* ---- system ---- */

static void hint(SnxJit* j, SnxU32 w) {
    unsigned k = (F(8, 4) << 3) | F(5, 3);
    gcc_jit_type* p = j->t_ptr;
    V c = j->c;
    if (k == 1) /* YIELD(): a compiler barrier (cpu.h adds x86's PAUSE, a hint without semantics) */
        jit_barrier(j);
    else if (k == 2)
        jit_rt_call(j, jit_rt_wfe(), j->t_void, 1, &p, &c);
    else if (k == 4 || k == 5)
        jit_rt_call(j, jit_rt_sev(), j->t_void, 0, NULL, NULL);
}

static void sys(SnxJit* j, SnxU32 w) {
    if (F(16, 3) == 3 && F(12, 4) == 7 && F(8, 4) == 4 && F(5, 3) == 1) { /* DC ZVA: memset(MP(a&~63),0,64) */
        V a = jit_val(j, AND(jit_rx(j, F(0, 5)), K64(~(SnxU64)63)));
        unsigned i;
        for (i = 0; i < 8; ++i) jit_wr(j, ADD(a, K64(8 * i)), 64, K64(0));
    }
}

static void mrs(SnxJit* j, SnxU32 w) {
    gcc_jit_type* p[2];
    V args[2];
    p[0] = j->t_ptr;
    p[1] = j->t_u32;
    args[0] = j->c;
    args[1] = K32(F(5, 15));
    jit_setx(j, F(0, 5), jit_call(j, jit_extern(j, "snx_mrs", j->t_u64, 2, p), 2, args));
}

static void msr(SnxJit* j, SnxU32 w) {
    gcc_jit_type* p[3];
    V args[3];
    p[0] = j->t_ptr;
    p[1] = j->t_u32;
    p[2] = j->t_u64;
    args[0] = j->c;
    args[1] = K32(F(5, 15));
    args[2] = jit_rx(j, F(0, 5));
    jit_call(j, jit_extern(j, "snx_msr", j->t_void, 3, p), 3, args);
}

/* ---- loads and stores ---- */

/* RDQ(c->v[rt],a) / WRQ(a,c->v[rt]): 16 bytes as two 8-byte guest accesses, both loaded before either store */
static void rdq(SnxJit* j, unsigned rt, V a) {
    V lo = jit_rd(j, a, 64), hi = jit_rd(j, ADD(a, K64(8)), 64);
    jit_set_vd(j, rt, 0, lo);
    jit_set_vd(j, rt, 1, hi);
}
static void wrq(SnxJit* j, V a, unsigned rt) {
    V lo = jit_vd(j, rt, 0), hi = jit_vd(j, rt, 1);
    jit_wr(j, a, 64, lo);
    jit_wr(j, ADD(a, K64(8)), 64, hi);
}

/* The single-register access of (size, V, lopc), as LDST_UIMM/IMM9/REG specialize it: rt from/to address */
static void access(SnxJit* j, unsigned size, unsigned v, unsigned lopc, unsigned rt, V address) {
    unsigned bits = 8u << size;
    if (!v && size == 3 && lopc == 2) return; /* PRFM: no access (pre/post-index forms still write back) */
    if (!v) {
        if (lopc == 0)
            jit_wr(j, address, bits, jit_rx(j, rt));
        else if (lopc == 1)
            jit_setx(j, rt, U64(jit_rd(j, address, bits)));
        else if (lopc == 2)
            jit_setx(j, rt, sext64(j, jit_rd(j, address, bits), bits));
        else /* (u64)(u32)(i32)(iN) */
            jit_setx(j, rt, U64(U32(jit_cast(j, jit_cast(j, jit_rd(j, address, bits), jit_sint(j, bits)), j->t_s32))));
    } else if (lopc >= 2) { /* size 0: the 128-bit register */
        if (lopc == 2)
            wrq(j, address, rt);
        else
            rdq(j, rt, address);
    } else if (lopc == 0)
        jit_wr(j, address, bits, jit_vd(j, rt, 0));
    else {
        V t = U64(jit_rd(j, address, bits));
        jit_set_vd(j, rt, 1, K64(0));
        jit_set_vd(j, rt, 0, t);
    }
}

static void ldst_uimm(SnxJit* j, SnxU32 w, size_t index) {
    unsigned lopc = index & 3, v = (index >> 2) & 1, size = (unsigned)(index >> 3);
    unsigned shift = v && !size && lopc >= 2 ? 4 : size;
    access(j, size, v, lopc, F(0, 5), jit_val(j, ADD(jit_x(j, F(5, 5)), K64((SnxU64)F(10, 12) << shift))));
}

static void ldst_imm9(SnxJit* j, SnxU32 w, size_t index) {
    unsigned mode = index & 3, lopc = (index >> 2) & 3, v = (index >> 4) & 1, size = (unsigned)(index >> 5);
    unsigned rn = F(5, 5), rt = F(0, 5);
    SnxU64 imm = SF(11, 23);
    V a;
    if (mode == 1) { /* post-index */
        a = jit_x(j, rn);
        access(j, size, v, lopc, rt, a);
        jit_set_x(j, rn, ADD(a, K64(imm)));
    } else if (mode == 3) { /* pre-index */
        a = jit_val(j, ADD(jit_x(j, rn), K64(imm)));
        access(j, size, v, lopc, rt, a);
        jit_set_x(j, rn, a);
    } else
        access(j, size, v, lopc, rt, jit_val(j, ADD(jit_x(j, rn), K64(imm))));
}

static void ldst_reg(SnxJit* j, SnxU32 w, size_t index) {
    unsigned lopc = index & 3, v = (index >> 2) & 1, size = (unsigned)(index >> 3);
    unsigned shift = v && !size && lopc >= 2 ? 4 : size;
    V offset = extend(j, jit_rx(j, F(16, 5)), F(13, 3), F(12, 1) ? shift : 0);
    access(j, size, v, lopc, F(0, 5), jit_val(j, ADD(jit_x(j, F(5, 5)), offset)));
}

static void ldr_lit(SnxJit* j, SnxU32 w, SnxU64 pc, size_t index) {
    unsigned rt = F(0, 5);
    V a = K64(pc + SF(8, 13) * 4), t;
    switch (index) {
        case 0: jit_setx(j, rt, U64(jit_rd(j, a, 32))); break;
        case 2: jit_setx(j, rt, jit_rd(j, a, 64)); break;
        case 4: jit_setx(j, rt, sext64(j, jit_rd(j, a, 32), 32)); break;
        case 5: rdq(j, rt, a); break;
        default: /* 1, 3: { u64 t_=RDn(..); c->v[rt].d[1]=0; c->v[rt].d[0]=t_; } */
            t = U64(jit_rd(j, a, index == 1 ? 32 : 64));
            jit_set_vd(j, rt, 1, K64(0));
            jit_set_vd(j, rt, 0, t);
            break;
    }
}

static void ldstp(SnxJit* j, SnxU32 w, size_t index) {
    unsigned l = index & 1, mode = (index >> 1) & 3, v = (index >> 3) & 1, opc = (unsigned)(index >> 4);
    unsigned rn = F(5, 5), rt = F(0, 5), rt2 = F(10, 5), bytes, bits;
    SnxU64 offset;
    V a, a2, v1, v2;
    bytes = v ? 4u << opc : (opc == 2 ? 8 : 4);
    bits = bytes * 8;
    offset = SF(10, 25) * bytes;
    a = jit_val(j, mode == 1 ? jit_x(j, rn) : ADD(jit_x(j, rn), K64(offset)));
    a2 = ADD(a, K64(bytes));
    if (v && bytes == 16) {
        if (!l) {
            wrq(j, a, rt);
            wrq(j, a2, rt2);
        } else { /* V128 q1_,q2_; RDQ(q1_,a_); RDQ(q2_,a_+16); c->v[rt]=q1_; c->v[rt2]=q2_; */
            V q[4];
            q[0] = jit_rd(j, a, 64);
            q[1] = jit_rd(j, ADD(a, K64(8)), 64);
            q[2] = jit_rd(j, a2, 64);
            q[3] = jit_rd(j, ADD(a2, K64(8)), 64);
            jit_set_vd(j, rt, 0, q[0]);
            jit_set_vd(j, rt, 1, q[1]);
            jit_set_vd(j, rt2, 0, q[2]);
            jit_set_vd(j, rt2, 1, q[3]);
        }
    } else if (!l) {
        V first = v ? jit_vd(j, rt, 0) : jit_rx(j, rt);
        jit_wr(j, a, bits, first);
        jit_wr(j, a2, bits, v ? jit_vd(j, rt2, 0) : jit_rx(j, rt2));
    } else {
        int sign = !v && opc == 1; /* LDPSW */
        v1 = jit_rd(j, a, bits);
        v2 = jit_rd(j, a2, bits);
        v1 = sign ? sext64(j, v1, bits) : U64(v1);
        v2 = sign ? sext64(j, v2, bits) : U64(v2);
        if (v) {
            jit_set_vd(j, rt, 0, v1);
            jit_set_vd(j, rt, 1, K64(0));
            jit_set_vd(j, rt2, 0, v2);
            jit_set_vd(j, rt2, 1, K64(0));
        } else {
            jit_setx(j, rt, v1);
            jit_setx(j, rt2, v2);
        }
    }
    if (mode == 1)
        jit_set_x(j, rn, ADD(a, K64(offset)));
    else if (mode == 3)
        jit_set_x(j, rn, a);
}

static void ldstx(SnxJit* j, SnxU32 w, size_t index) {
    unsigned o1 = index & 1, l = (index >> 1) & 1, o2 = (index >> 2) & 1, size = (unsigned)(index >> 3);
    unsigned rn = F(5, 5), rt = F(0, 5), rs = F(16, 5), rt2 = F(10, 5);
    V a = jit_x(j, rn);
    if (o2) {
        if (l)
            jit_setx(j, rt, U64(jit_ldacq(j, a, 8u << size)));
        else
            jit_strel(j, a, 8u << size, jit_rx(j, rt));
    } else if (!o1) {
        if (l)
            jit_setx(j, rt, load_exclusive(j, a, size));
        else
            jit_setx(j, rs, U64(store_exclusive(j, a, size, jit_rx(j, rt))));
    } else if (l) { /* ldxp(c,M,a,sz,&v1,&v2); SETX(rt,v1); SETX(rt2,v2); */
        V v1, v2;
        if (size == 2) {
            V t = jit_ldacq(j, a, 64);
            v1 = zext32(j, t);
            v2 = SHR(t, K64(32));
            jit_st(j, offsetof(CPU, excl_val), j->t_u64, t);
            jit_st(j, offsetof(CPU, excl_val2), j->t_u64, K64(0));
        } else {
            v1 = jit_ldacq(j, a, 64);
            v2 = jit_ldacq(j, ADD(a, K64(8)), 64);
            jit_st(j, offsetof(CPU, excl_val), j->t_u64, v1);
            jit_st(j, offsetof(CPU, excl_val2), j->t_u64, v2);
        }
        jit_st(j, offsetof(CPU, excl_addr), j->t_u64, a);
        jit_st(j, offsetof(CPU, excl_size), j->t_u32, K32(size | 0x10));
        jit_st(j, offsetof(CPU, excl_on), j->t_u32, K32(1));
        jit_setx(j, rt, v1);
        jit_setx(j, rt2, v2);
    } else {
        V v1 = jit_rx(j, rt), v2 = jit_rx(j, rt2);
        jit_setx(j, rs, U64(store_exclusive_pair(j, a, size, v1, v2)));
    }
}

/* ---- data processing, register ---- */

static void logic_reg(SnxJit* j, SnxU32 w, size_t index) {
    unsigned n = index & 1, opc = (index >> 1) & 3, sf = (unsigned)(index >> 3);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5), bits = sf ? 64 : 32;
    V operand = shf(j, jit_rx(j, rm), F(22, 2), F(10, 6), bits), r;
    if (n) operand = NOT(operand);
    r = jit_val(j, logic_op(j, opc, jit_cast(j, jit_rx(j, rn), jit_uint(j, bits)), operand));
    if (opc == 3) logic_flags(j, sf, r);
    jit_setx(j, rd, U64(r));
}

static void addsub_sreg(SnxJit* j, SnxU32 w, size_t index) {
    unsigned s = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5), bits = sf ? 64 : 32;
    V a = jit_cast(j, jit_rx(j, rn), jit_uint(j, bits)), b = shf(j, jit_rx(j, rm), F(22, 2), F(10, 6), bits);
    if (!s)
        jit_setx(j, rd, U64(op ? SUB(a, b) : ADD(a, b)));
    else
        jit_setx(j, rd, U64(op ? subs(j, sf, a, b) : adds(j, sf, a, b, NULL)));
}

static void addsub_ext(SnxJit* j, SnxU32 w, size_t index) {
    unsigned s = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    V e = jit_val(j, extend(j, jit_rx(j, rm), F(13, 3), F(10, 3))), a, b;
    a = sf ? jit_x(j, rn) : x32(j, rn);
    b = sf ? e : U32(e);
    if (!s)
        jit_set_x(j, rd, U64(op ? SUB(a, b) : ADD(a, b)));
    else
        jit_setx(j, rd, U64(op ? subs(j, sf, a, b) : adds(j, sf, a, b, NULL)));
}

static void adc(SnxJit* j, SnxU32 w, size_t index) {
    unsigned s = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    T t = width_type(j, sf);
    V a = jit_cast(j, jit_rx(j, rn), t), b = jit_cast(j, jit_rx(j, rm), t), cf = jit_ld(j, offsetof(CPU, cf), j->t_u32);
    if (op) b = NOT(b); /* sbc(s)(c,a,b,cin) is adc(s)(c,a,~b,cin) */
    if (!s)
        jit_setx(j, rd, U64(ADD(ADD(a, b), jit_cast(j, cf, t))));
    else
        jit_setx(j, rd, U64(adds(j, sf, a, b, cf)));
}

static void ccmp(SnxJit* j, SnxU32 w, size_t index) {
    unsigned imm = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2), rn = F(5, 5), rm = F(16, 5);
    gcc_jit_block *compare = jit_block(j), *flags = jit_block(j), *done = jit_block(j);
    T t = width_type(j, sf);
    V a, b;
    jit_branch(j, condition(j, F(12, 4)), compare, flags);
    jit_enter(j, compare);
    a = jit_cast(j, jit_rx(j, rn), t);
    b = imm ? jit_k(j, t, rm) : jit_cast(j, jit_rx(j, rm), t);
    if (op)
        subs(j, sf, a, b);
    else
        adds(j, sf, a, b, NULL);
    jit_goto(j, done);
    jit_enter(j, flags);
    nzcv_from(j, F(0, 4));
    jit_goto(j, done);
    jit_enter(j, done);
}

static void csel(SnxJit* j, SnxU32 w, size_t index) {
    unsigned o2 = index & 1, op = (index >> 1) & 1, sf = (unsigned)(index >> 2);
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    T t = width_type(j, sf);
    V ok = condition(j, F(12, 4)), a = jit_cast(j, jit_rx(j, rn), t), b = jit_cast(j, jit_rx(j, rm), t);
    if (!op && o2)
        b = ADD(b, jit_k(j, t, 1));
    else if (op && !o2)
        b = NOT(b);
    else if (op && o2)
        b = SUB(jit_k(j, t, 0), b);
    jit_setx(j, rd, U64(jit_sel(j, ok, a, b)));
}

static int dp1(SnxJit* j, SnxU32 w, size_t index) {
    unsigned rd = F(0, 5), rn = F(5, 5);
    V r;
    switch (index) {
        case 0: r = U64(U32(SHR(bit_reverse(j, U64(rx32(j, rn))), K64(32)))); break;
        case 1: r = zext32(j, rev16(j, U64(rx32(j, rn)))); break;
        case 2: r = U64(bswap(j, rx32(j, rn), 32)); break;
        case 4: r = U64(clz(j, rx32(j, rn), 32)); break;
        case 5: r = U64(cls(j, rx32(j, rn), 32)); break;
        case 64: r = bit_reverse(j, jit_rx(j, rn)); break;
        case 65: r = rev16(j, jit_rx(j, rn)); break;
        case 66: r = rev32(j, jit_rx(j, rn)); break;
        case 67: r = bswap(j, jit_rx(j, rn), 64); break;
        case 68: r = U64(clz(j, jit_rx(j, rn), 64)); break;
        case 69: r = U64(cls(j, jit_rx(j, rn), 64)); break;
        default: return 0;
    }
    jit_setx(j, rd, r);
    return 1;
}

/* snx_crc32(acc, value, bytes, castagnoli), in the runtime library */
static V crc32(SnxJit* j, V acc, V value, unsigned bytes, unsigned castagnoli) {
    gcc_jit_type* p[4];
    V args[4];
    p[0] = j->t_u32;
    p[1] = j->t_u64;
    p[2] = p[3] = j->t_int;
    args[0] = acc;
    args[1] = U64(value);
    args[2] = jit_int(j, (int)bytes);
    args[3] = jit_int(j, (int)castagnoli);
    return jit_call(j, jit_extern(j, "snx_crc32", j->t_u32, 4, p), 4, args);
}

static int dp2(SnxJit* j, SnxU32 w, size_t index) {
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5);
    V r;
    switch (index) {
        case 2: r = U64(udiv(j, rx32(j, rn), rx32(j, rm), 32)); break;
        case 3: r = U64(sdiv(j, rx32(j, rn), rx32(j, rm), 32)); break;
        case 8: r = U64(SHL(rx32(j, rn), AND(rx32(j, rm), K32(31)))); break;
        case 9: r = U64(SHR(rx32(j, rn), AND(rx32(j, rm), K32(31)))); break;
        case 10: r = U64(asr(j, rx32(j, rn), 32, AND(rx32(j, rm), K32(31)))); break;
        case 11: r = U64(ror(j, rx32(j, rn), rx32(j, rm), 32)); break;
        case 16:
        case 17:
        case 18:
        case 20:
        case 21:
        case 22: r = U64(crc32(j, rx32(j, rn), rx32(j, rm), 1u << (index & 3), index >= 20)); break;
        case 66: r = udiv(j, jit_rx(j, rn), jit_rx(j, rm), 64); break;
        case 67: r = sdiv(j, jit_rx(j, rn), jit_rx(j, rm), 64); break;
        case 72: r = SHL(jit_rx(j, rn), AND(jit_rx(j, rm), K64(63))); break;
        case 73: r = SHR(jit_rx(j, rn), AND(jit_rx(j, rm), K64(63))); break;
        case 74: r = asr(j, jit_rx(j, rn), 64, AND(jit_rx(j, rm), K64(63))); break;
        case 75: r = ror(j, jit_rx(j, rn), U64(rx32(j, rm)), 64); break;
        case 83:
        case 87: r = U64(crc32(j, rx32(j, rn), jit_rx(j, rm), 8, index == 87)); break;
        default: return 0;
    }
    jit_setx(j, rd, r);
    return 1;
}

static int dp3(SnxJit* j, SnxU32 w, size_t index) {
    unsigned rd = F(0, 5), rn = F(5, 5), rm = F(16, 5), ra = F(10, 5);
    V r, product;
    switch (index) {
        case 0:
        case 1:
            product = MUL(rx32(j, rn), rx32(j, rm));
            r = U64(index ? SUB(rx32(j, ra), product) : ADD(rx32(j, ra), product));
            break;
        case 16:
        case 17:
            product = MUL(jit_rx(j, rn), jit_rx(j, rm));
            r = index == 17 ? SUB(jit_rx(j, ra), product) : ADD(jit_rx(j, ra), product);
            break;
        case 18:
        case 19:
        case 26:
        case 27: {
            int sign = index < 20;
            V a = sign ? sext64(j, rx32(j, rn), 32) : U64(rx32(j, rn)),
              b = sign ? sext64(j, rx32(j, rm), 32) : U64(rx32(j, rm));
            product = MUL(a, b);
            r = index & 1 ? SUB(jit_rx(j, ra), product) : ADD(jit_rx(j, ra), product);
            break;
        }
        case 20: r = smulh(j, jit_rx(j, rn), jit_rx(j, rm)); break;
        case 28: r = umulh(j, jit_rx(j, rn), jit_rx(j, rm)); break;
        default: return 0;
    }
    jit_setx(j, rd, r);
    return 1;
}

/* ---- dispatch ---- */

int snx_jit_plain(SnxJit* j, SnxU32 w, SnxU64 pc, const SnxA64Case* instruction) {
    size_t index;
    const SnxA64Pattern* pattern = snx_a64_case_pattern(instruction, &index);
    const char* name;
    if (!pattern) return 0;
    /* snx_a64_static_step and codegen.c try the SIMD shortcuts first, for every PLAIN word */
    if (snx_jit_fast_simd(j, w)) return 1;
    if (!instruction->first[0]) return 1; /* an empty body: PRFM, MSR (immediate), ... */
    name = pattern->name;
    if (!strcmp(name, "ADR"))
        adr(j, w, pc, 0);
    else if (!strcmp(name, "ADRP"))
        adr(j, w, pc, 1);
    else if (!strcmp(name, "ADDSUB_IMM"))
        addsub_imm(j, w, index);
    else if (!strcmp(name, "LOGIC_IMM"))
        logic_imm(j, w, index);
    else if (!strcmp(name, "MOVW"))
        movw(j, w, index);
    else if (!strcmp(name, "BITFIELD"))
        bitfield(j, w, index);
    else if (!strcmp(name, "EXTR"))
        extr(j, w, index);
    else if (!strcmp(name, "HINT"))
        hint(j, w);
    else if (!strcmp(name, "CLREX"))
        jit_st(j, offsetof(CPU, excl_on), j->t_u32, K32(0));
    else if (!strcmp(name, "BARRIER"))
        jit_fence(j);
    else if (!strcmp(name, "SYS"))
        sys(j, w);
    else if (!strcmp(name, "MRS"))
        mrs(j, w);
    else if (!strcmp(name, "MSR"))
        msr(j, w);
    else if (!strcmp(name, "LDST_UIMM"))
        ldst_uimm(j, w, index);
    else if (!strcmp(name, "LDST_IMM9"))
        ldst_imm9(j, w, index);
    else if (!strcmp(name, "LDST_REG"))
        ldst_reg(j, w, index);
    else if (!strcmp(name, "LDR_LIT"))
        ldr_lit(j, w, pc, index);
    else if (!strcmp(name, "LDSTP"))
        ldstp(j, w, index);
    else if (!strcmp(name, "LDSTX"))
        ldstx(j, w, index);
    else if (!strcmp(name, "LOGIC_REG"))
        logic_reg(j, w, index);
    else if (!strcmp(name, "ADDSUB_SREG"))
        addsub_sreg(j, w, index);
    else if (!strcmp(name, "ADDSUB_EXT"))
        addsub_ext(j, w, index);
    else if (!strcmp(name, "ADC"))
        adc(j, w, index);
    else if (!strcmp(name, "CCMP"))
        ccmp(j, w, index);
    else if (!strcmp(name, "CSEL"))
        csel(j, w, index);
    else if (!strcmp(name, "DP1"))
        return dp1(j, w, index);
    else if (!strcmp(name, "DP2"))
        return dp2(j, w, index);
    else if (!strcmp(name, "DP3"))
        return dp3(j, w, index);
    else if (!strncmp(name, "SIMD", 4))
        return snx_jit_more_simd(j, w) || snx_jit_simd_plain(j, w, pc, pattern, index);
    else
        return snx_jit_fp_plain(j, w, pc, pattern, index);
    return 1;
}

gcc_jit_rvalue* snx_jit_flow_operand(SnxJit* j, SnxU32 w, SnxU64 pc, const SnxA64Case* instruction) {
    size_t index;
    const SnxA64Pattern* pattern = snx_a64_case_pattern(instruction, &index);
    const char* name = pattern ? pattern->name : "";
    unsigned rt = F(0, 5);
    (void)pc;
    if (!strcmp(name, "BCOND")) return condition(j, F(0, 4));
    if (!strcmp(name, "CBZ")) {
        V value = index & 2 ? jit_rx(j, rt) : U64(rx32(j, rt));
        return jit_val(j, index & 1 ? NE(value, K64(0)) : EQ(value, K64(0)));
    }
    if (!strcmp(name, "TBZ")) {
        V bit = AND(SHR(jit_rx(j, rt), K64((F(31, 1) << 5) | F(19, 5))), K64(1));
        return jit_val(j, index ? NE(bit, K64(0)) : EQ(bit, K64(0)));
    }
    if (!strcmp(name, "BR") || !strcmp(name, "BLR") || !strcmp(name, "RET")) return jit_rx(j, F(5, 5));
    return K32(F(5, 16)); /* SVC, BRK */
}
