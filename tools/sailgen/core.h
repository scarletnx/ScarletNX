/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's LLVM back end, the part every machine's shares: the generated emitters' values and the IR of Sail's
   builtins (README.md, "Builtins"), copied at the top of the file sailgen writes (gen.c) after bits.h and before the
   machine's own part (llvm.h for A64, llvm_arm.h for AArch32), which defines SgCtx, the hooks below, and where the
   Sail's registers and guest memory are kept.

   A value the generated code knows while it recompiles (a field of the word, the pc, a mask from DecodeBitMasks) is
   C (bits.h): SgS for bits (64 at most), long long for an integer, int for a bool or an enum member. A value known only when
   the guest runs is IR: SgI for an integer (an LLVM value with its width and signedness), LLVMValueRef (i1) for a
   bool, and SgD for bits. An SgD is a bit map until something computes with it: each bit is 0, 1, or a bit of one of
   up to SG_SRC source values, so slices, concatenations, extensions, constant shifts and rotations, masks and
   Replicate cost no IR, and sg_d_make turns the map into the cheapest IR that builds it (a field, a sign extended
   field, a rotation, a funnel shift, a byte or bit reversal, or the OR of shifted and masked sources) */

#include "common/utilities/common.h" /* SnxU32, SnxU64 */
#include <llvm-c/Core.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_W   128 /* the widest bits value: EXTR's concatenation */
#define SG_SRC 4

typedef struct {
    LLVMValueRef value; /* the bits as iW, once made */
    unsigned w;
    unsigned char sel[SG_W]; /* 0: the bit is 0; 1: it is 1; 2 + k: bit pos[i] of src[k] */
    unsigned char pos[SG_W];
    LLVMValueRef src[SG_SRC];
    unsigned char src_w[SG_SRC];
    unsigned char src_flag[SG_SRC]; /* the source is 0 or 1 (a flag as the CPU structure keeps it) */
    LLVMValueRef zero_of[SG_SRC];   /* the source is zext(X == 0) to i32 (IsZeroBit): X */
    unsigned char nsrc;
    /* NZCV before anything reads it, its bits not made yet: AddWithCarry's, of the sum a + b + cin = r (SG_NZCV_ADD,
       SUB, CARRY), or a logical result r's (SG_NZCV_LOGIC), which a write of the four flags keeps as what a
       condition on them then compares */
    unsigned char nzcv;
    LLVMValueRef nzcv_a, nzcv_b, nzcv_cin, nzcv_r;
} SgD;
enum { SG_NZCV_NONE, SG_NZCV_ADD, SG_NZCV_SUB, SG_NZCV_CARRY, SG_NZCV_LOGIC };

typedef struct {
    LLVMValueRef v;
    unsigned w;
    int is_signed;
    int wrap; /* only the low w bits are the value's (a quotient that can't fit): slicing them is the one use */
} SgI;

/* a bool known only when the guest runs: an i1, and the operations gen.c writes on it */
typedef LLVMValueRef SgB;

/* the generated code's context: the machine's part defines it */
typedef struct SgCtx SgCtx;

/* the hooks the machine's part defines: its IR builder and LLVM context, an intrinsic's call, and select(c, x, y) as a
   phi where a lowered function's run-time condition left one to extend (NULL where none) */
static inline LLVMBuilderRef sg_builder(SgCtx* g);
static inline LLVMContextRef sg_llvm_context(SgCtx* g);
static inline LLVMValueRef sg_intrinsic(SgCtx* g, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned n);
static inline LLVMValueRef sg_select_phi(SgCtx* g, LLVMValueRef c, LLVMValueRef x, LLVMValueRef y);

#define SG_B (sg_builder(g))

static inline SgB sg_b_not(SgCtx* g, SgB a) { return LLVMBuildNot(SG_B, a, ""); }
static inline SgB sg_b_and(SgCtx* g, SgB a, SgB b) { return LLVMBuildAnd(SG_B, a, b, ""); }
static inline SgB sg_b_or(SgCtx* g, SgB a, SgB b) { return LLVMBuildOr(SG_B, a, b, ""); }
static inline SgB sg_b_eq(SgCtx* g, int ne, SgB a, SgB b) {
    return LLVMBuildICmp(SG_B, ne ? LLVMIntNE : LLVMIntEQ, a, b, "");
}
static inline SgB sg_b_select(SgCtx* g, SgB c, SgB a, SgB b) { return LLVMBuildSelect(SG_B, c, a, b, ""); }

/* ---- dynamic bits: the map ---- */

static inline LLVMTypeRef sg_type(SgCtx* g, unsigned w) { return LLVMIntTypeInContext(sg_llvm_context(g), w); }
/* a bool known when it recompiles, as an i1 */
static inline LLVMValueRef sg_bool(SgCtx* g, int v) {
    return LLVMConstInt(sg_type(g, 1), (unsigned long long)(v != 0), 0);
}
/* an i1 as the i32 0 or 1 a flag is kept in */
static inline LLVMValueRef sg_bool32(SgCtx* g, LLVMValueRef c) { return LLVMBuildZExt(SG_B, c, sg_type(g, 32), ""); }
static inline LLVMValueRef sg_const(SgCtx* g, unsigned w, SnxU64 lo, SnxU64 hi) {
    uint64_t words[2];
    if (w <= 64) return LLVMConstInt(sg_type(g, w), lo & sg_ones(w), 0);
    words[0] = lo;
    words[1] = hi & sg_ones(w - 64);
    return LLVMConstIntOfArbitraryPrecision(sg_type(g, w), 2, words);
}

static inline SgD sg_d_empty(unsigned w) {
    SgD d = {0};
    if (w > SG_W) sg_fail("a dynamic bits value over SG_W bits", w, SG_W);
    d.w = w;
    return d;
}
static inline SgD sg_d_from_s(SgS s) {
    SgD d = sg_d_empty(s.w);
    unsigned i;
    for (i = 0; i < s.w; ++i) d.sel[i] = (unsigned char)((s.v >> i) & 1);
    return d;
}
/* w bits all 0 or all 1, or integer v's bits hi..lo (two's complement), known when it recompiles but over the 64 bits
   an SgS keeps (a SIMD&FP register's Zeros(128)): a map of constant bits */
static inline SgD sg_d_fill(long long w, int one) {
    SgD d = sg_d_empty((unsigned)w);
    unsigned i;
    for (i = 0; i < d.w; ++i) d.sel[i] = (unsigned char)(one != 0);
    return d;
}
static inline SgD sg_d_of_int(long long v, long long hi, long long lo) {
    SgD d;
    unsigned i;
    if (lo < 0 || hi < lo) sg_fail("bits of an integer out of it", hi, lo);
    d = sg_d_empty((unsigned)(hi - lo + 1));
    for (i = 0; i < d.w; ++i) {
        long long b = lo + (long long)i;
        d.sel[i] = (unsigned char)(b >= 63 ? v < 0 : (v >> b) & 1);
    }
    return d;
}
/* the bits of an LLVM integer value: a map of one source, already made */
static inline SgD sg_d_from(SgCtx* g, LLVMValueRef v) {
    unsigned w = LLVMGetIntTypeWidth(LLVMTypeOf(v)), i;
    SgD d = sg_d_empty(w);
    (void)g;
    if (LLVMIsAConstantInt(v) && w <= 64) {
        SnxU64 c = LLVMConstIntGetZExtValue(v);
        for (i = 0; i < w; ++i) d.sel[i] = (unsigned char)((c >> i) & 1);
        d.value = v;
        return d;
    }
    d.nsrc = 1;
    d.src[0] = v;
    d.src_w[0] = (unsigned char)(w == 256 ? 0 : w);
    for (i = 0; i < w; ++i) {
        d.sel[i] = 2;
        d.pos[i] = (unsigned char)i;
    }
    d.value = v;
    return d;
}
/* one bit that is a 0-or-1 value of any width (a flag as the CPU structure keeps it, a zext of a comparison) */
static inline SgD sg_d_flag(LLVMValueRef v, LLVMValueRef zero_of) {
    SgD d = sg_d_empty(1);
    if (LLVMIsAConstantInt(v)) {
        d.sel[0] = (unsigned char)(LLVMConstIntGetZExtValue(v) & 1);
        return d;
    }
    d.nsrc = 1;
    d.src[0] = v;
    d.src_w[0] = (unsigned char)LLVMGetIntTypeWidth(LLVMTypeOf(v));
    d.src_flag[0] = 1;
    d.zero_of[0] = zero_of;
    d.sel[0] = 2;
    return d;
}

