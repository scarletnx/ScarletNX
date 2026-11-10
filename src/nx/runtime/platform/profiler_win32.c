/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The profile option on Windows: samples the program counter of the runtime's threads every millisecond, and writes
   the functions they spent their time in to profile.txt beside the game every 10 seconds, by thread. A thread whose
   cycle count didn't move since the last tick is waiting, and gives no sample: counted as well, the waits of a hundred
   threads in the system's wait functions were 94% of one game's samples, and its GPU thread's work hid under them.
   An address outside the runtime is given as its module's name and offset, and with the first address into the runtime's
   code found on the stack above it, most often the return address of the runtime function that called into the module:
   a scan, not an unwind, which could take a lock the stopped thread holds */
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
#define PROF_BUCKETS   65536 /* a power of two */
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
static u64 g_samples, g_unplaced, g_ticks;
/* by slot: its thread's cycle count at the last tick, the samples it gave, its name when the file was last written */
static u64 g_cycles[PROF_THREADS];
static u64 g_slot_samples[PROF_THREADS];
static char g_slot_name[PROF_THREADS][32];
/* a bucket's key: the thread's slot + 1 above the address (user addresses are under 2^48) */
#define PROF_SLOT_SHIFT 52
#define PROF_ADDR_MASK  ((1ull << PROF_SLOT_SHIFT) - 1)
/* the samples outside the runtime and the game, by their bucket's key and the runtime address above them on the stack */
#define PROF_PAIRS      16384 /* a power of two */
typedef struct {
    u64 key, caller;
    u32 count;
} ProfPair;
static ProfPair g_pairs[PROF_PAIRS];
static u64 g_text_lo, g_text_hi; /* the runtime's code sections */
static void text_bounds(void) {
    const u8* base = (const u8*)&__ImageBase;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; k++) {
        u64 lo = (u64)(uintptr_t)base + sec[k].VirtualAddress, hi = lo + sec[k].Misc.VirtualSize;
        if (!(sec[k].Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        if (!g_text_lo || lo < g_text_lo) g_text_lo = lo;
        if (hi > g_text_hi) g_text_hi = hi;
    }
}
/* the first address into the runtime's code in the 64 words from the stack pointer sp on, 0 for none. VirtualQuery
   bounds the scan to the committed pages the stack pointer is in */
static u64 stack_caller(u64 sp) {
    MEMORY_BASIC_INFORMATION m;
    u64 end, n, k;
    if (!g_text_hi || !sp || !VirtualQuery((LPCVOID)(uintptr_t)sp, &m, sizeof m) || m.State != MEM_COMMIT) return 0;
    end = (u64)(uintptr_t)m.BaseAddress + m.RegionSize;
    n = end > sp ? (end - sp) / 8 : 0;
    for (k = 0; k < n && k < 64; k++) {
        u64 v = ((const u64*)(uintptr_t)sp)[k];
        if (v >= g_text_lo && v < g_text_hi) return v;
    }
    return 0;
}
static void record_pair(u64 key, u64 caller) {
    u32 i = (u32)(((key >> 4) * 0x9E3779B1u) ^ ((caller >> 4) * 0x85EBCA6Bu)) & (PROF_PAIRS - 1), probe;
    for (probe = 0; probe < 64; probe++, i = (i + 1) & (PROF_PAIRS - 1))
        if ((g_pairs[i].key == key && g_pairs[i].caller == caller) || !g_pairs[i].key) {
            g_pairs[i].key = key;
            g_pairs[i].caller = caller;
            g_pairs[i].count++;
            return;
        }
}
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

static void record(int slot, u64 pc, u64 sp) {
    const ProfFn* f = fn_at(pc);
    u64 key = f ? f->host : pc & ~(u64)15;
    u32 i, probe;
    g_samples++;
    g_slot_samples[slot]++;
    if (!key) key = 16;
    key = (key & PROF_ADDR_MASK) | (u64)(slot + 1) << PROF_SLOT_SHIFT; /* never 0, which marks a free bucket */
    if (!f && (pc < g_text_lo || pc >= g_text_hi)) record_pair(key, stack_caller(sp) & ~(u64)15);
    i = (u32)(((key >> 4) ^ (key >> PROF_SLOT_SHIFT) * 0x85EBCA6Bu) * 0x9E3779B1u) & (PROF_BUCKETS - 1);
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
static int pair_cmp(const void* a, const void* b) {
    u32 x = ((const ProfPair*)a)->count, y = ((const ProfPair*)b)->count;
    return x > y ? -1 : x < y;
}
/* the slots' names now, for those whose threads still run (GetThreadDescription is Windows 10 1607's) */
static void slot_names(void) {
    typedef HRESULT(WINAPI * DescriptionFn)(HANDLE, PWSTR*);
    static DescriptionFn description;
    int i, used = __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE);
    if (!description)
        description =
            (DescriptionFn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    for (i = 0; description && i < used; i++) {
        HANDLE h = __atomic_load_n(&g_threads[i], __ATOMIC_ACQUIRE);
        PWSTR wide = NULL;
        if (!h || !SUCCEEDED(description(h, &wide)) || !wide) continue;
        if (wide[0] &&
            !WideCharToMultiByte(CP_UTF8, 0, wide, -1, g_slot_name[i], (int)sizeof g_slot_name[i], NULL, NULL))
            g_slot_name[i][0] = 0;
        g_slot_name[i][sizeof g_slot_name[i] - 1] = 0;
        LocalFree(wide);
    }
}
/* what holds a host address outside the runtime and the game: its module's name, and the offset into it */
static void module_of(u64 a, char* out, size_t size) {
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)a, &m) &&
        GetModuleFileNameA(m, path, sizeof path)) {
        const char* base = strrchr(path, '\\');
        snprintf(out, size, "%s+%llx", base ? base + 1 : path, (unsigned long long)(a - (u64)(uintptr_t)m));
    } else
        snprintf(out, size, "host %llx", (unsigned long long)a);
}
static void write_profile(u64 started) {
    static ProfBucket sorted[PROF_BUCKETS];
    static int order[PROF_THREADS];
    char path[1200];
    u64 image = (u64)(size_t)&__ImageBase;
    FILE* f;
    int i, j, n = 0, used = __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE);
    slot_names();
    snprintf(path, sizeof path, "%sprofile.txt", g_exedir);
    f = fopen(path, "w");
    if (!f) return;
    for (i = 0; i < PROF_BUCKETS; i++)
        if (g_buckets[i].key) sorted[n++] = g_buckets[i];
    qsort(sorted, (size_t)n, sizeof *sorted, bucket_cmp);
    fprintf(f,
            "ScarletNX profile: %llu samples of threads that ran since the tick before, in %.0f s and %llu ticks, %d "
            "thread slots used, %llu not placed\n",
            (unsigned long long)g_samples, (double)(plat_ns() - started) / 1e9, (unsigned long long)g_ticks, used,
            (unsigned long long)g_unplaced);
    /* the threads by their samples, as a share of the ticks: how much of one processor each kept busy */
    for (i = 0; i < used; i++) order[i] = i;
    for (i = 1; i < used; i++)
        for (j = i; j > 0 && g_slot_samples[order[j]] > g_slot_samples[order[j - 1]]; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    fprintf(f, "threads by the ticks they ran in:");
    for (i = 0; i < used && i < 16 && g_slot_samples[order[i]]; i++)
        fprintf(f, "%s %s %.0f%%", i ? "," : "", g_slot_name[order[i]][0] ? g_slot_name[order[i]] : "?",
                g_ticks ? 100.0 * (double)g_slot_samples[order[i]] / (double)g_ticks : 0.0);
    fprintf(f, "\n");
    /* every bucket: a runtime function's samples are spread over its 16-byte runs, summed by its symbols afterwards */
    for (i = 0; i < n; i++) {
        u64 a = sorted[i].key & PROF_ADDR_MASK;
        int slot = (int)(sorted[i].key >> PROF_SLOT_SHIFT) - 1;
        const char* who = slot >= 0 && slot < PROF_THREADS && g_slot_name[slot][0] ? g_slot_name[slot] : "?";
        const ProfFn* fn = fn_at(a);
        double pct = g_samples ? 100.0 * sorted[i].count / (double)g_samples : 0;
        char where[MAX_PATH + 32];
        if (fn && fn->host == a)
            snprintf(where, sizeof where, "game function %llx", (unsigned long long)fn->guest);
        else if (a >= image && a - image < 0x10000000u)
            snprintf(where, sizeof where, "runtime +%llx", (unsigned long long)(a - image));
        else
            module_of(a, where, sizeof where);
        fprintf(f, "%6.2f%% %8u  %-24s %s\n", pct, sorted[i].count, who, where);
    }
    /* the samples outside the runtime and the game, by the runtime address above them on the stack */
    static ProfPair pairs[PROF_PAIRS];
    for (i = 0, n = 0; i < PROF_PAIRS; i++)
        if (g_pairs[i].key) pairs[n++] = g_pairs[i];
    qsort(pairs, (size_t)n, sizeof *pairs, pair_cmp);
    fprintf(f, "outside the runtime, with the runtime address above them on the stack (0: none):\n");
    for (i = 0; i < n; i++) {
        u64 a = pairs[i].key & PROF_ADDR_MASK;
        int slot = (int)(pairs[i].key >> PROF_SLOT_SHIFT) - 1;
        const char* who = slot >= 0 && slot < PROF_THREADS && g_slot_name[slot][0] ? g_slot_name[slot] : "?";
        char where[MAX_PATH + 32];
        module_of(a, where, sizeof where);
        fprintf(f, "%6.2f%% %8u  %-24s %s <- runtime +%llx\n",
                g_samples ? 100.0 * pairs[i].count / (double)g_samples : 0.0, pairs[i].count, who, where,
                (unsigned long long)(pairs[i].caller ? pairs[i].caller - image : 0));
    }
    fclose(f);
}

