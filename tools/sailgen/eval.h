/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --gpu's evaluating back end: the same Sail as spirv.h makes SPIR-V of, run on values, for the tests that
   hold a GPU ISA's Sail to a reference (tests/shader_sail_sm86.c: sm86_eval's run of the instructions, and each
   fusion to its instructions run one after another). A run-time value is its bits; the ISA's eval file (after this
   one) gives the state.

   Each operator computes what the instruction spirv.h makes for it computes, and where SPIR-V leaves that
   undefined (a shift by the width or more, a BitFieldUExtract past the word) it says so in sg_eval(g)->undefined,
   which the tests fail on: an emitter that makes such an instruction is wrong whatever the value it happened to give */
#ifndef SNX_SAILGEN_EVAL_H
#define SNX_SAILGEN_EVAL_H
#include <stdint.h>

typedef struct SgG SgG;
typedef struct {
    uint64_t v;
} SgV;
typedef struct {
    int undefined; /* a SPIR-V instruction of an undefined result was made */
    int thrown;    /* the instruction was refused (a throw) */
} SgEval;
static SgEval* sg_eval(SgG* g); /* the ISA's file gives it */

static inline uint64_t sg_mask(int w) { return w >= 64 ? ~0ull : (1ull << w) - 1; }
static inline SgV sg_ev(uint64_t v) {
    SgV r = {v};
    return r;
}
static inline SgV sg_const(SgG* g, int w, uint64_t value) {
    (void)g;
    return sg_ev(value & sg_mask(w));
}
static inline SgV sg_bool(SgG* g, int b) {
    (void)g;
    return sg_ev(b != 0);
}
/* the type's width that spirv.h computes in: a u32 below 33 bits, a u64 above */
static inline int sg_word(int w) { return w > 32 ? 64 : 32; }
static inline SgV sg_add(SgG* g, int w, SgV a, SgV b) { return sg_const(g, w, a.v + b.v); }
static inline SgV sg_sub(SgG* g, int w, SgV a, SgV b) { return sg_const(g, w, a.v - b.v); }
static inline SgV sg_mul(SgG* g, int w, SgV a, SgV b) { return sg_const(g, w, a.v * b.v); }
static inline SgV sg_and(SgG* g, int w, SgV a, SgV b) { return sg_const(g, w, a.v & b.v); }
static inline SgV sg_or(SgG* g, int w, SgV a, SgV b) { return sg_const(g, w, a.v | b.v); }
static inline SgV sg_xor(SgG* g, int w, SgV a, SgV b) { return sg_const(g, w, a.v ^ b.v); }
static inline SgV sg_not(SgG* g, int w, SgV a) { return sg_const(g, w, ~a.v); }
static inline SgV sg_shl(SgG* g, int w, SgV a, SgV n) {
    if (n.v >= (uint64_t)sg_word(w)) {
        sg_eval(g)->undefined = 1;
        return sg_ev(0);
    }
    return sg_const(g, w, a.v << n.v);
}
static inline SgV sg_lshr(SgG* g, int w, SgV a, SgV n) {
    if (n.v >= (uint64_t)sg_word(w)) {
        sg_eval(g)->undefined = 1;
        return sg_ev(0);
    }
    return sg_const(g, w, a.v >> n.v);
}
/* of a whole word (spirv.h's takes the u32 or u64 as it is) */
static inline SgV sg_ashr(SgG* g, int w, SgV a, SgV n) {
    int word = sg_word(w);
    int64_t s = word == 64 ? (int64_t)a.v : (int64_t)(int32_t)(uint32_t)a.v;
    if (n.v >= (uint64_t)word) {
        sg_eval(g)->undefined = 1;
        return sg_ev(0);
    }
    return sg_const(g, w, (uint64_t)(s >> n.v));
}
static inline SgV sg_slice(SgG* g, int w, SgV a, int hi, int lo) {
    (void)w;
    return sg_const(g, hi - lo + 1, a.v >> lo);
}
static inline SgV sg_zext(SgG* g, int from, int to, SgV a) {
    (void)g, (void)from, (void)to;
    return a;
}
static inline SgV sg_sext(SgG* g, int from, int to, SgV a) {
    uint64_t sign = 1ull << (from - 1);
    return sg_const(g, to, (a.v ^ sign) - sign);
}
static inline SgV sg_concat(SgG* g, int wa, SgV a, int wb, SgV b) { return sg_const(g, wa + wb, a.v << wb | b.v); }