static inline int sg_d_is_const(const SgD* x) {
    unsigned i;
    if (x->nzcv) return 0;
    for (i = 0; i < x->w; ++i)
        if (x->sel[i] > 1) return 0;
    return 1;
}
static inline SnxU64 sg_d_const_word(const SgD* x, unsigned word) {
    SnxU64 v = 0;
    unsigned i;
    for (i = 64 * word; i < x->w && i < 64 * word + 64; ++i) v |= (SnxU64)(x->sel[i] & 1) << (i - 64 * word);
    return v;
}

/* the sources no bit of m reads any more dropped (an overwritten slice leaves them) */
static inline void sg_d_compact(SgD* m) {
    unsigned used[SG_SRC] = {0}, to[SG_SRC], i, n = 0;
    for (i = 0; i < m->w; ++i)
        if (m->sel[i] > 1) used[m->sel[i] - 2] = 1;
    for (i = 0; i < m->nsrc; ++i) {
        to[i] = n;
        if (!used[i]) continue;
        m->src[n] = m->src[i];
        m->src_w[n] = m->src_w[i];
        m->src_flag[n] = m->src_flag[i];
        m->zero_of[n] = m->zero_of[i];
        ++n;
    }
    for (i = 0; i < m->w; ++i)
        if (m->sel[i] > 1) m->sel[i] = (unsigned char)(2 + to[m->sel[i] - 2]);
    m->nsrc = (unsigned char)n;
}

/* src's index in m for the source k of from, added when m has room: -1 when it has none */
static inline int sg_d_take_src(SgD* m, const SgD* from, unsigned k) {
    unsigned j;
    for (j = 0; j < m->nsrc; ++j)
        if (m->src[j] == from->src[k]) return (int)j;
    if (m->nsrc == SG_SRC) sg_d_compact(m);
    if (m->nsrc == SG_SRC) return -1;
    j = m->nsrc++;
    m->src[j] = from->src[k];
    m->src_w[j] = from->src_w[k];
    m->src_flag[j] = from->src_flag[k];
    m->zero_of[j] = from->zero_of[k];
    return (int)j;
}

static inline LLVMValueRef sg_d_make(SgCtx* g, SgD* x);
static inline void sg_d_realize(SgCtx* g, SgD* x);

/* the bits [lo, lo + n) of from into m at bit at: 0 when m has no room for from's sources */
static inline int sg_d_put(SgD* m, unsigned at, const SgD* from, unsigned lo, unsigned n) {
    unsigned i;
    int map[SG_SRC];
    for (i = 0; i < from->nsrc; ++i) map[i] = -2;
    for (i = 0; i < n; ++i) {
        unsigned s = from->sel[lo + i];
        if (s > 1 && map[s - 2] == -2) {
            map[s - 2] = sg_d_take_src(m, from, s - 2);
            if (map[s - 2] < 0) return 0;
        }
    }
    for (i = 0; i < n; ++i) {
        unsigned s = from->sel[lo + i];
        m->sel[at + i] = (unsigned char)(s > 1 ? 2 + map[s - 2] : s);
        m->pos[at + i] = from->pos[lo + i];
    }
    m->value = NULL;
    return 1;
}
/* as sg_d_put, making from (or m) one source first when the sources don't fit */
static inline void sg_d_put_any(SgCtx* g, SgD* m, unsigned at, SgD from, unsigned lo, unsigned n) {
    if (from.nzcv) sg_d_realize(g, &from);
    if (sg_d_put(m, at, &from, lo, n)) return;
    from = sg_d_from(g, sg_d_make(g, &from));
    if (sg_d_put(m, at, &from, lo, n)) return;
    {
        SgD whole = sg_d_from(g, sg_d_make(g, m));
        *m = whole;
    }
    if (!sg_d_put(m, at, &from, lo, n)) sg_fail("a bit map with no room for two sources", m->nsrc, from.nsrc);
}

static inline int sg_d_permutation(const SgD* x);
static inline int sg_d_low(const SgD* x, unsigned k);
/* x is one source's low bits rotated, some of them made constant (sg_d_make's rotation with an OR and an AND) */
static inline int sg_d_rotated(const SgD* x) {
    unsigned i, r = 0, n = 0;
    if (x->nsrc != 1 || x->w > 64 || !sg_d_low(x, 0)) return 0;
    for (i = 0; i < x->w; ++i) {
        if (x->sel[i] < 2) continue;
        if (!n++) r = (x->pos[i] + x->w - i) % x->w;
        if (x->pos[i] != (i + r) % x->w) return 0;
    }
    return n > 1 && r;
}
static inline SgD sg_d_slice(SgCtx* g, SgD x, long long hi, long long lo) {
    SgD r;
    if (lo < 0 || hi < lo || hi >= (long long)x.w) sg_fail("a slice out of its value", hi, lo);
    if (hi - lo + 1 < (long long)x.w && !x.nzcv && (sg_d_permutation(&x) || sg_d_rotated(&x))) {
        /* bits of a rotated value (ORRS's N of a rotated register): the rotation made, as its other use makes it, so
           LLVM finds one */
        SgD whole = sg_d_from(g, sg_d_make(g, &x));
        r = sg_d_empty((unsigned)(hi - lo + 1));
        sg_d_put_any(g, &r, 0, whole, (unsigned)lo, r.w);
        return r;
    }
    r = sg_d_empty((unsigned)(hi - lo + 1));
    sg_d_put_any(g, &r, 0, x, (unsigned)lo, r.w);
    if (lo == 0 && r.w == x.w) return x;
    return r;
}
static inline SgD sg_d_concat(SgCtx* g, SgD hi, SgD lo) {
    SgD r = sg_d_empty(hi.w + lo.w);
    sg_d_put_any(g, &r, 0, lo, 0, lo.w);
    sg_d_put_any(g, &r, lo.w, hi, 0, hi.w);
    return r;
}
static inline SgD sg_d_zext(SgCtx* g, SgD x, long long w) {
    SgD r;
    if (w < (long long)x.w) sg_fail("an extension to fewer bits", w, x.w);
    if (w == (long long)x.w) return x;
    r = sg_d_empty((unsigned)w);
    sg_d_put_any(g, &r, 0, x, 0, x.w);
    return r;
}
static inline SgD sg_d_sext(SgCtx* g, SgD x, long long w) {
    SgD r;
    unsigned i;
    if (w < (long long)x.w) sg_fail("an extension to fewer bits", w, x.w);
    if (w == (long long)x.w || !x.w) return x;
    r = sg_d_empty((unsigned)w);
    sg_d_put_any(g, &r, 0, x, 0, x.w);
    for (i = x.w; i < r.w; ++i) {
        r.sel[i] = r.sel[x.w - 1];
        r.pos[i] = r.pos[x.w - 1];
    }
    return r;
}
static inline SgD sg_d_replicate(SgCtx* g, SgD x, long long n) {
    SgD r;
    long long i;
    if (n < 1) sg_fail("Replicate fewer than once", n, 0);
    r = sg_d_empty((unsigned)(x.w * n));
    for (i = 0; i < n; ++i) sg_d_put_any(g, &r, (unsigned)(i * x.w), x, 0, x.w);
    return r;
}
static inline void sg_d_set_slice(SgCtx* g, SgD* x, long long hi, long long lo, SgD v) {
    if (lo < 0 || hi < lo || hi >= (long long)x->w || v.w != (unsigned)(hi - lo + 1))
        sg_fail("a slice assignment out of its value", hi, lo);
    if (x->nzcv) sg_d_realize(g, x);
    sg_d_put_any(g, x, (unsigned)lo, v, 0, v.w);
}
/* constant shifts and rotations move the map */
static inline SgD sg_d_lsl(SgCtx* g, SgD x, long long n) {
    SgD r = sg_d_empty(x.w);
    if (n < 0) sg_fail("a negative shift", n, 0);
    if (!n) return x;
    if (n < (long long)x.w) sg_d_put_any(g, &r, (unsigned)n, x, 0, x.w - (unsigned)n);
    return r;
}
static inline SgD sg_d_lsr(SgCtx* g, SgD x, long long n) {
    SgD r = sg_d_empty(x.w);
    if (n < 0) sg_fail("a negative shift", n, 0);
    if (!n) return x;
    if (n < (long long)x.w) sg_d_put_any(g, &r, 0, x, (unsigned)n, x.w - (unsigned)n);
    return r;
}
static inline SgD sg_d_asr(SgCtx* g, SgD x, long long n) {
    SgD r;
    unsigned i;
    if (n < 0) sg_fail("a negative shift", n, 0);
    if (!n) return x;
    if (n >= (long long)x.w) n = x.w - 1;
    r = sg_d_empty(x.w);
    sg_d_put_any(g, &r, 0, x, (unsigned)n, x.w - (unsigned)n);
    sg_d_put_any(g, &r, x.w - (unsigned)n, sg_d_slice(g, x, x.w - 1, x.w - 1), 0, 1);
    for (i = x.w - (unsigned)n; i < x.w; ++i) {
        r.sel[i] = r.sel[x.w - (unsigned)n];
        r.pos[i] = r.pos[x.w - (unsigned)n];
    }
    return r;
}
static inline SgD sg_d_ror(SgCtx* g, SgD x, long long n) {
    SgD r = sg_d_empty(x.w);
    n = sg_mod(n, (long long)x.w);
    if (!n) return x;
    sg_d_put_any(g, &r, 0, x, (unsigned)n, x.w - (unsigned)n);
    sg_d_put_any(g, &r, x.w - (unsigned)n, x, 0, (unsigned)n);
    return r;
}
static inline SgD sg_d_bitreverse(SgCtx* g, SgD x) {
    SgD r = sg_d_empty(x.w);
    unsigned i;
    for (i = 0; i < x.w; ++i) sg_d_put_any(g, &r, x.w - 1 - i, x, i, 1);
    return r;
}

