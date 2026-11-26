/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* x64_helpers.c's snx_x64_sse, what the x86-64 back end's code calls in place of an SSE instruction its host lacks
   (tools/sailgen/x64.h's sgx_vf_helped_forms), against the instruction itself on this host: every form, its every
   immediate the back end gives, random operands and the edges (0, all ones, one bit). Skipped (77) where the host
   lacks the instruction or is no x86-64 */
#include "common/cpu/aarch64/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

void snx_x64_sse(CPU* c, void* out, const void* x, const void* y, const void* z, u32 desc);

#if defined(__x86_64__)
static struct {
    int failures;
    u64 seed;
} g_sse;
static u64 rnd(void) {
    g_sse.seed ^= g_sse.seed << 13, g_sse.seed ^= g_sse.seed >> 7, g_sse.seed ^= g_sse.seed << 17;
    return g_sse.seed;
}
/* operand k of a run: the edges first, then random */
static void operand(unsigned k, u64 v[2]) {
    static const u64 edges[] = {0,
                                ~0ull,
                                1,
                                1ull << 63,
                                0x8000000000000001ull,
                                0x8080808080808080ull,
                                0x7FFF80007FFF8000ull,
                                0x0F0E0D0C8B0A0908ull};
    enum { EDGES = sizeof edges / sizeof *edges };
    if (k < EDGES * EDGES)
        v[0] = edges[k % EDGES], v[1] = edges[k / EDGES];
    else
        v[0] = rnd(), v[1] = rnd();
}
[[gnu::target("pclmul")]] static void pclmulqdq(const u64* a, const u64* b, unsigned imm, u64* r) {
    __m128i x = _mm_loadu_si128((const __m128i*)a), y = _mm_loadu_si128((const __m128i*)b), z;
    switch (imm) {
        case 0x00: z = _mm_clmulepi64_si128(x, y, 0x00); break;
        case 0x01: z = _mm_clmulepi64_si128(x, y, 0x01); break;
        case 0x10: z = _mm_clmulepi64_si128(x, y, 0x10); break;
        default: z = _mm_clmulepi64_si128(x, y, 0x11); break;
    }
    _mm_storeu_si128((__m128i*)r, z);
}
[[gnu::target("sse4.2")]] static void pcmpgtq(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_cmpgt_epi64(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("ssse3")]] static void pshufb(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
/* PALIGNR's immediate, a constant of the instruction's: every one the back end gives (EXT's 1 to 15), and 0, 16, 31 */
[[gnu::target("ssse3")]] static void palignr(const u64* a, const u64* b, unsigned imm, u64* r) {
    __m128i x = _mm_loadu_si128((const __m128i*)a), y = _mm_loadu_si128((const __m128i*)b), z;
    switch (imm) {
        case 0: z = _mm_alignr_epi8(x, y, 0); break;
        case 1: z = _mm_alignr_epi8(x, y, 1); break;
        case 2: z = _mm_alignr_epi8(x, y, 2); break;
        case 3: z = _mm_alignr_epi8(x, y, 3); break;
        case 4: z = _mm_alignr_epi8(x, y, 4); break;
        case 5: z = _mm_alignr_epi8(x, y, 5); break;
        case 6: z = _mm_alignr_epi8(x, y, 6); break;
        case 7: z = _mm_alignr_epi8(x, y, 7); break;
        case 8: z = _mm_alignr_epi8(x, y, 8); break;
        case 9: z = _mm_alignr_epi8(x, y, 9); break;
        case 10: z = _mm_alignr_epi8(x, y, 10); break;
        case 11: z = _mm_alignr_epi8(x, y, 11); break;
        case 12: z = _mm_alignr_epi8(x, y, 12); break;
        case 13: z = _mm_alignr_epi8(x, y, 13); break;
        case 14: z = _mm_alignr_epi8(x, y, 14); break;
        case 15: z = _mm_alignr_epi8(x, y, 15); break;
        case 16: z = _mm_alignr_epi8(x, y, 16); break;
        default: z = _mm_alignr_epi8(x, y, 31); break;
    }
    _mm_storeu_si128((__m128i*)r, z);
}
/* PABS's: of the second operand (the back end gives the source as both) */
[[gnu::target("ssse3")]] static void pabsb(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_abs_epi8(_mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("ssse3")]] static void pabsw(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_abs_epi16(_mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("ssse3")]] static void pabsd(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_abs_epi32(_mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("ssse3")]] static void pmulhrsw(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_mulhrs_epi16(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmulld(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_mullo_epi32(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmuldq(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_mul_epi32(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pcmpeqq(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_cmpeq_epi64(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmaxsb(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_max_epi8(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmaxsd(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_max_epi32(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmaxuw(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_max_epu16(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmaxud(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_max_epu32(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pminsb(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_min_epi8(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pminsd(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_min_epi32(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pminuw(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_min_epu16(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pminud(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)imm;
    _mm_storeu_si128((__m128i*)r,
                     _mm_min_epu32(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmovsxbw(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_cvtepi8_epi16(_mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmovsxwd(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmovsxdq(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_cvtepi32_epi64(_mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmovzxbw(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_cvtepu8_epi16(_mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmovzxwd(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)b)));
}
[[gnu::target("sse4.1")]] static void pmovzxdq(const u64* a, const u64* b, unsigned imm, u64* r) {
    (void)a, (void)imm;
    _mm_storeu_si128((__m128i*)r, _mm_cvtepu32_epi64(_mm_loadu_si128((const __m128i*)b)));
}
/* ROUNDPS's and ROUNDPD's every immediate (the back end gives 8 to 11): of the second operand */
[[gnu::target("sse4.1")]] static void roundps(const u64* a, const u64* b, unsigned imm, u64* r) {
    __m128i y = _mm_loadu_si128((const __m128i*)b), z;
    (void)a;
    switch (imm) {
        case 0: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 0)); break;
        case 1: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 1)); break;
        case 2: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 2)); break;
        case 3: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 3)); break;
        case 4: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 4)); break;
        case 5: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 5)); break;
        case 6: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 6)); break;
        case 7: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 7)); break;
        case 8: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 8)); break;
        case 9: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 9)); break;
        case 10: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 10)); break;
        case 11: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 11)); break;
        case 12: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 12)); break;
        case 13: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 13)); break;
        case 14: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 14)); break;
        case 15: z = _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(y), 15)); break;
        default: z = y; break;
    }
    _mm_storeu_si128((__m128i*)r, z);
}
[[gnu::target("sse4.1")]] static void roundpd(const u64* a, const u64* b, unsigned imm, u64* r) {
    __m128i y = _mm_loadu_si128((const __m128i*)b), z;
    (void)a;
    switch (imm) {
        case 0: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 0)); break;
        case 1: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 1)); break;
        case 2: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 2)); break;
        case 3: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 3)); break;
        case 4: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 4)); break;
        case 5: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 5)); break;
        case 6: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 6)); break;
        case 7: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 7)); break;
        case 8: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 8)); break;
        case 9: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 9)); break;
        case 10: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 10)); break;
        case 11: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 11)); break;
        case 12: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 12)); break;
        case 13: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 13)); break;
        case 14: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 14)); break;
        case 15: z = _mm_castpd_si128(_mm_round_pd(_mm_castsi128_pd(y), 15)); break;
        default: z = y; break;
    }
    _mm_storeu_si128((__m128i*)r, z);
}
/* for the roundings (fp 1 singles, 2 doubles): elements near the integers, exponents from 2^-3 to past the fraction's
   bits, halves and their neighbours, and NaNs, infinities and denormals among them */
