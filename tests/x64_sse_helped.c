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
    static const u64 edges[] = {0, ~0ull, 1, 1ull << 63, 0x8000000000000001ull};
    if (k < 25)
        v[0] = edges[k % 5], v[1] = edges[k / 5];
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
static void check(const char* form, unsigned number, unsigned imm,
                  void (*real)(const u64*, const u64*, unsigned, u64*)) {
    for (unsigned k = 0; k < 4096; ++k) {
        u64 a[2], b[2], want[2], got[2];
        operand(k, a);
        operand(k * 7 + 3, b);
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
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 1)) || !(c & (1u << 20))) {
        printf("x64_sse_helped: this host has no PCLMULQDQ or SSE4.2 to hold the helpers to\n");
        return 77;
    }
    /* x64.h's sgx_vf_helped_forms, in order; PMULL's immediates */
    for (unsigned imm = 0; imm < 0x12; imm += imm == 0x01 ? 0x0F : 1)
        if (imm <= 0x01 || imm >= 0x10) check("PCLMULQDQ", 0, imm, pclmulqdq);
    check("PCMPGTQ", 1, 0, pcmpgtq);
    printf("x64_sse_helped: %d failures\n", g_sse.failures);
    return g_sse.failures != 0;
#else
    printf("x64_sse_helped: not an x86-64 host\n");
    return 77;
#endif
}