/* ---- making the IR of a map ---- */

/* the source as an integer of w bits: truncated or zero extended */
static inline LLVMValueRef sg_cast(SgCtx* g, LLVMValueRef v, unsigned w, int is_signed) {
    unsigned from = LLVMGetIntTypeWidth(LLVMTypeOf(v));
    if (from == w) return v;
    if (from > w) return LLVMBuildTrunc(SG_B, v, sg_type(g, w), "");
    return is_signed ? LLVMBuildSExt(SG_B, v, sg_type(g, w), "") : LLVMBuildZExt(SG_B, v, sg_type(g, w), "");
}

/* a field: bits [p, p + n) are bits [f, f + n) of source k, the bits below p are 0, and the bits above are 0
   (unsigned) or the field's top bit up to bit e and 0 above it (signed: SBFM, a sign-extending operand, either
   zero-extended into an X register). NULL when x is not one. A field in the low w bits of a wider source reads
   them first, as a W register's read */
static inline LLVMValueRef sg_d_field(SgCtx* g, const SgD* x) {
    unsigned w = x->w, p = 0, n, i, k, f, sw, top, wide, e;
    int sign = 0;
    LLVMValueRef s, t;
    while (p < w && x->sel[p] == 0) ++p;
    if (p == w || x->sel[p] < 2) return NULL;
    k = x->sel[p] - 2u;
    f = x->pos[p];
    for (n = 0; p + n < w && x->sel[p + n] == 2 + k && x->pos[p + n] == f + n; ++n) {}
    e = p + n;
    if (e < w && x->sel[e] != 0) {
        if (x->sel[e] != 2 + k || x->pos[e] != f + n - 1) return NULL;
        sign = 1;
        while (e < w && x->sel[e] == 2 + k && x->pos[e] == f + n - 1) ++e;
    }
    for (i = e; i < w; ++i)
        if (x->sel[i] != 0) return NULL;
    sw = x->src_w[k] ? x->src_w[k] : 256;
    s = x->src[k];
    top = f + n;
    if (sw > w && top <= w) { /* the field is in the low w bits: those first, as a W register's read */
        s = sg_cast(g, s, w, 0);
        sw = w;
    }
    wide = sw > w ? sw : w;
    if (sign && e > p + n && n == 1) { /* one bit spread over [p, e): the mask when it is set */
        LLVMValueRef bit =
            LLVMBuildAnd(SG_B, s, sg_const(g, sw, f < 64 ? 1ull << f : 0, f >= 64 ? 1ull << (f - 64) : 0), "");
        SnxU64 lo = sg_ones(e < 64 ? e : 64) & ~sg_ones(p < 64 ? p : 64);
        SnxU64 hi = e > 64 ? sg_ones(e - 64) & ~(p > 64 ? sg_ones(p - 64) : 0) : 0;
        return LLVMBuildSelect(SG_B, LLVMBuildICmp(SG_B, LLVMIntNE, bit, LLVMConstNull(LLVMTypeOf(s)), ""),
                               sg_const(g, w, lo, hi), sg_const(g, w, 0, 0), "");
    }
    if (sign && e > p + n) {
        if (!f) /* the low n bits sign extended: a truncation and an extension, as the extends are written */
            t = sg_cast(g, sg_cast(g, s, n, 0), wide, 1);
        else { /* sextract: (s << (sw - top)) >> (sw - n), arithmetic */
            t = s;
            if (sw - top) t = LLVMBuildShl(SG_B, t, LLVMConstInt(LLVMTypeOf(t), sw - top, 0), "");
            if (sw - n) t = LLVMBuildAShr(SG_B, t, LLVMConstInt(LLVMTypeOf(t), sw - n, 0), "");
            t = sg_cast(g, t, wide, 1);
        }
        if (p) t = LLVMBuildShl(SG_B, t, LLVMConstInt(LLVMTypeOf(t), p, 0), "");
        if (e < wide) t = LLVMBuildAnd(SG_B, t, sg_const(g, wide, sg_ones(e), e > 64 ? sg_ones(e - 64) : 0), "");
        return sg_cast(g, t, w, 0);
    }
    t = s;
    if (f) t = LLVMBuildLShr(SG_B, t, LLVMConstInt(LLVMTypeOf(t), f, 0), "");
    if (top < sw && n < w - p) t = LLVMBuildAnd(SG_B, t, LLVMConstInt(LLVMTypeOf(t), sg_ones(n), 0), "");
    t = sg_cast(g, t, wide, 0);
    if (p) t = LLVMBuildShl(SG_B, t, LLVMConstInt(LLVMTypeOf(t), p, 0), "");
    return sg_cast(g, t, w, 0);
}

/* every bit from source k, bit i from pos(i): 1 when pos[i] == perm(i) for the permutation code names */
static inline int sg_d_perm(const SgD* x, unsigned k, int code, unsigned arg) {
    unsigned i, w = x->w;
    for (i = 0; i < w; ++i) {
        unsigned want;
        if (x->sel[i] != 2 + k) return 0;
        switch (code) {
            case 0: want = (i + arg) % w; break;                                         /* rotate right by arg */
            case 1: want = w - 1 - i; break;                                             /* bit reversal */
            default: want = (i / arg) * arg + (arg / 8 - 1 - (i % arg) / 8) * 8 + i % 8; /* bytes in arg bits */
        }
        if (x->pos[i] != want) return 0;
    }
    return 1;
}

static inline int sg_d_low(const SgD* x, unsigned k);
/* x is one of the permutations sg_d_make builds whole: a rotation, a reversal of bits or bytes, a funnel shift */
static inline int sg_d_permutation(const SgD* x) {
    unsigned i, k, w = x->w;
    if (x->nsrc == 1 && sg_d_low(x, 0))
        return (x->pos[0] && sg_d_perm(x, 0, 0, x->pos[0])) || sg_d_perm(x, 0, 1, 0) ||
               (w % 16 == 0 && sg_d_perm(x, 0, 2, w)) || (w == 64 && sg_d_perm(x, 0, 2, 32));
    if (x->nsrc != 2 || !sg_d_low(x, 0) || !sg_d_low(x, 1)) return 0;
    for (k = 0; k < 2; ++k) {
        unsigned lo = k, hi = 1 - k, l = x->pos[0];
        int ok = x->sel[0] == 2 + lo && l > 0 && l < w;
        for (i = 0; ok && i < w; ++i)
            ok =
                i < w - l ? x->sel[i] == 2 + lo && x->pos[i] == i + l : x->sel[i] == 2 + hi && x->pos[i] == i - (w - l);
        if (ok) return 1;
    }
    return 0;
}