static inline int64_t sg_signed(int w, uint64_t v) {
    uint64_t sign = 1ull << (sg_word(w) - 1);
    return (int64_t)((v ^ sign) - sign);
}
static inline SgV sg_eq(SgG* g, int w, SgV a, SgV b) { return (void)w, sg_bool(g, a.v == b.v); }
static inline SgV sg_ne(SgG* g, int w, SgV a, SgV b) { return (void)w, sg_bool(g, a.v != b.v); }
static inline SgV sg_ult(SgG* g, int w, SgV a, SgV b) { return (void)w, sg_bool(g, a.v < b.v); }
static inline SgV sg_ule(SgG* g, int w, SgV a, SgV b) { return (void)w, sg_bool(g, a.v <= b.v); }
static inline SgV sg_ugt(SgG* g, int w, SgV a, SgV b) { return (void)w, sg_bool(g, a.v > b.v); }
static inline SgV sg_uge(SgG* g, int w, SgV a, SgV b) { return (void)w, sg_bool(g, a.v >= b.v); }
static inline SgV sg_slt(SgG* g, int w, SgV a, SgV b) { return sg_bool(g, sg_signed(w, a.v) < sg_signed(w, b.v)); }
static inline SgV sg_sle(SgG* g, int w, SgV a, SgV b) { return sg_bool(g, sg_signed(w, a.v) <= sg_signed(w, b.v)); }
static inline SgV sg_sgt(SgG* g, int w, SgV a, SgV b) { return sg_bool(g, sg_signed(w, a.v) > sg_signed(w, b.v)); }
static inline SgV sg_sge(SgG* g, int w, SgV a, SgV b) { return sg_bool(g, sg_signed(w, a.v) >= sg_signed(w, b.v)); }
static inline SgV sg_and_bool(SgG* g, SgV a, SgV b) { return sg_bool(g, a.v && b.v); }
static inline SgV sg_or_bool(SgG* g, SgV a, SgV b) { return sg_bool(g, a.v || b.v); }
static inline SgV sg_eq_bool(SgG* g, SgV a, SgV b) { return sg_bool(g, a.v == b.v); }
static inline SgV sg_ne_bool(SgG* g, SgV a, SgV b) { return sg_bool(g, a.v != b.v); }
static inline SgV sg_not_bool(SgG* g, SgV a) { return sg_bool(g, !a.v); }
static inline SgV sg_select(SgG* g, int w, SgV c, SgV a, SgV b) {
    (void)g, (void)w;
    return c.v ? a : b;
}
static inline SgV sg_umin(SgG* g, int w, SgV a, SgV b) { return (void)g, (void)w, a.v < b.v ? a : b; }
static inline SgV sg_umax(SgG* g, int w, SgV a, SgV b) { return (void)g, (void)w, a.v > b.v ? a : b; }
static inline SgV sg_smin(SgG* g, int w, SgV a, SgV b) { return (void)g, sg_signed(w, a.v) < sg_signed(w, b.v) ? a : b; }
static inline SgV sg_smax(SgG* g, int w, SgV a, SgV b) { return (void)g, sg_signed(w, a.v) > sg_signed(w, b.v) ? a : b; }
/* SAbs of the most negative value is itself */
static inline SgV sg_sabs(SgG* g, int w, SgV a) {
    int64_t s = sg_signed(w, a.v);
    return sg_const(g, w, (uint64_t)(s < 0 ? -s : s));
}

static inline SgV sg_PopCount(SgG* g, SgV a) {
    uint32_t x = (uint32_t)a.v, n = 0;
    (void)g;
    for (; x; x &= x - 1) ++n;
    return sg_ev(n);
}
static inline SgV sg_BitReverse32(SgG* g, SgV a) {
    uint32_t x = (uint32_t)a.v, r = 0;
    int i;
    (void)g;
    for (i = 0; i < 32; ++i) r |= ((x >> i) & 1u) << (31 - i);
    return sg_ev(r);
}
static inline SgV sg_HighestSetBit32(SgG* g, SgV a) {
    uint32_t x = (uint32_t)a.v;
    int i;
    (void)g;
    for (i = 31; i >= 0; --i)
        if (x >> i & 1) return sg_ev((uint64_t)i);
    return sg_ev(0xffffffffu);
}
static inline SgV sg_HighestSignBit32(SgG* g, SgV a) {
    uint32_t x = (uint32_t)a.v;
    return sg_HighestSetBit32(g, sg_ev(x >> 31 ? ~x : x));
}
#endif
