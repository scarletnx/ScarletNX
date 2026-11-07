/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The production ownership map with synthetic resources and trap hooks. No GPU or host trapping is needed here:
   cuts preserve byte ranges, resource offsets, region counts and references. memory_own.c tests real traps/threads. */
#include "nx/memory/own.c"
#include <stdarg.h>
#include <time.h>

Settings g_settings;
u8* g_mem;
u64 g_as_size = 64ull << 20;
static unsigned checks;
static u32 landed;
typedef struct { u32 refs; } Resource;

static void check(int ok, int line) {
    checks++;
    if (!ok) {
        fprintf(stderr, "memory_own_cut.c:%d: check failed\n", line);
        exit(1);
    }
}
#define CHECK(x) check((x), __LINE__)

/* Platform calls are inert: this fixture tests metadata only, on one thread. */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    (void)c; (void)m; (void)ns;
    fatal("Unexpected ownership wait in single-threaded fixture");
}
u64 plat_vm_page(void) { return 4096; }
u8* plat_alias_mirror(void) { return g_mem; }
int plat_trap_init(void (*fault)(u8*, int, u32)) { (void)fault; return 0; }
u32 plat_tid(void) { return 1; }
int plat_thread_in_fault(u32 tid) { (void)tid; return 0; }
void plat_dump_fault_thread(u32 tid) { (void)tid; }
int plat_trap_arm(u8* p, u64 n) { (void)p; (void)n; return 0; }
int plat_trap_continue(u8* p, u64 n) { (void)p; (void)n; return 0; }
void plat_trap_disarm(u8* p, u64 n) { (void)p; (void)n; }
int mem_pool_offset(u64 a, u64* off) { *off = a; return 0; }
int mem_own_prepare(u64 a, u64 size, MemPoolRun* runs, u32 max, u32* n) {
    CHECK(max > 0);
    runs[0] = (MemPoolRun){a, size, a};
    *n = 1;
    return 0;
}
void log_crash_mode(void) {}
void logf_(int level, const char* fmt, ...) { (void)level; (void)fmt; }
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
static void retain_resource(void* p) { ((Resource*)p)->refs++; }
static void release_resource(void* p) {
    Resource* r = p;
    CHECK(r->refs > 0);
    r->refs--;
}
static void land_resources(const OwnRange* r, u8* const* dst, u32 n) {
    for (u32 i = 0; i < n; i++) {
        CHECK(r[i].size > 0 && dst[i] == g_mem + r[i].addr);
        CHECK(((Resource*)r[i].res)->refs > 0);
    }
    landed += n;
}