/* source k has w bits, or more of which x reads only the low w */
static inline int sg_d_low(const SgD* x, unsigned k) {
    unsigned i, sw = x->src_w[k] ? x->src_w[k] : 256;
    if (sw < x->w) return 0;
    for (i = 0; i < x->w; ++i)
        if (x->sel[i] == 2 + k && x->pos[i] >= x->w) return 0;
    return 1;
}
/* source k at x's width (sg_d_low's): truncated when it is wider */
static inline LLVMValueRef sg_d_src(SgCtx* g, const SgD* x, unsigned k) { return sg_cast(g, x->src[k], x->w, 0); }

/* v is NOT of one bit of a value, as sg_d_not makes it of a 1-bit map: xor (trunc (lshr S, p)), true. S, and p in *pos;
   NULL when it is not */
static inline LLVMValueRef sg_d_flipped(LLVMValueRef v, unsigned* pos) {
    LLVMValueRef t, s;
    if (!LLVMIsAInstruction(v) || LLVMGetInstructionOpcode(v) != LLVMXor || LLVMGetIntTypeWidth(LLVMTypeOf(v)) != 1 ||
        !LLVMIsAConstantInt(LLVMGetOperand(v, 1)) || !LLVMConstIntGetZExtValue(LLVMGetOperand(v, 1)))
        return NULL;
    t = LLVMGetOperand(v, 0);
    if (!LLVMIsAInstruction(t) || LLVMGetInstructionOpcode(t) != LLVMTrunc) return NULL;
    s = LLVMGetOperand(t, 0);
    *pos = 0;
    if (LLVMIsAInstruction(s) && LLVMGetInstructionOpcode(s) == LLVMLShr && LLVMIsAConstantInt(LLVMGetOperand(s, 1))) {
        *pos = (unsigned)LLVMConstIntGetZExtValue(LLVMGetOperand(s, 1));
        s = LLVMGetOperand(s, 0);
    }
    return LLVMGetTypeKind(LLVMTypeOf(s)) == LLVMIntegerTypeKind && *pos < LLVMGetIntTypeWidth(LLVMTypeOf(s)) ? s
                                                                                                              : NULL;
}

