/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_BITS_H
#define SNX_BITS_H

/* Small integer helpers every part of the program had its own copy of: rounding to a multiple, clamping, 128-bit
   products and quotients, byte swaps and bit counts, and the FNV-1a hash the caches key their entries with. */

#include "common/utilities/bits.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* value rounded up to a multiple of a (any a above zero, not only powers of two); the caller keeps the sum in range */
static inline uint64_t snx_align_up(uint64_t value, uint64_t a) { return (value + a - 1) / a * a; }
static inline uint64_t snx_align_down(uint64_t value, uint64_t a) { return value / a * a; }
static inline uint64_t snx_div_up(uint64_t value, uint64_t d) { return (value + d - 1) / d; }

/* count bits of v from bit at (count below 32) */
static inline uint32_t snx_bits(uint32_t v, unsigned at, unsigned count) { return (v >> at) & ((1u << count) - 1); }
/* whether length bytes from offset lie inside a range of total bytes, without the sum overflowing */
static inline int snx_in_range(uint64_t offset, uint64_t length, uint64_t total) {
    return offset <= total && length <= total - offset;
}
/* whether two ranges of bytes share one; an empty range shares none */
static inline int snx_ranges_overlap(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
    return a_size && b_size && a < b + b_size && b < a + a_size;
}

