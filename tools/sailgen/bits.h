/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What every CPU back end of sailgen shares: the values the generated emitters know while they
   recompile (SgS, bits of 128 at most; integers as long long) and the arithmetic of them, copied at the top of the file
   sailgen writes before the back end's own (x64.h for the x86-64 one's) */

#include "common/utilities/common.h" /* SnxU32, SnxU64 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SnxU64 v;
    unsigned w;
    SnxU64 vh; /* over 64 bits (a vector's Zeros and Ones, a 128-bit constant): the bits from 64 up */
} SgS;

static inline SnxU64 sg_ones(unsigned n) { return n >= 64 ? ~0ull : (1ull << n) - 1; }

/* what the back end can't make: the process ends, but where the includer defines SG_FAIL_JUMP (a jmp_buf* the
   thread's emitter sets, the x86-64 recompiler's), the emitter takes the word back and another back end makes it */
[[noreturn]] static inline void sg_fail(const char* what, long long a, long long b) {
#ifdef SG_FAIL_JUMP
    if (SG_FAIL_JUMP) {
#ifdef SG_FAIL_NOTE
        SG_FAIL_NOTE(what, a, b);
#endif
        longjmp(*SG_FAIL_JUMP, 1);
    }
#endif
    fprintf(stderr, "sailgen back end: %s (%lld, %lld)\n", what, a, b);
    abort();
}

/* ---- static bits ---- */

