/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the production memory SVCs and block list without game files. The POSIX backing is sparse, with
   injectable commit failures. Thread/handle/GPU services are stubs, not the code under test. */
#include "nx/memory/own.h"
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/hle.h"
#include "nx/runtime/clock.h"
#include "nx/gpu/gpu.h"
#include "nx/runtime/nce/nce.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0 /* FreeBSD never reserves swap for a mapping */
#endif
#define MIB (1ull << 20)
static unsigned checks;
static int fail_commit_after, commit_calls, decommit_calls;
static int mapping_depth, mapping_begins, mapping_ends;

static void test_fail(const char* expr, int line) {
    fprintf(stderr, "memory_accounting.c:%d: %s\n", line, expr);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pm_lock_announce(PMutex* m, const char* name) { (void)m, (void)name; }
/* the host page mem.c works in: the suite runs with this host's, then as with 16 and 64 KiB ones (main) */
static u64 host_page;
u64 plat_vm_page(void) { return host_page; }
/* aligned to host_page, a page more reserved for that (the unaligned head and tail stay inaccessible) */
static u8* reservation;
static u64 reservation_size;
static u64 refuse_above; /* a system that gives no more than this much address space (0: any) */
/* the reservations mem.c asked for, in order: their size and whether with the write watch; also to asked_fd, for a
   child's */
static struct {
    u64 size;
    int watched;
} asked[32];
static unsigned asked_n;
static int asked_fd = -1;
static int watch_refused;   /* a Windows that gives no reservation with the write watch */
static int unwatched_given; /* 0: ENOSYS, as POSIX, whose tracking comes after the reservation */
static void ask(u64 size, int watched) {
    u64 w[2] = {size, (u64)watched};
    if (asked_n < sizeof asked / sizeof *asked) asked[asked_n].size = size, asked[asked_n].watched = watched;
    asked_n++;
    if (asked_fd >= 0 && write(asked_fd, w, sizeof w) != (ssize_t)sizeof w) abort();
}
static u8* reserve(u64 size) {
    void* p;
    if (refuse_above && size > refuse_above) {
        errno = ENOMEM;
        return NULL;
    }
    p = mmap(NULL, size + host_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    reservation = (u8*)p;
    reservation_size = size + host_page;
    return (u8*)(((uintptr_t)p + host_page - 1) & ~(uintptr_t)(host_page - 1));
}
u8* plat_vm_reserve(u64 size) {
    ask(size, 1);
    if (watch_refused) {
        errno = ENOMEM;
        return NULL;
    }
    return reserve(size);
}
u8* plat_vm_reserve_unwatched(u64 size) {
    ask(size, 0);
    if (!unwatched_given) {
        errno = ENOSYS;
        return NULL;
    }
    return reserve(size);
}
/* no tracking here: the range mem_init asks it of, which test_moved holds to the reservation where it runs */
#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__)
static u8* tracked;
static u64 tracked_size;
void plat_vm_track(u8* p, u64 size) { tracked = p, tracked_size = size; }
#else
void plat_vm_track(u8* p, u64 size) { (void)p, (void)size; }
#endif
/* mem_ww_probe's scan: where it looked */
static u8* scanned;
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    (void)size, (void)cb;
    scanned = p;
    return -1;
}
/* mem.c's reservation at the guest's own addresses (NCE), which test_moved asks for: at addr or nowhere */
static int reserve_at_on;
u8* plat_vm_reserve_at(u64 addr, u64 size) {
    void* p;
    if (!reserve_at_on) {
        errno = ENOSYS;
        return NULL;
    }
    p = mmap((void*)(uintptr_t)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (p != (void*)(uintptr_t)addr) {
        munmap(p, size);
        errno = EEXIST;
        return NULL;
    }
    reservation = (u8*)p;
    reservation_size = size;
    return (u8*)p;
}
/* plat_posix.c's look at the maps (platform_posix tests it), which mem_foreign asks only of NCE's reservation: the
   named mappings test_moved lists, each said by its start unless own claims it, and the range it was asked for */
static struct {
    u64 named[2][2], lo, hi;
    int n, calls;
} foreign;
int plat_vm_foreign(u64 lo, u64 hi, int (*own)(u64 a, u64 b), char* out, size_t size) {
    size_t at = 0;
    int found = 0;
    foreign.calls++;
    foreign.lo = lo, foreign.hi = hi;
    if (size) out[0] = 0;
    for (int i = 0; i < foreign.n; i++) {
        int k;
        if (own(foreign.named[i][0], foreign.named[i][1])) continue;
        k = at < size
                ? snprintf(out + at, size - at, "%s%#llx", found ? "; " : "", (unsigned long long)foreign.named[i][0])
                : 0;
        at += k > 0 ? (size_t)k : 0;
        found++;
    }
    return found;
}
/* nce.c, whose NCE no game here is of: mem.c's reservation asks it first */
int nce_wanted(void) { return 0; }
int plat_vm_commit(u8* p, u64 size) {
    if (mapping_begins) CHECK(mapping_depth == 1);
    commit_calls++;
    if (fail_commit_after && !--fail_commit_after) return -1;
    return mprotect(p, size, PROT_READ | PROT_WRITE);
}
void plat_vm_decommit(u8* p, u64 size) {
    if (mapping_begins) CHECK(mapping_depth == 1);
    decommit_calls++;
#if defined(__linux__)
    if (madvise(p, size, MADV_DONTNEED) || mprotect(p, size, PROT_NONE)) abort();
#else
    /* as plat_posix.c: only Linux's MADV_DONTNEED drops the contents (Darwin's and FreeBSD's keep them) */
    if (mmap(p, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) abort();
#endif
}
/* no alias pool here: views are copies (memory_alias tests the pool) */
int plat_alias_init(u64 size) {
    (void)size;
    return -1;
}
int plat_alias_map(u8* p, u64 off, u64 size) {
    (void)p, (void)off, (void)size;
    return -1;
}
int plat_alias_unmap(u8* p, u64 size) {
    (void)p, (void)size;
    return -1;
}
int plat_alias_write(u64 off, const void* src, u64 size) {
    (void)off, (void)src, (void)size;
    return -1;
}
int plat_alias_read(u64 off, void* dst, u64 size) {
    (void)off, (void)dst, (void)size;
    return -1;
}
void plat_alias_free(u64 off, u64 size) { (void)off, (void)size; }
u8* plat_alias_host(u64 off, u64 size) {
    (void)off, (void)size;
    return NULL;
}
void plat_alias_host_free(u8* p, u64 size) { (void)p, (void)size; }
/* memory/own.c and the freeze its pages need: nothing is GPU-owned here */
void own_acquire(u64 addr, u64 size) { (void)addr, (void)size; }
void own_drop(u64 addr, u64 size) { (void)addr, (void)size; }
void own_unwatch(u64 addr, u64 size) { (void)addr, (void)size; }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { /* nothing is the GPU's here */
    (void)addr, (void)size, (void)out, (void)max;
    return 0;
}
/* guest memory moved into a GPU's buffer and back (mem_migrate): refused, but for test_moved's, which maps nothing */
static int share_ok;
int plat_vm_share(u8* p, int fd, u64 size) {
    (void)p, (void)fd, (void)size;
    return share_ok ? 0 : -1;
}
int plat_vm_unshare(u8* p, u64 size) {
    (void)p, (void)size;
    return share_ok ? 0 : -1;
}
void plat_freeze(u8* p, u64 size) { (void)p, (void)size; }
void plat_thaw(void) {}
void gpu_mem_changed(u64 a, u64 size) {
    (void)a;
    (void)size;
}
static u64 mapping_a, mapping_size; /* the guest range the last change named */
static int mapping_kind;            /* and its kind (GPU_MAPPING_*) */
static void mapping_begin(u64 a, u64 size, int kind) {
    CHECK((kind == GPU_MAPPING_GUEST || kind == GPU_MAPPING_GUEST_ONLY) && size != 0);
    mapping_a = a;
    mapping_size = size;
    mapping_kind = kind;
    CHECK(mapping_depth == 0);
    mapping_depth++;
    mapping_begins++;
    /* The renderer lock must come before mem.c's lock: Vulkan may query guest mappings while landing. */
    MemBlock b;
    mem_query(HEAP_BASE, &b);
}
static void mapping_end(void) {
    CHECK(mapping_depth == 1);
    mapping_depth--;
    mapping_ends++;
}
void (*g_gpu_mapping_begin)(u64, u64, int);
void (*g_gpu_mapping_end)(void);
static char said[4096]; /* what the runtime logged since said[0] was last cleared */
void logf_(int level, const char* fmt, ...) {
    size_t n = strlen(said);
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vsnprintf(said + n, sizeof said - n, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
u64 plat_ns(void) { return 1; }
u64 guest_ticks(void) { return 1; }
int g_ounce; /* main.c's: a Switch program */
int kthread_live_count(void) { return 1; }
static KObj* test_handle; /* what handle 0x1234 names, for the shared memory test */
/* the native SDK's mapping of a shared memory (kshmem_map_free) takes a handle, which no test here makes */
u32 h_new(KObj* o) {
    (void)o;
    return 0;
}
int h_close(u32 h) {
    (void)h;
    return 0;
}
KObj* h_get_ref(u32 h, int type) {
    return h == 0x1234 && test_handle && test_handle->type == type ? test_handle : NULL;
}
void kobj_ref(KObj* o) { (void)o; }
void kobj_unref(KObj* o) { (void)o; }

static u64 info(u32 id) {
    u64 out = ~0ull;
    CHECK(svc_get_info(id, H_CUR_PROC, 0, &out) == 0);
    return out;
}
static u32 state(u64 a) {
    MemBlock b;
    mem_query(a, &b);
    return b.state;
}
static void base_process(void) {
    CHECK(g_heap_size == 0 && g_phys_mapped == 0);
    g_settings.memory_gb = 0;
    g_code_memory = 55 * MIB;
    g_main_stack_size = MIB;
    g_hle.kernel.sysres_size = 0;
    g_heap_max = 0;
    fail_commit_after = 0;
}
static void test_heap(void) {
    base_process();
    CHECK(mem_total() == 3285 * MIB);
    CHECK(used_memory() == 56 * MIB);
    CHECK(info(6) == 3285 * MIB && info(7) == 56 * MIB);
    CHECK(info(21) == info(6) && info(22) == info(7));
    CHECK(info(33) == 0x1000);             /* guest page size, also on hosts with larger pages */
    CHECK(info(29) == 0 && info(30) == 0); /* no remote address space */
    CHECK(info(31) == 0 && info(32) == 0); /* no remote allocations */
    static const u32 process_ids[] = {29, 30, 31, 32, 33};
    for (size_t i = 0; i < sizeof process_ids / sizeof process_ids[0]; ++i) {
        u32 id = process_ids[i];
        u64 out = ~0ull;
        CHECK(svc_get_info(id, H_CUR_PROC, 1, &out) == SNX_RC_SVC_INVALID_ENUM_VALUE);
        CHECK(svc_get_info(id, 0, 0, &out) == SNX_RC_SVC_INVALID_HANDLE);
        CHECK(svc_get_info(id, 0x1234, 0, &out) == SNX_RC_SVC_INVALID_HANDLE);
    }
    CHECK(svc_set_heap_size(3200 * MIB) == 0); /* the reported startup failure */
    CHECK(g_heap_size == 3200 * MIB && used_memory() == 3256 * MIB);
    CHECK(state(HEAP_BASE) == MS_NORMAL);
    CHECK(svc_set_heap_size(3228 * MIB) == 0);
    int calls = commit_calls;
    CHECK(svc_set_heap_size(3230 * MIB) == SNX_RC_SVC_LIMIT_REACHED);
    CHECK(commit_calls == calls && g_heap_size == 3228 * MIB);
    CHECK(svc_set_heap_size(3228 * MIB) == 0);
    CHECK(commit_calls == calls);
    CHECK(svc_set_heap_size(0) == 0 && g_heap_size == 0);
    CHECK(state(HEAP_BASE) == MS_FREE);
    CHECK(svc_set_heap_size(3200 * MIB + 1) == SNX_RC_SVC_INVALID_SIZE);
    CHECK(svc_set_heap_size(8ull << 30) == SNX_RC_SVC_INVALID_SIZE);
    CHECK(svc_set_heap_size(~0ull) == SNX_RC_SVC_INVALID_SIZE);
    g_heap_max = 3000 * MIB;
    CHECK(svc_set_heap_size(3200 * MIB) == SNX_RC_SVC_OUT_OF_MEMORY);
    g_heap_max = 0;
    CHECK(svc_set_heap_size(2 * MIB) == 0);
    fail_commit_after = 1;
    CHECK(svc_set_heap_size(4 * MIB) == SNX_RC_SVC_OUT_OF_MEMORY);
    CHECK(g_heap_size == 2 * MIB && state(HEAP_BASE + 2 * MIB) == MS_FREE);
    CHECK(svc_set_heap_size(0) == 0);
    for (int gb = 4; gb <= 8; gb += 2) {
        g_settings.memory_gb = gb;
        CHECK(mem_total() == (gb == 4 ? 3285 : gb == 6 ? 4916 : 6964) * MIB);
        CHECK(svc_set_heap_size(3200 * MIB) == 0);
        CHECK(svc_set_heap_size(0) == 0);
    }
}
static void test_large_heap(void) {
    MemLayout saved = g_layout;
    base_process();
    g_settings.memory_gb = 12;
    mem_layout_large_heap();
    CHECK(g_layout.heap_size == 12ull << 30);
    CHECK(g_layout.stack == saved.stack + (4ull << 30));
    CHECK(g_layout.kern == saved.kern + (4ull << 30));
    CHECK(mem_total() == 10ull << 30);
    CHECK(info(5) == 12ull << 30 && info(6) == 10ull << 30);
    CHECK(svc_set_heap_size(8500 * MIB) == 0);
    CHECK(g_heap_size == 8500 * MIB && state(HEAP_BASE + 8499 * MIB) == MS_NORMAL);
    CHECK(svc_set_heap_size(0) == 0);
    CHECK(svc_set_heap_size(12ull << 30) == SNX_RC_SVC_INVALID_SIZE);
    /* an Ounce game gets the same budget with memory_gb at its default */
    g_settings.memory_gb = 0;
    g_ounce = 1;
    CHECK(mem_total() == 10ull << 30 && info(5) == 12ull << 30);
    CHECK(svc_set_heap_size(8500 * MIB) == 0);
    CHECK(svc_set_heap_size(0) == 0);
    g_ounce = 0;
    CHECK(mem_total() == 3285 * MIB);
    g_layout = saved;
}
/* a 32-bit process's GetInfo total is its heap region's size (2 GiB without an alias region), not the application
   pool's: a title that sets its heap to nearly the total it reads otherwise asks for 3.2 GiB and gets none */
static void test_heap32(void) {
    MemLayout saved = g_layout;
    mem_layout32(2);
    base_process();
    g_code_memory = 22 * MIB;
    CHECK(info(5) == 2ull << 30);
    CHECK(info(6) == 2ull << 30 && info(7) == 23 * MIB);
    CHECK(info(21) == 2ull << 30 && info(22) == 23 * MIB);
    CHECK(svc_set_heap_size(3260 * MIB) == SNX_RC_SVC_OUT_OF_MEMORY); /* the pool's total, more than the heap region */
    CHECK(svc_set_heap_size(2022 * MIB) == 0);          /* what it asks once GetInfo names the heap region */
    CHECK(svc_set_heap_size(0) == 0);
    mem_layout32(1); /* with an alias region the heap is 1 GiB */
    base_process();
    g_code_memory = 22 * MIB;
    CHECK(info(5) == 1ull << 30 && info(6) == 1ull << 30 && info(21) == 1ull << 30);
    CHECK(svc_set_heap_size(3260 * MIB) == SNX_RC_SVC_OUT_OF_MEMORY);
    CHECK(svc_set_heap_size(1000 * MIB) == 0);
    CHECK(svc_set_heap_size(0) == 0);
    g_layout = saved;
}
/* memory_fit: the game is told the budget the measure leaves, at most Memory (GB)'s and never less than the console's,
   and the console's amount when the system told nothing */
static void test_fit(void) {
    base_process();
    CHECK(mem_fit_budget(3000 * MIB, 300 * MIB) == 2600 * MIB);  /* the rest, at least 700 MiB */
    CHECK(mem_fit_budget(8000 * MIB, 1000 * MIB) == 7744 * MIB); /* the runtime's 1000, the graphics' 256 */
    CHECK(mem_fit_budget(100 * MIB, 100 * MIB) == 1024 * MIB);   /* a GiB however little is left */
    said[0] = 0;
    mem_fit(1, 2000 * MIB, 300 * MIB); /* the measure leaves 1600: the console's is the least Auto tells */
    CHECK(g_hle.svcmem.fit == 3285 * MIB && mem_total() == 3285 * MIB && info(6) == 3285 * MIB);
    CHECK(!strcmp(said, "memory: memory_fit_mode=0 (Auto): the game is told 3285 MiB, as Auto tells no less than the "
                        "console's 3285, where the measure leaves 1600 (the system can still give 2000 MiB, the "
                        "process holds 300)\n"));
    mem_fit(1, 8000 * MIB, 1000 * MIB); /* more than the console's: the console's */
    CHECK(mem_total() == 3285 * MIB);
    g_settings.memory_gb = 8; /* what memory_gb adds above the console's is what Auto trims */
    CHECK(mem_total() == 6964 * MIB);
    mem_fit(1, 5000 * MIB, 300 * MIB);
    CHECK(g_hle.svcmem.fit == 4600 * MIB && mem_total() == 4600 * MIB && info(6) == 4600 * MIB);
    CHECK(svc_set_heap_size(4546 * MIB) == SNX_RC_SVC_LIMIT_REACHED); /* 56 MiB used: 4544 is the most */
    CHECK(svc_set_heap_size(4544 * MIB) == 0 && svc_set_heap_size(0) == 0);
    mem_fit(1, 2000 * MIB, 300 * MIB);
    CHECK(mem_total() == 3285 * MIB);
    g_settings.memory_gb = 0;
    mem_fit(0, 0, 0);
    CHECK(!g_hle.svcmem.fit && mem_total() == 3285 * MIB);
    { /* a 32-bit process is told its heap region, less than the console's whatever the measure */
        MemLayout saved = g_layout;
        mem_layout32(2);
        base_process();
        mem_fit(1, 2000 * MIB, 300 * MIB);
        CHECK(mem_total() == 3285 * MIB && info(6) == 2ull << 30 && info(21) == 2ull << 30);
        mem_fit(0, 0, 0);
        g_layout = saved;
    }
}
/* a crash's hint for a game short of memory: the amount the game was told, what gave it, and only a key that tells
   this game more, with the amount it gives */
static int hint_is(const char* text) {
    char hint[512] = "unset";
    int has = mem_short_hint(hint, sizeof hint);
    if (strcmp(hint, text)) fprintf(stderr, "the hint is \"%s\"\n", hint);
    return !strcmp(hint, text) && has == (text[0] != 0);
}
static void test_short_hint(void) {
    MemLayout saved = g_layout;
    base_process();
    mem_fit(0, 0, 0);
    CHECK(hint_is("It was told 3285 MiB (memory_gb=0): memory_gb=6 in its .ini tells it 4916 MiB"));
    g_settings.memory_gb = 4;
    CHECK(hint_is("It was told 3285 MiB (memory_gb=4): memory_gb=6 in its .ini tells it 4916 MiB"));
    mem_fit(1, 4400 * MIB, 300 * MIB); /* 4000 MiB for the game: more than it has, less than memory_gb=6's */
    CHECK(hint_is("It was told 3285 MiB (memory_gb=4): memory_gb=6 in its .ini tells it 4000 MiB of its 4916, as "
                  "memory_fit_mode=0 (Auto) measured 4400 MiB the system could still give as the game started"));
    mem_fit(1, 2000 * MIB, 300 * MIB); /* less than it has: Auto tells the console's, and memory_gb=6 the same */
    CHECK(mem_total() == 3285 * MIB && hint_is(""));
    g_settings.memory_gb = 6; /* what memory_gb adds is what Auto trims, and Off tells */
    mem_fit(1, 4400 * MIB, 300 * MIB);
    CHECK(hint_is("It was told 4000 MiB of the console's 4916: memory_fit_mode=0 (Auto) measured 4400 MiB the system "
                  "could still give as the game started. memory_fit_mode=1 (Off) in its .ini tells it 4916 MiB "
                  "whatever the system can give, and Auto does once the system can still give 5316 MiB"));
    mem_fit(1, 5316 * MIB, 300 * MIB); /* which is what that last amount gives */
    CHECK(mem_total() == 4916 * MIB && hint_is(""));
    mem_fit(1, 4400 * MIB, 300 * MIB + 1); /* a byte more held is a byte less to give: the whole MiB above it */
    CHECK(hint_is("It was told 4000 MiB of the console's 4916: memory_fit_mode=0 (Auto) measured 4400 MiB the system "
                  "could still give as the game started. memory_fit_mode=1 (Off) in its .ini tells it 4916 MiB "
                  "whatever the system can give, and Auto does once the system can still give 5316 MiB"));
    mem_fit(1, 5315 * MIB, 300 * MIB + 1);
    CHECK(mem_total() < 4916 * MIB);
    mem_fit(1, 5316 * MIB, 300 * MIB + 1);
    CHECK(mem_total() == 4916 * MIB);
    mem_fit(1, 4400 * MIB, 1000 * MIB); /* a larger runtime: its own 1000 and the graphics' 256 */
    CHECK(hint_is("It was told 4144 MiB of the console's 4916: memory_fit_mode=0 (Auto) measured 4400 MiB the system "
                  "could still give as the game started. memory_fit_mode=1 (Off) in its .ini tells it 4916 MiB "
                  "whatever the system can give, and Auto does once the system can still give 5172 MiB"));
    mem_fit(0, 0, 0);
    for (int gb = 6; gb <= 12; gb += 2) { /* no key tells it more than these do, 12 being experimental */
        g_settings.memory_gb = gb;
        CHECK(hint_is(""));
    }
    /* an Ounce game's amount is its own whatever memory_gb says, so the hint never names that key */
    g_settings.memory_gb = 8;
    mem_layout_large_heap();
    g_ounce = 1;
    CHECK(hint_is(""));
    mem_fit(1, 8006 * MIB, 106 * MIB); /* the measure leaves 7412: an Ounce game's 10240 is the least Auto tells */
    CHECK(mem_total() == 10240 * MIB && hint_is(""));
    g_settings.memory_gb = 0;
    mem_fit(0, 0, 0);
    CHECK(hint_is(""));
    g_ounce = 0;
    g_layout = saved;
    mem_layout32(2); /* a 32-bit process is told its 2 GiB heap region at any memory_gb */
    base_process();
    CHECK(info(6) == 2ull << 30 && hint_is(""));
    mem_fit(1, 2000 * MIB, 300 * MIB); /* Auto tells no less than the console's, which is more than the region */
    CHECK(info(6) == 2ull << 30 && hint_is(""));
    mem_fit(0, 0, 0);
    g_layout = saved;
}
/* memory_gb given for an Ounce game: the log says once that it changes nothing there, and the amount in effect */
static void test_gb_said(void) {
    MemLayout saved = g_layout;
    base_process();
    mem_fit(0, 0, 0);
    said[0] = 0;
    g_settings.memory_gb = 8;
    mem_gb_said(); /* another game's memory follows the key */
    CHECK(!said[0] && mem_total() == 6964 * MIB);
    mem_layout_large_heap();
    g_ounce = 1;
    mem_gb_said();
    CHECK(!strcmp(said, "memory: memory_gb=8 does not change an Ounce game's memory: it is told 10240 MiB\n"));
    mem_fit(1, 8006 * MIB, 106 * MIB); /* the measure leaves 7412: Auto tells no less than its own 10240 */
    said[0] = 0;
    g_settings.memory_gb = 6;
    mem_gb_said();
    CHECK(!strcmp(said, "memory: memory_gb=6 does not change an Ounce game's memory: it is told 10240 MiB\n"));
    said[0] = 0;
    g_settings.memory_gb = 0; /* the key's default: nothing was asked */
    mem_gb_said();
    CHECK(!said[0]);
    mem_fit(0, 0, 0);
    said[0] = 0;
    g_ounce = 0;
    g_layout = saved;
}
static void test_system_resource(void) {
    base_process();
    CHECK(svc_map_physical(ALIAS_BASE, 0x1000) == SNX_RC_SVC_INVALID_STATE);
    CHECK(svc_unmap_physical(ALIAS_BASE, 0x1000) == SNX_RC_SVC_INVALID_STATE);
    g_hle.kernel.sysres_size = 64 * MIB;
    CHECK(info(16) == 64 * MIB);
    CHECK(info(6) == 3285 * MIB && info(21) == 3221 * MIB);
    CHECK(info(7) == 120 * MIB && info(22) == 56 * MIB);
    CHECK(svc_set_heap_size(3200 * MIB) == SNX_RC_SVC_LIMIT_REACHED);
    CHECK(svc_set_heap_size(3164 * MIB) == 0);
    CHECK(svc_map_physical(ALIAS_BASE, 2 * MIB) == SNX_RC_SVC_LIMIT_REACHED);
    CHECK(g_phys_mapped == 0 && state(ALIAS_BASE) == MS_FREE);
    CHECK(svc_set_heap_size(0) == 0);
    CHECK(svc_map_physical(ALIAS_BASE, 2 * MIB) == 0);
    CHECK(g_phys_mapped == 2 * MIB);
    CHECK(svc_map_physical(ALIAS_BASE + MIB, 2 * MIB) == 0);
    CHECK(g_phys_mapped == 3 * MIB); /* overlap is charged once */
    int calls = commit_calls;
    CHECK(svc_map_physical(ALIAS_BASE, 3 * MIB) == 0 && commit_calls == calls);
    CHECK(svc_set_heap_size(3164 * MIB) == SNX_RC_SVC_LIMIT_REACHED); /* the opposite allocation order */
    CHECK(svc_unmap_physical(ALIAS_BASE, 4 * MIB) == 0); /* includes a free tail */
    CHECK(g_phys_mapped == 0);
    CHECK(svc_unmap_physical(ALIAS_BASE, 4 * MIB) == 0);
    CHECK(svc_map_physical(ALIAS_BASE + 1, 0x1000) == SNX_RC_SVC_INVALID_ADDRESS);
    CHECK(svc_map_physical(ALIAS_BASE, 0) == SNX_RC_SVC_INVALID_SIZE);
    CHECK(svc_map_physical(ALIAS_BASE, 1) == SNX_RC_SVC_INVALID_SIZE);
    CHECK(svc_map_physical(0xfffffffffffff000ull, 0x2000) == SNX_RC_SVC_INVALID_MEMORY_REGION);
    CHECK(svc_map_physical(HEAP_BASE, 0x1000) == SNX_RC_SVC_INVALID_MEMORY_REGION);
    CHECK(svc_map_physical(ALIAS_BASE + ALIAS_SIZE - 0x1000, 0x2000) == SNX_RC_SVC_INVALID_MEMORY_REGION);
}
static void test_rollback(void) {
    base_process();
    g_hle.kernel.sysres_size = 2 * MIB;
    u64 a = ALIAS_BASE;
    CHECK(svc_map_physical(a + 0x1000, 0x1000) == 0);
    gw32(a + 0x1000, 0x12345678);
    int decommits = decommit_calls;
    fail_commit_after = 2; /* first hole succeeds, second fails */
    CHECK(svc_map_physical(a, 0x3000) == SNX_RC_SVC_OUT_OF_MEMORY);
    /* the first hole given back: decommitted, or zeroed where it shares a host page with a + 0x1000 */
    CHECK(decommit_calls == decommits + (host_page == 0x1000));
    CHECK(g_phys_mapped == 0x1000);
    CHECK(state(a) == MS_FREE && state(a + 0x2000) == MS_FREE);
    CHECK(state(a + 0x1000) == MS_NORMAL && gr32(a + 0x1000) == 0x12345678);
    CHECK(svc_map_physical(a, 0x3000) == 0);
    CHECK(g_phys_mapped == 0x3000 && gr32(a + 0x1000) == 0x12345678);
    CHECK(gr32(a) == 0 && gr32(a + 0x2000) == 0);
    CHECK(svc_unmap_physical(a, 0x3000) == 0);
    CHECK(g_phys_mapped == 0);
    /* A failed unmap must not remove either normal pages or an unrelated shared mapping. */
    CHECK(svc_map_physical(a, 0x1000) == 0);
    CHECK(mem_map(a + 0x1000, 0x1000, MS_SHARED, PERM_RW) == 0);
    CHECK(svc_unmap_physical(a, 0x2000) == SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    CHECK(g_phys_mapped == 0x1000 && state(a) == MS_NORMAL && state(a + 0x1000) == MS_SHARED);
    CHECK(mem_unmap(a + 0x1000, 0x1000) == 0);
    CHECK(mem_set_attr(a, 0x1000, 1, 1) == 0);
    CHECK(svc_unmap_physical(a, 0x1000) == SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    CHECK(g_phys_mapped == 0x1000);
    CHECK(mem_set_attr(a, 0x1000, 1, 0) == 0);
    /* Unlike a heap shrink, physical unmapping permits a normal page whose permissions changed. */
    CHECK(mem_set_state(a, 0x1000, MS_NORMAL, PERM_R) == 0);
    CHECK(svc_unmap_physical(a, 0x1000) == 0);
    CHECK(g_phys_mapped == 0);
}
static void test_locked_shrink(void) {
    base_process();
    CHECK(svc_set_heap_size(4 * MIB) == 0);
    CHECK(mem_set_attr(HEAP_BASE + 2 * MIB, 0x1000, 1, 1) == 0);
    CHECK(svc_set_heap_size(2 * MIB) == SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    CHECK(g_heap_size == 4 * MIB && state(HEAP_BASE + 3 * MIB) == MS_NORMAL);
    CHECK(mem_set_attr(HEAP_BASE + 2 * MIB, 0x1000, 1, 0) == 0);
    CHECK(mem_set_state(HEAP_BASE + 2 * MIB, 0x1000, MS_NORMAL, PERM_R) == 0);
    CHECK(svc_set_heap_size(2 * MIB) == SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    CHECK(mem_set_state(HEAP_BASE + 2 * MIB, 0x1000, MS_NORMAL, PERM_RW) == 0);
    CHECK(svc_set_heap_size(2 * MIB) == 0);
    CHECK(svc_set_heap_size(0) == 0);
}
static int ready, go;
typedef struct {
    int physical;
    u32 result;
} Request;
static void* race_request(void* arg) {
    Request* q = arg;
    __atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&go, __ATOMIC_SEQ_CST)) sched_yield();
    q->result = q->physical ? svc_map_physical(ALIAS_BASE, 2 * MIB) : svc_set_heap_size(2 * MIB);
    return NULL;
}
static void test_shared_budget(void) {
    base_process();
    g_hle.kernel.sysres_size = 2 * MIB;
    g_code_memory = mem_total() - g_main_stack_size - g_hle.kernel.sysres_size - 2 * MIB;
    for (int i = 0; i < 50; i++) {
        ready = go = 0;
        Request q[2] = {{0, ~0u}, {1, ~0u}};
        pthread_t t[2];
        CHECK(pthread_create(&t[0], NULL, race_request, &q[0]) == 0);
        CHECK(pthread_create(&t[1], NULL, race_request, &q[1]) == 0);
        while (__atomic_load_n(&ready, __ATOMIC_SEQ_CST) != 2) sched_yield();
        __atomic_store_n(&go, 1, __ATOMIC_SEQ_CST);
        CHECK(pthread_join(t[0], NULL) == 0 && pthread_join(t[1], NULL) == 0);
        CHECK((q[0].result == 0 && q[1].result == SNX_RC_SVC_LIMIT_REACHED) ||
              (q[0].result == SNX_RC_SVC_LIMIT_REACHED && q[1].result == 0));
        CHECK(used_memory() == mem_total());
        CHECK(svc_set_heap_size(0) == 0);
        CHECK(svc_unmap_physical(ALIAS_BASE, 2 * MIB) == 0);
        CHECK(g_heap_size == 0 && g_phys_mapped == 0);
    }
}
static void test_mapping_boundary(void) {
    base_process();
    g_gpu_mapping_begin = mapping_begin;
    g_gpu_mapping_end = mapping_end;
    /* the range it changes. Its own pages alone are landed or dropped there (GPU_MAPPING_GUEST_ONLY): the renderer
       lets a change that nothing of it covers go on without its lock */
    CHECK(mem_map(ALIAS_BASE, 0x1000, MS_SHARED, PERM_RW) == 0);
    CHECK(mapping_a == ALIAS_BASE && mapping_size == 0x1000 && mapping_kind == GPU_MAPPING_GUEST_ONLY);
    CHECK(mem_unmap(ALIAS_BASE, 0x1000) == 0);
    CHECK(mapping_a == ALIAS_BASE && mapping_size == 0x1000 && mapping_kind == GPU_MAPPING_GUEST_ONLY);
    CHECK(mem_map_physical(ALIAS_BASE, 0x2000, mem_total()) == 0);
    CHECK(mapping_a == ALIAS_BASE && mapping_size == 0x2000 && mapping_kind == GPU_MAPPING_GUEST_ONLY);
    CHECK(mem_unmap_physical(ALIAS_BASE, 0x2000) == 0);
    CHECK(mapping_a == ALIAS_BASE && mapping_size == 0x2000 && mapping_kind == GPU_MAPPING_GUEST_ONLY);
    /* the heap's change: any of it, and a view's source lands what the GPU holds of it, beyond the range */
    CHECK(mem_heap_set(0x2000, mem_total()) == 0);
    CHECK(mapping_a == 0 && mapping_size == ~0ull && mapping_kind == GPU_MAPPING_GUEST);
    CHECK(mem_heap_set(0x2000, mem_total()) == 0);
    CHECK(mem_map(ALIAS_BASE, 0x1000, MS_NORMAL, PERM_RW) == 0);
    CHECK(mem_alias_begin(ALIAS_BASE, 0x1000) == 0);
    CHECK(mapping_a == ALIAS_BASE && mapping_kind == GPU_MAPPING_GUEST);
    CHECK(mem_alias_map(ALIAS_BASE + 0x10000, ALIAS_BASE, 0x1000, MS_STACK, PERM_RW) == 0);
    CHECK(mapping_a == ALIAS_BASE + 0x10000 && mapping_kind == GPU_MAPPING_GUEST);
    CHECK(mem_alias_unmap(ALIAS_BASE + 0x10000, 0x1000, ALIAS_BASE) == 0);
    CHECK(mapping_a == ALIAS_BASE + 0x10000 && mapping_kind == GPU_MAPPING_GUEST);
    mem_alias_end(ALIAS_BASE, 0x1000);
    CHECK(mapping_a == ALIAS_BASE && mapping_kind == GPU_MAPPING_GUEST);
    CHECK(mem_unmap(ALIAS_BASE, 0x1000) == 0);
    CHECK(mem_heap_set(0, mem_total()) == 0);
    CHECK(mapping_begins == 12 && mapping_ends == 12 && mapping_depth == 0);
    g_gpu_mapping_begin = NULL;
    g_gpu_mapping_end = NULL;
}
/* MapMemory's view is a copy of the locked source, copied back on unmap; QueryMemory reports it */
static void test_map_memory(void) {
    u64 src, dst = STACK_BASE + 0x10000, info = STACK_BASE;
    base_process();
    CHECK(svc_set_heap_size(2 * MIB) == 0);
    src = HEAP_BASE + 0x3000;
    CHECK(mem_map(info, 0x1000, MS_STACK, PERM_RW) == 0);
    gw32(src, 0xCAFE);
    CHECK(svc_map_memory(dst + 1, src, 0x1000) == SNX_RC_SVC_INVALID_ADDRESS);
    CHECK(svc_map_memory(dst, src, 0) == SNX_RC_SVC_INVALID_SIZE);
    CHECK(svc_map_memory(HEAP_BASE, src, 0x1000) == SNX_RC_SVC_INVALID_MEMORY_REGION);
    CHECK(svc_map_memory(dst, src, 0x2000) == 0);
    CHECK(gr32(dst) == 0xCAFE && state(dst) == MS_STACK);
    CHECK(svc_query(info, src) == 0);
    CHECK(gr64(info) == src && gr64(info + 8) == 0x2000 && gr32(info + 16) == (MS_NORMAL & 0xFF));
    CHECK(gr32(info + 20) == 1 && gr32(info + 24) == PERM_NONE); /* locked, no permission */
    CHECK(svc_map_memory(dst + 0x4000, src, 0x1000) == SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    CHECK(svc_set_heap_size(0) == SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    gw32(dst + 0x1000, 0xBEEF);
    CHECK(svc_unmap_memory(dst, src, 0x2000) == 0);
    CHECK(gr32(src + 0x1000) == 0xBEEF && state(dst) == MS_FREE);
    /* the alias region takes the view too, as normal memory */
    CHECK(svc_map_memory(ALIAS_BASE, src, 0x1000) == 0 && state(ALIAS_BASE) == MS_NORMAL && gr32(ALIAS_BASE) == 0xCAFE);
    CHECK(svc_unmap_memory(dst, src, 0x1000) == SNX_RC_SVC_INVALID_CURRENT_MEMORY); /* not where it is mapped */
    CHECK(svc_unmap_memory(ALIAS_BASE, src, 0x1000) == 0 && state(ALIAS_BASE) == MS_FREE && state(src) == MS_NORMAL);
    CHECK(svc_query(info, AS_END + 5) == 0 && gr64(info) == AS_END && gr32(info + 16) == MS_INACCESSIBLE);
    CHECK(svc_query(0, HEAP_BASE) == 0xE601);
    CHECK(svc_set_mem_perm(src, 0x1000, PERM_RX) == 0xD801);
    CHECK(svc_set_mem_perm(src, 0x1000, PERM_R) == 0);
    CHECK(svc_set_mem_attr(src, 0x1000, 1, 1) == SNX_RC_SVC_INVALID_COMBINATION);
    CHECK(svc_set_mem_attr(src, 0x1000, 8, 8) == 0);
    CHECK(svc_set_mem_attr(src, 0x1000, 8, 0) == 0);
    CHECK(svc_set_gpu_mem_attr(src, 0x1000, MA_GPU_SHARABLE, MA_GPU_SHARABLE) == 0);
    {
        MemBlock b;
        mem_query(src, &b);
        CHECK((b.attr & MA_GPU_SHARABLE) != 0);
    }
    CHECK(svc_set_mem_attr(src, 0x1000, 8, 8) == 0);
    CHECK(svc_set_mem_attr(src, 0x1000, 8, 0) == 0);
    CHECK(svc_set_gpu_mem_attr(src, 0x1000, MA_GPU_SHARABLE, 0) == 0);
    CHECK(svc_set_gpu_mem_attr(src, 0x1000, 8, 8) == SNX_RC_SVC_INVALID_COMBINATION);
    CHECK(svc_set_mem_perm(src, 0x1000, PERM_RW) == 0);
    CHECK(mem_unmap(info, 0x1000) == 0);
    CHECK(svc_set_heap_size(0) == 0);
}
/* Guest pages sharing a host page: a mapping commits the whole host page, an unmapping zeroes its own pages while a
   neighbour maps the rest, and decommits the host page with the last of them, so a page mapped again reads zero */
static void test_host_pages(void) {
    u64 a = ALIAS_BASE + 0x200000, b = a + 0x1000;
    int before;
    base_process();
    CHECK(mem_map(a, 0x1000, MS_SHARED, PERM_RW) == 0 && mem_map(b, 0x1000, MS_SHARED, PERM_RW) == 0);
    gw32(a, 0x11111111);
    gw32(b, 0x22222222);
    before = decommit_calls;
    CHECK(mem_unmap(a, 0x1000) == 0 && state(a) == MS_FREE);
    CHECK(gr32(b) == 0x22222222);
    if (host_page > 0x1000) {
        CHECK(decommit_calls == before); /* b still maps the host page: a's part is zeroed instead */
        CHECK(gr32(a) == 0);
    }
    CHECK(mem_map(a, 0x1000, MS_SHARED, PERM_RW) == 0 && gr32(a) == 0);
    CHECK(mem_unmap(a, 0x2000) == 0 && state(b) == MS_FREE);
    CHECK(decommit_calls > before);
    CHECK(mem_map(b, 0x1000, MS_SHARED, PERM_RW) == 0 && gr32(b) == 0);
    if (host_page > 0x1000) CHECK(gr32(a) == 0); /* free, but in b's host page: committed, and zero */
    CHECK(mem_unmap(b, 0x1000) == 0);
}
/* the runs a GPU read keeps to (mem_mapped_run): mapped pages up to the first free one across blocks of other states,
   none from a free one, none at the address space's end; a run the thread kept is dropped when a page of it goes */
static void test_mapped_runs(void) {
    u64 a = ALIAS_BASE + 0x400000;
    base_process();
    CHECK(mem_map(a, 0x2000, MS_SHARED, PERM_RW) == 0 && mem_map(a + 0x2000, 0x1000, MS_STACK, PERM_RW) == 0);
    CHECK(mem_mapped_run(a, 0x3000) == 0x3000 && mem_mapped_run(a + 0x10, 0x100) == 0x100);
    CHECK(mem_mapped_run(a + 0x2000, 0x2000) == 0x1000 && mem_mapped_run(a, 0) == 0);
    CHECK(mem_mapped_run(a, 0x3000) == 0x3000); /* the run this thread keeps, until a page of it goes */
    CHECK(mem_unmap(a + 0x1000, 0x1000) == 0);
    CHECK(mem_mapped_run(a + 0x1000, 0x10) == 0);
    CHECK(mem_mapped_run(a, 0x3000) == 0x1000 && mem_mapped_run(a + 0x1000, 0x2000) == 0);
    CHECK(mem_mapped_run(a + 0x2000, 0x1000) == 0x1000);
    CHECK(mem_mapped_run(AS_END, 0x1000) == 0 && mem_mapped_run(AS_END - 0x1000, ~0ull) <= 0x1000);
    CHECK(mem_unmap(a, 0x1000) == 0 && mem_unmap(a + 0x2000, 0x1000) == 0);
    CHECK(mem_mapped_run(a, 0x3000) == 0);
}
/* a game's pointer as the kernel reads or writes at it (mem_user_mapped, mem_user_string): mapped through, or the SVC
   answers InvalidCurrentMemory or InvalidPointer; a string is copied up to its NUL, which may sit before a free page */
static void test_user_access(void) {
    u64 a = ALIAS_BASE + 0x400000;
    char s[12];
    base_process();
    CHECK(mem_map(a, 0x1000, MS_STACK, PERM_RW) == 0 && mem_map(a + 0x2000, 0x1000, MS_SHARED, PERM_RW) == 0);
    CHECK(mem_user_mapped(a, 0x1000) && mem_user_mapped(a + 0xFFC, 4) && mem_user_mapped(a + 0x1000, 0));
    CHECK(!mem_user_mapped(a + 0xFFC, 8) && !mem_user_mapped(a + 0x1000, 4) && !mem_user_mapped(a, 0x3000));
    CHECK(mem_user_mapped(a + 0x2000, 0x1000) && !mem_user_mapped(AS_END, 4) && !mem_user_mapped(AS_END - 4, 8));
    memcpy(GP(a + 0xFF8), "fsp-srv", 8);
    memset(s, 'x', sizeof s);
    CHECK(mem_user_string(s, a + 0xFF8, sizeof s) == 8 && !strcmp(s, "fsp-srv")); /* its NUL before the free page */
    memcpy(GP(a + 0xFF8), "abcdefgh", 8);
    CHECK(mem_user_string(s, a + 0xFF8, sizeof s) == -1); /* no NUL before the free page */
    memcpy(GP(a + 0x10), "abcdefghijkl", 12);
    CHECK(mem_user_string(s, a + 0x10, sizeof s) == 0); /* twelve bytes and no NUL */
    CHECK(mem_user_string(s, a + 0x1000, sizeof s) == -1 && mem_user_string(s, AS_END, sizeof s) == -1);
    CHECK(mem_unmap(a, 0x1000) == 0);
    CHECK(!mem_user_mapped(a, 4)); /* a run this thread kept goes with the page */
    CHECK(mem_unmap(a + 0x2000, 0x1000) == 0);
}
/* rtld's walk of the modules (the game's own rtld: from address 0, QueryMemory on each region; a region of code that
   is readable and executable is a module, the word at +4 the offset of its MOD0 header) over NCE's layout: rtld and a
   pool of stubs after it, a pool before main and main, each module's header where its +4 says. The walk finds the two
   modules and no pool, whose word at +4 is a stub's (STP x16, x17) and would send it past any mapping */
static void map_module(u64 base, u32 mod0) {
    CHECK(mem_map(base, 0x2000, MS_CODE, PERM_RX) == 0 && mem_map(base + 0x2000, 0x1000, MS_CODE, PERM_R) == 0 &&
          mem_map(base + 0x3000, 0x1000, MS_CODEDATA, PERM_RW) == 0);
    gw32(base, 0x14000000u | (0x24u >> 2)); /* B past the header, as a module begins */
    gw32(base + 4, mod0);
    gw32(base + mod0, 0x30444F4Du); /* "MOD0" */
}
static void map_pool(u64 base) {
    CHECK(mem_map(base, 0x1000, NCE_POOL_STATE, NCE_POOL_PERM) == 0);
    gw32(base, 0xD10083FFu);     /* SUB sp, sp, #32: a stub's first words */
    gw32(base + 4, 0xA90047F0u); /* STP x16, x17, [sp] */
}
static void test_pool_walk(void) {
    const u64 rtld = AS_BASE, main_mod = AS_BASE + 0x8000, info = STACK_BASE;
    u64 addr = 0, found[4];
    unsigned n = 0, bad = 0;
    base_process();
    CHECK(mem_map(info, 0x1000, MS_STACK, PERM_RW) == 0);
    map_module(rtld, 0x100);
    map_pool(rtld + 0x4000);     /* after rtld's image */
    map_pool(main_mod - 0x2000); /* before main's, a guard page between */
    map_module(main_mod, 0x80);
    map_pool(main_mod + 0x4000);
    for (;;) {
        u64 base, size, end;
        CHECK(svc_query(info, addr) == 0);
        base = gr64(info);
        size = gr64(info + 8);
        if (gr32(info + 16) == 3 && gr32(info + 24) == PERM_RX) { /* CodeStatic and RX: rtld's test */
            u64 mod0 = base + gr32(base + 4);
            MemBlock b;
            mem_query(mod0, &b);
            if (mod0 + 4 > AS_END || b.state == MS_FREE || b.state == MS_INACCESSIBLE || mod0 + 4 > b.addr + b.size ||
                gr32(mod0) != 0x30444F4Du)
                bad++; /* where rtld faults, or finds no MOD0 */
            else if (n < 4)
                found[n++] = base;
        }
        end = base + size;
        if (end <= addr || end <= base) break; /* the last block, to the end of the address space */
        addr = end;
    }
    CHECK(bad == 0 && n == 2 && found[0] == rtld && found[1] == main_mod);
    CHECK(mem_unmap(rtld, 0x5000) == 0 && mem_unmap(main_mod - 0x2000, 0x7000) == 0 && mem_unmap(info, 0x1000) == 0);
}
/* a service's shared memory: its host buffer until the game maps it, the guest's pages while mapped */
static void test_shared_memory(void) {
    KShmem* s = kshmem_new(0x2000, "test");
    u64 a = ALIAS_BASE - 0x100000;
    base_process();
    test_handle = &s->o;
    kshmem_ptr(s)[0] = 7;
    CHECK(svc_map_shm(0x1234, HEAP_BASE, 0x2000, PERM_R) == SNX_RC_SVC_INVALID_MEMORY_REGION);
    /* another size than the object's (InvalidSize on the console) maps the whole object, and unmaps all of it */
    CHECK(svc_map_shm(0x1234, a, 0x1000, PERM_R) == 0 && s->gsize == 0x2000 && state(a + 0x1000) == MS_SHARED);
    CHECK(svc_unmap_shm(0x1234, a, 0x1000) == 0 && state(a) == MS_FREE && state(a + 0x1000) == MS_FREE);
    CHECK(svc_map_shm(0x1234, a, 0x2000, PERM_RX) == 0xD801);
    CHECK(svc_map_shm(0x1235, a, 0x2000, PERM_R) == SNX_RC_SVC_INVALID_HANDLE);
    CHECK(svc_map_shm(0x1234, a, 0x2000, PERM_R) == 0);
    CHECK(GP(a)[0] == 7 && kshmem_ptr(s) == GP(a) && state(a) == MS_SHARED);
    CHECK(svc_map_shm(0x1234, a + 0x10000, 0x2000, PERM_R) == SNX_RC_SVC_INVALID_STATE);
    kshmem_ptr(s)[1] = 9;
    CHECK(svc_unmap_shm(0x1234, a + 0x1000, 0x2000) == SNX_RC_SVC_INVALID_CURRENT_MEMORY);
    CHECK(svc_unmap_shm(0x1234, a, 0x2000) == 0);
    CHECK(kshmem_ptr(s) == s->host && s->host[1] == 9 && state(a) == MS_FREE);
    test_handle = NULL;
    kshmem_destroy(&s->o);
}

/* mem_reserve's tiers: the console's 512 GiB first, then 256, 128 and the smaller ones, each refusal and the size taken
   said with its value. Windows makes its write watch with the reservation: every tier with it before any without, and
   the size taken without it is said. Elsewhere that second kind is ENOSYS, and a refused first pass ends the game */
static const u64 ladder[] = {512, 256, 128, 64, 48, 32, 24, 16, 12, 8, 7, 6, 5}; /* GiB */
#define LADDER_N (sizeof ladder / sizeof *ladder)
static void reserve_again(void) {
    asked_n = 0;
    said[0] = 0;
    mem_reserve();
}
static void let_go(void) {
    CHECK(munmap(reservation, reservation_size) == 0);
    g_mem = NULL;
}
/* the first n tiers, with the watch, were the asks from asked[from] */
static int asked_watched(unsigned from, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (asked[from + i].size != ladder[i] << 30 || !asked[from + i].watched) return 0;
    return 1;
}
static int console_layout(u64 end) {
    return g_layout.base == 0x8000000ull && g_layout.end == end && g_layout.alias == 4ull << 30 &&
           g_layout.alias_size == 64ull << 30 && g_layout.heap == 0x1100000000ull && g_layout.heap_size == 8ull << 30 &&
           g_layout.stack == 0x1300000000ull && g_layout.stack_size == 2ull << 30 && g_layout.kern == 0x1380000000ull &&
           g_layout.kern_end == 0x13C0000000ull;
}
/* mem_layout_large_heap on the layout mem_init made: the heap region 4 GiB larger and the stack region after it 4 GiB
   up, with the kernel's pages where they follow the stack (the console's regions, whatever the reservation past
   them) or kept at the end; nothing where the 4 GiB don't fit. The layout is put back after */
static int large_heap(int grows, int kern_moves) {
    MemLayout was = g_layout;
    u64 up = grows ? 4ull << 30 : 0, kern_up = kern_moves ? up : 0;
    mem_layout_large_heap();
    int ok = g_layout.heap == was.heap && g_layout.heap_size == was.heap_size + up &&
             g_layout.stack == was.stack + up && g_layout.stack_size == was.stack_size &&
             g_layout.kern == was.kern + kern_up && g_layout.kern_end == was.kern_end + kern_up &&
             g_layout.end == was.end;
    g_layout = was;
    return ok;
}
static void test_ladder(void) {
    int fds[2], status = 0;
    u64 got[2 * (LADDER_N + 2)];
    size_t n = 0;
    ssize_t r;
    pid_t child;
    /* a host that gives it: the console's whole address space, asked first, and the game told so */
    reserve_again();
    CHECK(g_mem && g_as_size == 512ull << 30 && console_layout(512ull << 30));
    CHECK(large_heap(1, 1));
    CHECK(asked_n == 1 && asked_watched(0, 1));
    CHECK(strstr(said, "memory: 512 GiB of address space reserved, the console's\n"));
    CHECK(info(12) == 0x8000000ull && info(13) == (512ull << 30) - 0x8000000ull);
    let_go();
    /* a 39-bit kernel, or an RLIMIT_AS, with room for 256 GiB */
    refuse_above = 256ull << 30;
    reserve_again();
    CHECK(g_mem && g_as_size == 256ull << 30 && console_layout(256ull << 30));
    CHECK(large_heap(1, 1));
    CHECK(asked_n == 2 && asked_watched(0, 2));
    CHECK(strstr(said, "memory: 512 GiB of address space refused: "));
    CHECK(strstr(said, "memory: 256 GiB of address space reserved, of the console's 512: the game's address space ends "
                       "there\n"));
    let_go();
    /* down to 128 GiB, the console's regions still */
    refuse_above = 128ull << 30;
    reserve_again();
    CHECK(g_mem && g_as_size == 128ull << 30 && console_layout(128ull << 30));
    CHECK(large_heap(1, 1));
    CHECK(asked_n == 3 && asked_watched(0, 3));
    let_go();
    /* a Windows that gives no reservation with the write watch: every tier with it, then 512 and 256 GiB without */
    refuse_above = 256ull << 30;
    watch_refused = 1;
    unwatched_given = 1;
    reserve_again();
    CHECK(g_mem && g_as_size == 256ull << 30 && console_layout(256ull << 30));
    CHECK(asked_n == LADDER_N + 2 && asked_watched(0, LADDER_N));
    CHECK(asked[LADDER_N].size == 512ull << 30 && !asked[LADDER_N].watched);
    CHECK(asked[LADDER_N + 1].size == 256ull << 30 && !asked[LADDER_N + 1].watched);
    CHECK(strstr(said, "memory: 5 GiB of address space refused: ") &&
          strstr(said, "memory: 512 GiB of address space without the write watch refused: "));
    CHECK(strstr(said, "memory: 256 GiB of address space reserved without the write watch, as no tier from 512 GiB "
                       "down was given with it"));
    let_go();
    /* elsewhere: a refused first pass asks the second kind once, ENOSYS, and fatal ends the game */
    unwatched_given = 0;
    CHECK(pipe(fds) == 0);
    fflush(stdout);
    child = fork();
    if (!child) {
        struct rlimit none = {0, 0};
        close(fds[0]);
        asked_fd = fds[1];
        if (setrlimit(RLIMIT_CORE, &none) != 0) _exit(2);
        alarm(10);
        fclose(stderr); /* fatal's message: the abort is what the parent checks */
        reserve_again();
        _exit(0);
    }
    close(fds[1]);
    while (n < sizeof got && (r = read(fds[0], (u8*)got + n, sizeof got - n)) > 0) n += (size_t)r;
    close(fds[0]);
    CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    CHECK(n == sizeof *got * 2 * (LADDER_N + 1));
    for (unsigned i = 0; i < LADDER_N; i++) CHECK(got[2 * i] == ladder[i] << 30 && got[2 * i + 1] == 1);
    CHECK(got[2 * LADDER_N] == 512ull << 30 && got[2 * LADDER_N + 1] == 0);
    watch_refused = 0;
    refuse_above = 0;
    printf("address space ladder: 512, 256 and 128 GiB, the write watch's second pass, %u checks passed\n", checks);
}

/* The 32 and 48 GiB layouts (an iPhone app without the extended virtual addressing entitlement has about 52 GiB): a
   game asks 12 GiB of the alias region at once (an allocator's heap reserved there, then backed page by page with
   MapPhysicalMemory), so the region is 16 GiB. GetInfo names each region as the layout lays it out, a physical mapping
   goes up to the alias region's last page and not past it, and the stack region keeps a free GiB before the kernel's
   pages */
static void test_alias_layouts(void) {
    static const u64 sizes[] = {32, 48}; /* GiB */
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        const u64 size = sizes[i] << 30, want = 12ull << 30;
        base_process();
        refuse_above = size;
        mapping_begins = mapping_ends = 0;
        mem_init();
        CHECK(g_mem && g_as_size == size);
        /* the 12 GiB the game reserves, as it finds room for it: GetInfo's alias region holds a free run of it from
           the region's base, and its first and last pages map */
        g_hle.kernel.sysres_size = 64 * MIB;
        CHECK(info(3) >= want && mem_find_free(info(2), info(2) + info(3), want, 0x1000) == info(2));
        CHECK(svc_map_physical(ALIAS_BASE, 0x1000) == 0);
        CHECK(svc_map_physical(ALIAS_BASE + want - 0x1000, 0x1000) == 0);
        /* the regions as GetInfo names them, apart, and a free GiB between the stack region and the kernel's pages */
        CHECK(g_layout.alias == 4ull << 30 && g_layout.alias_size == 16ull << 30 && g_layout.heap == 20ull << 30 &&
              g_layout.heap_size == 8ull << 30 && g_layout.stack == 28ull << 30 && g_layout.stack_size == 2ull << 30 &&
              g_layout.kern == size - (1ull << 30) && g_layout.kern_end == size);
        CHECK(info(2) == 4ull << 30 && info(3) == 16ull << 30 && info(4) == 20ull << 30 && info(5) == 8ull << 30);
        CHECK(info(12) == 0x8000000ull && info(13) == size - 0x8000000ull);
        CHECK(info(14) == 28ull << 30 && info(15) == 2ull << 30);
        CHECK(STACK_BASE + STACK_SIZE + (1ull << 30) <= KERN_BASE);
        /* the kernel's pages stay in the last GiB: 4 GiB more heap fit before them in 48 GiB, not in 32 */
        CHECK(large_heap(size >= (48ull << 30), 0));
        /* the region's edges */
        CHECK(svc_map_physical(ALIAS_BASE + ALIAS_SIZE - 0x1000, 0x1000) == 0);
        CHECK(g_phys_mapped == 3 * 0x1000 && state(ALIAS_BASE + want - 0x1000) == MS_NORMAL);
        CHECK(svc_map_physical(ALIAS_BASE + ALIAS_SIZE - 0x1000, 0x2000) == SNX_RC_SVC_INVALID_MEMORY_REGION);
        CHECK(svc_map_physical(ALIAS_BASE - 0x1000, 0x1000) == SNX_RC_SVC_INVALID_MEMORY_REGION);
        CHECK(svc_map_physical(HEAP_BASE, 0x1000) == SNX_RC_SVC_INVALID_MEMORY_REGION);
        CHECK(svc_unmap_physical(ALIAS_BASE, ALIAS_SIZE) == 0 && g_phys_mapped == 0);
        g_hle.kernel.sysres_size = 0;
        CHECK(munmap(reservation, reservation_size) == 0);
        g_mem = NULL;
    }
    refuse_above = 0;
    printf("32 and 48 GiB layouts: a 16 GiB alias region with the game's 12 GiB in it, %u checks passed\n", checks);
}

/* a word GP reaches from a guest address, checked as the runtime checks an object it is handed: the address can be 0,
   so a null host pointer stands for none. Not inlined, so the compiler sees only GP's own arithmetic */
#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__) /* test_moved's hosts, the one caller */
static __attribute__((noinline)) int gp_holds(u64 a, u32 want) {
    const u32* p = a ? (const u32*)(void*)GP(a) : NULL;
    return p && *p == want;
}
#endif
/* NCE on Android (mem_reserve_identity from 64 GiB): the guest's addresses are the host's, above what the app's process
   holds, so the whole layout moves up to the first base from 64 GiB, in 2 MiB steps, where a tier is free. A page held
   at 65 GiB takes the 128 GiB tier's first 449 bases; the console's layout then lies from 64 GiB + 898 MiB, each
   region moved alike with its size kept (the 64 GiB alias region that an engine's 62 GB address region needs), and
   guest memory there is the host's. A layout of fixed addresses (36-bit) then ends the game. Linux only, and not under
   ASan, whose shadow holds those addresses */
#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__)
static MemLayout g_console; /* the layout a process starts with, which the scenarios before test_moved change */
#endif
static void test_moved(void) {
#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__)
    const u64 from = 64ull << 30, held_at = 65ull << 30, by = from + 898 * MIB, size = 128ull << 30;
    void* held = mmap((void*)(uintptr_t)held_at, 2 * MIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pid_t child;
    int status = 0;
    u8* p;
    if (held != (void*)(uintptr_t)held_at) {
        printf("moved layout: 65 GiB is taken in this process, skipped\n");
        if (held != MAP_FAILED) munmap(held, 2 * MIB);
        return;
    }
    mapping_begins = mapping_ends = 0; /* test_mapping_boundary's */
    reserve_at_on = 1;
    g_layout = g_console; /* as at a process's start, where NCE reserves before anything else */
    g_as_size = AS_MAX;
    CHECK(mem_reserve_identity(from) == 1);
    CHECK(mem_identity() && mem_moved() == by && g_mem == NULL);
    CHECK(g_layout.base == by + 0x8000000ull && g_layout.end == by + size && g_as_size == by + size);
    CHECK(g_layout.alias == by + (4ull << 30) && g_layout.alias_size == 64ull << 30 &&
          g_layout.heap == by + (68ull << 30) && g_layout.heap_size == 8ull << 30 &&
          g_layout.stack == by + (76ull << 30) && g_layout.stack_size == 2ull << 30 &&
          g_layout.kern == by + (78ull << 30) && g_layout.kern_end == by + (79ull << 30));
    CHECK(g_as_size <= AS_TOP); /* the tables indexed by guest address hold every one of them */
    CHECK((u64)(uintptr_t)reservation == g_layout.base && reservation_size == size - 0x8000000ull);
    mem_init();
    CHECK(tracked == reservation && tracked_size == reservation_size);
    /* the tracking's probe scans the moved code base, not g_mem's 0, which no tracking covers */
    CHECK(mem_ww_probe(NULL) == -1 && scanned == tracked && g_mem == NULL);
    CHECK(mem_map(HEAP_BASE, 0x1000, MS_NORMAL, PERM_RW) == 0 && state(HEAP_BASE) == MS_NORMAL);
    p = GP(HEAP_BASE);
    CHECK((u64)(uintptr_t)p == HEAP_BASE);
    /* with g_mem 0, a pointer GP makes is not null for a nonzero address: as g_mem plus a it was undefined, and the
       compiler took it for non-null only when g_mem is, which dropped every NVN program bind under NCE */
    memcpy(p, "PROG", 4);
    CHECK(gp_holds(HEAP_BASE, 0x474f5250));
    p[0] = 0x5A;
    CHECK(*(volatile u8*)(uintptr_t)(by + (68ull << 30)) == 0x5A);
    /* the 62 GiB an engine reserves in the alias region at startup (AllocateAddressRegion), which ends the game when it
       doesn't fit, there in the moved layout too */
    CHECK(mem_find_free(ALIAS_BASE, ALIAS_BASE + ALIAS_SIZE, 62ull << 30, 0x1000) == ALIAS_BASE);
    { /* what is not the game's in its addresses (mem_foreign), asked of the moved reservation: a named mapping over
         guest memory moved into a GPU's buffer is the runtime's own, one beside it another owner's */
        char out[64], want[64];
        share_ok = 1;
        CHECK(mem_migrate(HEAP_BASE, 0x1000, 7) == 0);
        foreign.named[0][0] = HEAP_BASE, foreign.named[0][1] = HEAP_BASE + 0x1000;
        foreign.named[1][0] = HEAP_BASE + 0x1000, foreign.named[1][1] = HEAP_BASE + 0x2000;
        foreign.n = 2;
        CHECK(mem_foreign(out, sizeof out) == 1 && foreign.lo == by + 0x8000000ull && foreign.hi == by + size);
        snprintf(want, sizeof want, "%#llx", (unsigned long long)(HEAP_BASE + 0x1000));
        CHECK(!strcmp(out, want));
        CHECK(mem_unmigrate(HEAP_BASE, 0x1000) == 0);
        CHECK(mem_foreign(out, sizeof out) == 2); /* moved back, it is no buffer's */
        foreign.n = 0;
        share_ok = 0;
    }
    CHECK(mem_unmap(HEAP_BASE, 0x1000) == 0);
    { /* a crash report's line on where a fault lies: below, in and past the moved layout, whose addresses are the
         host's */
        char out[256], want[256];
        mem_place((void*)(uintptr_t)0x8000000ull, out, sizeof out);
        snprintf(want, sizeof want, "0x8000000 lies below the game's address space, which starts at %#llx, by %#llx",
                 (unsigned long long)(by + 0x8000000ull), (unsigned long long)by);
        CHECK(!strcmp(out, want));
        mem_place((void*)(uintptr_t)(by + (68ull << 30)), out, sizeof out);
        snprintf(want, sizeof want, "%#llx lies in the game's address space, %#llx to %#llx: game address %#llx",
                 (unsigned long long)(by + (68ull << 30)), (unsigned long long)(by + 0x8000000ull),
                 (unsigned long long)(by + size), (unsigned long long)(by + (68ull << 30)));
        CHECK(!strcmp(out, want));
        mem_place((void*)(uintptr_t)(by + size + 0x5000), out, sizeof out);
        snprintf(want, sizeof want,
                 "%#llx lies past the end of the game's address space at %#llx, by 0x5000: its layout holds 128 GiB, "
                 "the console's 512",
                 (unsigned long long)(by + size + 0x5000), (unsigned long long)(by + size));
        CHECK(!strcmp(out, want));
        /* past the console's 512 GiB from the moved origin (the runtime's heap, a library): no layout's size to blame */
        mem_place((void*)(uintptr_t)(by + AS_TOP), out, sizeof out);
        snprintf(want, sizeof want,
                 "%#llx lies past the end of the game's address space at %#llx, by %#llx, past the console's 512 GiB "
                 "too",
                 (unsigned long long)(by + AS_TOP), (unsigned long long)(by + size),
                 (unsigned long long)(AS_TOP - size));
        CHECK(!strcmp(out, want));
        mem_place((void*)(uintptr_t)(by + AS_TOP - 1), out, sizeof out);
        CHECK(strstr(out, ": its layout holds 128 GiB, the console's 512") != NULL);
    }
    fflush(stdout);
    child = fork();
    if (!child) {
        /* no core of the abort it waits for: the child holds the 128 GiB reservation, which a core_pattern pipe
           (systemd-coredump, apport) reads for minutes, as it cannot skip the holes a file seeks over; the limit for a
           file, not dumpable for a pipe. The alarm ends a child that hangs anyway, which the WTERMSIG check names */
        struct rlimit none = {0, 0};
        if (setrlimit(RLIMIT_CORE, &none) != 0 || prctl(PR_SET_DUMPABLE, 0) != 0) _exit(2);
        alarm(10);
        fclose(stderr); /* fatal's message is the parent's to check, not the log's */
        mem_layout36();
        _exit(0);
    }
    CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    CHECK(munmap(reservation, reservation_size) == 0);
    CHECK(munmap(held, 2 * MIB) == 0);
    reserve_at_on = 0;
    printf("moved layout: the 128 GiB tier from %#llx, %u checks passed\n", (unsigned long long)by, checks);
#else
    printf("moved layout: skipped, as it takes the host's addresses from 64 GiB (Linux, without ASan)\n");
#endif
}

int main(void) {
#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__)
    g_console = g_layout;
#endif
    { /* a crash before the reservation */
        char out[96];
        mem_place((void*)(uintptr_t)0x1234, out, sizeof out);
        CHECK(!strcmp(out, "0x1234: no address space is reserved for the game yet"));
    }
    hle_init();
    /* this host's pages, then those of hosts with larger ones (16 KiB: Apple silicon, some Android kernels; 64 KiB:
       PowerPC64 and some AArch64 distributions), which mem.c commits in whole */
    static const u64 pages[] = {16 << 10, 64 << 10};
    unsigned i;
    host_page = (u64)sysconf(_SC_PAGESIZE);
    for (i = 0; i <= sizeof pages / sizeof *pages; i++) {
        if (i) {
            if (pages[i - 1] <= host_page) continue;
            host_page = pages[i - 1];
        }
        mapping_begins = mapping_ends = 0; /* test_mapping_boundary's, from the round before */
        mem_init();
        CHECK(mem_ww_probe(NULL) == -1 && scanned == g_mem && g_mem); /* the tracking starts at g_mem */
        test_heap();
        test_large_heap();
        test_heap32();
        test_fit();
        test_short_hint();
        test_gb_said();
        test_system_resource();
        test_rollback();
        test_locked_shrink();
        test_shared_budget();
        test_map_memory();
        test_shared_memory();
        test_host_pages();
        test_mapped_runs();
        test_user_access();
        test_pool_walk();
        test_mapping_boundary(); /* last: every commit after it must be inside a mapping */
        CHECK(munmap(reservation, reservation_size) == 0);
        g_mem = NULL; /* the next round reserves it again */
        printf("memory accounting with %llu KiB host pages: %u checks passed\n",
               (unsigned long long)(host_page >> 10), checks);
    }
    test_ladder();
    test_alias_layouts();
    /* a system that gives less than 128 GiB: the largest tier it gives, in a smaller layout of that size */
    host_page = (u64)sysconf(_SC_PAGESIZE);
    refuse_above = 24ull << 30;
    mem_reserve();
    CHECK(g_mem && g_as_size == 24ull << 30 && g_layout.end == 24ull << 30);
    { /* past a reservation smaller than the console's: a game address no layout here holds */
        char out[256], want[256];
        mem_place(g_mem + (24ull << 30) + 0x40, out, sizeof out);
        snprintf(want, sizeof want,
                 "%#llx lies past the end of the game's address space at %#llx, by 0x40: its layout holds 24 GiB, "
                 "the console's 512",
                 (unsigned long long)(uintptr_t)(g_mem + (24ull << 30) + 0x40),
                 (unsigned long long)(uintptr_t)(g_mem + (24ull << 30)));
        CHECK(!strcmp(out, want));
        mem_place(g_mem + 0x1000, out, sizeof out);
        CHECK(strstr(out, ": game address 0x1000") != NULL);
        /* a reservation the system placed: no other owner's fixed address lies there, and nothing is looked at */
        CHECK(mem_foreign(out, sizeof out) == -1 && !out[0] && !foreign.calls);
    }
    CHECK(g_layout.alias == 4ull << 30 && g_layout.alias_size == 4ull << 30 && g_layout.heap == 8ull << 30 &&
          g_layout.heap_size == 8ull << 30 && g_layout.stack == 16ull << 30 && g_layout.stack_size == 2ull << 30 &&
          g_layout.kern == 23ull << 30 && g_layout.kern_end == 24ull << 30);
    CHECK(munmap(reservation, reservation_size) == 0);
    g_mem = NULL;
    refuse_above = 0;
    test_moved(); /* last: it leaves the layout moved */
    return 0;
}