static inline int snx_clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float snx_clamp_float(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline double snx_clamp_double(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint32_t snx_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t snx_max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }
static inline uint64_t snx_min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint64_t snx_max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

/* 128-bit products and quotients of 64-bit values: from 32-bit halves in ISO C, or by the compiler's 128-bit integer
   where it has one (GNU's __int128, which a 32-bit host's compiler lacks). SNX_BITS_PORTABLE takes the halves always,
   as the bits_wide test does to check both */
#if defined(__SIZEOF_INT128__) && !defined(SNX_BITS_PORTABLE)
#define SNX_BITS_INT128 1
__extension__ typedef unsigned __int128 snx_bits_u128;
__extension__ typedef __int128 snx_bits_i128;
#endif

/* a * b: its low 64 bits, and the high 64 in *hi */
static inline uint64_t snx_mul_wide(uint64_t a, uint64_t b, uint64_t* hi) {
#ifdef SNX_BITS_INT128
    snx_bits_u128 p = (snx_bits_u128)a * b;
    *hi = (uint64_t)(p >> 64);
    return (uint64_t)p;
#else
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *hi = ah * bh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return mid << 32 | (uint32_t)ll;
#endif
}
/* hi:lo divided by d, which is above hi: the quotient, and the remainder in *rem */
static inline uint64_t snx_div_wide(uint64_t hi, uint64_t lo, uint64_t d, uint64_t* rem) {
#ifdef SNX_BITS_INT128
    snx_bits_u128 n = (snx_bits_u128)hi << 64 | lo;
    *rem = (uint64_t)(n % d);
    return (uint64_t)(n / d);
#else
    uint64_t q = 0;
    for (int i = 63; i >= 0; --i) { /* hi is the remainder, below d, with lo's bits shifted in from the top */
        uint64_t carry = hi >> 63;
        hi = hi << 1 | (lo >> i & 1);
        q <<= 1;
        if (carry || hi >= d) {
            hi -= d;
            q |= 1;
        }
    }
    *rem = hi;
    return q;
#endif
}
/* the signed product a * b shifted right by k (below 64): the low 64 bits of that 128-bit value */
static inline int64_t snx_mul_shr_s64(int64_t a, int64_t b, unsigned k) {
#ifdef SNX_BITS_INT128
    return (int64_t)((snx_bits_i128)a * b >> k);
#else
    uint64_t hi, lo = snx_mul_wide(a < 0 ? 0 - (uint64_t)a : (uint64_t)a, b < 0 ? 0 - (uint64_t)b : (uint64_t)b, &hi);
    if ((a < 0) != (b < 0)) { /* the product's two's complement */
        lo = ~lo + 1;
        hi = ~hi + (lo == 0);
    }
    return (int64_t)(k ? lo >> k | hi << (64 - k) : lo);
#endif
}
/* the signed quotient a * 2^k / d (k below 64, d not 0), toward zero: the low 64 bits of that 128-bit value */
static inline int64_t snx_div_shl_s64(int64_t a, unsigned k, int64_t d) {
#ifdef SNX_BITS_INT128
    return (int64_t)((snx_bits_i128)a * ((snx_bits_i128)1 << k) / d);
#else
    uint64_t ua = a < 0 ? 0 - (uint64_t)a : (uint64_t)a, ud = d < 0 ? 0 - (uint64_t)d : (uint64_t)d;
    uint64_t hi = k ? ua >> (64 - k) : 0, rem, q = snx_div_wide(hi % ud, ua << k, ud, &rem);
    return (int64_t)((a < 0) != (d < 0) ? 0 - q : q);
#endif
}

/* byte swaps and bit counts (a count of 0 is the width): the compiler's builtins where it has GNU's, the same values in
   ISO C elsewhere and under SNX_BITS_PORTABLE */
#if defined(__GNUC__) && !defined(SNX_BITS_PORTABLE)
static inline uint16_t snx_bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t snx_bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t snx_bswap64(uint64_t v) { return __builtin_bswap64(v); }
static inline unsigned snx_clz32(uint32_t v) { return v ? (unsigned)__builtin_clz(v) : 32; }
static inline unsigned snx_clz64(uint64_t v) { return v ? (unsigned)__builtin_clzll(v) : 64; }
static inline unsigned snx_ctz32(uint32_t v) { return v ? (unsigned)__builtin_ctz(v) : 32; }
static inline unsigned snx_ctz64(uint64_t v) { return v ? (unsigned)__builtin_ctzll(v) : 64; }
static inline unsigned snx_popcount64(uint64_t v) { return (unsigned)__builtin_popcountll(v); }
#else
static inline uint16_t snx_bswap16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint32_t snx_bswap32(uint32_t v) { return v << 24 | (v & 0xFF00u) << 8 | (v >> 8 & 0xFF00u) | v >> 24; }
static inline uint64_t snx_bswap64(uint64_t v) {
    return (uint64_t)snx_bswap32((uint32_t)v) << 32 | snx_bswap32(v >> 32);
}
static inline unsigned snx_clz64(uint64_t v) {
    unsigned n = 0;
    if (!v) return 64;
    for (; !(v >> 63); v <<= 1) ++n;
    return n;
}
static inline unsigned snx_clz32(uint32_t v) { return v ? snx_clz64(v) - 32 : 32; }
static inline unsigned snx_ctz64(uint64_t v) {
    unsigned n = 0;
    if (!v) return 64;
    for (; !(v & 1); v >>= 1) ++n;
    return n;
}
static inline unsigned snx_ctz32(uint32_t v) { return v ? snx_ctz64(v) : 32; }
static inline unsigned snx_popcount64(uint64_t v) {
    unsigned n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}
#endif

/* 64-bit FNV-1a: SNX_FNV_BASIS to start, each call continuing the hash of the bytes before */
#define SNX_FNV_BASIS 0xcbf29ce484222325ull
#define SNX_FNV_PRIME 0x100000001b3ull
/* one FNV-1a step over a whole value (a word, a hash of a part) instead of a byte */
static inline uint64_t snx_fnv_step(uint64_t hash, uint64_t value) { return (hash ^ value) * SNX_FNV_PRIME; }
static inline uint64_t snx_fnv1a(uint64_t hash, const void* bytes, size_t size) {
    const unsigned char* p = (const unsigned char*)bytes;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * SNX_FNV_PRIME;
    return hash;
}

/* qsort comparators over arrays of strings and of 64-bit values */
static inline int snx_compare_strings(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}
static inline int snx_compare_u64(const void* a, const void* b) {
    uint64_t x, y;
    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    return (x > y) - (x < y);
}

#endif