static void fp_operand(unsigned k, unsigned fp, u64 v[2]) {
    const unsigned f = fp == 1 ? 23 : 52, x = fp == 1 ? 8 : 11, bias = (1u << (x - 1)) - 1, n = fp == 1 ? 4 : 2;
    v[0] = v[1] = 0;
    for (unsigned i = 0; i < n; ++i) {
        const u64 s = rnd(), m = s % 4 == 0 ? 1ull << (f - 1) : s % 4 == 1 ? (rnd() & ((1ull << f) - 1)) | 1 : rnd();
        u64 e = bias - 3 + (rnd() % (f + 6)), bits;
        if (s % 29 == 0) e = 0;             /* a denormal */
        if (s % 31 == 0) e = (1u << x) - 1; /* an infinity or a NaN */
        bits = (s >> 40 & 1) << (f + x) | e << f | (m & ((1ull << f) - 1));
        if (k < 16 && s % 2) bits = (u64)(bias + k % 4) << f | 1ull << (f - 1 - k / 4 % 2); /* 1.5, 2.25... */
        if (k >= 16 && k < 48) { /* zeros, the smallest denormal, a half, one, an infinity, a NaN, each signed */
            const u64 exps[8] = {0, 0, bias - 1, bias, (1u << x) - 1, (1u << x) - 1, bias - 1, bias - 2};
            const u64 fracs[8] = {0, 1, 0, 0, 0, 1, 1, (1ull << f) - 1};
            const unsigned kind = (k + i) % 8;
            bits = (u64)(k / 8 % 2) << (f + x) | exps[kind] << f | fracs[kind];
        }
        v[i * (fp == 1 ? 32 : 64) / 64] |= bits << (i * (fp == 1 ? 32 : 64) % 64);
    }
}
static void check(const char* form, unsigned number, unsigned imm,
                  void (*real)(const u64*, const u64*, unsigned, u64*)) {
    const unsigned fp = !strcmp(form, "ROUNDPS") ? 1 : !strcmp(form, "ROUNDPD") ? 2 : 0;
    for (unsigned k = 0; k < 4096; ++k) {
        u64 a[2], b[2], want[2], got[2];
        operand(k, a);
        operand(k * 7 + 3, b);
        if (fp) fp_operand(k, fp, b);
        real(a, b, imm, want);
        snx_x64_sse(NULL, got, a, b, a, number | imm << 8);
        if (memcmp(want, got, 16)) {
            if (++g_sse.failures < 8)
                fprintf(stderr,
                        "FAIL: %s imm %02x of %016llx:%016llx and %016llx:%016llx is %016llx:%016llx, not "
                        "%016llx:%016llx\n",
                        form, imm, (unsigned long long)a[1], (unsigned long long)a[0], (unsigned long long)b[1],
                        (unsigned long long)b[0], (unsigned long long)got[1], (unsigned long long)got[0],
                        (unsigned long long)want[1], (unsigned long long)want[0]);
        }
    }
}
#endif