static inline LLVMValueRef sg_d_make(SgCtx* g, SgD* x) {
    unsigned w = x->w, i, k, done[SG_W], run_bit[SG_W];
    LLVMValueRef r = NULL, t;
    if (x->value) return x->value;
    if (x->nzcv) sg_d_realize(g, x);
    if (sg_d_is_const(x)) return x->value = sg_const(g, w, sg_d_const_word(x, 0), sg_d_const_word(x, 1));
    sg_d_compact(x);
    { /* a bit that is another value's bit flipped (FPNeg's not_vec([op[N-1]]) @ op[N - 2 .. 0]): that bit, and one xor
         of the places it flips, as a sign was always flipped */
        SgD y = *x;
        SnxU64 flip[2] = {0, 0};
        int any = 0;
        for (k = 0; k < x->nsrc; ++k) {
            unsigned p;
            int j;
            LLVMValueRef s;
            SgD from;
            if (x->src_w[k] != 1 || x->src_flag[k] || !(s = sg_d_flipped(x->src[k], &p))) continue;
            from = sg_d_from(g, s);
            if ((j = sg_d_take_src(&y, &from, 0)) < 0) continue;
            for (i = 0; i < w; ++i)
                if (x->sel[i] == 2 + k) {
                    y.sel[i] = (unsigned char)(2 + j);
                    y.pos[i] = (unsigned char)p;
                    flip[i / 64] |= 1ull << (i % 64);
                    any = 1;
                }
        }
        if (any) {
            y.value = NULL;
            sg_d_compact(&y);
            t = sg_d_make(g, &y);
            return x->value = LLVMBuildXor(SG_B, t, sg_const(g, w, flip[0], flip[1]), "");
        }
    }
    /* a W value zero-extended into an X register: its 32 bits made (a rotation, a reversal, a funnel), then extended */
    if (w == 64) {
        for (i = 32; i < 64 && x->sel[i] == 0; ++i) {}
        if (i == 64) {
            SgD low = *x;
            low.w = 32;
            low.value = NULL;
            if (sg_d_permutation(&low)) return x->value = sg_cast(g, sg_d_make(g, &low), 64, 0);
        }
    }
    /* the permutations of one source of w bits, or of the low w bits of a wider one (a W register's) */
    if (x->nsrc == 1 && sg_d_low(x, 0)) {
        LLVMValueRef args[3];
        if (sg_d_perm(x, 0, 0, 0)) return x->value = sg_d_src(g, x, 0);
        i = x->pos[0]; /* a rotation right by i reads bit i into bit 0 */
        if (i && sg_d_perm(x, 0, 0, i)) {
            args[0] = args[1] = sg_d_src(g, x, 0);
            args[2] = LLVMConstInt(sg_type(g, w), i, 0);
            return x->value = sg_intrinsic(g, "llvm.fshr", sg_type(g, w), args, 3);
        }
        if (sg_d_perm(x, 0, 1, 0)) {
            args[0] = sg_d_src(g, x, 0);
            return x->value = sg_intrinsic(g, "llvm.bitreverse", sg_type(g, w), args, 1);
        }
        if (w % 16 == 0 && sg_d_perm(x, 0, 2, w)) {
            args[0] = sg_d_src(g, x, 0);
            return x->value = sg_intrinsic(g, "llvm.bswap", sg_type(g, w), args, 1);
        }
        if (w == 64 && sg_d_perm(x, 0, 2, 32)) { /* each word's bytes reversed: the whole swapped, then rotated */
            args[0] = sg_d_src(g, x, 0);
            t = sg_intrinsic(g, "llvm.bswap", sg_type(g, w), args, 1);
            args[0] = t;
            args[1] = t;
            args[2] = LLVMConstInt(sg_type(g, w), 32, 0);
            return x->value = sg_intrinsic(g, "llvm.fshr", sg_type(g, w), args, 3);
        }
    }
    if ((t = sg_d_field(g, x)) != NULL) return x->value = t;
    if (x->nsrc == 2 && sg_d_low(x, 0) && sg_d_low(x, 1)) { /* a funnel shift: hi's low bits over lo's high */
        for (k = 0; k < 2; ++k) {
            unsigned lo = k, hi = 1 - k, l = x->pos[0];
            int ok = x->sel[0] == 2 + lo && l > 0 && l < w;
            for (i = 0; ok && i < w; ++i)
                ok = i < w - l ? x->sel[i] == 2 + lo && x->pos[i] == i + l
                               : x->sel[i] == 2 + hi && x->pos[i] == i - (w - l);
            if (ok) {
                LLVMValueRef args[3];
                args[0] = sg_d_src(g, x, hi);
                args[1] = sg_d_src(g, x, lo);
                args[2] = LLVMConstInt(sg_type(g, w), l, 0);
                return x->value = sg_intrinsic(g, "llvm.fshr", sg_type(g, w), args, 3);
            }
        }
    }
    { /* a permutation of the low bits, sign extended (REVSH's swapped halfword): that permutation, then extended */
        unsigned top = w - 1;
        while (top > 0 && x->sel[top] >= 2 && x->sel[top - 1] == x->sel[top] && x->pos[top - 1] == x->pos[top]) --top;
        if (top + 1 < w && top > 0) {
            SgD low = *x;
            low.w = top + 1;
            low.value = NULL;
            if (sg_d_permutation(&low)) return x->value = sg_cast(g, sg_d_make(g, &low), w, 1);
        }
    }
    { /* one source bit in several places apart, the rest constant (a sign ANDed with a constant): one select of their
         mask, as LLVM folds the emitters' AND of a spread sign */
        unsigned k = 0, p = 0, n = 0, sw;
        SnxU64 mask = 0, ones = 0;
        int ok = w <= 64;
        for (i = 0; i < w && ok; ++i) {
            if (x->sel[i] == 1)
                ones |= 1ull << i;
            else if (x->sel[i] >= 2) {
                if (!n) {
                    k = x->sel[i];
                    p = x->pos[i];
                }
                ok = x->sel[i] == k && x->pos[i] == p;
                mask |= 1ull << i;
                ++n;
            }
        }
        sw = n ? (x->src_w[k - 2] ? x->src_w[k - 2] : 256) : 0;
        if (ok && n >= 2 && sw <= 64 && !x->src_flag[k - 2]) {
            LLVMValueRef src = x->src[k - 2], bit;
            bit = LLVMBuildAnd(SG_B, src, LLVMConstInt(LLVMTypeOf(src), 1ull << p, 0), "");
            t = LLVMBuildSelect(SG_B, LLVMBuildICmp(SG_B, LLVMIntNE, bit, LLVMConstNull(LLVMTypeOf(src)), ""),
                                sg_const(g, w, mask, 0), sg_const(g, w, 0, 0), "");
            if (ones) t = LLVMBuildOr(SG_B, t, sg_const(g, w, ones, 0), "");
            return x->value = t;
        }
    }
    { /* a field with some of its bits made constant (ORR or BIC of an immediate after a shift): each such bit filled
         with the field's own, the field made, then one OR of the ones and one AND of the zeros, as the emitters written
         by hand build it. The filled bits' values do not matter: the OR and the AND decide them */
        unsigned lo = w, hi = 0, k0 = 0;
        int d = 0;
        for (i = 0; i < w; ++i)
            if (x->sel[i] >= 2) {
                if (lo == w) {
                    lo = i;
                    k0 = x->sel[i];
                    d = (int)x->pos[i] - (int)i;
                }
                hi = i;
            }
        if (lo < w && hi > lo + 1 && w <= 64) {
            SgD y = *x;
            SnxU64 one = 0, zero = 0;
            int ok = 1;
            for (i = lo + 1; i < hi && ok; ++i) {
                int p;
                if (x->sel[i] >= 2) continue;
                p = x->sel[i - 1] == k0 && x->sel[i + 1] == k0 && x->pos[i - 1] == x->pos[i + 1] ? (int)x->pos[i - 1]
                                                                                                 : (int)i + d;
                if (x->src_w[k0 - 2] && p >= (int)x->src_w[k0 - 2]) p = x->src_w[k0 - 2] - 1; /* a sign's run */
                ok = p >= 0 && p < 256;
                y.sel[i] = (unsigned char)k0;
                y.pos[i] = (unsigned char)p;
                if (x->sel[i])
                    one |= 1ull << i;
                else
                    zero |= 1ull << i;
            }
            if (ok && (one || zero)) {
                y.value = NULL;
                if ((t = sg_d_field(g, &y)) != NULL) {
                    if (one) t = LLVMBuildOr(SG_B, t, sg_const(g, w, one, 0), "");
                    if (zero) t = LLVMBuildAnd(SG_B, t, sg_const(g, w, ~zero & sg_ones(w), 0), "");
                    return x->value = t;
                }
            }
        }
        if (lo < w && x->nsrc == 1 && w <= 64 && sg_d_low(x, 0)) {
            /* a rotation with constant bits (ORR of the PC and a rotated register, a target's bit 0 cleared): every
               constant bit filled as the rotation has it, the rotation made, then the OR and the AND */
            SgD y = *x;
            SnxU64 one = 0, zero = 0;
            unsigned r = (unsigned)(((int)x->pos[lo] - (int)lo + (int)w) % (int)w);
            for (i = 0; i < w; ++i) {
                if (x->sel[i] >= 2) continue;
                y.sel[i] = (unsigned char)k0;
                y.pos[i] = (unsigned char)((i + r) % w);
                if (x->sel[i])
                    one |= 1ull << i;
                else
                    zero |= 1ull << i;
            }
            y.value = NULL;
            if ((one || zero) && r && sg_d_perm(&y, 0, 0, r)) {
                t = sg_d_make(g, &y);
                if (one) t = LLVMBuildOr(SG_B, t, sg_const(g, w, one, 0), "");
                if (zero) t = LLVMBuildAnd(SG_B, t, sg_const(g, w, ~zero & sg_ones(w), 0), "");
                return x->value = t;
            }
        }
    }
    /* the OR of each source's bits that move by the same distance, masked, and the bits that are 1. The bits of a run
       of one source bit over more than two neighbours (Replicate, a sign) are found first: the one that lands on that
       bit's own place (XPAC's bit 55) is the run's, not one of the bits that keep their place */
    memset(done, 0, sizeof done);
    memset(run_bit, 0, sizeof run_bit);
    for (i = 0; i < w;) {
        unsigned n = 1;
        while (x->sel[i] >= 2 && i + n < w && x->sel[i + n] == x->sel[i] && x->pos[i + n] == x->pos[i]) ++n;
        if (n > 2)
            for (k = i; k < i + n; ++k) run_bit[k] = 1;
        i += n;
    }
    for (i = 0; i < w; ++i) {
        SnxU64 mask[2] = {0, 0};
        unsigned j, sw;
        int d;
        if (x->sel[i] < 2 || done[i]) continue;
        k = x->sel[i] - 2u;
        d = (int)i - (int)x->pos[i];
        {
            /* one source bit over a run of neighbours (Replicate, a sign): the run's mask when the bit is set */
            unsigned p = x->pos[i], run = 0;
            while (i + run < w && x->sel[i + run] == 2 + k && x->pos[i + run] == p && !done[i + run]) ++run;
            if (run > 2) {
                LLVMValueRef s = x->src[k], bit;
                for (j = i; j < i + run; ++j) {
                    done[j] = 1;
                    mask[j / 64] |= 1ull << (j % 64);
                }
                bit = LLVMBuildAnd(SG_B, s,
                                   sg_const(g, LLVMGetIntTypeWidth(LLVMTypeOf(s)), p < 64 ? 1ull << p : 0,
                                            p >= 64 ? 1ull << (p - 64) : 0),
                                   "");
                t = LLVMBuildSelect(SG_B, LLVMBuildICmp(SG_B, LLVMIntNE, bit, LLVMConstNull(LLVMTypeOf(s)), ""),
                                    sg_const(g, w, mask[0], mask[1]), sg_const(g, w, 0, 0), "");
                r = r ? LLVMBuildOr(SG_B, r, t, "") : t;
                continue;
            }
        }
        for (j = i; j < w; ++j)
            if (x->sel[j] == 2 + k && (int)j - (int)x->pos[j] == d && !run_bit[j]) {
                done[j] = 1;
                mask[j / 64] |= 1ull << (j % 64);
            }
        sw = x->src_w[k] ? x->src_w[k] : 256;
        t = x->src[k];
        if (d < 0) { /* (s >> -d) & mask, the mask dropped when the shift leaves nothing else */
            SnxU64 low = sg_ones(sw - (unsigned)-d < w ? sw - (unsigned)-d : w);
            t = LLVMBuildLShr(SG_B, t, LLVMConstInt(LLVMTypeOf(t), (unsigned)-d, 0), "");
            t = sg_cast(g, t, w, 0);
            if (w > 64 || mask[0] != low || mask[1]) t = LLVMBuildAnd(SG_B, t, sg_const(g, w, mask[0], mask[1]), "");
        } else { /* (s & (mask >> d)) << d */
            unsigned keep = sw < w - (unsigned)d ? sw : w - (unsigned)d;
            SnxU64 m = w > 64 ? 0 : mask[0] >> d;
            int whole = w <= 64 && m == sg_ones(keep) && keep == sw;
            t = sg_cast(g, t, w, 0);
            if (!whole && w <= 64) t = LLVMBuildAnd(SG_B, t, sg_const(g, w, m, 0), "");
            if (d) t = LLVMBuildShl(SG_B, t, LLVMConstInt(sg_type(g, w), (unsigned)d, 0), "");
            if (w > 64) t = LLVMBuildAnd(SG_B, t, sg_const(g, w, mask[0], mask[1]), "");
        }
        r = r ? LLVMBuildOr(SG_B, r, t, "") : t;
    }
    for (i = 0; i < w; ++i)
        if (x->sel[i] == 1) {
            SgD ones = *x;
            for (k = 0; k < w; ++k) ones.sel[k] = ones.sel[k] == 1;
            r = LLVMBuildOr(SG_B, r, sg_const(g, w, sg_d_const_word(&ones, 0), sg_d_const_word(&ones, 1)), "");
            break;
        }
    return x->value = r;
}

static inline LLVMValueRef sg_d_value(SgCtx* g, SgD x) { return sg_d_make(g, &x); }
/* the source x is the low bits of, whole, when it is one; else x made */
static inline LLVMValueRef sg_d_low_source(SgCtx* g, SgD* x) {
    unsigned i;
    int one = x->w && x->sel[0] >= 2;
    for (i = 0; one && i < x->w; ++i) one = x->sel[i] == x->sel[0] && x->pos[i] == i;
    return one ? x->src[x->sel[0] - 2] : sg_d_make(g, x);
}