/* a value of w bits from its low and high doublewords (up to 128 bits) */
static inline SgS sg_s2(SnxU64 v, SnxU64 vh, long long w) {
    SgS s;
    if (w < 0 || w > 128) sg_fail("a static bits value of this width", w, 128);
    s.v = v & sg_ones((unsigned)w);
    s.vh = w > 64 ? vh & sg_ones((unsigned)w - 64) : 0;
    s.w = (unsigned)w;
    return s;
}
static inline SgS sg_s(SnxU64 v, long long w) { return sg_s2(v, 0, w); }
static inline SgS sg_s_zeros(long long w) { return sg_s2(0, 0, w); }
static inline SgS sg_s_ones(long long w) { return sg_s2(~0ull, ~0ull, w); }
/* x's 128 bits shifted left (right) by n */
static inline SgS sgs_shl(SgS x, unsigned n, unsigned w) {
    if (n >= 128) return sg_s2(0, 0, w);
    if (n >= 64) return sg_s2(0, x.v << (n - 64), w);
    return sg_s2(x.v << n, n ? (x.vh << n) | (x.v >> (64 - n)) : x.vh, w);
}
static inline SgS sgs_shr(SgS x, unsigned n, unsigned w) {
    if (n >= 128) return sg_s2(0, 0, w);
    if (n >= 64) return sg_s2(x.vh >> (n - 64), 0, w);
    return sg_s2(n ? (x.v >> n) | (x.vh << (64 - n)) : x.v, x.vh >> n, w);
}
static inline SgS sg_s_concat(SgS a, SgS b) {
    SgS h;
    if (a.w + b.w > 128) sg_fail("a static concatenation over 128 bits", a.w, b.w);
    h = sgs_shl(a, b.w, a.w + b.w);
    return sg_s2(h.v | b.v, h.vh | b.vh, a.w + b.w);
}
static inline SgS sg_s_slice(SgS x, long long hi, long long lo) {
    if (lo < 0 || hi < lo || hi >= (long long)x.w) sg_fail("a static slice out of its value", hi, lo);
    return sgs_shr(x, (unsigned)lo, (unsigned)(hi - lo + 1));
}
static inline SgS sg_s_set_slice(SgS x, long long hi, long long lo, SgS v) {
    SgS mask, put;
    if (lo < 0 || hi < lo || hi >= (long long)x.w || v.w != (unsigned)(hi - lo + 1))
        sg_fail("a static slice assignment out of its value", hi, lo);
    mask = sgs_shl(sg_s_ones(v.w), (unsigned)lo, x.w);
    put = sgs_shl(v, (unsigned)lo, x.w);
    return sg_s2((x.v & ~mask.v) | put.v, (x.vh & ~mask.vh) | put.vh, x.w);
}
static inline SgS sg_s_zext(SgS x, long long w) {
    if (w < (long long)x.w) sg_fail("a static extension to fewer bits", w, x.w);
    return sg_s2(x.v, x.vh, w);
}
static inline SgS sg_s_sext(SgS x, long long w) {
    if (w < (long long)x.w) sg_fail("a static extension to fewer bits", w, x.w);
    if (x.w && (x.w > 64 ? (x.vh >> (x.w - 65)) & 1 : (x.v >> (x.w - 1)) & 1)) {
        SgS fill = sgs_shl(sg_s_ones(w), x.w, (unsigned)w);
        return sg_s2(x.v | fill.v, x.vh | fill.vh, w);
    }
    return sg_s2(x.v, x.vh, w);
}
static inline SgS sg_s_replicate(SgS x, long long n) {
    SgS r = sg_s(0, 0);
    long long i;
    for (i = 0; i < n; ++i) r = sg_s_concat(r, x);
    return r;
}
static inline SgS sg_s_not(SgS x) { return sg_s2(~x.v, ~x.vh, x.w); }
static inline SgS sg_s_and(SgS a, SgS b) { return sg_s2(a.v & b.v, a.vh & b.vh, a.w); }
static inline SgS sg_s_or(SgS a, SgS b) { return sg_s2(a.v | b.v, a.vh | b.vh, a.w); }
static inline SgS sg_s_xor(SgS a, SgS b) { return sg_s2(a.v ^ b.v, a.vh ^ b.vh, a.w); }
static inline SgS sg_s_add(SgS a, SgS b) {
    if (a.w > 64) sg_fail("a static sum over 64 bits", a.w, b.w);
    return sg_s(a.v + b.v, a.w);
}
static inline SgS sg_s_sub(SgS a, SgS b) {
    if (a.w > 64) sg_fail("a static difference over 64 bits", a.w, b.w);
    return sg_s(a.v - b.v, a.w);
}
static inline int sg_s_eq(SgS a, SgS b) { return a.w == b.w && a.v == b.v && a.vh == b.vh; }
static inline long long sg_s_uint(SgS x) {
    if ((x.w >= 64 && (x.v >> 63)) || x.vh) sg_fail("UInt of a static value over 2^63", (long long)(x.v >> 32), 0);
    return (long long)x.v;
}
static inline long long sg_s_sint(SgS x) {
    if (x.w && (x.v >> (x.w - 1)) & 1) return (long long)(x.v | ~sg_ones(x.w));
    return (long long)x.v;
}
/* integer_subrange: bits hi..lo of an integer, two's complement */
static inline SgS sg_s_of_int(long long v, long long hi, long long lo) { return sg_s((SnxU64)v >> lo, hi - lo + 1); }
static inline SgS sg_s_lsl(SgS x, long long n) { return sgs_shl(x, n < 0 ? 128u : (unsigned)(n > 128 ? 128 : n), x.w); }
static inline SgS sg_s_lsr(SgS x, long long n) { return sgs_shr(x, n < 0 ? 128u : (unsigned)(n > 128 ? 128 : n), x.w); }
static inline SgS sg_s_ror(SgS x, long long n) {
    if (x.w > 64) sg_fail("a static rotation over 64 bits", x.w, n);
    n %= (long long)x.w;
    return n ? sg_s((x.v >> n) | (x.v << (x.w - n)), x.w) : x;
}
static inline long long sg_highest_set_bit(SgS x) {
    long long i;
    for (i = (long long)x.w - 1; i >= 0; --i)
        if ((x.v >> i) & 1) return i;
    return -1;
}
/* BitCount and LowestSetBit of bits the recompiler knows (a register list) */
static inline long long sg_s_bitcount(SgS x) {
    long long n = 0;
    unsigned i;
    for (i = 0; i < x.w; ++i) n += (long long)((x.v >> i) & 1);
    return n;
}
static inline long long sg_s_lowest_set_bit(SgS x) {
    unsigned i;
    for (i = 0; i < x.w; ++i)
        if ((x.v >> i) & 1) return (long long)i;
    return (long long)x.w;
}
/* Align(x, y): x with its bits below y cleared, y a power of two (an address aligned for a literal load) */
static inline SgS sg_s_align(SgS x, long long y) {
    if (y < 1 || (y & (y - 1)) || y > (1ll << 62)) sg_fail("Align to other than a power of two", y, 0);
    return sg_s(x.v & ~(SnxU64)(y - 1), x.w);
}
/* Sail's MOD and DIV on integers: Euclidean, the remainder never negative */
static inline long long sg_mod(long long a, long long b) {
    long long r;
    if (!b) sg_fail("MOD by zero", a, b);
    r = a % b;
    return r < 0 ? r + (b < 0 ? -b : b) : r;
}
static inline long long sg_div(long long a, long long b) {
    if (!b) sg_fail("DIV by zero", a, b);
    return (a - sg_mod(a, b)) / b;
}
static inline long long sg_pow2(long long n) {
    if (n < 0 || n > 62) sg_fail("2 ^ n out of a long long", n, 0);
    return 1ll << n;
}

