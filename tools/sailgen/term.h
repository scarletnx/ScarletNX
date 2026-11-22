/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Terms in a canonical form (term.c): what an instruction computes, out of the SDM's pseudocode or the back end's
   primitives, built so that two ways of writing the same function become the same term, which the instruction
   selection then compares by index. README.md, "Terms", gives the forms and the identities they rest on */
#ifndef SNX_TOOLS_SAILGEN_TERM_H
#define SNX_TOOLS_SAILGEN_TERM_H

#include <stddef.h>

typedef unsigned long long TU64;

/* an integer of 128 bits, two's complement: the constants of terms of up to 128 bits and the exact integers'
   (2^64 is the range of a 64-bit carry) */
typedef struct {
    TU64 lo;
    TU64 hi;
} I128;

I128 i128(long long v);
I128 i128_u(TU64 v);
I128 i128_add(I128 a, I128 b);
I128 i128_sub(I128 a, I128 b);
I128 i128_neg(I128 a);
I128 i128_mul(I128 a, I128 b); /* modulo 2^128 */
I128 i128_shl(I128 a, unsigned n);
I128 i128_shr(I128 a, unsigned n); /* arithmetic */
I128 i128_pow2(unsigned n);
I128 i128_mask(I128 a, unsigned w); /* the low w bits, zero extended */
I128 i128_sext(I128 a, unsigned w); /* the low w bits, sign extended */
int i128_cmp(I128 a, I128 b);       /* signed: -1, 0, 1 */
int i128_eq(I128 a, I128 b);
int i128_is_zero(I128 a);
int i128_neg_p(I128 a);
I128 i128_and(I128 a, I128 b);
I128 i128_or(I128 a, I128 b);
I128 i128_xor(I128 a, I128 b);
I128 i128_not(I128 a);
I128 i128_fdiv(I128 a, I128 b); /* floor division, b > 0 */
I128 i128_gcd(I128 a, I128 b);  /* of absolute values */
int i128_fits_ll(I128 a);
const char* i128_str(I128 a); /* a static buffer of a few */

/* term kinds. Bits terms have width w (1 to 128); integer terms width 0 */
enum {
    TK_NONE,
    /* bits */
    TK_CONST,  /* k (masked to w) */
    TK_VAR,    /* name */
    TK_LIN,    /* sum of coef[i] * arg[i] + k, modulo 2^w; at least one argument, each an atom */
    TK_AND,    /* args sorted, at least two, a constant last */
    TK_OR,
    TK_XOR,
    TK_MUL,    /* two args, sorted: their product modulo 2^w */
    TK_EXTRACT, /* arg[0][lo + w - 1 : lo]; lo in k */
    TK_CONCAT, /* arg[0] above arg[1] */
    TK_ZEXT,   /* arg[0] zero extended to w */
    TK_SEXT,
    TK_SHLV,   /* arg[0] shifted left by the integer arg[1] (at most w - 1: a count the caller masked) */
    TK_LSHRV,
    TK_ASHRV,
    TK_ROLV,
    TK_RORV,
    TK_ITE,    /* arg[0] (1 bit) ? arg[1] : arg[2] */
    TK_EQZ,    /* 1 bit: arg[0] == 0 */
    TK_GEZ,    /* 1 bit: the integer arg[0] >= 0 */
    TK_CLZ,    /* w bits: the leading zeros of arg[0], as an integer of w bits */
    TK_CTZ,
    TK_POPCNT,
    TK_BSWAP,
    TK_UDIV,   /* arg[0] / arg[1] (arg[1] nonzero), unsigned, truncated to w */
    TK_SDIV,
    TK_UREM,
    TK_SREM,
    TK_FN,     /* an uninterpreted function: name(args), w bits (the manual's helpers sailgen doesn't know) */
    /* integers */
    TK_ICONST, /* k */
    TK_ILIN,   /* sum of coef[i] * arg[i] + k, exact */
    TK_UINT,   /* the bits arg[0] as an unsigned integer */
    TK_SINT,
    TK_IMUL,   /* two integers' exact product, sorted */
    TK_IUDIV,  /* integers: arg[0] / arg[1] of nonnegative ones; truncated toward zero of signed ones; the remainders */
    TK_ISDIV,
    TK_IUREM,
    TK_ISREM,
    TK_UNKNOWN, /* a value the evaluation could not give (a flag a loop leaves, the manual's 'undefined'): w bits.
                   Every one is new, so nothing compares equal to it */
    TK_COUNT
};