/* ---- dynamic bits: computing ---- */

static inline SgD sg_d_not(SgCtx* g, SgD x) {
    unsigned i;
    if (sg_d_is_const(&x)) {
        for (i = 0; i < x.w; ++i) x.sel[i] ^= 1;
        x.value = NULL;
        return x;
    }
    return sg_d_from(g, LLVMBuildNot(SG_B, sg_d_make(g, &x), ""));
}
/* AND and OR stay maps where every bit has a constant side */
static inline SgD sg_d_and(SgCtx* g, SgD a, SgD b) {
    SgD r = sg_d_empty(a.w);
    unsigned i;
    int map = 1;
    for (i = 0; i < a.w && map; ++i) map = a.sel[i] < 2 || b.sel[i] < 2;
    if (map) {
        for (i = 0; i < a.w; ++i) {
            const SgD* keep = a.sel[i] == 0 || b.sel[i] == 0 ? NULL : a.sel[i] == 1 ? &b : &a;
            if (keep)
                sg_d_put_any(g, &r, i, *keep, i, 1);
            else
                r.sel[i] = 0;
        }
        return r;
    }
    return sg_d_from(g, LLVMBuildAnd(SG_B, sg_d_make(g, &a), sg_d_make(g, &b), ""));
}
static inline SgD sg_d_or(SgCtx* g, SgD a, SgD b) {
    SgD r = sg_d_empty(a.w);
    unsigned i;
    int map = 1;
    for (i = 0; i < a.w && map; ++i) map = a.sel[i] < 2 || b.sel[i] < 2;
    if (map) {
        for (i = 0; i < a.w; ++i) {
            const SgD* keep = a.sel[i] == 1 || b.sel[i] == 1 ? NULL : a.sel[i] == 0 ? &b : &a;
            if (keep)
                sg_d_put_any(g, &r, i, *keep, i, 1);
            else
                r.sel[i] = 1;
        }
        return r;
    }
    return sg_d_from(g, LLVMBuildOr(SG_B, sg_d_make(g, &a), sg_d_make(g, &b), ""));
}
static inline SgD sg_d_xor(SgCtx* g, SgD a, SgD b) {
    return sg_d_from(g, LLVMBuildXor(SG_B, sg_d_make(g, &a), sg_d_make(g, &b), ""));
}
static inline SgD sg_d_add(SgCtx* g, SgD a, SgD b) {
    return sg_d_from(g, LLVMBuildAdd(SG_B, sg_d_make(g, &a), sg_d_make(g, &b), ""));
}
static inline SgD sg_d_sub(SgCtx* g, SgD a, SgD b) {
    return sg_d_from(g, LLVMBuildSub(SG_B, sg_d_make(g, &a), sg_d_make(g, &b), ""));
}
/* a + n, n an integer: the bits of n of a's width */
static inline SgD sg_d_add_int(SgCtx* g, SgD a, long long n) {
    return sg_d_from(g, LLVMBuildAdd(SG_B, sg_d_make(g, &a), sg_const(g, a.w, (SnxU64)n, n < 0 ? ~0ull : 0), ""));
}

/* a 1-bit map's bit as an i1: a flag's 0 or 1 compared with 0, another bit extracted */
static inline LLVMValueRef sg_d_bit_i1(SgCtx* g, const SgD* x, int one) {
    if (x->w == 1 && x->sel[0] >= 2 && x->src_flag[x->sel[0] - 2]) {
        LLVMValueRef s = x->src[x->sel[0] - 2];
        return LLVMBuildICmp(SG_B, one ? LLVMIntNE : LLVMIntEQ, s, LLVMConstNull(LLVMTypeOf(s)), "");
    }
    return NULL;
}
static inline LLVMValueRef sg_d_eq(SgCtx* g, SgD a, SgD b) {
    LLVMValueRef r;
    if (a.w == 1 && sg_d_is_const(&b) && (r = sg_d_bit_i1(g, &a, b.sel[0])) != NULL) return r;
    if (b.w == 1 && sg_d_is_const(&a) && (r = sg_d_bit_i1(g, &b, a.sel[0])) != NULL) return r;
    if (a.w == 1 && b.w == 1 && a.sel[0] >= 2 && b.sel[0] >= 2 && a.src_flag[a.sel[0] - 2] &&
        b.src_flag[b.sel[0] - 2]) {
        LLVMValueRef x = a.src[a.sel[0] - 2], y = b.src[b.sel[0] - 2];
        if (LLVMTypeOf(x) == LLVMTypeOf(y)) return LLVMBuildICmp(SG_B, LLVMIntEQ, x, y, "");
    }
    return LLVMBuildICmp(SG_B, LLVMIntEQ, sg_d_make(g, &a), sg_d_make(g, &b), "");
}
static inline LLVMValueRef sg_d_is_zero(SgCtx* g, SgD x) {
    LLVMValueRef v = sg_d_make(g, &x);
    return LLVMBuildICmp(SG_B, LLVMIntEQ, v, LLVMConstNull(LLVMTypeOf(v)), "");
}
/* IsZeroBit: '1' when x is zero, as the i32 a flag is kept in */
static inline SgD sg_d_is_zero_bit(SgCtx* g, SgD x) {
    LLVMValueRef v = sg_d_make(g, &x);
    return sg_d_flag(sg_bool32(g, LLVMBuildICmp(SG_B, LLVMIntEQ, v, LLVMConstNull(LLVMTypeOf(v)), "")), v);
}
/* a bool as bits(1) */
static inline SgD sg_d_of_bool(SgCtx* g, LLVMValueRef b) { return sg_d_flag(sg_bool32(g, b), NULL); }
/* bit i of x is a constant or a 0-or-1 source; and that value as an i32 */
static inline int sg_d_is_flag_bit(const SgD* x, unsigned i) {
    unsigned s = x->sel[i];
    return s < 2 || (x->src_flag[s - 2] && !x->pos[i]);
}
static inline LLVMValueRef sg_d_flag_bit(SgCtx* g, const SgD* x, unsigned i) {
    unsigned s = x->sel[i];
    return s < 2 ? LLVMConstInt(sg_type(g, 32), s, 0) : sg_cast(g, x->src[s - 2], 32, 0);
}
static inline SgD sg_d_select(SgCtx* g, LLVMValueRef c, SgD a, SgD b) {
    unsigned i;
    int flags = a.w <= 8;
    if (LLVMIsAConstantInt(c)) return LLVMConstIntGetZExtValue(c) ? a : b;
    if (a.nzcv) sg_d_realize(g, &a);
    if (b.nzcv) sg_d_realize(g, &b);
    for (i = 0; i < a.w && flags; ++i) flags = sg_d_is_flag_bit(&a, i) && sg_d_is_flag_bit(&b, i);
    if (flags) { /* flags (CCMP's NZCV): a select of each, which stays a 0-or-1 source */
        SgD r = sg_d_empty(a.w);
        for (i = 0; i < a.w; ++i) {
            LLVMValueRef x = sg_d_flag_bit(g, &a, i), y = sg_d_flag_bit(g, &b, i), v = sg_select_phi(g, c, x, y);
            sg_d_put_any(g, &r, i, sg_d_flag(v ? v : LLVMBuildSelect(SG_B, c, x, y, ""), NULL), 0, 1);
        }
        return r;
    }
    return sg_d_from(g, LLVMBuildSelect(SG_B, c, sg_d_make(g, &a), sg_d_make(g, &b), ""));
}
/* a shift by an amount known only at run time, below the width (MOD of it) */
static inline SgD sg_d_shift_dyn(SgCtx* g, SgD x, LLVMValueRef amount, int kind) {
    LLVMValueRef v = sg_d_make(g, &x), n = sg_cast(g, amount, x.w, 0);
    switch (kind) {
        case 0: return sg_d_from(g, LLVMBuildShl(SG_B, v, n, ""));
        case 1: return sg_d_from(g, LLVMBuildLShr(SG_B, v, n, ""));
        case 2: return sg_d_from(g, LLVMBuildAShr(SG_B, v, n, ""));
        default: {
            LLVMValueRef args[3];
            args[0] = v;
            args[1] = v;
            args[2] = n;
            return sg_d_from(g, sg_intrinsic(g, "llvm.fshr", LLVMTypeOf(v), args, 3));
        }
    }
}