static DWORD WINAPI sampler(LPVOID unused) {
    u64 started = plat_ns(), next_write = started + (u64)PROF_PERIOD_MS * 1000000u;
    (void)unused;
    SNX_THREAD_NAME("profiler");
    fns_index();
    text_bounds();
    for (;;) {
        int i, used = __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE);
        Sleep(1);
        g_ticks++;
        for (i = 0; i < used; i++) {
            HANDLE h = __atomic_load_n(&g_threads[i], __ATOMIC_ACQUIRE);
            CONTEXT c;
            ULONG64 cycles = 0, last;
            if (!h) continue;
            if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { /* the thread ended: its slot is free again */
                __atomic_store_n(&g_threads[i], NULL, __ATOMIC_RELEASE);
                CloseHandle(h);
                g_cycles[i] = 0;
                continue;
            }
            /* a thread that ran less than about 20 microseconds since the last tick is taken as waiting */
            last = g_cycles[i];
            if (!QueryThreadCycleTime(h, &cycles) || (g_cycles[i] = cycles) - last < 100000 || !last) continue;
            if (SuspendThread(h) == (DWORD)-1) continue;
            /* nothing here may allocate or lock: the stopped thread could hold the heap's lock */
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c)) {
#if defined(_M_ARM64) || defined(__aarch64__)
                record(i, (u64)c.Pc, (u64)c.Sp);
#else
                record(i, (u64)c.Rip, (u64)c.Rsp);
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