typedef struct {
    unsigned char kind;
    unsigned short w;
    unsigned n;        /* arguments */
    unsigned* arg;
    I128* coef;        /* TK_LIN, TK_ILIN: one per argument */
    I128 k;            /* TK_CONST, TK_ICONST, TK_LIN, TK_ILIN: the constant; TK_EXTRACT: lo */
    const char* name;  /* TK_VAR, TK_FN */
    I128 lo, hi;       /* integers: the range the term's value is in */
} Term;

extern Term* g_terms;

/* every constructor returns a canonical term's index (0 is none); a name given is copied */
unsigned t_const(unsigned w, I128 k);
unsigned t_var(const char* name, unsigned w);
unsigned t_unknown(unsigned w);
unsigned t_fn(const char* name, unsigned w, const unsigned* args, unsigned n);
unsigned t_add(unsigned a, unsigned b);
unsigned t_sub(unsigned a, unsigned b);
unsigned t_neg(unsigned a);
unsigned t_not(unsigned a);
unsigned t_mul(unsigned a, unsigned b);
unsigned t_and(unsigned a, unsigned b);
unsigned t_or(unsigned a, unsigned b);
unsigned t_xor(unsigned a, unsigned b);
unsigned t_extract(unsigned a, unsigned hi, unsigned lo);
unsigned t_concat(unsigned hi, unsigned lo);
unsigned t_zext(unsigned a, unsigned w);
unsigned t_sext(unsigned a, unsigned w);
unsigned t_resize(unsigned a, unsigned w); /* zero extended or truncated to w */
unsigned t_shl(unsigned a, unsigned n);    /* n: an integer term */
unsigned t_lshr(unsigned a, unsigned n);
unsigned t_ashr(unsigned a, unsigned n);
unsigned t_rol(unsigned a, unsigned n);
unsigned t_ror(unsigned a, unsigned n);
unsigned t_ite(unsigned c, unsigned a, unsigned b);
unsigned t_eq(unsigned a, unsigned b);  /* bits or integers: 1 bit */
unsigned t_ult(unsigned a, unsigned b); /* bits as unsigned integers, or integers */
unsigned t_slt(unsigned a, unsigned b);
unsigned t_ge0(unsigned i);             /* the integer i >= 0 */
unsigned t_clz(unsigned a);
unsigned t_ctz(unsigned a);
unsigned t_popcnt(unsigned a);
unsigned t_bswap(unsigned a);
unsigned t_udiv(unsigned a, unsigned b);
unsigned t_sdiv(unsigned a, unsigned b);
unsigned t_urem(unsigned a, unsigned b);
unsigned t_srem(unsigned a, unsigned b);
/* integers */
unsigned t_iconst(I128 k);
unsigned t_uint(unsigned a);
unsigned t_sint(unsigned a);
unsigned t_iadd(unsigned a, unsigned b);
unsigned t_isub(unsigned a, unsigned b);
unsigned t_ineg(unsigned a);
unsigned t_imul(unsigned a, unsigned b);
unsigned t_trunc(unsigned i, unsigned w); /* the integer i modulo 2^w, as bits */
unsigned t_iudiv(unsigned a, unsigned b);
unsigned t_isdiv(unsigned a, unsigned b);
unsigned t_iurem(unsigned a, unsigned b);
unsigned t_isrem(unsigned a, unsigned b);

/* t with each from[i] (a variable) replaced by to[i] */
unsigned t_subst(unsigned t, const unsigned* from, const unsigned* to, unsigned n);
int t_is_const(unsigned t, I128* k); /* bits or integer */
int t_mentions(unsigned t, unsigned var); /* whether t's value can depend on var */
unsigned t_width(unsigned t);
/* the term as text, for the reports */
void t_print(unsigned t, char* buf, size_t size);
void t_reset(void);

#endif
