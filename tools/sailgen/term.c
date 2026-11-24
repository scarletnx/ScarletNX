/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Terms in a canonical form, hash-consed: each constructor applies the identities below before it makes a node, and
   a node is made once, so two terms are equal exactly when their indices are (for what the identities reach; a pair
   they miss compares unequal, which only ever costs a match, never makes a wrong one).

   Bits (width w, modulo 2^w):
   - addition, subtraction, negation, NOT and multiplication by a constant are one linear form, sum c_i * x_i + k
     modulo 2^w over atoms x_i (what is not linear), its coefficients in [0, 2^w), merged, nonzero, by the atoms'
     order: NOT x = -x - 1 (two's complement), x << k = 2^k * x, x * 2 = x + x;
   - AND, OR and XOR are flat, sorted and deduplicated (of one bit, OR is NOT of the AND of the NOTs) (x AND x = x, x XOR x = 0), a constant last and folded; a mask
     of the low bits is a zero extension of an extraction (x AND (2^k - 1) = ZeroExtend(x[k-1:0])); XOR with all ones is
     NOT; of one bit, the linear form is the XOR of the atoms of odd coefficient;
   - an extraction is pushed into what it extracts from: a concatenation's part, an extension's operand, the operands
     of AND, OR and XOR, and the atoms of a linear form or of a product when it starts at bit 0 (the low bits of a sum
     and of a product depend on the operands' low bits alone);
   - a shift or a rotation by a constant is an extraction and an extension or a concatenation; by a variable it stays
     a node, its count an integer;
   Integers (exact): a linear form over atoms UInt(x), SInt(x) and exact products, with the range of its value:
   UInt(x) in [0, 2^w - 1], SInt(x) in [-2^(w-1), 2^(w-1) - 1]; UInt(NOT x) = 2^w - 1 - UInt(x); SInt(NOT x) =
   -SInt(x) - 1; UInt(ZeroExtend(x)) = UInt(x), SInt(SignExtend(x)) = SInt(x). Truncating one to w bits is the linear
   form modulo 2^w of its atoms truncated.
   Predicates (one bit): an integer comparison is L >= 0 for a linear form L, decided by L's range where it can be,
   its coefficients divided by their gcd (sum g c_i x_i + k >= 0 iff sum c_i x_i + floor(k / g) >= 0) and its first
   coefficient positive (L >= 0 iff NOT (-L - 1 >= 0)); bits' equality is a difference compared with zero.

   This is what makes the two manuals' flags one: Arm's AddWithCarry gives C = (UInt(result) != unsigned_sum), Intel's
   Vol. 1 sets CF where the unsigned result doesn't fit the destination; both are (sum not in [0, 2^w)), which the
   range of the sum turns into one comparison of the operands (SUB's CF: UInt(a) - UInt(b) < 0; SUBS's C:
   UInt(a) + 2^w - 1 - UInt(b) + 1 >= 2^w, the same comparison's negation) */
#include "term.h"
#include "sailgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 128-bit integers ---- */

I128 i128(long long v) {
    I128 r;
    r.lo = (TU64)v;
    r.hi = v < 0 ? ~0ull : 0;
    return r;
}
I128 i128_u(TU64 v) {
    I128 r = {v, 0};
    return r;
}
I128 i128_add(I128 a, I128 b) {
    I128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}
I128 i128_not(I128 a) {
    I128 r = {~a.lo, ~a.hi};
    return r;
}
I128 i128_neg(I128 a) { return i128_add(i128_not(a), i128(1)); }
I128 i128_sub(I128 a, I128 b) { return i128_add(a, i128_neg(b)); }
I128 i128_and(I128 a, I128 b) {
    I128 r = {a.lo & b.lo, a.hi & b.hi};
    return r;
}
I128 i128_or(I128 a, I128 b) {
    I128 r = {a.lo | b.lo, a.hi | b.hi};
    return r;
}
I128 i128_xor(I128 a, I128 b) {
    I128 r = {a.lo ^ b.lo, a.hi ^ b.hi};
    return r;
}
static void mul64(TU64 a, TU64 b, TU64* lo, TU64* hi) {
    TU64 a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    TU64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    TU64 mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    *lo = (p00 & 0xffffffffu) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
I128 i128_mul(I128 a, I128 b) {
    I128 r;
    mul64(a.lo, b.lo, &r.lo, &r.hi);
    r.hi += a.lo * b.hi + a.hi * b.lo;
    return r;
}
I128 i128_shl(I128 a, unsigned n) {
    I128 r;
    if (n >= 128) return i128(0);
    if (n >= 64) {
        r.hi = a.lo << (n - 64);
        r.lo = 0;
        return r;
    }
    if (n == 0) return a;
    r.hi = (a.hi << n) | (a.lo >> (64 - n));
    r.lo = a.lo << n;
    return r;
}
I128 i128_shr(I128 a, unsigned n) {
    I128 r;
    long long h = (long long)a.hi;
    if (n >= 128) return i128(h < 0 ? -1 : 0);
    if (n >= 64) {
        r.lo = n == 64 ? a.hi : (TU64)(h >> (n - 64));
        r.hi = h < 0 ? ~0ull : 0;
        return r;
    }
    if (n == 0) return a;
    r.lo = (a.lo >> n) | (a.hi << (64 - n));
    r.hi = (TU64)(h >> n);
    return r;
}
I128 i128_pow2(unsigned n) { return i128_shl(i128(1), n); }
I128 i128_mask(I128 a, unsigned w) {
    if (w >= 128) return a;
    if (w >= 64) {
        a.hi &= w == 64 ? 0 : (~0ull >> (128 - w));
        return a;
    }
    a.hi = 0;
    a.lo &= w == 0 ? 0 : (~0ull >> (64 - w));
    return a;
}
I128 i128_sext(I128 a, unsigned w) {
    if (w == 0 || w >= 128) return a;
    return i128_shr(i128_shl(a, 128 - w), 128 - w);
}
int i128_cmp(I128 a, I128 b) {
    long long ah = (long long)a.hi, bh = (long long)b.hi;
    if (ah != bh) return ah < bh ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}
int i128_eq(I128 a, I128 b) { return a.lo == b.lo && a.hi == b.hi; }
int i128_is_zero(I128 a) { return !a.lo && !a.hi; }
int i128_neg_p(I128 a) { return (long long)a.hi < 0; }
static I128 i128_abs(I128 a) { return i128_neg_p(a) ? i128_neg(a) : a; }
/* unsigned division of nonnegative numbers by shift and subtract */
static I128 udivmod(I128 a, I128 b, I128* rem) {
    I128 q = i128(0), r = i128(0);
    int i;
    for (i = 127; i >= 0; --i) {
        r = i128_shl(r, 1);
        if ((i >= 64 ? a.hi >> (i - 64) : a.lo >> i) & 1) r.lo |= 1;
        if (i128_cmp(r, b) >= 0) {
            r = i128_sub(r, b);
            if (i >= 64) q.hi |= 1ull << (i - 64);
            else
                q.lo |= 1ull << i;
        }
    }
    if (rem) *rem = r;
    return q;
}
I128 i128_fdiv(I128 a, I128 b) {
    I128 rem, q;
    if (!i128_neg_p(a)) return udivmod(a, b, NULL);
    q = udivmod(i128_neg(a), b, &rem);
    q = i128_neg(q);
    if (!i128_is_zero(rem)) q = i128_sub(q, i128(1));
    return q;
}
I128 i128_gcd(I128 a, I128 b) {
    a = i128_abs(a);
    b = i128_abs(b);
    while (!i128_is_zero(b)) {
        I128 r;
        udivmod(a, b, &r);
        a = b;
        b = r;
    }
    return a;
}
int i128_fits_ll(I128 a) { return i128_eq(a, i128((long long)a.lo)); }
const char* i128_str(I128 a) {
    static char bufs[8][48];
    static int at;
    char* b = bufs[at++ & 7];
    if (i128_fits_ll(a)) snprintf(b, 48, "%lld", (long long)a.lo);
    else
        snprintf(b, 48, "0x%016llx%016llx", (unsigned long long)a.hi, (unsigned long long)a.lo);
    return b;
}

/* ---- the table ---- */

Term* g_terms;
static unsigned g_count, g_cap;
static unsigned* g_hash;
static unsigned g_hash_cap;

static TU64 mix(TU64 h, TU64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

static TU64 hash_of(const Term* t) {
    TU64 h = mix(t->kind, t->w);
    unsigned i;
    h = mix(h, t->n);
    for (i = 0; i < t->n; ++i) {
        h = mix(h, t->arg[i]);
        if (t->coef) h = mix(mix(h, t->coef[i].lo), t->coef[i].hi);
    }
    h = mix(mix(h, t->k.lo), t->k.hi);
    if (t->name) {
        const char* s;
        for (s = t->name; *s; ++s) h = mix(h, (unsigned char)*s);
    }
    return h;
}

static int same(const Term* a, const Term* b) {
    unsigned i;
    if (a->kind != b->kind || a->w != b->w || a->n != b->n || !i128_eq(a->k, b->k)) return 0;
    if ((a->name == NULL) != (b->name == NULL) || (a->name && strcmp(a->name, b->name))) return 0;
    if ((a->coef == NULL) != (b->coef == NULL)) return 0;
    for (i = 0; i < a->n; ++i) {
        if (a->arg[i] != b->arg[i]) return 0;
        if (a->coef && !i128_eq(a->coef[i], b->coef[i])) return 0;
    }
    return 1;
}

static void rehash(void) {
    unsigned i, cap = g_hash_cap ? g_hash_cap * 2 : 1u << 14;
    free(g_hash);
    g_hash = (unsigned*)sg_alloc(cap * sizeof *g_hash);
    g_hash_cap = cap;
    for (i = 1; i < g_count; ++i) {
        unsigned at = (unsigned)(hash_of(&g_terms[i]) & (cap - 1));
        while (g_hash[at]) at = (at + 1) & (cap - 1);
        g_hash[at] = i;
    }
}

static void range_of(Term* t);
static int range_known(const Term* t);

/* the canonical node of t (its arrays and its name are copied) */
static unsigned make(Term t) {
    unsigned at, id;
    if (t.kind != TK_UNKNOWN) {
        if (!g_hash || (g_count + 1) * 2 > g_hash_cap) rehash();
        at = (unsigned)(hash_of(&t) & (g_hash_cap - 1));
        while (g_hash[at]) {
            if (same(&g_terms[g_hash[at]], &t)) return g_hash[at];
            at = (at + 1) & (g_hash_cap - 1);
        }
    }
    if (g_count + 1 >= g_cap) {
        g_cap = g_cap ? g_cap * 2 : 4096;
        g_terms = (Term*)realloc(g_terms, g_cap * sizeof *g_terms);
        if (!g_terms) {
            fprintf(stderr, "sailgen: out of memory for terms\n");
            exit(1);
        }
        if (!g_count) {
            memset(g_terms, 0, sizeof *g_terms);
            g_count = 1;
        }
    }
    if (!g_count) g_count = 1;
    id = g_count++;
    {
        Term* n = &g_terms[id];
        *n = t;
        if (t.n) {
            n->arg = (unsigned*)sg_alloc(t.n * sizeof *n->arg);
            memcpy(n->arg, t.arg, t.n * sizeof *n->arg);
            if (t.coef) {
                n->coef = (I128*)sg_alloc(t.n * sizeof *n->coef);
                memcpy(n->coef, t.coef, t.n * sizeof *n->coef);
            }
        } else {
            n->arg = NULL;
            n->coef = NULL;
        }
        if (t.name) {
            size_t size = strlen(t.name) + 1;
            char* name = (char*)sg_alloc(size);
            memcpy(name, t.name, size);
            n->name = name;
        }
        if (n->w == 0) range_of(n);
    }
    if (t.kind != TK_UNKNOWN) {
        if ((g_count + 1) * 2 > g_hash_cap) rehash();
        else {
            at = (unsigned)(hash_of(&g_terms[id]) & (g_hash_cap - 1));
            while (g_hash[at]) at = (at + 1) & (g_hash_cap - 1);
            g_hash[at] = id;
        }
    }
    return id;
}

void t_reset(void) {
    unsigned i;
    for (i = 1; i < g_count; ++i) {
        free(g_terms[i].arg);
        free(g_terms[i].coef);
        free((char*)g_terms[i].name);
    }
    free(g_terms);
    free(g_hash);
    g_terms = NULL;
    g_hash = NULL;
    g_count = g_cap = g_hash_cap = 0;
}

static Term blank(int kind, unsigned w) {
    Term t;
    memset(&t, 0, sizeof t);
    t.kind = (unsigned char)kind;
    t.w = (unsigned short)w;
    return t;
}

static unsigned node1(int kind, unsigned w, unsigned a) {
    Term t = blank(kind, w);
    t.n = 1;
    t.arg = &a;
    return make(t);
}
static unsigned node2(int kind, unsigned w, unsigned a, unsigned b) {
    unsigned args[2] = {a, b};
    Term t = blank(kind, w);
    t.n = 2;
    t.arg = args;
    return make(t);
}

[[noreturn]] static void bad(const char* what, unsigned a, unsigned b) {
    fprintf(stderr, "sailgen: terms: %s (%u, %u)\n", what, a, b);
    abort();
}

unsigned t_width(unsigned t) { return g_terms[t].w; }

unsigned t_const(unsigned w, I128 k) {
    Term t = blank(TK_CONST, w);
    if (w == 0 || w > 128) bad("a constant of this width", w, 0);
    t.k = i128_mask(k, w);
    return make(t);
}
unsigned t_iconst(I128 k) {
    Term t = blank(TK_ICONST, 0);
    t.k = k;
    return make(t);
}
unsigned t_var(const char* name, unsigned w) {
    Term t = blank(TK_VAR, w);
    t.name = name;
    return make(t);
}
unsigned t_unknown(unsigned w) { return make(blank(TK_UNKNOWN, w)); }
unsigned t_fn(const char* name, unsigned w, const unsigned* args, unsigned n) {
    Term t = blank(TK_FN, w);
    t.name = name;
    t.n = n;
    t.arg = (unsigned*)args;
    return make(t);
}

int t_is_const(unsigned t, I128* k) {
    if (g_terms[t].kind == TK_CONST || g_terms[t].kind == TK_ICONST) {
        if (k) *k = g_terms[t].k;
        return 1;
    }
    return 0;
}

/* ---- linear forms ---- */

#define MAXLIN 64
typedef struct {
    unsigned n;
    unsigned atom[MAXLIN];
    I128 coef[MAXLIN];
    I128 k;
    unsigned w; /* 0: exact */
} Lin;

static I128 reduce(I128 v, unsigned w) { return w ? i128_mask(v, w) : v; }

static void lin_add(Lin* l, unsigned atom, I128 c) {
    unsigned i;
    c = reduce(c, l->w);
    if (i128_is_zero(c)) return;
    for (i = 0; i < l->n; ++i)
        if (l->atom[i] == atom) {
            l->coef[i] = reduce(i128_add(l->coef[i], c), l->w);
            return;
        }
    if (l->n == MAXLIN) bad("a linear form of too many atoms", l->n, 0);
    l->atom[l->n] = atom;
    l->coef[l->n++] = c;
}

/* t's parts, times c, added to l (t of l's width, or an integer for an exact l) */
static unsigned not_of(unsigned x);
static unsigned idiv_node(int kind, unsigned a, unsigned b);

static void lin_parts(Lin* l, unsigned t, I128 c) {
    const Term* x = &g_terms[t];
    unsigned i;
    if (x->kind == TK_CONST || x->kind == TK_ICONST) {
        l->k = reduce(i128_add(l->k, i128_mul(c, x->k)), l->w);
        return;
    }
    if (x->kind == TK_LIN || x->kind == TK_ILIN) {
        I128 k = x->k;
        unsigned n = x->n;
        for (i = 0; i < n; ++i) lin_add(l, g_terms[t].arg[i], i128_mul(c, g_terms[t].coef[i]));
        l->k = reduce(i128_add(l->k, i128_mul(c, k)), l->w);
        return;
    }
    if (x->kind == TK_ZEXT && l->w) {
        /* ZeroExtend(NOT y) = 2^k - 1 - ZeroExtend(y), y of k bits (SBB's borrow, the carry's negation) */
        unsigned y = not_of(x->arg[0]);
        if (y) {
            unsigned k = g_terms[y].w, z = t_zext(y, l->w);
            lin_parts(l, z, i128_neg(c));
            l->k = reduce(i128_add(l->k, i128_mul(c, i128_sub(i128_pow2(k), i128(1)))), l->w);
            return;
        }
    }
    if (l->w == 1 && x->kind == TK_XOR) {
        /* of one bit, XOR is addition modulo 2 */
        unsigned n = x->n;
        for (i = 0; i < n; ++i) lin_parts(l, g_terms[t].arg[i], c);
        return;
    }
    lin_add(l, t, c);
}

static void lin_sort(Lin* l) {
    unsigned i, j;
    for (i = 1; i < l->n; ++i)
        for (j = i; j > 0 && l->atom[j - 1] > l->atom[j]; --j) {
            unsigned a = l->atom[j];
            I128 c = l->coef[j];
            l->atom[j] = l->atom[j - 1];
            l->coef[j] = l->coef[j - 1];
            l->atom[j - 1] = a;
            l->coef[j - 1] = c;
        }
}

static unsigned xor_n(unsigned w, unsigned* args, unsigned n);

/* a sum of pieces whose bits don't overlap, each a power of two times a value (2^k * x, its low w - k bits at k) or
   times a zero extension (2^k * ZeroExtend(y), y's bits at k): the pieces side by side, which is what the sum is (no
   two add a carry). ROL's 'tempDEST * 2 + tempCF' and ROR's 'tempDEST / 2 + tempCF << (SIZE - 1)' are rotations so.
   0 when the form is not one */
static unsigned lin_as_concat(const Lin* l) {
    unsigned lo[MAXLIN], hi[MAXLIN], piece[MAXLIN], i, j, n = l->n, w = l->w;
    for (i = 0; i < n; ++i) {
        const Term* x = &g_terms[l->atom[i]];
        I128 c = l->coef[i];
        unsigned k = 0, width;
        if (i128_is_zero(c) || !i128_is_zero(i128_and(c, i128_sub(c, i128(1))))) return 0; /* not 2^k */
        while (!((k >= 64 ? c.hi >> (k - 64) : c.lo >> k) & 1)) ++k;
        if (k >= w) return 0;
        if (x->kind == TK_ZEXT) {
            unsigned y = x->arg[0];
            width = g_terms[y].w;
            if (k + width > w) width = w - k;
            piece[i] = width == g_terms[y].w ? y : t_extract(y, width - 1, 0);
        } else {
            width = w - k;
            piece[i] = k ? t_extract(l->atom[i], width - 1, 0) : l->atom[i];
        }
        lo[i] = k;
        hi[i] = k + width - 1;
    }
    for (i = 0; i < n; ++i)
        for (j = i + 1; j < n; ++j)
            if (!(hi[i] < lo[j] || hi[j] < lo[i])) return 0;
    {
        /* from the top down, zeros in the gaps */
        unsigned r = 0, at = w, done = 0;
        while (done < n) {
            unsigned best = n;
            for (i = 0; i < n; ++i)
                if (hi[i] < at && (best == n || hi[i] > hi[best])) best = i;
            if (hi[best] + 1 < at) {
                unsigned z = t_const(at - hi[best] - 1, i128(0));
                r = r ? t_concat(r, z) : z;
            }
            r = r ? t_concat(r, piece[best]) : piece[best];
            at = lo[best];
            ++done;
        }
        if (at > 0) r = t_concat(r, t_const(at, i128(0)));
        return r;
    }
}

static unsigned lin_make(Lin* l) {
    unsigned i, o = 0;
    Term t;
    for (i = 0; i < l->n; ++i)
        if (!i128_is_zero(l->coef[i])) {
            l->atom[o] = l->atom[i];
            l->coef[o++] = l->coef[i];
        }
    l->n = o;
    lin_sort(l);
    if (l->w == 0) {
        if (!l->n) return t_iconst(l->k);
        if (l->n == 1 && i128_eq(l->coef[0], i128(1)) && i128_is_zero(l->k)) return l->atom[0];
        t = blank(TK_ILIN, 0);
    } else {
        if (!l->n) return t_const(l->w, l->k);
        if (l->n == 1 && i128_eq(l->coef[0], i128(1)) && i128_is_zero(l->k)) return l->atom[0];
        if (l->w == 1) {
            /* the XOR of the atoms of odd coefficient, and the constant */
            unsigned args[MAXLIN + 1], m = 0;
            for (i = 0; i < l->n; ++i)
                if (l->coef[i].lo & 1) args[m++] = l->atom[i];
            if (l->k.lo & 1) args[m++] = t_const(1, i128(1));
            if (!m) return t_const(1, i128(0));
            return xor_n(1, args, m);
        }
        if (l->n >= 2 && i128_is_zero(l->k)) {
            unsigned r = lin_as_concat(l);
            if (r) return r;
        }
        t = blank(TK_LIN, l->w);
    }
    t.n = l->n;
    t.arg = l->atom;
    t.coef = l->coef;
    t.k = l->k;
    return make(t);
}

static Lin lin_new(unsigned w) {
    Lin l;
    memset(&l, 0, sizeof l);
    l.w = w;
    return l;
}

static void same_width(unsigned a, unsigned b, const char* what) {
    if (g_terms[a].w != g_terms[b].w) bad(what, g_terms[a].w, g_terms[b].w);
}

unsigned t_add(unsigned a, unsigned b) {
    Lin l;
    same_width(a, b, "an addition of different widths");
    l = lin_new(g_terms[a].w);
    lin_parts(&l, a, i128(1));
    lin_parts(&l, b, i128(1));
    return lin_make(&l);
}
unsigned t_sub(unsigned a, unsigned b) {
    Lin l;
    same_width(a, b, "a subtraction of different widths");
    l = lin_new(g_terms[a].w);
    lin_parts(&l, a, i128(1));
    lin_parts(&l, b, i128(-1));
    return lin_make(&l);
}
unsigned t_neg(unsigned a) {
    Lin l = lin_new(g_terms[a].w);
    lin_parts(&l, a, i128(-1));
    return lin_make(&l);
}
unsigned t_iadd(unsigned a, unsigned b) { return t_add(a, b); }
unsigned t_isub(unsigned a, unsigned b) { return t_sub(a, b); }
unsigned t_ineg(unsigned a) { return t_neg(a); }

unsigned t_not(unsigned a) {
    unsigned w = g_terms[a].w;
    Lin l;
    if (!w) bad("NOT of an integer", a, 0);
    if (w == 1) return t_xor(a, t_const(1, i128(1)));
    l = lin_new(w);
    lin_parts(&l, a, i128(-1));
    l.k = reduce(i128_sub(l.k, i128(1)), w);
    return lin_make(&l);
}

unsigned t_mul(unsigned a, unsigned b) {
    unsigned w = g_terms[a].w;
    I128 k;
    same_width(a, b, "a product of different widths");
    if (t_is_const(b, &k) || t_is_const(a, &k)) {
        Lin l = lin_new(w);
        lin_parts(&l, t_is_const(b, NULL) ? a : b, k);
        return lin_make(&l);
    }
    if (a > b) {
        unsigned s = a;
        a = b;
        b = s;
    }
    return node2(w ? TK_MUL : TK_IMUL, w, a, b);
}
unsigned t_imul(unsigned a, unsigned b) { return t_mul(a, b); }

/* ---- AND, OR, XOR ---- */

static void sort_u(unsigned* v, unsigned n) {
    unsigned i, j;
    for (i = 1; i < n; ++i)
        for (j = i; j > 0 && v[j - 1] > v[j]; --j) {
            unsigned s = v[j];
            v[j] = v[j - 1];
            v[j - 1] = s;
        }
}

static unsigned bitwise(int kind, unsigned w, unsigned* in, unsigned n_in) {
    unsigned args[MAXLIN], n = 0, i, j;
    if (kind == TK_OR && w == 1) {
        /* of one bit, OR is NOT (NOT a AND NOT b): conditions have one form (BE's CF OR ZF, LS's NOT (C AND NOT Z)) */
        unsigned nots[MAXLIN];
        for (i = 0; i < n_in; ++i) nots[i] = t_not(in[i]);
        return t_not(bitwise(TK_AND, 1, nots, n_in));
    }
    I128 ones = i128_mask(i128(-1), w), k = kind == TK_AND ? ones : i128(0);
    int have_k = 0;
    for (i = 0; i < n_in; ++i) {
        const Term* x = &g_terms[in[i]];
        if (x->w != w) bad("a bitwise operation of different widths", x->w, w);
        if (x->kind == kind) {
            for (j = 0; j < x->n; ++j) {
                unsigned y = g_terms[in[i]].arg[j];
                I128 c;
                if (t_is_const(y, &c)) {
                    k = kind == TK_AND ? i128_and(k, c) : kind == TK_OR ? i128_or(k, c) : i128_xor(k, c);
                    have_k = 1;
                } else {
                    if (n == MAXLIN) bad("a bitwise operation of too many operands", n, 0);
                    args[n++] = y;
                }
            }
        } else if (x->kind == TK_CONST) {
            k = kind == TK_AND ? i128_and(k, x->k) : kind == TK_OR ? i128_or(k, x->k) : i128_xor(k, x->k);
            have_k = 1;
        } else {
            if (n == MAXLIN) bad("a bitwise operation of too many operands", n, 0);
            args[n++] = in[i];
        }
    }
    (void)have_k;
    sort_u(args, n);
    /* duplicates: x AND x = x, x OR x = x, x XOR x = 0 */
    {
        unsigned o = 0;
        for (i = 0; i < n; ++i) {
            if (kind == TK_XOR && i + 1 < n && args[i] == args[i + 1]) {
                ++i;
                continue;
            }
            if (kind != TK_XOR && o && args[o - 1] == args[i]) continue;
            args[o++] = args[i];
        }
        n = o;
    }
    if (kind == TK_AND && i128_is_zero(k)) return t_const(w, i128(0));
    if (kind == TK_AND && w == 1)
        /* x AND NOT x is 0, and so is NOT (y1 AND y2 ...) beside each of the y (the AND flattened among them) */
        for (j = 0; j < n; ++j) {
            unsigned y = not_of(args[j]);
            if (!y) continue;
            for (i = 0; i < n; ++i)
                if (i != j && args[i] == y) return t_const(1, i128(0));
            if (g_terms[y].kind == TK_AND) {
                unsigned q, all = 1;
                for (q = 0; q < g_terms[y].n && all; ++q) {
                    unsigned found = 0;
                    for (i = 0; i < n; ++i)
                        if (args[i] == g_terms[y].arg[q]) found = 1;
                    all = found;
                }
                if (all) return t_const(1, i128(0));
            }
        }
    if (kind == TK_OR && i128_eq(k, ones)) return t_const(w, ones);
    if (!n) return t_const(w, k);
    if (kind == TK_XOR && w > 1 && i128_eq(k, ones)) {
        /* XOR with all ones is NOT */
        unsigned r = n == 1 ? args[0] : bitwise(TK_XOR, w, args, n);
        return t_not(r);
    }
    if (kind == TK_AND && !i128_eq(k, ones)) {
        /* a mask of the low bits: an extraction, zero extended */
        unsigned b = 0;
        I128 m = k;
        while (b < w && (b >= 64 ? (m.hi >> (b - 64)) & 1 : (m.lo >> b) & 1)) ++b;
        if (b && i128_eq(k, i128_mask(i128(-1), b))) {
            unsigned r = n == 1 ? args[0] : bitwise(TK_AND, w, args, n);
            return t_zext(t_extract(r, b - 1, 0), w);
        }
    }
    if (kind == TK_OR && n == 2 && i128_is_zero(k)) {
        /* (x >> n) OR (x << (w - n)), n in [0, w]: x rotated right by n (RORX's DEST); the other way a left one */
        unsigned q;
        for (q = 0; q < 2; ++q) {
            const Term* a = &g_terms[args[q]];
            const Term* b = &g_terms[args[1 - q]];
            if (a->kind == TK_LSHRV && b->kind == TK_SHLV && a->arg[0] == b->arg[0] &&
                b->arg[1] == t_isub(t_iconst(i128(w)), a->arg[1]) && i128_cmp(g_terms[a->arg[1]].lo, i128(0)) >= 0 &&
                i128_cmp(g_terms[a->arg[1]].hi, i128(w)) <= 0)
                return t_ror(g_terms[args[q]].arg[0], g_terms[args[q]].arg[1]);
        }
    }
    {
        int keep_k = kind == TK_AND ? !i128_eq(k, ones) : !i128_is_zero(k);
        Term t;
        if (n == 1 && !keep_k) return args[0];
        if (keep_k) args[n++] = t_const(w, k);
        t = blank(kind, w);
        t.n = n;
        t.arg = args;
        return make(t);
    }
}

static unsigned xor_n(unsigned w, unsigned* args, unsigned n) { return bitwise(TK_XOR, w, args, n); }

unsigned t_and(unsigned a, unsigned b) {
    unsigned v[2] = {a, b};
    return bitwise(TK_AND, g_terms[a].w, v, 2);
}
unsigned t_or(unsigned a, unsigned b) {
    unsigned v[2] = {a, b};
    return bitwise(TK_OR, g_terms[a].w, v, 2);
}
unsigned t_xor(unsigned a, unsigned b) {
    unsigned v[2] = {a, b};
    if (g_terms[a].w == 1) {
        Lin l = lin_new(1);
        lin_parts(&l, a, i128(1));
        lin_parts(&l, b, i128(1));
        return lin_make(&l);
    }
    return bitwise(TK_XOR, g_terms[a].w, v, 2);
}

/* ---- widths ---- */

unsigned t_zext(unsigned a, unsigned w) {
    const Term* x = &g_terms[a];
    if (w < x->w) bad("a zero extension to fewer bits", w, x->w);
    if (w == x->w) return a;
    if (x->kind == TK_CONST) return t_const(w, x->k);
    if (x->kind == TK_ZEXT) return t_zext(x->arg[0], w);
    return node1(TK_ZEXT, w, a);
}
unsigned t_sext(unsigned a, unsigned w) {
    const Term* x = &g_terms[a];
    if (w < x->w) bad("a sign extension to fewer bits", w, x->w);
    if (w == x->w) return a;
    if (x->kind == TK_CONST) return t_const(w, i128_sext(x->k, x->w));
    if (x->kind == TK_SEXT) return t_sext(x->arg[0], w);
    if (x->kind == TK_ZEXT) return t_zext(a, w); /* its top bit is 0 */
    return node1(TK_SEXT, w, a);
}
unsigned t_resize(unsigned a, unsigned w) {
    unsigned aw = g_terms[a].w;
    if (w == aw) return a;
    return w > aw ? t_zext(a, w) : t_extract(a, w - 1, 0);
}

static unsigned extract_n(unsigned* args, unsigned n, int kind, unsigned hi, unsigned lo) {
    unsigned out[MAXLIN], i;
    for (i = 0; i < n; ++i) out[i] = t_extract(args[i], hi, lo);
    return bitwise(kind, hi - lo + 1, out, n);
}

unsigned t_extract(unsigned a, unsigned hi, unsigned lo) {
    Term x = g_terms[a];
    unsigned w = hi - lo + 1;
    if (hi < lo || hi >= x.w) bad("an extraction out of its value", hi, x.w);
    if (lo == 0 && hi == x.w - 1u) return a;
    switch (x.kind) {
    case TK_CONST: return t_const(w, i128_shr(x.k, lo));
    case TK_EXTRACT: return t_extract(x.arg[0], (unsigned)x.k.lo + hi, (unsigned)x.k.lo + lo);
    case TK_CONCAT: {
        unsigned l = x.arg[1], h = x.arg[0], wl = g_terms[l].w;
        if (hi < wl) return t_extract(l, hi, lo);
        if (lo >= wl) return t_extract(h, hi - wl, lo - wl);
        return t_concat(t_extract(h, hi - wl, 0), t_extract(l, wl - 1, lo));
    }
    case TK_ZEXT: {
        unsigned y = x.arg[0], wy = g_terms[y].w;
        if (hi < wy) return t_extract(y, hi, lo);
        if (lo >= wy) return t_const(w, i128(0));
        return t_zext(t_extract(y, wy - 1, lo), w);
    }
    case TK_SEXT: {
        unsigned y = x.arg[0], wy = g_terms[y].w;
        if (hi < wy) return t_extract(y, hi, lo);
        if (lo >= wy) return t_sext(t_extract(y, wy - 1, wy - 1), w);
        return t_sext(t_extract(y, wy - 1, lo), w);
    }
    case TK_AND:
    case TK_OR:
    case TK_XOR: {
        unsigned args[MAXLIN];
        memcpy(args, x.arg, x.n * sizeof *args);
        return extract_n(args, x.n, x.kind, hi, lo);
    }
    case TK_LIN:
        if (lo == 0) {
            Lin l = lin_new(w);
            unsigned i, n = x.n;
            I128 k = x.k;
            unsigned atoms[MAXLIN];
            I128 coefs[MAXLIN];
            memcpy(atoms, x.arg, n * sizeof *atoms);
            memcpy(coefs, x.coef, n * sizeof *coefs);
            for (i = 0; i < n; ++i) lin_parts(&l, t_extract(atoms[i], hi, 0), coefs[i]);
            l.k = reduce(i128_add(l.k, k), w);
            return lin_make(&l);
        }
        break;
    case TK_MUL:
        if (lo == 0) return t_mul(t_extract(x.arg[0], hi, 0), t_extract(x.arg[1], hi, 0));
        break;
    case TK_ITE: return t_ite(x.arg[0], t_extract(x.arg[1], hi, lo), t_extract(x.arg[2], hi, lo));
    default: break;
    }
    {
        Term t = blank(TK_EXTRACT, w);
        t.n = 1;
        t.arg = &a;
        t.k = i128_u(lo);
        return make(t);
    }
}

unsigned t_concat(unsigned h, unsigned l) {
    Term xh = g_terms[h], xl = g_terms[l];
    unsigned w = xh.w + xl.w;
    if (w > 128) bad("a concatenation over 128 bits", xh.w, xl.w);
    if (xh.kind == TK_CONST && xl.kind == TK_CONST) return t_const(w, i128_or(i128_shl(xh.k, xl.w), xl.k));
    if (xh.kind == TK_CONST && i128_is_zero(xh.k)) return t_zext(l, w);
    /* adjacent parts of one value */
    if (xh.kind == TK_EXTRACT && xl.kind == TK_EXTRACT && xh.arg[0] == xl.arg[0] &&
        xh.k.lo == xl.k.lo + xl.w)
        return t_extract(xh.arg[0], (unsigned)xh.k.lo + xh.w - 1, (unsigned)xl.k.lo);
    /* the low part's sign, repeated: a sign extension */
    if (xh.kind == TK_SEXT && g_terms[xh.arg[0]].kind == TK_EXTRACT) {
        const Term* e = &g_terms[xh.arg[0]];
        if (e->w == 1 && e->arg[0] == l && e->k.lo == xl.w - 1u) return t_sext(l, w);
    }
    if (xh.kind == TK_EXTRACT && xh.w == 1 && xh.arg[0] == l && xh.k.lo == xl.w - 1u) return t_sext(l, w);
    if (xh.kind == TK_CONCAT) return t_concat(xh.arg[0], t_concat(xh.arg[1], l));
    return node2(TK_CONCAT, w, h, l);
}

/* ---- shifts ---- */

static int small_const(unsigned n, unsigned* v) {
    I128 k;
    if (!t_is_const(n, &k)) return 0;
    if (i128_neg_p(k) || k.hi || k.lo > 4096) {
        *v = 4096;
        return 1;
    }
    *v = (unsigned)k.lo;
    return 1;
}

/* a count as an integer: bits are their unsigned value */
static unsigned count_of(unsigned n) { return g_terms[n].w ? t_uint(n) : n; }

unsigned t_shl(unsigned a, unsigned n) {
    unsigned w = g_terms[a].w, k;
    n = count_of(n);
    if (small_const(n, &k)) {
        Lin l;
        if (k >= w) return t_const(w, i128(0));
        l = lin_new(w);
        lin_parts(&l, a, i128_pow2(k));
        return lin_make(&l);
    }
    return node2(TK_SHLV, w, a, n);
}
unsigned t_lshr(unsigned a, unsigned n) {
    unsigned w = g_terms[a].w, k;
    n = count_of(n);
    if (small_const(n, &k)) {
        if (k >= w) return t_const(w, i128(0));
        if (!k) return a;
        return t_zext(t_extract(a, w - 1, k), w);
    }
    return node2(TK_LSHRV, w, a, n);
}
unsigned t_ashr(unsigned a, unsigned n) {
    unsigned w = g_terms[a].w, k;
    n = count_of(n);
    if (small_const(n, &k)) {
        if (k >= w) k = w - 1;
        if (!k) return a;
        return t_sext(t_extract(a, w - 1, k), w);
    }
    return node2(TK_ASHRV, w, a, n);
}
unsigned t_ror(unsigned a, unsigned n) {
    unsigned w = g_terms[a].w, k;
    n = count_of(n);
    if (small_const(n, &k)) {
        k %= w;
        if (!k) return a;
        return t_concat(t_extract(a, k - 1, 0), t_extract(a, w - 1, k));
    }
    return node2(TK_RORV, w, a, n);
}
unsigned t_rol(unsigned a, unsigned n) {
    unsigned w = g_terms[a].w, k;
    n = count_of(n);
    if (small_const(n, &k)) {
        k %= w;
        if (!k) return a;
        return t_ror(a, t_iconst(i128(w - k)));
    }
    return node2(TK_ROLV, w, a, n);
}

/* ---- choices and predicates ---- */

unsigned t_ite(unsigned c, unsigned a, unsigned b) {
    I128 k;
    unsigned args[3];
    Term t;
    if (g_terms[c].w != 1) bad("a choice on more than one bit", g_terms[c].w, 0);
    same_width(a, b, "a choice of different widths");
    if (t_is_const(c, &k)) return k.lo & 1 ? a : b;
    if (a == b) return a;
    if (g_terms[a].w == 1) {
        /* of one bit, a choice with a constant way is AND or OR: c ? a : 1 is NOT c OR a, c ? a : 0 is c AND a, c ? 1 :
           b is c OR b, c ? 0 : b is NOT c AND b (a fault on one way and a condition on the other: IDIV's) */
        I128 ka, kb;
        int ca = t_is_const(a, &ka), cb = t_is_const(b, &kb);
        if (ca && cb) return ka.lo ? c : t_not(c);
        if (cb) return kb.lo & 1 ? t_or(t_not(c), a) : t_and(c, a);
        if (ca) return ka.lo & 1 ? t_or(c, b) : t_and(t_not(c), b);
    }
    {
        I128 ka, kb;
        unsigned w = g_terms[a].w;
        /* c ? 1 : 0 is c zero extended (SETcc's byte), c ? 0 : 1 its negation */
        if (t_is_const(a, &ka) && t_is_const(b, &kb) && w > 1) {
            if (i128_eq(ka, i128(1)) && i128_is_zero(kb)) return t_zext(c, w);
            if (i128_eq(kb, i128(1)) && i128_is_zero(ka)) return t_zext(t_not(c), w);
        }
        /* a choice between concatenations that share a part: the part, and the choice of the rest */
        if (g_terms[a].kind == TK_CONCAT && g_terms[b].kind == TK_CONCAT &&
            g_terms[g_terms[a].arg[0]].w == g_terms[g_terms[b].arg[0]].w) {
            unsigned ah = g_terms[a].arg[0], al = g_terms[a].arg[1], bh = g_terms[b].arg[0], bl = g_terms[b].arg[1];
            if (ah == bh) return t_concat(ah, t_ite(c, al, bl));
            if (al == bl) return t_concat(t_ite(c, ah, bh), al);
        }
    }
    /* a negated condition picks the other way */
    if (g_terms[c].kind == TK_XOR && g_terms[c].n == 2 && t_is_const(g_terms[c].arg[1], &k) && k.lo == 1)
        return t_ite(g_terms[c].arg[0], b, a);
    args[0] = c, args[1] = a, args[2] = b;
    t = blank(TK_ITE, g_terms[a].w);
    t.n = 3;
    t.arg = args;
    return make(t);
}

unsigned t_ge0(unsigned i) {
    Lin l = lin_new(0);
    const Term* x = &g_terms[i];
    I128 g;
    unsigned j;
    if (x->w) bad("a comparison of bits as an integer", x->w, 0);
    if (range_known(x) && i128_cmp(x->lo, i128(0)) >= 0) return t_const(1, i128(1));
    if (range_known(x) && i128_neg_p(x->hi)) return t_const(1, i128(0));
    lin_parts(&l, i, i128(1));
    if (!l.n) return t_const(1, i128(i128_neg_p(l.k) ? 0 : 1));
    lin_sort(&l);
    g = l.coef[0];
    for (j = 1; j < l.n; ++j) g = i128_gcd(g, l.coef[j]);
    g = i128_abs(g);
    if (!i128_eq(g, i128(1))) {
        for (j = 0; j < l.n; ++j) {
            I128 q = udivmod(i128_abs(l.coef[j]), g, NULL);
            l.coef[j] = i128_neg_p(l.coef[j]) ? i128_neg(q) : q;
        }
        l.k = i128_fdiv(l.k, g);
    }
    if (i128_neg_p(l.coef[0])) {
        /* L >= 0 iff NOT (-L - 1 >= 0) */
        Lin m = lin_new(0);
        for (j = 0; j < l.n; ++j) lin_add(&m, l.atom[j], i128_neg(l.coef[j]));
        m.k = i128_sub(i128_neg(l.k), i128(1));
        return t_not(t_ge0(lin_make(&m)));
    }
    {
        unsigned li = lin_make(&l);
        const Term* y = &g_terms[li];
        if (range_known(y) && i128_cmp(y->lo, i128(0)) >= 0) return t_const(1, i128(1));
        if (range_known(y) && i128_neg_p(y->hi)) return t_const(1, i128(0));
        /* SInt(a) / SInt(b) (truncated) - 2^(w-1) >= 0: the quotient is at most |SInt(a)| <= 2^(w-1), and is that
           only for a the most negative value and b -1 (any other divisor's magnitude is 1 with a positive result
           below 2^(w-1), or at least 2): IDIV's overflow, an identity of a and b */
        if (y->kind == TK_ILIN && y->n == 1 && i128_eq(y->coef[0], i128(1)) && g_terms[y->arg[0]].kind == TK_ISDIV) {
            const Term* q = &g_terms[y->arg[0]];
            unsigned sa = q->arg[0], sb = q->arg[1];
            if (g_terms[sa].kind == TK_SINT && g_terms[sb].kind == TK_SINT &&
                g_terms[g_terms[sa].arg[0]].w == g_terms[g_terms[sb].arg[0]].w) {
                unsigned a = g_terms[sa].arg[0], b = g_terms[sb].arg[0], w = g_terms[a].w;
                if (i128_eq(y->k, i128_neg(i128_pow2(w - 1))))
                    return t_and(t_eq(a, t_const(w, i128_pow2(w - 1))), t_eq(b, t_const(w, i128(-1))));
            }
        }
        /* UInt(b) - 1 >= 0 of one bit b: b */
        if (y->kind == TK_ILIN && y->n == 1 && i128_eq(y->coef[0], i128(1)) && i128_eq(y->k, i128(-1)) &&
            g_terms[y->arg[0]].kind == TK_UINT && g_terms[g_terms[y->arg[0]].arg[0]].w == 1)
            return g_terms[y->arg[0]].arg[0];
        return node1(TK_GEZ, 1, li);
    }
}

unsigned t_eq(unsigned a, unsigned b) {
    unsigned w = g_terms[a].w;
    same_width(a, b, "a comparison of different widths");
    if (!w) {
        unsigned d, k;
        /* UInt(L modulo 2^w) = L iff L is in [0, 2^w); SInt of it, iff L is in [-2^(w-1), 2^(w-1)) (AddWithCarry's
           C and V, and the flags 'set according to the result') */
        for (k = 0; k < 2; ++k) {
            unsigned x = k ? b : a, y = k ? a : b;
            const Term* t = &g_terms[x];
            if ((t->kind == TK_UINT || t->kind == TK_SINT) && t_trunc(y, g_terms[t->arg[0]].w) == t->arg[0]) {
                unsigned wx = g_terms[g_terms[x].arg[0]].w;
                I128 lo = g_terms[x].kind == TK_UINT ? i128(0) : i128_neg(i128_pow2(wx - 1));
                I128 hi = i128_sub(i128_add(lo, i128_pow2(wx)), i128(1));
                return t_and(t_ge0(t_isub(y, t_iconst(lo))), t_ge0(t_isub(t_iconst(hi), y)));
            }
        }
        d = t_isub(a, b);
        return t_and(t_ge0(d), t_ge0(t_ineg(d)));
    }
    if (w == 1) return t_xor(t_xor(a, b), t_const(1, i128(1)));
    {
        unsigned d = t_sub(a, b);
        I128 k;
        const Term* x;
        if (t_is_const(d, &k)) return t_const(1, i128(i128_is_zero(k)));
        x = &g_terms[d];
        /* d == 0 iff -d == 0: the one whose first coefficient is in the lower half */
        if (x->kind == TK_LIN && i128_cmp(x->coef[0], i128_pow2(w - 1)) > 0 && w < 128) d = t_neg(d);
        return node1(TK_EQZ, 1, d);
    }
}

unsigned t_ult(unsigned a, unsigned b) {
    if (g_terms[a].w) {
        a = t_uint(a);
        b = t_uint(b);
    }
    return t_ge0(t_isub(t_isub(b, a), t_iconst(i128(1))));
}
unsigned t_slt(unsigned a, unsigned b) {
    if (g_terms[a].w) {
        a = t_sint(a);
        b = t_sint(b);
    }
    return t_ge0(t_isub(t_isub(b, a), t_iconst(i128(1))));
}

/* ---- integers of bits ---- */

/* x as NOT y (-y - 1 modulo 2^w): y, or 0 */
static unsigned not_of(unsigned x) {
    const Term* t = &g_terms[x];
    unsigned w = t->w;
    I128 ones = i128_mask(i128(-1), w);
    if (w == 1 && t->kind == TK_XOR && t->n == 2) {
        I128 k;
        if (t_is_const(t->arg[1], &k) && k.lo == 1) return t->arg[0];
    }
    if (t->kind == TK_LIN && t->n == 1 && i128_eq(t->coef[0], ones) && i128_eq(t->k, ones)) return t->arg[0];
    return 0;
}

unsigned t_uint(unsigned a) {
    const Term* x = &g_terms[a];
    unsigned w = x->w, y;
    if (!w) return a;
    if (x->kind == TK_CONST) return t_iconst(x->k);
    if (x->kind == TK_ZEXT) return t_uint(x->arg[0]);
    if ((y = not_of(a)) != 0) return t_isub(t_iconst(i128_sub(i128_pow2(w), i128(1))), t_uint(y));
    return node1(TK_UINT, 0, a);
}
unsigned t_sint(unsigned a) {
    const Term* x = &g_terms[a];
    unsigned w = x->w, y;
    if (!w) return a;
    if (x->kind == TK_CONST) return t_iconst(i128_sext(x->k, w));
    if (x->kind == TK_SEXT) return t_sint(x->arg[0]);
    if (x->kind == TK_ZEXT && g_terms[x->arg[0]].w < w) return t_uint(x->arg[0]);
    if ((y = not_of(a)) != 0) return t_isub(t_iconst(i128(-1)), t_sint(y));
    if (w == 1) return t_ineg(t_uint(a));
    return node1(TK_SINT, 0, a);
}

unsigned t_trunc(unsigned i, unsigned w) {
    const Term* x = &g_terms[i];
    if (x->w) return t_resize(i, w);
    switch (x->kind) {
    case TK_ICONST: return t_const(w, x->k);
    case TK_ILIN: {
        Lin l = lin_new(w);
        unsigned j, n = x->n, atoms[MAXLIN];
        I128 coefs[MAXLIN], k = x->k;
        memcpy(atoms, x->arg, n * sizeof *atoms);
        memcpy(coefs, x->coef, n * sizeof *coefs);
        for (j = 0; j < n; ++j) lin_parts(&l, t_trunc(atoms[j], w), coefs[j]);
        l.k = reduce(i128_add(l.k, k), w);
        return lin_make(&l);
    }
    case TK_UINT: {
        unsigned y = x->arg[0], wy = g_terms[y].w;
        return wy >= w ? t_extract(y, w - 1, 0) : t_zext(y, w);
    }
    case TK_SINT: {
        unsigned y = x->arg[0], wy = g_terms[y].w;
        return wy >= w ? t_extract(y, w - 1, 0) : t_sext(y, w);
    }
    case TK_IMUL: {
        unsigned a = x->arg[0], b = x->arg[1], ta = t_trunc(a, w);
        return t_mul(ta, t_trunc(b, w));
    }
    case TK_IUDIV:
    case TK_ISDIV:
    case TK_IUREM:
    case TK_ISREM: return t_fn("trunc", w, &i, 1); /* the quotient's low bits: no identity reaches into it */
    default: bad("a truncation of this integer", x->kind, w);
    }
}

/* ---- ranges ---- */

/* a range past what 128 bits hold exactly is unknown: [-2^126, 2^126], which decides no comparison (t_ge0 folds by
   a range only where it is known) */
static int range_known(const Term* t) {
    I128 big = i128_pow2(125);
    return i128_cmp(t->lo, i128_neg(big)) > 0 && i128_cmp(t->hi, big) < 0;
}
static void range_unknown(Term* t) {
    t->lo = i128_neg(i128_pow2(126));
    t->hi = i128_pow2(126);
}

static void range_of(Term* t) {
    unsigned i;
    for (i = 0; i < t->n; ++i)
        if (g_terms[t->arg[i]].w == 0 && !range_known(&g_terms[t->arg[i]])) {
            range_unknown(t);
            return;
        }
    switch (t->kind) {
    case TK_ICONST: t->lo = t->hi = t->k; break;
    case TK_UINT: {
        unsigned w = g_terms[t->arg[0]].w;
        if (w > 120) {
            range_unknown(t);
            break;
        }
        t->lo = i128(0);
        t->hi = i128_sub(i128_pow2(w), i128(1));
        break;
    }
    case TK_SINT: {
        unsigned w = g_terms[t->arg[0]].w;
        if (w > 120) {
            range_unknown(t);
            break;
        }
        t->lo = i128_neg(i128_pow2(w - 1));
        t->hi = i128_sub(i128_pow2(w - 1), i128(1));
        break;
    }
    case TK_ILIN:
        t->lo = t->hi = t->k;
        for (i = 0; i < t->n; ++i) {
            const Term* a = &g_terms[t->arg[i]];
            I128 x = i128_mul(t->coef[i], a->lo), y = i128_mul(t->coef[i], a->hi);
            if (i128_cmp(x, y) > 0) {
                I128 s = x;
                x = y;
                y = s;
            }
            t->lo = i128_add(t->lo, x);
            t->hi = i128_add(t->hi, y);
        }
        if (!range_known(t) || i128_cmp(t->lo, t->hi) > 0) range_unknown(t);
        break;
    case TK_IMUL: {
        const Term* a = &g_terms[t->arg[0]];
        const Term* b = &g_terms[t->arg[1]];
        I128 c[4];
        int k;
        c[0] = i128_mul(a->lo, b->lo), c[1] = i128_mul(a->lo, b->hi), c[2] = i128_mul(a->hi, b->lo),
        c[3] = i128_mul(a->hi, b->hi);
        t->lo = t->hi = c[0];
        for (k = 1; k < 4; ++k) {
            if (i128_cmp(c[k], t->lo) < 0) t->lo = c[k];
            if (i128_cmp(c[k], t->hi) > 0) t->hi = c[k];
        }
        if (!range_known(t)) range_unknown(t);
        break;
    }
    case TK_IUDIV:
    case TK_IUREM: {
        /* a nonnegative dividend's quotient and remainder are no more than it */
        const Term* a = &g_terms[t->arg[0]];
        t->lo = i128(0);
        t->hi = i128_neg_p(a->hi) ? i128(0) : a->hi;
        break;
    }
    case TK_ISDIV:
    case TK_ISREM: {
        /* truncated toward zero: no larger in magnitude than the dividend */
        const Term* a = &g_terms[t->arg[0]];
        I128 m = i128_abs(a->lo);
        if (i128_cmp(i128_abs(a->hi), m) > 0) m = i128_abs(a->hi);
        t->lo = i128_neg(m);
        t->hi = m;
        break;
    }
    default:
        /* an integer the table knows nothing of: as wide as it can be */
        t->lo = i128_neg(i128_pow2(126));
        t->hi = i128_pow2(126);
        break;
    }
}

/* ---- the rest ---- */

static unsigned unary_bits(int kind, unsigned a) {
    I128 k;
    unsigned w = g_terms[a].w;
    if (t_is_const(a, &k)) {
        unsigned i, n = 0;
        I128 r = i128(0);
        switch (kind) {
        case TK_CLZ:
            for (i = w; i-- > 0 && !((i >= 64 ? k.hi >> (i - 64) : k.lo >> i) & 1);) ++n;
            return t_const(w, i128(n));
        case TK_CTZ:
            for (i = 0; i < w && !((i >= 64 ? k.hi >> (i - 64) : k.lo >> i) & 1); ++i) ++n;
            return t_const(w, i128(n));
        case TK_POPCNT:
            for (i = 0; i < w; ++i) n += (unsigned)((i >= 64 ? k.hi >> (i - 64) : k.lo >> i) & 1);
            return t_const(w, i128(n));
        case TK_BSWAP:
            for (i = 0; i < w / 8; ++i) r = i128_or(r, i128_shl(i128_mask(i128_shr(k, 8 * i), 8), w - 8 - 8 * i));
            return t_const(w, r);
        default: break;
        }
    }
    return node1(kind, w, a);
}
unsigned t_clz(unsigned a) { return unary_bits(TK_CLZ, a); }
unsigned t_ctz(unsigned a) { return unary_bits(TK_CTZ, a); }
unsigned t_popcnt(unsigned a) { return unary_bits(TK_POPCNT, a); }
unsigned t_bswap(unsigned a) { return unary_bits(TK_BSWAP, a); }
unsigned t_udiv(unsigned a, unsigned b) { return node2(TK_UDIV, g_terms[a].w, a, b); }
unsigned t_sdiv(unsigned a, unsigned b) { return node2(TK_SDIV, g_terms[a].w, a, b); }
unsigned t_urem(unsigned a, unsigned b) { return node2(TK_UREM, g_terms[a].w, a, b); }
unsigned t_srem(unsigned a, unsigned b) { return node2(TK_SREM, g_terms[a].w, a, b); }

int t_mentions(unsigned t, unsigned var) {
    const Term* x = &g_terms[t];
    unsigned i;
    if (t == var) return 1;
    for (i = 0; i < x->n; ++i)
        if (t_mentions(g_terms[t].arg[i], var)) return 1;
    return 0;
}

static const char* const k_names[TK_COUNT] = {
    "?", "const", "var", "lin", "and", "or", "xor", "mul", "extract", "concat", "zext", "sext", "shlv", "lshrv",
    "ashrv", "rolv", "rorv", "ite", "eqz", "gez", "clz", "ctz", "popcnt", "bswap", "udiv", "sdiv", "urem", "srem", "fn",
    "iconst", "ilin", "uint", "sint", "imul", "iudiv", "isdiv", "iurem", "isrem", "unknown"};

static size_t put(char* buf, size_t size, size_t at, const char* s) {
    size_t n = strlen(s);
    if (at + n + 1 < size) memcpy(buf + at, s, n + 1);
    return at + n;
}

static size_t print_at(unsigned t, char* buf, size_t size, size_t at) {
    const Term* x = &g_terms[t];
    char tmp[96];
    unsigned i;
    switch (x->kind) {
    case TK_CONST: snprintf(tmp, sizeof tmp, "%s'%u", i128_str(x->k), x->w); return put(buf, size, at, tmp);
    case TK_ICONST: return put(buf, size, at, i128_str(x->k));
    case TK_VAR: snprintf(tmp, sizeof tmp, "%s'%u", x->name, x->w); return put(buf, size, at, tmp);
    case TK_UNKNOWN: snprintf(tmp, sizeof tmp, "unknown#%u'%u", t, x->w); return put(buf, size, at, tmp);
    default: break;
    }
    if (x->kind == TK_FN) at = put(buf, size, at, x->name);
    else
        at = put(buf, size, at, k_names[x->kind]);
    if (x->kind == TK_EXTRACT) {
        snprintf(tmp, sizeof tmp, "[%u:%u]", (unsigned)x->k.lo + x->w - 1, (unsigned)x->k.lo);
        at = put(buf, size, at, tmp);
    } else if (x->w) {
        snprintf(tmp, sizeof tmp, "'%u", x->w);
        at = put(buf, size, at, tmp);
    }
    at = put(buf, size, at, "(");
    for (i = 0; i < x->n; ++i) {
        if (i) at = put(buf, size, at, ", ");
        if (x->coef) {
            at = put(buf, size, at, i128_str(x->coef[i]));
            at = put(buf, size, at, "*");
        }
        at = print_at(x->arg[i], buf, size, at);
    }
    if (x->kind == TK_LIN || x->kind == TK_ILIN) {
        at = put(buf, size, at, " + ");
        at = put(buf, size, at, i128_str(x->k));
    }
    return put(buf, size, at, ")");
}

void t_print(unsigned t, char* buf, size_t size) {
    if (size) buf[0] = 0;
    if (!t) {
        put(buf, size, 0, "none");
        return;
    }
    print_at(t, buf, size, 0);
}

/* ---- substitution: t with each from[i] replaced by to[i], rebuilt through the constructors (so canonical) ---- */

typedef struct {
    const unsigned* from;
    const unsigned* to;
    unsigned n;
} Subst;

/* the memo of the substitution being made: an entry counts where its generation is this one's */
static unsigned* g_memo;
static unsigned* g_memo_gen;
static unsigned g_memo_cap, g_gen;

static unsigned subst_at(Subst* s, unsigned t) {
    Term x;
    unsigned i, r = 0;
    if (t < g_memo_cap && g_memo_gen[t] == g_gen) return g_memo[t];
    for (i = 0; i < s->n; ++i)
        if (s->from[i] == t) return s->to[i];
    x = g_terms[t];
    switch (x.kind) {
    case TK_CONST:
    case TK_ICONST:
    case TK_VAR:
    case TK_UNKNOWN: r = t; break;
    case TK_LIN:
    case TK_ILIN: {
        Lin l = lin_new(x.w);
        unsigned atoms[MAXLIN];
        I128 coefs[MAXLIN];
        memcpy(atoms, x.arg, x.n * sizeof *atoms);
        memcpy(coefs, x.coef, x.n * sizeof *coefs);
        for (i = 0; i < x.n; ++i) lin_parts(&l, subst_at(s, atoms[i]), coefs[i]);
        l.k = reduce(i128_add(l.k, x.k), x.w);
        r = lin_make(&l);
        break;
    }
    case TK_AND:
    case TK_OR:
    case TK_XOR: {
        unsigned args[MAXLIN];
        for (i = 0; i < x.n; ++i) args[i] = subst_at(s, x.arg[i]);
        r = x.w == 1 && x.kind == TK_XOR ? 0 : bitwise(x.kind, x.w, args, x.n);
        if (x.w == 1 && x.kind == TK_XOR) {
            r = args[0];
            for (i = 1; i < x.n; ++i) r = t_xor(r, args[i]);
        }
        break;
    }
    case TK_MUL:
    case TK_IMUL: {
        unsigned a = subst_at(s, x.arg[0]);
        r = t_mul(a, subst_at(s, x.arg[1]));
        break;
    }
    case TK_EXTRACT: r = t_extract(subst_at(s, x.arg[0]), (unsigned)x.k.lo + x.w - 1, (unsigned)x.k.lo); break;
    case TK_CONCAT: {
        unsigned h = subst_at(s, x.arg[0]);
        r = t_concat(h, subst_at(s, x.arg[1]));
        break;
    }
    case TK_ZEXT: r = t_zext(subst_at(s, x.arg[0]), x.w); break;
    case TK_SEXT: r = t_sext(subst_at(s, x.arg[0]), x.w); break;
    case TK_SHLV:
    case TK_LSHRV:
    case TK_ASHRV:
    case TK_ROLV:
    case TK_RORV: {
        unsigned a = subst_at(s, x.arg[0]), n = subst_at(s, x.arg[1]);
        r = x.kind == TK_SHLV ? t_shl(a, n) : x.kind == TK_LSHRV ? t_lshr(a, n) : x.kind == TK_ASHRV ? t_ashr(a, n)
            : x.kind == TK_ROLV ? t_rol(a, n) : t_ror(a, n);
        break;
    }
    case TK_ITE: {
        unsigned c = subst_at(s, x.arg[0]), a = subst_at(s, x.arg[1]);
        r = t_ite(c, a, subst_at(s, x.arg[2]));
        break;
    }
    case TK_EQZ: {
        unsigned d = subst_at(s, x.arg[0]);
        r = t_eq(d, t_const(g_terms[d].w, i128(0)));
        break;
    }
    case TK_GEZ: r = t_ge0(subst_at(s, x.arg[0])); break;
    case TK_CLZ: r = t_clz(subst_at(s, x.arg[0])); break;
    case TK_CTZ: r = t_ctz(subst_at(s, x.arg[0])); break;
    case TK_POPCNT: r = t_popcnt(subst_at(s, x.arg[0])); break;
    case TK_BSWAP: r = t_bswap(subst_at(s, x.arg[0])); break;
    case TK_UDIV:
    case TK_SDIV:
    case TK_UREM:
    case TK_SREM: {
        unsigned a = subst_at(s, x.arg[0]), b = subst_at(s, x.arg[1]);
        r = x.kind == TK_UDIV ? t_udiv(a, b) : x.kind == TK_SDIV ? t_sdiv(a, b) : x.kind == TK_UREM ? t_urem(a, b)
                                                                                    : t_srem(a, b);
        break;
    }
    case TK_IUDIV:
    case TK_ISDIV:
    case TK_IUREM:
    case TK_ISREM: {
        unsigned a = subst_at(s, x.arg[0]), b = subst_at(s, x.arg[1]);
        r = idiv_node(x.kind, a, b);
        break;
    }
    case TK_UINT: r = t_uint(subst_at(s, x.arg[0])); break;
    case TK_SINT: r = t_sint(subst_at(s, x.arg[0])); break;
    case TK_FN: {
        unsigned args[MAXLIN];
        for (i = 0; i < x.n; ++i) args[i] = subst_at(s, x.arg[i]);
        r = t_fn(x.name, x.w, args, x.n);
        break;
    }
    default: bad("a substitution in this term", x.kind, t);
    }
    if (t < g_memo_cap) {
        g_memo[t] = r;
        g_memo_gen[t] = g_gen;
    }
    return r;
}

unsigned t_subst(unsigned t, const unsigned* from, const unsigned* to, unsigned n) {
    Subst s;
    if (g_memo_cap < g_count) {
        unsigned cap = g_count * 2;
        g_memo = (unsigned*)realloc(g_memo, cap * sizeof *g_memo);
        g_memo_gen = (unsigned*)realloc(g_memo_gen, cap * sizeof *g_memo_gen);
        if (!g_memo || !g_memo_gen) exit(1);
        memset(g_memo_gen + g_memo_cap, 0, (cap - g_memo_cap) * sizeof *g_memo_gen);
        g_memo_cap = cap;
    }
    ++g_gen;
    s.from = from;
    s.to = to;
    s.n = n;
    return subst_at(&s, t);
}

/* ---- integer division (DIV's and IDIV's, UDIV's and SDIV's) ---- */

static unsigned idiv_node(int kind, unsigned a, unsigned b) {
    I128 ka, kb;
    if (t_is_const(a, &ka) && t_is_const(b, &kb) && !i128_is_zero(kb)) {
        I128 q, r;
        int neg = i128_neg_p(ka) != i128_neg_p(kb);
        q = udivmod(i128_abs(ka), i128_abs(kb), &r);
        if (kind == TK_IUDIV || kind == TK_ISDIV) return t_iconst(kind == TK_ISDIV && neg ? i128_neg(q) : q);
        return t_iconst(kind == TK_ISREM && i128_neg_p(ka) ? i128_neg(r) : r);
    }
    return node2(kind, 0, a, b);
}
unsigned t_iudiv(unsigned a, unsigned b) { return idiv_node(TK_IUDIV, a, b); }
unsigned t_isdiv(unsigned a, unsigned b) { return idiv_node(TK_ISDIV, a, b); }
unsigned t_iurem(unsigned a, unsigned b) { return idiv_node(TK_IUREM, a, b); }
unsigned t_isrem(unsigned a, unsigned b) { return idiv_node(TK_ISREM, a, b); }
