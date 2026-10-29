/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bits.h's 128-bit products and quotients of 64-bit values, byte swaps and bit counts, built twice
   (tests/CMakeLists.txt): as a host with GNU's builtins and 128-bit integer takes them, and in ISO C alone
   (SNX_BITS_PORTABLE), as a host without them would. Each against values worked out by hand, which any host checks,
   and where the compiler has them against its own, over a million pseudo-random operands and every edge (0, 1, the
   extremes) */
#include "common/utilities/bits.h"
#include <inttypes.h>
#include <stdio.h>

#define INT64_MIN_ (-9223372036854775807ll - 1)

static int g_failures;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static const struct {
    uint64_t a, b, lo, hi;
} g_mul[] = {
    {0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {0x0000000000000001ull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0x0000000000000000ull},
    {0xffffffffffffffffull, 0xffffffffffffffffull, 0x0000000000000001ull, 0xfffffffffffffffeull},
    {0x123456789abcdef0ull, 0xfedcba9876543210ull, 0x236d88fe5618cf00ull, 0x121fa00ad77d7422ull},
    {0x8000000000000000ull, 0x0000000000000002ull, 0x0000000000000000ull, 0x0000000000000001ull},
    {0x00000000ffffffffull, 0x00000000ffffffffull, 0xfffffffe00000001ull, 0x0000000000000000ull},
};
static const struct {
    uint64_t hi, lo, d, q, rem;
} g_div[] = {
    {0x0000000000000000ull, 0x0000000000000005ull, 0x0000000000000007ull, 0x0000000000000000ull, 0x5ull},
    {0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000002ull, 0x8000000000000000ull, 0x0ull},
    {0x0000000000000006ull, 0xffffffffffffffffull, 0x0000000000000007ull, 0xffffffffffffffffull, 0x6ull},
    {0x0123456789abcdefull, 0xfedcba9876543210ull, 0x8000000000000001ull, 0x02468acf13579bdfull, 0x7c962fc962fc9631ull},
    {0xfffffffffffffffeull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xfffffffffffffffeull},
};
static const struct {
    int64_t a, b;
    unsigned k;
    int64_t r;
} g_mul_shr[] = {
    {-3ll, 5ll, 1, -8ll},
    {INT64_MIN_, INT64_MIN_, 14, 0ll},
    {9223372036854775807ll, INT64_MIN_, 32, 2147483648ll},
    {123456789ll, -987654321ll, 14, -7442177191934ll},
    {-1ll, 1ll, 14, -1ll},
    {274877920ll, 206158430208000ll, 32, 13194140160000ll},
};
static const struct {
    int64_t a;
    unsigned k;
    int64_t d, r;
} g_div_shl[] = {
    {-7ll, 14, 3ll, -38229ll},
    {9223372036854775807ll, 14, -1ll, 16384ll},
    {INT64_MIN_, 14, 7ll, -5270498306774157604ll},
    {1ll, 63, 3ll, 3074457345618258602ll},
    {-123456789ll, 14, -1000ll, 2022716030ll},
    {5ll, 0, -2ll, -2ll},
};

#ifdef __SIZEOF_INT128__
/* the reference, the compiler's own 128-bit arithmetic */
__extension__ typedef unsigned __int128 u128;
__extension__ typedef __int128 i128;

static uint64_t g_state = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { /* xorshift64*, with an edge value one time in eight */
    static const uint64_t edges[] = {
        0, 1, 2, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFull, 0x100000000ull};
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    uint64_t v = g_state * 0x2545F4914F6CDD1Dull;
    return (v & 7) ? v : edges[(v >> 3) & 7];
}

static void against_int128(void) {
    for (int i = 0; i < 1000000 && g_failures < 10; ++i) {
        uint64_t a = next(), b = next(), hi, rem, lo = snx_mul_wide(a, b, &hi), d = next() | 1, top = next() % d, q;
        u128 p = (u128)a * b, n = (u128)top << 64 | b;
        CHECK(lo == (uint64_t)p && hi == (uint64_t)(p >> 64), "%016" PRIx64 " * %016" PRIx64, a, b);
        q = snx_div_wide(top, b, d, &rem);
        CHECK(q == (uint64_t)(n / d) && rem == (uint64_t)(n % d), "%016" PRIx64 ":%016" PRIx64 " / %016" PRIx64, top, b,
              d);
        unsigned k = (unsigned)(next() & 63);
        int64_t sa = (int64_t)a, sb = (int64_t)b, sd = (int64_t)d;
        CHECK(snx_mul_shr_s64(sa, sb, k) == (int64_t)((i128)sa * sb >> k), "%" PRId64 " * %" PRId64 " >> %u", sa, sb,
              k);
        CHECK(snx_div_shl_s64(sa, k, sd) == (int64_t)((i128)sa * ((i128)1 << k) / sd), "%" PRId64 " * 2^%u / %" PRId64,
              sa, k, sd);
        uint32_t w = (uint32_t)a;
        CHECK(snx_bswap64(a) == __builtin_bswap64(a) && snx_bswap32(w) == __builtin_bswap32(w) &&
                  snx_bswap16((uint16_t)w) == __builtin_bswap16((uint16_t)w),
              "byte swaps of %016" PRIx64, a);
        CHECK((!a || snx_clz64(a) == (unsigned)__builtin_clzll(a)) &&
                  (!a || snx_ctz64(a) == (unsigned)__builtin_ctzll(a)) &&
                  (!w || snx_clz32(w) == (unsigned)__builtin_clz(w)) &&
                  (!w || snx_ctz32(w) == (unsigned)__builtin_ctz(w)) &&
                  snx_popcount64(a) == (unsigned)__builtin_popcountll(a),
              "bit counts of %016" PRIx64, a);
    }
}
#endif

int main(void) {
    size_t i;
    for (i = 0; i < sizeof g_mul / sizeof *g_mul; ++i) {
        uint64_t hi, lo = snx_mul_wide(g_mul[i].a, g_mul[i].b, &hi);
        CHECK(lo == g_mul[i].lo && hi == g_mul[i].hi, "mul %zu: %016" PRIx64 ":%016" PRIx64, i, hi, lo);
    }
    for (i = 0; i < sizeof g_div / sizeof *g_div; ++i) {
        uint64_t rem, q = snx_div_wide(g_div[i].hi, g_div[i].lo, g_div[i].d, &rem);
        CHECK(q == g_div[i].q && rem == g_div[i].rem, "div %zu: %016" PRIx64 " rem %016" PRIx64, i, q, rem);
    }
    for (i = 0; i < sizeof g_mul_shr / sizeof *g_mul_shr; ++i) {
        int64_t r = snx_mul_shr_s64(g_mul_shr[i].a, g_mul_shr[i].b, g_mul_shr[i].k);
        CHECK(r == g_mul_shr[i].r, "mul_shr %zu: %" PRId64 ", not %" PRId64, i, r, g_mul_shr[i].r);
    }
    for (i = 0; i < sizeof g_div_shl / sizeof *g_div_shl; ++i) {
        int64_t r = snx_div_shl_s64(g_div_shl[i].a, g_div_shl[i].k, g_div_shl[i].d);
        CHECK(r == g_div_shl[i].r, "div_shl %zu: %" PRId64 ", not %" PRId64, i, r, g_div_shl[i].r);
    }
    CHECK(snx_bswap16(0x1234) == 0x3412 && snx_bswap32(0x12345678u) == 0x78563412u &&
              snx_bswap64(0x0123456789ABCDEFull) == 0xEFCDAB8967452301ull,
          "byte swaps");
    CHECK(snx_clz32(0) == 32 && snx_clz32(1) == 31 && snx_clz32(0x80000000u) == 0 && snx_clz64(0) == 64 &&
              snx_clz64(1) == 63 && snx_clz64(0x0000800000000000ull) == 16,
          "leading zeros");
    CHECK(snx_ctz32(0) == 32 && snx_ctz32(0x80000000u) == 31 && snx_ctz32(12) == 2 && snx_ctz64(0) == 64 &&
              snx_ctz64(0x8000000000000000ull) == 63 && snx_ctz64(1) == 0,
          "trailing zeros");
    CHECK(snx_popcount64(0) == 0 && snx_popcount64(~0ull) == 64 && snx_popcount64(0x8000000000000001ull) == 2,
          "population count");
#ifdef __SIZEOF_INT128__
    against_int128();
#endif
#ifdef SNX_BITS_INT128
    printf("%s: the compiler's builtins and 128-bit integer\n", g_failures ? "FAIL" : "ok");
#else
    printf("%s: ISO C alone\n", g_failures ? "FAIL" : "ok");
#endif
    return g_failures != 0;
}