int main(void) {
#if defined(__x86_64__)
    unsigned a, b, c, d;
    g_sse.seed = 0x9e3779b97f4a7c15ull;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 1)) || !(c & (1u << 9)) || !(c & (1u << 19)) ||
        !(c & (1u << 20))) {
        printf("x64_sse_helped: this host has no PCLMULQDQ, SSSE3, SSE4.1 or SSE4.2 to hold the helpers to\n");
        return 77;
    }
    /* x64.h's sgx_vf_helped_forms, in order; PMULL's immediates */
    for (unsigned imm = 0; imm < 0x12; imm += imm == 0x01 ? 0x0F : 1)
        if (imm <= 0x01 || imm >= 0x10) check("PCLMULQDQ", 0, imm, pclmulqdq);
    check("PCMPGTQ", 1, 0, pcmpgtq);
    check("PSHUFB", 2, 0, pshufb);
    for (unsigned imm = 0; imm <= 17; ++imm) check("PALIGNR", 3, imm == 17 ? 31 : imm, palignr);
    check("PABSB", 4, 0, pabsb);
    check("PABSW", 5, 0, pabsw);
    check("PABSD", 6, 0, pabsd);
    check("PMULHRSW", 7, 0, pmulhrsw);
    check("PMULLD", 8, 0, pmulld);
    check("PMULDQ", 9, 0, pmuldq);
    check("PCMPEQQ", 10, 0, pcmpeqq);
    check("PMAXSB", 11, 0, pmaxsb);
    check("PMAXSD", 12, 0, pmaxsd);
    check("PMAXUW", 13, 0, pmaxuw);
    check("PMAXUD", 14, 0, pmaxud);
    check("PMINSB", 15, 0, pminsb);
    check("PMINSD", 16, 0, pminsd);
    check("PMINUW", 17, 0, pminuw);
    check("PMINUD", 18, 0, pminud);
    check("PMOVSXBW", 19, 0, pmovsxbw);
    check("PMOVSXWD", 20, 0, pmovsxwd);
    check("PMOVSXDQ", 21, 0, pmovsxdq);
    check("PMOVZXBW", 22, 0, pmovzxbw);
    check("PMOVZXWD", 23, 0, pmovzxwd);
    check("PMOVZXDQ", 24, 0, pmovzxdq);
    for (unsigned imm = 0; imm < 16; ++imm) check("ROUNDPS", 25, imm, roundps);
    for (unsigned imm = 0; imm < 16; ++imm) check("ROUNDPD", 26, imm, roundpd);
    printf("x64_sse_helped: %d failures\n", g_sse.failures);
    return g_sse.failures != 0;
#else
    printf("x64_sse_helped: not an x86-64 host\n");
    return 77;
#endif
}