/* LSL_C, LSR_C, ASR_C and ROR_C (kind 0 to 3) by an amount known when it recompiles: the shifted value and the last bit
   shifted out (AArch32's shifter carry), as maps: no IR */
static inline void sg_d_shift_c(SgCtx* g, SgD x, long long n, int kind, SgD* result, SgD* carry) {
    long long w = (long long)x.w;
    if (n <= 0 || (kind == 3 && !w)) sg_fail("a shift with carry by an amount out of its range", n, kind);
    switch (kind) {
        case 0:
            *result = n >= w ? sg_d_from_s(sg_s_zeros(w)) : sg_d_lsl(g, x, n);
            *carry = n <= w ? sg_d_slice(g, x, w - n, w - n) : sg_d_from_s(sg_s_zeros(1));
            return;
        case 1:
            *result = n >= w ? sg_d_from_s(sg_s_zeros(w)) : sg_d_lsr(g, x, n);
            *carry = n <= w ? sg_d_slice(g, x, n - 1, n - 1) : sg_d_from_s(sg_s_zeros(1));
            return;
        case 2:
            *result = sg_d_asr(g, x, n >= w ? w - 1 : n);
            *carry = sg_d_slice(g, x, n >= w ? w - 1 : n - 1, n >= w ? w - 1 : n - 1);
            return;
        default:
            *result = sg_d_ror(g, x, n % w);
            *carry = sg_d_slice(g, *result, w - 1, w - 1);
            return;
    }
}
/* the same by an amount known only at run time (AArch32's register shifts, 0 to 255): llvm.c's shift, every LLVM shift's
   amount kept below the width, so none is poison */
static inline void sg_d_shift_c_dyn(SgCtx* g, SgD x, SgI n, int kind, SgD* result, SgD* carry) {
    LLVMValueRef v = sg_d_make(g, &x), r, c;
    LLVMTypeRef t = LLVMTypeOf(v);
    unsigned w = x.w;
    LLVMValueRef k, one = LLVMConstInt(t, 1, 0), mask = LLVMConstInt(t, w - 1, 0);
    if (LLVMIsATruncInst(n.v) && !n.is_signed && LLVMGetIntTypeWidth(LLVMTypeOf(LLVMGetOperand(n.v, 0))) == w)
        /* a register's low byte (R[s][7 .. 0]): that register ANDed, as the block compiler read the amount */
        k = LLVMBuildAnd(SG_B, LLVMGetOperand(n.v, 0), LLVMConstInt(t, sg_ones(n.w), 0), "");
    else
        k = sg_cast(g, n.v, w, 0);
    LLVMValueRef kw = LLVMBuildAnd(SG_B, k, mask, "");
    LLVMValueRef small = LLVMBuildICmp(SG_B, LLVMIntULT, k, LLVMConstInt(t, w, 0), "");
    LLVMValueRef atmost = LLVMBuildICmp(SG_B, LLVMIntULE, k, LLVMConstInt(t, w, 0), "");
    LLVMValueRef km1 = LLVMBuildAnd(SG_B, LLVMBuildSub(SG_B, k, one, ""), mask, "");
    LLVMValueRef negk = LLVMBuildAnd(SG_B, LLVMBuildSub(SG_B, LLVMConstNull(t), k, ""), mask, "");
    if (n.w > w && !LLVMIsAConstantInt(n.v)) sg_fail("a shift amount wider than the value shifted", n.w, w);
    switch (kind) {
        case 0:
            r = LLVMBuildSelect(SG_B, small, LLVMBuildShl(SG_B, v, kw, ""), LLVMConstNull(t), "");
            c = LLVMBuildSelect(SG_B, atmost, LLVMBuildAnd(SG_B, LLVMBuildLShr(SG_B, v, negk, ""), one, ""),
                                LLVMConstNull(t), "");
            break;
        case 1:
            r = LLVMBuildSelect(SG_B, small, LLVMBuildLShr(SG_B, v, kw, ""), LLVMConstNull(t), "");
            c = LLVMBuildSelect(SG_B, atmost, LLVMBuildAnd(SG_B, LLVMBuildLShr(SG_B, v, km1, ""), one, ""),
                                LLVMConstNull(t), "");
            break;
        case 2:
            r = LLVMBuildAShr(SG_B, v, LLVMBuildSelect(SG_B, small, kw, mask, ""), "");
            c = LLVMBuildSelect(SG_B, small, LLVMBuildAnd(SG_B, LLVMBuildLShr(SG_B, v, km1, ""), one, ""),
                                LLVMBuildLShr(SG_B, v, mask, ""), "");
            break;
        default:
            r = LLVMBuildOr(SG_B, LLVMBuildLShr(SG_B, v, kw, ""), LLVMBuildShl(SG_B, v, negk, ""), "");
            c = LLVMBuildLShr(SG_B, r, mask, "");
            break;
    }
    *result = sg_d_from(g, r);
    *carry = sg_d_flag(sg_cast(g, c, 32, 0), NULL);
}

/* ---- dynamic integers ---- */

