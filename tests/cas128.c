/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RtAPI.cas128 (common/runtime/cas128.c): the host's 16-byte compare-and-swap, and the locked one the RISC-V and
   LoongArch machines without it take. Both store only a pair that matches, leave it as it is otherwise (the locked
   one puts back the half it swapped), and, under threads adding to a pair {n, ~n} through it, end on the sum with the
   halves still of one pair */
#include "common/runtime/interface.h"
#include <assert.h>
#include <stdio.h>
#ifndef _WIN32
#include <pthread.h>
#endif

typedef uint32_t (*Cas)(uint64_t*, uint64_t, uint64_t, uint64_t, uint64_t);

static void one(Cas cas) {
    _Alignas(16) uint64_t p[2];
    p[0] = 1;
    p[1] = 2;
    assert(cas(p, 1, 2, 3, 4) && p[0] == 3 && p[1] == 4);  /* both match: both stored */
    assert(!cas(p, 9, 4, 5, 6) && p[0] == 3 && p[1] == 4); /* the low half differs: nothing */
    assert(!cas(p, 3, 9, 5, 6) && p[0] == 3 && p[1] == 4); /* the high half differs: the low one back as it was */
    assert(cas(p, 3, 4, 3, 4) && p[0] == 3 && p[1] == 4);  /* the same pair again */
}

#ifndef _WIN32
#define THREADS 4
#define ADDS    20000
static _Alignas(16) uint64_t g_pair[2];
static Cas g_cas;
static void* adder(void* unused) {
    int i;
    (void)unused;
    for (i = 0; i < ADDS; i++)
        for (;;) { /* as LDXP reads it: a half at a time, the compare-and-swap telling a pair torn */
            uint64_t lo = __atomic_load_n(&g_pair[0], __ATOMIC_SEQ_CST),
                     hi = __atomic_load_n(&g_pair[1], __ATOMIC_SEQ_CST);
            if (g_cas(g_pair, lo, hi, lo + 1, ~(lo + 1))) break;
        }
    return NULL;
}
static void threads(Cas cas, const char* name) {
    pthread_t t[THREADS];
    int i;
    g_cas = cas;
    g_pair[0] = 0;
    g_pair[1] = ~(uint64_t)0;
    for (i = 0; i < THREADS; i++) assert(!pthread_create(&t[i], NULL, adder, NULL));
    for (i = 0; i < THREADS; i++) assert(!pthread_join(t[i], NULL));
    printf("%s: %llu adds\n", name, (unsigned long long)g_pair[0]);
    assert(g_pair[0] == (uint64_t)THREADS * ADDS && g_pair[1] == ~g_pair[0]);
}
#endif

int main(void) {
    one(snx_runtime_cas128_locked);
    one(snx_runtime_cas128);
#ifndef _WIN32
    threads(snx_runtime_cas128_locked, "locked");
    threads(snx_runtime_cas128, "the host's");
#endif
    puts("PASS RtAPI.cas128: a pair stored whole or not at all");
    return 0;
}
