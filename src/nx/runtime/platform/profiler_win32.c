/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The profile option on Windows: samples the program counter of the runtime's threads every millisecond, and writes
   the functions they spent their time in to profile.txt beside the game every 10 seconds */
#include "common/utilities/common.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/loader/module_abi.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROF_THREADS 256
#define PROF_BUCKETS 16384 /* a power of two */
#define PROF_SHOWN 200
#define PROF_PERIOD_MS 10000

extern char __ImageBase; /* the linker's: the executable's load address */

/* A slot is free when NULL. Threads take one with a compare-exchange; the sampler frees it once the thread ended */
static HANDLE g_threads[PROF_THREADS];
static int g_threads_used; /* slots ever taken: only grows */

/* the sampler's own (no other thread touches these) */
typedef struct {
    u64 key; /* a game function's host address, or a runtime address rounded to 16 bytes; 0 for a free bucket */
    u32 count;
} ProfBucket;
static ProfBucket g_buckets[PROF_BUCKETS];
static u64 g_samples, g_unplaced;
typedef struct {
    u64 host;
    u64 guest;
} ProfFn;
static ProfFn* g_fns; /* the game module's functions by host address */
static u32 g_nfns;

void plat_prof_add_thread(HANDLE h) {
    HANDLE own;
    int i, used;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &own,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                         FALSE, 0))
        return;
    for (i = 0; i < PROF_THREADS; i++) {
        HANDLE none = NULL;
        if (__atomic_compare_exchange_n(&g_threads[i], &none, own, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            used = __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE);
            while (used < i + 1 &&
                   !__atomic_compare_exchange_n(&g_threads_used, &used, i + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            }
            return;
        }
    }
    CloseHandle(own); /* all taken: this one goes unsampled */
}

static int fn_cmp(const void* a, const void* b) {
    u64 x = ((const ProfFn*)a)->host, y = ((const ProfFn*)b)->host;
    return x < y ? -1 : x > y;
}
static void fns_index(void) {
    u32 i;
    if (!g_mod || !g_mod->nentries || !g_mod->entry_fn) return;
    g_fns = malloc((size_t)g_mod->nentries * sizeof *g_fns);
    if (!g_fns) return;
    for (i = 0; i < g_mod->nentries; i++) {
        g_fns[i].host = (u64)(size_t)g_mod->entry_fn[i];
        g_fns[i].guest = g_mod->entry_addr[i];
    }
    qsort(g_fns, g_mod->nentries, sizeof *g_fns, fn_cmp);
    g_nfns = g_mod->nentries;
}
/* the game function pc is in: the closest start at or below it, within 1 MiB */
static const ProfFn* fn_at(u64 pc) {
    u32 lo = 0, hi = g_nfns;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_fns[mid].host <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo && pc - g_fns[lo - 1].host < (1u << 20) ? &g_fns[lo - 1] : NULL;
}

static void record(u64 pc) {
    const ProfFn* f = fn_at(pc);
    u64 key = f ? f->host : pc & ~(u64)15;
    u32 i, probe;
    g_samples++;
    if (!key) key = 16; /* 0 marks a free bucket */
    i = (u32)((key >> 4) * 0x9E3779B1u) & (PROF_BUCKETS - 1);
    for (probe = 0; probe < 64; probe++, i = (i + 1) & (PROF_BUCKETS - 1)) {
        if (g_buckets[i].key == key || !g_buckets[i].key) {
            g_buckets[i].key = key;
            g_buckets[i].count++;
            return;
        }
    }
    g_unplaced++;
}

static int bucket_cmp(const void* a, const void* b) {
    u32 x = ((const ProfBucket*)a)->count, y = ((const ProfBucket*)b)->count;
    return x > y ? -1 : x < y;
}
static void write_profile(u64 started) {
    static ProfBucket sorted[PROF_BUCKETS];
    char path[1200];
    u64 image = (u64)(size_t)&__ImageBase;
    FILE* f;
    int i, n = 0;
    snprintf(path, sizeof path, "%sprofile.txt", g_exedir);
    f = fopen(path, "w");
    if (!f) return;
    for (i = 0; i < PROF_BUCKETS; i++)
        if (g_buckets[i].key) sorted[n++] = g_buckets[i];
    qsort(sorted, (size_t)n, sizeof *sorted, bucket_cmp);
    fprintf(f, "ScarletNX profile: %llu samples in %.0f s, %d thread slots used, %llu not placed\n",
            (unsigned long long)g_samples, (double)(plat_ns() - started) / 1e9,
            __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE), (unsigned long long)g_unplaced);
    for (i = 0; i < n && i < PROF_SHOWN; i++) {
        const ProfFn* fn = fn_at(sorted[i].key);
        double pct = g_samples ? 100.0 * sorted[i].count / (double)g_samples : 0;
        if (fn && fn->host == sorted[i].key)
            fprintf(f, "%6.2f%% %8u  game function %llx\n", pct, sorted[i].count, (unsigned long long)fn->guest);
        else if (sorted[i].key >= image && sorted[i].key - image < 0x10000000u)
            fprintf(f, "%6.2f%% %8u  runtime +%llx\n", pct, sorted[i].count,
                    (unsigned long long)(sorted[i].key - image));
        else
            fprintf(f, "%6.2f%% %8u  host %llx\n", pct, sorted[i].count, (unsigned long long)sorted[i].key);
    }
    fclose(f);
}

static DWORD WINAPI sampler(LPVOID unused) {
    u64 started = plat_ns(), next_write = started + (u64)PROF_PERIOD_MS * 1000000u;
    (void)unused;
    SNX_THREAD_NAME("profiler");
    fns_index();
    for (;;) {
        int i, used = __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE);
        Sleep(1);
        for (i = 0; i < used; i++) {
            HANDLE h = __atomic_load_n(&g_threads[i], __ATOMIC_ACQUIRE);
            CONTEXT c;
            if (!h) continue;
            if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { /* the thread ended: its slot is free again */
                __atomic_store_n(&g_threads[i], NULL, __ATOMIC_RELEASE);
                CloseHandle(h);
                continue;
            }
            if (SuspendThread(h) == (DWORD)-1) continue;
            /* nothing here may allocate or lock: the stopped thread could hold the heap's lock */
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c)) {
#if defined(_M_ARM64) || defined(__aarch64__)
                record((u64)c.Pc);
#else
                record((u64)c.Rip);
#endif
            }
            ResumeThread(h);
        }
        if (plat_ns() >= next_write) {
            write_profile(started);
            next_write += (u64)PROF_PERIOD_MS * 1000000u;
        }
    }
}

void plat_prof_start(void) {
    HANDLE h;
    plat_prof_add_thread(GetCurrentThread()); /* the main thread (the window and the frame loop) */
    h = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (!h) {
        char why[256];
        WARN("profile: the sampling thread could not start: %s\n", snx_os_error(why, sizeof why));
        return;
    }
    SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
    CloseHandle(h);
}