static inline SgI sg_i(LLVMValueRef v, int is_signed) {
    SgI r;
    r.v = v;
    r.w = LLVMGetIntTypeWidth(LLVMTypeOf(v));
    r.is_signed = is_signed;
    r.wrap = 0;
    return r;
}
static inline SgI sg_i_const(SgCtx* g, long long v) { return sg_i(LLVMConstInt(sg_type(g, 64), (SnxU64)v, 1), 1); }
static inline SgI sg_i_uint(SgCtx* g, SgD x) { return sg_i(sg_d_make(g, &x), 0); }
static inline SgI sg_i_sint(SgCtx* g, SgD x) { return sg_i(sg_d_make(g, &x), 1); }
static inline SgI sg_i_widen(SgCtx* g, SgI a, unsigned w, int is_signed) {
    if (a.wrap) sg_fail("an integer known only modulo 2^w, used whole", a.w, w);
    a.v = sg_cast(g, a.v, w, a.is_signed);
    a.w = w;
    a.is_signed = is_signed;
    return a;
}
/* the width both operands of + - * take: one more bit for an unsigned operand of a signed result */
static inline unsigned sg_i_room(SgI a, int is_signed) { return a.w + (is_signed && !a.is_signed); }
static inline SgI sg_i_arith(SgCtx* g, SgI a, SgI b, int op) {
    int s = a.is_signed || b.is_signed || op == 1;
    unsigned wa = sg_i_room(a, s), wb = sg_i_room(b, s);
    unsigned w = op == 2 ? wa + wb : (wa > wb ? wa : wb) + 1;
    a = sg_i_widen(g, a, w, s);
    b = sg_i_widen(g, b, w, s);
    if (op == 0) return sg_i(LLVMBuildAdd(SG_B, a.v, b.v, ""), s);
    if (op == 1) return sg_i(LLVMBuildSub(SG_B, a.v, b.v, ""), s);
    return sg_i(LLVMBuildMul(SG_B, a.v, b.v, ""), s);
}
/* a == b, !=, <, <=, >, >= (op 0 to 5) of integers known only at run time: at a width that holds both exactly */
static inline LLVMValueRef sg_i_cmp(SgCtx* g, int op, SgI a, SgI b) {
    static const LLVMIntPredicate sp[6] = {LLVMIntEQ, LLVMIntNE, LLVMIntSLT, LLVMIntSLE, LLVMIntSGT, LLVMIntSGE};
    static const LLVMIntPredicate up[6] = {LLVMIntEQ, LLVMIntNE, LLVMIntULT, LLVMIntULE, LLVMIntUGT, LLVMIntUGE};
    int s = a.is_signed || b.is_signed;
    unsigned wa = sg_i_room(a, s), wb = sg_i_room(b, s), w = wa > wb ? wa : wb;
    if (op < 0 || op > 5) sg_fail("a comparison of integers it does not know", op, 5);
    if (LLVMIsAConstantInt(b.v) && !LLVMIsAConstantInt(a.v)) { /* a constant fits the other side's width */
        long long k = LLVMConstIntGetSExtValue(b.v);
        if (k >= 0 && (a.w >= 63 || k < (1ll << (a.w - (a.is_signed != 0))))) {
            w = a.w;
            s = a.is_signed;
            if (!s && !a.wrap && LLVMIsATruncInst(a.v) && a.w < 64) {
                /* a register's low bits (Shift_C's amount == 0 of R[s][7 .. 0]): that register ANDed, the AND a shift
                   by the same amount reads (sg_d_shift_c_dyn), so LLVM finds one value where the block compiler read
                   one */
                LLVMValueRef src = LLVMGetOperand(a.v, 0);
                LLVMTypeRef t = LLVMTypeOf(src);
                return LLVMBuildICmp(SG_B, up[op], LLVMBuildAnd(SG_B, src, LLVMConstInt(t, sg_ones(a.w), 0), ""),
                                     LLVMConstInt(t, (SnxU64)k, 0), "");
            }
        }
    }
    a = sg_i_widen(g, a, w, s);
    b = sg_i_widen(g, b, w, s);
    return LLVMBuildICmp(SG_B, (s ? sp : up)[op], a.v, b.v, "");
}
static inline SgI sg_i_add(SgCtx* g, SgI a, SgI b) { return sg_i_arith(g, a, b, 0); }
static inline SgI sg_i_sub(SgCtx* g, SgI a, SgI b) { return sg_i_arith(g, a, b, 1); }
static inline SgI sg_i_mul(SgCtx* g, SgI a, SgI b) { return sg_i_arith(g, a, b, 2); }
/* integer_subrange: bits hi..lo of the two's complement integer */
static inline SgD sg_i_slice(SgCtx* g, SgI x, long long hi, long long lo) {
    if (hi >= (long long)x.w && x.wrap) sg_fail("bits above a wrapped integer's width", hi, x.w);
    /* the integer at hi + 1 bits: the bits above aren't read (an exact sum's carry), the ones missing its sign */
    if ((long long)x.w != hi + 1) {
        x.v = sg_cast(g, x.v, (unsigned)hi + 1, x.is_signed);
        x.w = (unsigned)hi + 1;
    }
    return sg_d_slice(g, sg_d_from(g, x.v), hi, lo);
}
/* MOD by a power of two: the low bits, never negative */
static inline SgI sg_i_mod(SgCtx* g, SgI a, long long m) {
    if (m <= 0 || (m & (m - 1))) sg_fail("MOD of a run-time integer by other than a power of two", m, 0);
    if (a.wrap) sg_fail("MOD of a wrapped integer", a.w, m);
    return sg_i(LLVMBuildAnd(SG_B, a.v, LLVMConstInt(LLVMTypeOf(a.v), (SnxU64)m - 1, 0), ""), 0);
}
static inline SgI sg_i_select(SgCtx* g, LLVMValueRef c, SgI a, SgI b) {
    unsigned w = a.w > b.w ? a.w : b.w;
    int s = a.is_signed || b.is_signed;
    SgI r;
    if (LLVMIsAConstantInt(c)) return LLVMConstIntGetZExtValue(c) ? a : b;
    if (!a.wrap && !b.wrap) {
        if ((s && !a.is_signed && a.w == w) || (s && !b.is_signed && b.w == w)) ++w;
        a = sg_i_widen(g, a, w, s);
        b = sg_i_widen(g, b, w, s);
    } else if (a.w != b.w) { /* a wrapped quotient: the other side cast to its width */
        if (a.wrap)
            b.v = sg_cast(g, b.v, a.w, b.is_signed);
        else
            a.v = sg_cast(g, a.v, b.w, a.is_signed);
        w = a.wrap ? a.w : b.w;
    }
    r = sg_i(LLVMBuildSelect(SG_B, c, a.v, b.v, ""), s);
    r.wrap = a.wrap || b.wrap;
    return r;
}
/* RoundTowardsZero(Real(a) / Real(b)) for b != 0, modulo 2^w: a divisor of 0 (a select of the caller's takes that
   case) or -1 under the most negative value, whose quotient doesn't fit, is replaced by 1 */
static inline SgI sg_i_div(SgCtx* g, SgI a, SgI b) {
    unsigned w = a.w > b.w ? a.w : b.w;
    int s = a.is_signed;
    LLVMValueRef zero, avoid, q;
    SgI r;
    if (a.is_signed != b.is_signed) sg_fail("a division of integers of two signednesses", a.is_signed, b.is_signed);
    a = sg_i_widen(g, a, w, s);
    b = sg_i_widen(g, b, w, s);
    zero = LLVMConstNull(LLVMTypeOf(b.v));
    avoid = LLVMBuildICmp(SG_B, LLVMIntEQ, b.v, zero, "");
    if (s) {
        LLVMValueRef minimum = LLVMBuildICmp(SG_B, LLVMIntEQ, a.v, sg_const(g, w, 1ull << (w - 1), 0), "");
        avoid = LLVMBuildOr(
            SG_B, avoid,
            LLVMBuildAnd(SG_B, minimum, LLVMBuildICmp(SG_B, LLVMIntEQ, b.v, LLVMConstAllOnes(LLVMTypeOf(b.v)), ""), ""),
            "");
    }
    b.v = LLVMBuildSelect(SG_B, avoid, LLVMConstInt(LLVMTypeOf(b.v), 1, 0), b.v, "");
    q = s ? LLVMBuildSDiv(SG_B, a.v, b.v, "") : LLVMBuildUDiv(SG_B, a.v, b.v, "");
    r = sg_i(q, s);
    r.wrap = 1;
    return r;
}
/* CountLeadingZeroBits and CountLeadingSignBits (the leading zeros of x XOR (x ASR 1), less one) */
static inline SgI sg_i_clz(SgCtx* g, SgD x) {
    LLVMValueRef v = sg_d_make(g, &x), args[2];
    args[0] = v;
    args[1] = sg_bool(g, 0);
    return sg_i(sg_intrinsic(g, "llvm.ctlz", LLVMTypeOf(v), args, 2), 0);
}
static inline SgI sg_i_cls(SgCtx* g, SgD x) {
    LLVMValueRef v = sg_d_make(g, &x), args[2], one = LLVMConstInt(LLVMTypeOf(v), 1, 0);
    args[0] = LLVMBuildXor(SG_B, v, LLVMBuildAShr(SG_B, v, one, ""), "");
    args[1] = sg_bool(g, 0);
    return sg_i(LLVMBuildSub(SG_B, sg_intrinsic(g, "llvm.ctlz", LLVMTypeOf(v), args, 2), one, ""), 0);
}

/* ---- flags: AddWithCarry and a logical result's NZCV ---- */

/* bit i of x as a flag: the 0-or-1 source itself, or the bit shifted down (and masked unless it is the top) */
static inline LLVMValueRef sg_d_flag_value(SgCtx* g, const SgD* x, unsigned i) {
    unsigned s = x->sel[i], k, p, sw;
    LLVMValueRef v;
    if (s < 2) return LLVMConstInt(sg_type(g, 32), s, 0);
    k = s - 2u;
    p = x->pos[i];
    v = x->src[k];
    if (x->src_flag[k] && !p) return v;
    sw = LLVMGetIntTypeWidth(LLVMTypeOf(v));
    if (p) v = LLVMBuildLShr(SG_B, v, LLVMConstInt(LLVMTypeOf(v), p, 0), "");
    if (p != sw - 1) v = LLVMBuildAnd(SG_B, v, LLVMConstInt(LLVMTypeOf(v), 1, 0), "");
    return v;
}

/* the machine's: AddWithCarry's result and its NZCV (sg_add_with_carry), and the four flags an NZCV not made yet stands
   for, as values (sg_nzcv_flags), each the IR the machine's emitters written by hand made */
static inline void sg_nzcv_flags(SgCtx* g, const SgD* f, LLVMValueRef out[4]);
/* NZCV read other than by the flags: its four bits as a map */
static inline void sg_d_realize(SgCtx* g, SgD* x) {
    LLVMValueRef v[4];
    unsigned i;
    SgD m = sg_d_empty(4);
    if (!x->nzcv) return;
    sg_nzcv_flags(g, x, v);
    for (i = 0; i < 4; ++i) sg_d_put_any(g, &m, 3 - i, sg_d_flag(v[i], NULL), 0, 1);
    *x = m;
}
/* [r[N-1]] @ IsZeroBit(r) @ 0b00, a logical result's NZCV (gen.c's rule): N its top bit, Z whether it is zero, C and
   V 0, made when they are read or written */
static inline SgD sg_d_logic_flags(SgCtx* g, SgD r) {
    SgD f = sg_d_empty(4);
    f.nzcv = SG_NZCV_LOGIC;
    f.nzcv_r = sg_d_make(g, &r);
    return f;
}