static void verify_regions(void) {
    for (u64 region = 0; region < g_own_nregion; region++) {
        u32 count = 0;
        for (u32 i = 0; i < g_nown; i++)
            count += g_own[i].lo >> OWN_REGION_SHIFT <= region && (g_own[i].hi - 1) >> OWN_REGION_SHIFT >= region;
        CHECK(g_own_region[region] == count);
    }
}
static void reset_map(void) {
    for (u32 i = 0; i < g_nown; i++) release_resource(g_own[i].res);
    g_nown = 0;
    memset(g_own_region, 0, sizeof *g_own_region * (size_t)g_own_nregion);
}
static u32 random_word(u32* state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void differential_cuts(void) {
    Resource resources[128] = {0};
    Own expected[256];
    u32 seed = 781;
    for (u32 trial = 0; trial < 4096; trial++) {
        reset_map();
        u64 at = (random_word(&seed) % 4) * (1ull << OWN_REGION_SHIFT) - 32 + 64;
        for (u32 i = 0; i < 128; i++) {
            u64 gap = random_word(&seed) % 64, size = random_word(&seed) % 96 + 1;
            Own o = {at + gap, at + gap + size, at + gap + 19, &resources[i], i * 100, 0};
            retain_resource(o.res);
            insert_nl(g_nown, &o);
            at = o.hi;
        }
        u64 lo = g_own[0].lo + random_word(&seed) % (u32)(at - g_own[0].lo + 1);
        u64 hi = lo + random_word(&seed) % (u32)(at - lo + 1);
        /* Include exact endpoints, full cuts, empty cuts, and single-owner splits. */
        if (trial % 8 == 0) lo = g_own[0].lo, hi = at;
        if (trial % 8 == 1) lo = hi = g_own[64].lo;
        if (trial % 8 == 2) lo = g_own[32].lo, hi = g_own[96].hi;
        if (trial % 8 == 3) lo = g_own[32].lo + 1, hi = g_own[32].hi - 1;
        if (hi < lo) hi = lo;
        u32 count = 0;
        for (u32 i = 0; i < g_nown; i++) {
            Own o = g_own[i];
            if (hi <= lo || o.hi <= lo || o.lo >= hi) {
                expected[count++] = o;
                continue;
            }
            if (o.lo < lo) {
                expected[count] = o;
                expected[count++].hi = lo;
            }
            if (o.hi > hi) {
                expected[count] = o;
                expected[count].lo = hi;
                expected[count].pool += hi - o.lo;
                expected[count++].res_off += hi - o.lo;
            }
        }
        ResList rel = {0};
        if (hi > lo) cut_nl(lo, hi, &rel);
        res_release(&rel);
        CHECK(g_nown == count);
        for (u32 i = 0; i < count; i++) {
            CHECK(g_own[i].lo == expected[i].lo && g_own[i].hi == expected[i].hi);
            CHECK(g_own[i].pool == expected[i].pool && g_own[i].res == expected[i].res);
            CHECK(g_own[i].res_off == expected[i].res_off && !g_own[i].landing);
        }
        for (u32 r = 0; r < 128; r++) {
            u32 refs = 0;
            for (u32 i = 0; i < count; i++) refs += expected[i].res == &resources[r];
            CHECK(resources[r].refs == refs);
        }
        verify_regions();
    }
    reset_map();
}

static void land_and_replace(void) {
    Resource r = {0}, replacement = {0};
    for (u32 i = 0; i < 256; i++) CHECK(own_begin(0x1000 + i * 64, 32, &r, i * 32) == 0);
    CHECK(r.refs == 256 && g_nown == 256);
    own_acquire(0x1000, 0x1000);
    CHECK(landed == 64 && r.refs == 192 && g_nown == 192);
    CHECK(g_own[0].lo == 0x2000 && g_own[0].res_off == 64 * 32);
    verify_regions();
    CHECK(own_begin(0x2001, 0x1ffe, &replacement, 51) == 0);
    CHECK(r.refs == 65 && replacement.refs == 1 && g_nown == 66);
    CHECK(g_own[0].lo == 0x2000 && g_own[0].hi == 0x2001 && g_own[0].res_off == 64 * 32);
    CHECK(g_own[1].lo == 0x2001 && g_own[1].hi == 0x3fff && g_own[1].res_off == 51);
    verify_regions();
    own_drop(0x1000, 0x5000);
    CHECK(!g_nown && !r.refs && !replacement.refs);
    verify_regions();
}

static u64 now_ns(void) {
    struct timespec ts;
    CHECK(timespec_get(&ts, TIME_UTC) == TIME_UTC);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
static void benchmark(void) {
    const u32 sizes[] = {64, 1024, 4096};
    const u32 repeats[] = {4096, 256, 64};
    for (u32 shape = 0; shape < 3; shape++) {
        u32 count = sizes[shape], rounds = repeats[shape];
        Resource r = {0};
        Own* original = malloc(sizeof *original * count * 2);
        CHECK(original != NULL);
        for (u32 i = 0; i < count * 2; i++) {
            Own o = {0x1000 + i * 64, 0x1020 + i * 64, 0x1000 + i * 64, &r, i * 32, 0};
            retain_resource(&r);
            insert_nl(g_nown, &o);
        }
        memcpy(original, g_own, sizeof *original * count * 2);
        u64 elapsed = 0;
        for (u32 round = 0; round < rounds; round++) {
            ResList rel = {0};
            u64 start = now_ns();
            cut_nl(original[0].lo, original[count - 1].hi, &rel);
            elapsed += now_ns() - start;
            CHECK(g_nown == count && rel.n == count);
            for (u32 i = 0; i < count; i++)
                CHECK(g_own[i].lo == original[count + i].lo && g_own[i].res_off == original[count + i].res_off);
            res_release(&rel);
            CHECK(r.refs == count);
            reset_map();
            CHECK(r.refs == 0);
            if (round + 1 < rounds) {
                memcpy(g_own, original, sizeof *original * count * 2);
                g_nown = count * 2;
                r.refs = count * 2;
                for (u32 i = 0; i < g_nown; i++) cover_nl(&g_own[i], 1);
            }
        }
        printf("GPU_HOTPATH_BENCH workload=owner_cut_%u operations=%u ns=%llu exact=1 results=%u bytes=%zu\n",
               count, rounds, (unsigned long long)elapsed, count, sizeof *original * count * 2);
        free(original);
    }
}

int main(int argc, char** argv) {
    g_mem = malloc((size_t)g_as_size);
    CHECK(g_mem != NULL);
    OwnHooks hooks = {.land = land_resources, .retain = retain_resource, .release = release_resource};
    CHECK(own_init(&hooks) == 0);
    differential_cuts();
    land_and_replace();
    if (argc == 2 && !strcmp(argv[1], "--bench")) benchmark();
    else CHECK(argc == 1);
    printf("memory_own_cut: %u checks passed\n", checks);
    free(g_own);
    free(g_own_region);
    free(g_mem);
    return 0;
}
