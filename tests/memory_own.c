/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GPU-owned guest memory (memory/own.c) over the real alias pool and traps (platform/plat_posix.c: userfaultfd
   minor faults), with owners whose "GPU copy" is host memory, so no GPU is needed: a read and a write of an owned
   byte land the owner first, the CPU's bytes on a page it shares with an owner stay, a neighbour writing during the
   move into the pool loses nothing, a system call fails on owned memory until it is acquired, dropping and unmapping
   land nothing, a view of owned memory lands it first, a fault on the thread holding the producer's lock ends the
   process, the write watch's scan skips owned spans, and threads faulting at once on many pages while owners come
   and go read every owner's bytes. Skips (77) where the host has no traps. Debug settings come as the runtime takes
   them, --key=value (--write_tracking=1, --memory_traps=1: the protection's paths); with memory_traps=2, or 1 and no
   pool, it checks only that the log says why there are no traps.
   On Windows (platform/plat_win32.c) the traps are page protection and a vectored exception handler on the
   reservation's own pages, with no pool: landings write in place, a view is a copy, and ReadFile or WriteFile over an
   owned page fails with ERROR_NOACCESS until it is acquired */
#include "common/utilities/common.h"
#include "nx/memory/mem.h"
#include "nx/runtime/nce/nce.h"
#include "nx/memory/own.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/platform/plat.h"
#include "snx_settings.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/prctl.h>
#endif

static unsigned checks;
static int diagnostic_log;
static char g_log[4096]; /* the log's lines until the threads start (g_log_on), as far as they fit */
static int g_log_on = 1;
static const char* g_settings_given[8]; /* the command line's --key=value, again for each child */
static int g_settings_count;
static void test_fail(const char* expr, int line) {
    fprintf(stderr, "memory_own.c:%d: %s\n", line, expr);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED);                                                              \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void logf_(int level, const char* fmt, ...) {
    va_list ap;
    size_t n = g_log_on ? strlen(g_log) : 0;
    (void)level;
    if (g_log_on) {
        va_start(ap, fmt);
        vsnprintf(g_log + n, sizeof g_log - n, fmt, ap);
        va_end(ap);
    }
    if (!diagnostic_log) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
#if defined(__linux__)
    /* Let the diagnostics inspect proc registers first, then prevent a piped core dump. */
    prctl(PR_SET_DUMPABLE, 0);
#endif
    abort();
}
void log_crash_mode(void) {}
/* nce.c, whose NCE no game here is of: mem.c's reservation asks it first, and the crash handler gives it the
   signal's context */
int nce_wanted(void) { return 0; }
void nce_crash_context(const void* context, const void* fault) {
    (void)context;
    (void)fault;
}
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
}
#if defined(_WIN32)
void plat_prof_add_thread(HANDLE h) { (void)h; } /* profiler_win32.c's: no profiler here */
#endif
/* the renderer's side of host memory changes: nothing to land here */
void (*g_gpu_mapping_begin)(u64, u64, int);
void (*g_gpu_mapping_end)(void);
void gpu_mem_changed(u64 a, u64 size) {
    (void)a;
    (void)size;
}

/* ---------------------------------------------------------------- owners whose GPU copy is host memory */
typedef struct {
    u8* data;
    int refs;
} Res;
static int g_lands, g_landed_bytes;
static u32 g_busy_tid;
/* test_republish_after_landing's order: the thread that lands g_hold_res (once) waits in producer_unlock, after it lets
   go of the producer's lock, until g_republished. Its own unlock alone: the thread's unlock of a landing begun before,
   seen after the hold is set, would wait with the reader's fault behind it, which nothing then lands */
static void* g_hold_res;  /* atomic */
static u32 g_hold_tid;    /* atomic: the thread that landed it, until that unlock */
static int g_republished; /* atomic */
static void land(const OwnRange* r, u8* const* dst, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        const Res* res = (const Res*)r[i].res;
        void* held = r[i].res;
        memcpy(dst[i], res->data + r[i].res_off, r[i].size);
        __atomic_add_fetch(&g_landed_bytes, (int)r[i].size, __ATOMIC_RELAXED);
        if (__atomic_compare_exchange_n(&g_hold_res, &held, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            __atomic_store_n(&g_hold_tid, plat_tid(), __ATOMIC_RELEASE);
    }
    __atomic_add_fetch(&g_lands, 1, __ATOMIC_RELAXED);
}
static void retain(void* res) { __atomic_add_fetch(&((Res*)res)->refs, 1, __ATOMIC_RELAXED); }
static u64 g_release_touch; /* a guest address the release reads, 0 for none: test_release_order's */
static int g_release_read;
static void release(void* res) {
    __atomic_sub_fetch(&((Res*)res)->refs, 1, __ATOMIC_RELAXED);
    if (g_release_touch) g_release_read = *(volatile u8*)GP(g_release_touch);
}
static u32 busy_tid(void) { return g_busy_tid; }
/* the producer's answer to a watch (own_watch): g_resolve_res published over [g_resolve_lo, + g_resolve_size), or
   nothing when it is NULL. Called with the producer's lock held, as own_begin needs */
static int g_resolves; /* atomic */
static void* g_resolve_res;
static u64 g_resolve_lo, g_resolve_size;
static void resolve(u64 addr, u64 size) {
    (void)addr;
    (void)size;
    __atomic_add_fetch(&g_resolves, 1, __ATOMIC_RELAXED);
    if (g_resolve_res) CHECK(own_begin(g_resolve_lo, g_resolve_size, g_resolve_res, 0) == 0);
}
/* the producer's lock: recursive, as the renderer's */
static pthread_mutex_t g_producer;
static void producer_lock(void) { pthread_mutex_lock(&g_producer); }
static void producer_unlock(void) {
    u32 self = plat_tid();
    pthread_mutex_unlock(&g_producer);
    if (!__atomic_compare_exchange_n(&g_hold_tid, &self, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    for (int waited_ms = 0; !__atomic_load_n(&g_republished, __ATOMIC_ACQUIRE); waited_ms++) {
        if (waited_ms == 10000) test_fail("the reader republished the page within 10 s", __LINE__);
        plat_sleep_ns(1000000);
    }
}
/* r's references once no landing is under way: a landing holds the producer's lock from before it cuts its entries
   until it has released them, so a thread it let go, or an own_acquire that found the entries cut already, can come
   back before the release */
static int refs(Res* r) {
    producer_lock();
    producer_unlock();
    return __atomic_load_n(&r->refs, __ATOMIC_RELAXED);
}
static Res* res_new(u64 size, u8 fill) {
    Res* r = (Res*)calloc(1, sizeof *r);
    u64 i;
    r->data = (u8*)malloc(size);
    for (i = 0; i < size; i++) r->data[i] = (u8)(fill + i);
    return r;
}
static void res_free(Res* r) {
    free(r->data);
    free(r);
}

/* a region of normal memory, its bytes the CPU's: 0x11 + offset */
static u64 region(u64 a, u64 size) {
    u64 i;
    CHECK(mem_map(a, size, MS_NORMAL, PERM_RW) == 0);
    for (i = 0; i < size; i++) GP(a)[i] = (u8)(0x11 + i);
    return a;
}
#define R0 (HEAP_BASE + 0x100000)

/* reads and writes of owned bytes land the owner first; the CPU's bytes beside them stay */
static void test_read_write(void) {
    u64 a = region(R0, 0x10000);
    Res* r = res_new(0x3000, 0xA0);
    int lands = g_lands;
    CHECK(own_begin(a + 0x1100, 0x2000, r, 0) == 0 && refs(r) == 1);
    {
        OwnRange o[2];
        CHECK(own_lookup(a, 0x10000, o, 2) == 1 && o[0].addr == a + 0x1100 && o[0].size == 0x2000);
        CHECK(own_lookup(a + 0x2000, 0x10, o, 2) == 1 && o[0].addr == a + 0x2000 && o[0].res_off == 0xF00);
    }
    CHECK(GP(a)[0x1200] == (u8)(0xA0 + 0x100)); /* the read faults: the owner lands */
    CHECK(g_lands == lands + 1 && refs(r) == 0);
    CHECK(GP(a)[0x10FF] == (u8)(0x11 + 0x10FF) && GP(a)[0x3100] == (u8)(0x11 + 0x3100)); /* the CPU's bytes */
    CHECK(GP(a)[0x30FF] == (u8)(0xA0 + 0x1FFF));
    {
        OwnRange o;
        CHECK(own_lookup(a, 0x10000, &o, 1) == 0);
    }
    /* a write: the owner lands, then the write is done */
    CHECK(own_begin(a + 0x5000, 0x800, r, 0) == 0);
    GP(a)[0x5010] = 0x77;
    CHECK(g_lands == lands + 2 && GP(a)[0x5010] == 0x77 && GP(a)[0x5011] == (u8)(0xA0 + 0x11));
    /* a neighbour on the owner's page, outside it: lands it too, and its own write stays */
    CHECK(own_begin(a + 0x6100, 0x100, r, 0) == 0);
    GP(a)[0x6000] = 0x55;
    CHECK(g_lands == lands + 3 && GP(a)[0x6000] == 0x55 && GP(a)[0x6100] == 0xA0 && GP(a)[0x61FF] == 0x9F);
    /* a later owner of part of the bytes splits the older one */
    CHECK(own_begin(a + 0x8000, 0x3000, r, 0) == 0 && own_begin(a + 0x9000, 0x800, r, 0x1000) == 0);
    {
        OwnRange o[4];
        CHECK(own_lookup(a + 0x8000, 0x3000, o, 4) == 3);
        CHECK(o[0].size == 0x1000 && o[1].addr == a + 0x9000 && o[1].res_off == 0x1000 && o[2].addr == a + 0x9800);
        CHECK(o[2].res_off == 0x1800 && refs(r) == 3);
    }
    own_acquire(a + 0x8000, 0x3000);
    CHECK(refs(r) == 0);
    CHECK(GP(a)[0x8000] == 0xA0);
    CHECK(GP(a)[0x9000] == (u8)(0xA0 + 0x1000));
    CHECK(GP(a)[0xA7FF] == (u8)(0xA0 + 0x27FF));
    CHECK(mem_unmap(a, 0x10000) == 0);
    res_free(r);
}

/* a neighbour writing the page while the range moves into the pool: no write is lost. Each value goes to a slot of
   its own, once, paced so the move happens while the owner's page is written: a write lost in the copy leaves its
   slot short */
#define SLOTS 1984 /* u32s: all but the owner's last 0x100 bytes of the two pages */
static volatile u32 g_progress;
static u64 g_hammer;
static void* hammer(void* arg) {
    u32 i;
    (void)arg;
    for (i = 0; i < SLOTS; i++) {
        volatile u32 spin;
        u32 v = i + 1;
        memcpy(GP(g_hammer + 4ull * i), &v, 4);
        __atomic_store_n(&g_progress, i, __ATOMIC_RELEASE);
        for (spin = 0; spin < 300; spin++) {
        }
    }
    return NULL;
}
static void test_freeze(void) {
    u64 a = region(R0 + 0x100000, 0x80000), i;
    Res* r = res_new(0x100, 0x40);
    for (i = 0; i < 32; i++) {
        pthread_t t;
        u32 k, v;
        u64 pages = a + i * 0x4000; /* two pages, each trial a first move into the pool */
        g_hammer = pages;
        g_progress = 0;
        pthread_create(&t, NULL, hammer, NULL);
        /* on the owner's page (the second, the one that moves), early on it */
        while (__atomic_load_n(&g_progress, __ATOMIC_ACQUIRE) < 1024 + 64) {
        }
        CHECK(own_begin(pages + 0x2000 - 0x100, 0x100, r, 0) == 0);
        pthread_join(t, NULL);
        for (k = 0; k < SLOTS; k++) {
            memcpy(&v, GP(pages + 4ull * k), 4);
            CHECK(v == k + 1);
        }
        CHECK(GP(pages + 0x1F00)[0] == 0x40); /* and the owner's bytes */
    }
    CHECK(mem_unmap(a, 0x80000) == 0 && refs(r) == 0);
    res_free(r);
}

/* the kernel can't fault owned memory in: a system call fails until the range is acquired */
#if defined(_WIN32)
/* WriteFile reading owned memory and ReadFile writing it fail with ERROR_NOACCESS, and land nothing; acquired, both go */
static void test_syscall(void) {
    u64 a = region(R0 + 0x200000, 0x4000);
    Res *r = res_new(0x1000, 0x30), *w = res_new(0x1000, 0x50);
    HANDLE rd, wr;
    DWORD n = 0;
    u8 buf[16];
    int lands = g_lands;
    CHECK(CreatePipe(&rd, &wr, NULL, 0x10000));
    CHECK(own_begin(a + 0x1000, 0x1000, r, 0) == 0 && own_begin(a + 0x2000, 0x1000, w, 0) == 0);
    SetLastError(0);
    CHECK(!WriteFile(wr, GP(a + 0x1000), 16, &n, NULL) && GetLastError() == ERROR_NOACCESS && g_lands == lands);
    own_acquire(a + 0x1000, 16);
    CHECK(WriteFile(wr, GP(a + 0x1000), 16, &n, NULL) && n == 16 && ReadFile(rd, buf, 16, &n, NULL) && n == 16);
    CHECK(buf[0] == 0x30 && buf[15] == 0x3F);
    /* into owned memory: refused, the owner still there, until acquired */
    CHECK(WriteFile(wr, "0123456789abcdef", 16, &n, NULL) && n == 16);
    SetLastError(0);
    CHECK(!ReadFile(rd, GP(a + 0x2000), 16, &n, NULL) && GetLastError() == ERROR_NOACCESS);
    {
        OwnRange o;
        CHECK(own_lookup(a + 0x2000, 0x1000, &o, 1) == 1 && o.res == w);
    }
    own_acquire(a + 0x2000, 16);
    { /* whatever the failed read left in the pipe, then a read into the acquired page */
        DWORD avail = 0;
        u8 drain[64];
        if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) CHECK(ReadFile(rd, drain, avail, &n, NULL));
    }
    CHECK(WriteFile(wr, "ghijklmnopqrstuv", 16, &n, NULL) && n == 16);
    CHECK(ReadFile(rd, GP(a + 0x2000), 16, &n, NULL) && n == 16);
    CHECK(GP(a + 0x2000)[0] == 'g' && GP(a + 0x2000)[15] == 'v' && GP(a + 0x2000)[16] == (u8)(0x50 + 16));
    CloseHandle(rd);
    CloseHandle(wr);
    /* a file's read into owned memory, as fs.c's (its fread reads the kernel's way, ReadFile, into the buffer) */
    {
        char dir[MAX_PATH], path[MAX_PATH];
        HANDLE f;
        Res* r3 = res_new(0x2000, 0x90);
        CHECK(GetTempPathA(sizeof dir, dir) && GetTempFileNameA(dir, "own", 0, path));
        f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        CHECK(f != INVALID_HANDLE_VALUE);
        CHECK(WriteFile(f, GP(a + 0x1000), 0x1000, &n, NULL) && n == 0x1000); /* acquired above: the CPU's bytes */
        CHECK(own_begin(a + 0x2000, 0x2000, r3, 0) == 0);
        lands = g_lands;
        SetFilePointer(f, 0, NULL, FILE_BEGIN);
        SetLastError(0);
        CHECK(!ReadFile(f, GP(a + 0x2800), 0x1000, &n, NULL) && GetLastError() == ERROR_NOACCESS && g_lands == lands);
        own_acquire(a + 0x2800, 0x1000);
        CHECK(g_lands == lands + 1 && refs(r3) == 0);
        SetFilePointer(f, 0, NULL, FILE_BEGIN);
        CHECK(ReadFile(f, GP(a + 0x2800), 0x1000, &n, NULL) && n == 0x1000);
        CHECK(GP(a + 0x2800)[0] == 0x30 && GP(a + 0x27FF)[0] == (u8)(0x90 + 0x7FF) &&
              GP(a + 0x3800)[0] == (u8)(0x90 + 0x1800));
        CloseHandle(f);
        res_free(r3);
    }
    CHECK(mem_unmap(a, 0x4000) == 0 && refs(r) == 0 && refs(w) == 0);
    res_free(r);
    res_free(w);
}
#else
static void test_syscall(void) {
    u64 a = region(R0 + 0x200000, 0x4000);
    Res* r = res_new(0x1000, 0x30);
    int fd[2];
    u8 buf[16];
    CHECK(pipe(fd) == 0);
    CHECK(own_begin(a + 0x1000, 0x1000, r, 0) == 0);
    errno = 0;
    CHECK(write(fd[1], GP(a + 0x1000), 16) == -1 && errno == EFAULT);
    own_acquire(a + 0x1000, 16);
    CHECK(write(fd[1], GP(a + 0x1000), 16) == 16 && read(fd[0], buf, 16) == 16 && buf[0] == 0x30 && buf[15] == 0x3F);
    close(fd[0]);
    close(fd[1]);
    CHECK(mem_unmap(a, 0x4000) == 0);
    res_free(r);
}
#endif

/* dropping and unmapping let go without landing */
static void test_drop(void) {
    u64 a = region(R0 + 0x300000, 0x4000);
    Res* r = res_new(0x2000, 0x60);
    int lands = g_lands;
    CHECK(own_begin(a, 0x2000, r, 0) == 0);
    own_drop(a, 0x1000);
    CHECK(refs(r) == 1 && GP(a)[5] == (u8)(0x11 + 5) && g_lands == lands); /* the CPU's old bytes, no landing */
    CHECK(mem_unmap(a, 0x4000) == 0 && refs(r) == 0 && g_lands == lands);
    CHECK(region(a, 0x4000) == a && GP(a)[0x1005] == (u8)(0x11 + 0x1005)); /* mapped again: nothing trapped */
    CHECK(mem_unmap(a, 0x4000) == 0);
    res_free(r);
}

/* A wide unmap may contain no owner, or a small owner surrounded by ordinary pages. A byte drop sharing a host page
   with retained ownership must leave that page trapped until its remaining contents land. */
static void test_drop_sparse(void) {
    u64 a = region(R0 + 0x1000000, 0x800000);
    Res* r = res_new(0x1000, 0x60);
    Res* other = res_new(0x1000, 0x70);
    int lands = g_lands;
    CHECK(own_begin(a + 0x310000, 0x1000, r, 0) == 0);
    CHECK(own_begin(a + 0x710000, 0x1000, other, 0) == 0);
    own_drop(a, 0x200000); /* ownership elsewhere must not affect this hole */
    CHECK(refs(r) == 1 && refs(other) == 1 && g_lands == lands);
    own_drop(a + 0x310000, 0x100);
    CHECK(refs(r) == 1 && g_lands == lands);
    CHECK(GP(a)[0x310100] == r->data[0x100] && refs(r) == 0 && g_lands == lands + 1);
    CHECK(GP(a)[0x310005] == (u8)(0x11 + 5)); /* dropped bytes retain the CPU's copy */
    CHECK(own_begin(a + 0x310000, 0x1000, r, 0) == 0);
    lands = g_lands;
    own_drop(a, 0x600000); /* open the middle owner's pages, retaining the far one */
    CHECK(refs(r) == 0 && refs(other) == 1 && g_lands == lands);
    CHECK(mem_unmap(a, 0x800000) == 0 && refs(other) == 0 && g_lands == lands);
    res_free(r);
    res_free(other);
}

/* staging reads the CPU's bytes around owned ones through the mirror and lands nothing: the owned parts come back for
   the caller to copy from their resources */
static void test_stage(void) {
    u64 a = region(R0 + 0x380000, 0x4000), i;
    Res* r = res_new(0x1000, 0x70);
    int lands = g_lands;
    u8 dst[0x3000];
    OwnRange o[4];
    CHECK(own_begin(a + 0x1800, 0x1000, r, 0) == 0);
    GP(a)[0x3800] = 0x99; /* a page with no owner on it: written as ever */
    lands = g_lands;
    memset(dst, 0xEE, sizeof dst);
    CHECK(own_stage(a + 0x1000, 0x3000, dst, o, 4) == 1 && g_lands == lands);
    CHECK(o[0].addr == a + 0x1800 && o[0].size == 0x1000 && o[0].res == r && o[0].res_off == 0);
    for (i = 0; i < 0x800; i++) CHECK(dst[i] == (u8)(0x11 + 0x1000 + i)); /* the CPU's, beside the owner */
    for (i = 0x800; i < 0x1800; i++) CHECK(dst[i] == 0xEE); /* the owner's: left for the caller's copy */
    CHECK(dst[0x1800] == (u8)(0x11 + 0x2800) && dst[0x2800] == 0x99);
    CHECK(own_stage(a + 0x1000, 0x3000, dst, o, 0) == 1); /* no room: how many, nothing copied */
    /* a second owner within a page: the CPU's bytes between and after the two, and neither owner's */
    Res* r2 = res_new(0x200, 0x30);
    CHECK(own_begin(a + 0x3200, 0x200, r2, 0) == 0);
    lands = g_lands;
    memset(dst, 0xEE, sizeof dst);
    CHECK(own_stage(a + 0x1000, 0x3000, dst, o, 4) == 2 && g_lands == lands);
    CHECK(o[1].addr == a + 0x3200 && o[1].size == 0x200 && o[1].res == r2);
    for (i = 0; i < 0x3000; i++)
        if ((i >= 0x800 && i < 0x1800) || (i >= 0x2200 && i < 0x2400))
            CHECK(dst[i] == 0xEE);
        else
            CHECK(dst[i] == (i == 0x2800 ? 0x99 : (u8)(0x11 + 0x1000 + i)));
    CHECK(own_lookup(a, 0x4000, o, 4) == 2 && g_lands == lands);
    CHECK(mem_unmap(a, 0x4000) == 0 && refs(r) == 0 && refs(r2) == 0);
    res_free(r);
    res_free(r2);
}

/* bytes own_drop uncovered on a page an owner keeps trapped: staging reads them as the CPU left them. Through the
   mirror nothing lands; in place (Windows) the page has no copy of them (it was the owner's whole when armed), so it
   lands first */
static void test_stage_dropped(void) {
    u64 a = region(R0 + 0xD00000, 0x3000), i;
    Res* r = res_new(0x1000, 0x70);
    int lands;
    u8 dst[0x200];
    CHECK(own_begin(a + 0x1000, 0x1000, r, 0) == 0);
    own_drop(a + 0x1000, 0x100);
    lands = g_lands;
    memset(dst, 0xEE, sizeof dst);
    CHECK(own_stage(a + 0x1000, 0x100, dst, NULL, 0) == 0);
    CHECK(g_lands == lands + (plat_alias_mirror() ? 0 : 1));
    for (i = 0; i < 0x100; i++) CHECK(dst[i] == (u8)(0x11 + 0x1000 + i)); /* the CPU's copy, as test_drop_sparse */
    CHECK(GP(a)[0x1100] == r->data[0x100] && GP(a)[0x10FF] == (u8)(0x11 + 0x10FF));
    CHECK(mem_unmap(a, 0x3000) == 0 && refs(r) == 0);
    res_free(r);
}

/* a second mapping of owned memory lands it first: owned memory has one */
static void test_view(void) {
    u64 a = region(R0 + 0x400000, 0x4000), v = STACK_BASE + 0x100000;
    Res* r = res_new(0x1000, 0x90);
    int lands = g_lands;
    CHECK(own_begin(a + 0x1000, 0x1000, r, 0) == 0);
    CHECK(mem_alias_begin(a, 0x4000) == 0);
    CHECK(mem_alias_map(v, a, 0x4000, MS_STACK, PERM_RW) == 0);
    CHECK(g_lands == lands + 1 && refs(r) == 0 && GP(v)[0x1001] == 0x91 && GP(a)[0x1001] == 0x91);
    if (!mem_pool_active()) {
        /* without the pool (Windows) a view is a copy, one mapping of pages of its own, which can be owned. Unmapped
           with its copy written back, what the GPU holds of it lands first, outside the memory lock */
        Res* r2 = res_new(0x1000, 0xB0);
        CHECK(own_begin(v + 0x2000, 0x1000, r2, 0) == 0);
        lands = g_lands;
        CHECK(mem_alias_unmap(v, 0x4000, a) == 0);
        CHECK(g_lands == lands + 1 && refs(r2) == 0 && GP(a)[0x2005] == 0xB5 && GP(a)[0x1001] == 0x91);
        mem_alias_end(a, 0x4000);
        CHECK(mem_unmap(a, 0x4000) == 0);
        res_free(r);
        res_free(r2);
        return;
    }
    /* nor is a view's memory owned: own_begin refuses it, and the producer lands it itself */
    CHECK(own_begin(a + 0x1000, 0x1000, r, 0) == -1 && own_begin(v, 0x1000, r, 0) == -1);
    CHECK(mem_alias_unmap(v, 0x4000, 0) == 0);
    mem_alias_end(a, 0x4000);
    CHECK(mem_unmap(a, 0x4000) == 0);
    res_free(r);
}

/* a fault on the thread holding the producer's lock is the runtime's bug: it ends the process, not hangs. In a
   process of its own (this test run again: a forked one has no traps, which aren't inherited) */
#if defined(_WIN32)
/* the child: its stderr to the file SNX_TEST_BUSY_OUT names, no error box for the abort, and a watchdog in place of
   alarm */
static DWORD WINAPI busy_watchdog(LPVOID arg) {
    (void)arg;
    Sleep(10000);
    fprintf(stderr, "busy child: no abort within 10 s\n");
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 9);
    return 0;
}
static int busy_child(int write, int restricted) {
    u64 a = region(R0 + 0x500000, 0x2000);
    Res* r = res_new(0x1000, 0x10);
    const char* out = getenv("SNX_TEST_BUSY_OUT");
    HANDLE t;
    (void)restricted;
    if (out && !freopen(out, "w", stderr)) return 6;
    setvbuf(stderr, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    t = CreateThread(NULL, 0, busy_watchdog, NULL, 0, NULL);
    if (t) CloseHandle(t);
    diagnostic_log = 1;
    if (own_begin(a, 0x1000, r, 0)) return 2;
    g_busy_tid = plat_tid();
    fprintf(stderr, "(an abort with the thread and address is expected next)\n");
    if (write) *(volatile u8*)GP(a) = 0x42;
    if (GP(a)[3] == 0x13) return 3; /* the fault must not be answered */
    return 4;
}
static void test_busy(const char* self, int write, int restricted) {
    char out[MAX_PATH], dir[MAX_PATH], report[32768];
    const char* mode = restricted ? "--busy-private" : write ? "--busy-write" : "--busy";
    const char* args[sizeof g_settings_given / sizeof *g_settings_given + 3] = {self, mode};
    FILE* f;
    size_t n;
    intptr_t status;
    CHECK(GetTempPathA(sizeof dir, dir) && GetTempFileNameA(dir, "own", 0, out));
    CHECK(_putenv_s("SNX_TEST_BUSY_OUT", out) == 0);
    memcpy(args + 2, g_settings_given, (size_t)g_settings_count * sizeof *args);
    status = _spawnv(_P_WAIT, self, args);
    _putenv_s("SNX_TEST_BUSY_OUT", "");
    f = fopen(out, "rb");
    CHECK(f != NULL);
    n = fread(report, 1, sizeof report - 1, f);
    report[n] = 0;
    fclose(f);
    DeleteFileA(out);
    if (status != 3) fprintf(stderr, "busy child: exit %lld, its output:\n%s\n", (long long)status, report);
    CHECK(status == 3); /* abort's */
    CHECK(strstr(report, write ? "access=write" : "access=read") != NULL);
    CHECK(strstr(report, "ownership-fault: owner guest=[") != NULL);
    CHECK(strstr(report, "resource-offset=0 landing=0") != NULL);
    CHECK(strstr(report, "own_acquire first") != NULL);
    CHECK(strstr(report, "host-pc=") || strstr(report, "unavailable"));
    if (strstr(report, "host-pc=")) CHECK(strstr(report, "ownership-fault: pc[0] host="));
}
#else
static int busy_child(int write, int restricted) {
    u64 a = region(R0 + 0x500000, 0x2000);
    Res* r = res_new(0x1000, 0x10);
    /* no core of the abort it expects: its address space is the guest's whole reservation, which a core_pattern pipe
       (systemd-coredump) takes minutes to read, and the test waits for the abort. A zero limit for a core written to
       a file, and on Linux not dumpable at all, as a pipe takes the core whatever the limit */
    struct rlimit none = {0, 0};
    CHECK(setrlimit(RLIMIT_CORE, &none) == 0);
#if defined(__linux__)
    CHECK(prctl(PR_SET_DUMPABLE, restricted ? 0 : 1) == 0);
#else
    (void)restricted;
#endif
    alarm(10);
    diagnostic_log = 1;
    if (own_begin(a, 0x1000, r, 0)) return 2;
    g_busy_tid = plat_tid();
    fprintf(stderr, "(an abort with the thread and address is expected next) busy access=%llx\n",
            (unsigned long long)(uintptr_t)(write ? GP(a) : GP(a) + 3));
    if (write) *(volatile u8*)GP(a) = 0x42;
    if (GP(a)[3] == 0x13) return 3; /* the fault must not be answered */
    return 4;
}
/* the report's line that starts with what, up to its end, in line (empty when there is none) */
static void report_line(const char* report, const char* what, char* line, size_t cap) {
    const char* at = strstr(report, what);
    size_t n = 0;
    if (at)
        while (at[n] && at[n] != '\n' && n + 1 < cap) n++;
    if (at) memcpy(line, at, n);
    line[n] = 0;
}
static void test_busy(const char* self, int write, int restricted) {
    FILE* output = tmpfile();
    char report[32768];
    size_t n;
    pid_t pid;
    int status = 0;
    CHECK(output != NULL);
    pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        const char* mode = restricted ? "--busy-private" : write ? "--busy-write" : "--busy";
        const char* args[sizeof g_settings_given / sizeof *g_settings_given + 3] = {self, mode};
        memcpy(args + 2, g_settings_given, (size_t)g_settings_count * sizeof *args);
        if (dup2(fileno(output), STDERR_FILENO) < 0) _exit(6);
        execv(self, (char* const*)args);
        _exit(5);
    }
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    rewind(output);
    n = fread(report, 1, sizeof report - 1, output);
    report[n] = 0;
    fclose(output);
    CHECK(strstr(report, write ? "access=write" : "access=read") != NULL);
    CHECK(strstr(report, "ownership-fault: owner guest=[") != NULL);
    CHECK(strstr(report, "resource-offset=0 landing=0") != NULL);
    CHECK(strstr(report, "own_acquire first") != NULL);
    /* Restricted procfs must explain missing context rather than silently printing the handler's PC. */
    CHECK(strstr(report, "host-pc=") || strstr(report, "unavailable"));
    if (strstr(report, "host-pc=")) CHECK(strstr(report, "ownership-fault: pc[0] host="));
    /* the code it names is the access's, busy_child's in this program, and the address the access's own: under
       protection traps the thread waits in its handler's futex, whose code is libc's */
    char pc[1024], given[256], told[256];
    const char* name = strrchr(self, '/') ? strrchr(self, '/') + 1 : self;
    report_line(report, "ownership-fault: pc[0] host=", pc, sizeof pc);
    if (pc[0] && !strstr(pc, name)) fprintf(stderr, "busy child: the faulting code named is not %s's: %s\n", name, pc);
    CHECK(!pc[0] || strstr(pc, name));
    report_line(report, "busy access=", given, sizeof given);
    report_line(report, "access-address=", told, sizeof told);
    CHECK(given[0]);
    if (told[0]) {
        unsigned long long a = 0, b = 0;
        CHECK(sscanf(given, "busy access=%llx", &a) == 1 && sscanf(told, "access-address=%llx", &b) == 1);
        if (a != b) fprintf(stderr, "busy child: access-address=%llx, the access was at %llx\n", b, a);
        CHECK(a == b);
    }
}
#endif

/* a landing lets its pages go before it releases the resources: the trap thread lands this one (the main thread's
   read faults), and its release reads the landed page, which would fault on the trap thread itself, with nothing to
   answer it, if the page were still armed */
static void test_release_order(void) {
    u64 a = region(R0 + 0x700000, 0x2000);
    Res* r = res_new(0x1000, 0x50);
    CHECK(own_begin(a, 0x1000, r, 0) == 0);
    g_release_touch = a + 7;
    CHECK(GP(a)[7] == 0x57 && refs(r) == 0 && g_release_read == 0x57);
    g_release_touch = 0;
    CHECK(mem_unmap(a, 0x2000) == 0);
    res_free(r);
}

/* A landing lands what is on its pages once. Its let_go wakes the reader, which may publish over the page again
   before the trap's thread is done with the fault (a VIC job over the picture just read): that owner stays until
   something touches its bytes. The lander is held after it lets go of the producer's lock until the new owner is in;
   a read of another owned page then faults after the first fault is done (the trap's thread takes them in order), and
   lands that page alone */
static void test_republish_after_landing(void) {
    u64 a = region(R0 + 0xC00000, 0x4000);
    Res *r1 = res_new(0x1000, 0x20), *r2 = res_new(0x1000, 0x80), *r3 = res_new(0x1000, 0xC0);
    OwnRange o;
    int lands = g_lands;
    CHECK(own_begin(a, 0x1000, r1, 0) == 0 && own_begin(a + 0x2000, 0x1000, r3, 0) == 0);
    __atomic_store_n(&g_republished, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hold_res, r1, __ATOMIC_RELEASE);
    CHECK(GP(a)[5] == 0x25); /* r1 lands on the trap's thread, which then waits in producer_unlock */
    producer_lock();
    CHECK(own_begin(a, 0x1000, r2, 0) == 0); /* its caller holds the producer's lock, as the renderer's do */
    producer_unlock();
    __atomic_store_n(&g_republished, 1, __ATOMIC_RELEASE);
    CHECK(GP(a + 0x2000)[7] == 0xC7);
    CHECK(g_lands == lands + 2 && refs(r1) == 0 && refs(r3) == 0);
    /* r1's lander was the one held, and has let go */
    CHECK(!__atomic_load_n(&g_hold_res, __ATOMIC_ACQUIRE) && !__atomic_load_n(&g_hold_tid, __ATOMIC_ACQUIRE));
    CHECK(own_lookup(a, 0x1000, &o, 1) == 1 && o.res == r2 && refs(r2) == 1); /* nothing touched r2's bytes */
    CHECK(GP(a)[9] == (u8)(0x80 + 9) && g_lands == lands + 3 && refs(r2) == 0);
    CHECK(mem_unmap(a, 0x4000) == 0);
    res_free(r1);
    res_free(r2);
    res_free(r3);
}

/* the producer's lock holder faulting on a page that is armed with no entry on it (a landing's, cut and not let go
   yet) goes on: nothing has to land there */
static void test_busy_unowned(void) {
    u64 a = region(R0 + 0x800000, 0x2000);
    Res* r = res_new(0x1000, 0x60);
    CHECK(own_begin(a, 0x1000, r, 0) == 0);
    own_drop(a, 0x1000); /* the pool keeps the CPU's bytes, and the page is let go */
    CHECK(refs(r) == 0 && plat_trap_arm(GP(a), 0x1000) == 0);
    g_busy_tid = plat_tid();
    CHECK(GP(a)[9] == 0x1A); /* the CPU's byte, 0x11 + 9 */
    g_busy_tid = 0;
    plat_trap_disarm(GP(a), 0x1000);
    CHECK(mem_unmap(a, 0x2000) == 0);
    res_free(r);
}

/* the write watch's scan over memory with an owned span in it: the span, in minor mode for its trap, is skipped and
   the scan goes on, reporting the pages written around it. A failed scan turns write tracking off for the run */
static u64 g_scan_lo, g_scan_hi;
static void on_scan(u8* p, u64 size) {
    u64 lo = (u64)(p - g_mem), hi = lo + size;
    if (!g_scan_lo || lo < g_scan_lo) g_scan_lo = lo;
    if (hi > g_scan_hi) g_scan_hi = hi;
}
static void test_scan_owned(void) {
    u64 a = region(R0 + 0xA00000, 0x8000);
    Res* r = res_new(0x2000, 0x40);
    if (plat_ww_scan(GP(a), 0x8000, on_scan)) { /* no write tracking here (write_tracking=2, or before Linux 6.7) */
        CHECK(mem_unmap(a, 0x8000) == 0);
        res_free(r);
        return;
    }
    CHECK(own_begin(a + 0x2000, 0x2000, r, 0) == 0);
    GP(a)[0x6010] = 0x99;
    g_scan_lo = g_scan_hi = 0;
    CHECK(plat_ww_scan(GP(a), 0x8000, on_scan) == 0);
    CHECK(g_scan_lo == a + 0x6000 && g_scan_hi == a + 0x7000); /* the written page, and nothing in the span */
    own_acquire(a + 0x2000, 0x2000);
    CHECK(refs(r) == 0 && GP(a)[0x2000] == 0x40);
    CHECK(mem_unmap(a, 0x8000) == 0);
    res_free(r);
}

/* a page a trap lets go reads as written to the next scan, with the write of the thread it wakes, though the trap
   isn't disarmed yet (a landing disarms right after it lets go; own_fault lets a page with no entry on it go, one cut
   and not let go yet, and its let_go disarms it later). A scan that skipped the page after the write would stand for
   its write epoch, and a cache's check in that epoch would take memory without the write */
static void test_let_go_watched(void) {
    u64 a = region(R0 + 0xB00000, 0x2000);
    Res* r = res_new(0x1000, 0x70);
    if (plat_ww_scan(GP(a), 0x2000, on_scan)) { /* no write tracking here */
        CHECK(mem_unmap(a, 0x2000) == 0);
        res_free(r);
        return;
    }
    CHECK(own_begin(a, 0x1000, r, 0) == 0);
    own_drop(a, 0x1000); /* the page let go, in the pool */
    CHECK(refs(r) == 0 && plat_ww_scan(GP(a), 0x2000, on_scan) == 0);
    CHECK(plat_trap_arm(GP(a), 0x1000) == 0);
    GP(a)[0x10] = 0x39; /* faults: no entry on it, so the trap's thread lets it go and the write goes on */
    g_scan_lo = g_scan_hi = 0;
    CHECK(plat_ww_scan(GP(a), 0x2000, on_scan) == 0);
    if (g_scan_lo != a || g_scan_hi != a + 0x1000)
        fprintf(stderr, "let go: the scan after the write reported [%llx, %llx), not the written page at %llx\n",
                (unsigned long long)g_scan_lo, (unsigned long long)g_scan_hi, (unsigned long long)a);
    CHECK(g_scan_lo == a && g_scan_hi == a + 0x1000);
    plat_trap_disarm(GP(a), 0x1000);
    CHECK(GP(a)[0x10] == 0x39);
    CHECK(mem_unmap(a, 0x2000) == 0);
    res_free(r);
}

/* a watch: trapped pages that no resource holds. own_lookup skips it, own_stage reads its CPU bytes (through the
   mirror, or in place the shadows that own_watch took), own_trapped and own_hash see it. The first access asks the
   producer once (resolve), and lands what it published there. own_drop and own_unwatch open its pages with no resolve,
   and a thread that holds the producer's lock drops it with no resolve and goes on. A watch never cuts an owner */
static void test_watch(void) {
    u64 a = region(R0 + 0xC00000, 0x10000), h = 0;
    Res* r = res_new(0x2000, 0xA0);
    OwnRange o[4];
    u8 buf[0x4000];
    int resolves = g_resolves, lands = g_lands;
    CHECK(own_watch(a + 0x1100, 0x2000) == 0);
    CHECK(own_lookup(a, 0x10000, o, 4) == 0 && own_any());
    CHECK(own_trapped(a + 0x1000, 1) && own_trapped(a + 0x30FF, 1) && !own_trapped(a + 0x4000, 0x1000));
    CHECK(own_watched(a + 0x1100, 0x2000) && !own_watched(a + 0x1000, 0x2100) && !own_watched(a + 0x8000, 4));
    CHECK(own_hash(a, 0x10000, &h) == 1 && h != 0);
    CHECK(own_stage(a, sizeof buf, buf, o, 4) == 0);
    for (u32 i = 0; i < sizeof buf; i++) CHECK(buf[i] == (u8)(0x11 + i));
    CHECK(g_resolves == resolves);
    /* the read faults: the producer publishes r over the watch, once, and the read lands it */
    g_resolve_res = r;
    g_resolve_lo = a + 0x1100;
    g_resolve_size = 0x2000;
    CHECK(GP(a)[0x1200] == (u8)(0xA0 + 0x100));
    CHECK(refs(r) == 0 && g_resolves == resolves + 1 && g_lands == lands + 1);
    CHECK(GP(a)[0x10FF] == (u8)(0x11 + 0x10FF) && GP(a)[0x30FF] == (u8)(0xA0 + 0x1FFF) && !own_trapped(a, 0x10000));
    g_resolve_res = NULL;
    /* dropped: the pages open, with no resolve */
    CHECK(own_watch(a + 0x5000, 0x800) == 0);
    own_drop(a + 0x5000, 0x800);
    CHECK(GP(a)[0x5010] == (u8)(0x11 + 0x5010) && !own_trapped(a + 0x5000, 0x800) && g_resolves == resolves + 1);
    /* a part let go: its page opens, the rest stays watched until an access, whose resolve publishes nothing */
    CHECK(own_watch(a + 0x6000, 0x2000) == 0);
    own_unwatch(a + 0x6000, 0x1000);
    CHECK(!own_watched(a + 0x6000, 0x1000) && own_watched(a + 0x7000, 0x1000) && !own_trapped(a + 0x6000, 0x1000));
    CHECK(GP(a)[0x6010] == (u8)(0x11 + 0x6010) && g_resolves == resolves + 1);
    CHECK(GP(a)[0x7010] == (u8)(0x11 + 0x7010));
    refs(r); /* the landing's thread done: it holds the producer's lock until then */
    CHECK(g_resolves == resolves + 2 && !own_trapped(a + 0x7000, 0x1000));
    /* the producer's lock holder: its own_acquire and its own read drop the watch, with no resolve and no fatal */
    CHECK(own_watch(a + 0x9000, 0x1000) == 0);
    g_busy_tid = plat_tid();
    own_acquire(a + 0x9000, 0x10);
    CHECK(!own_trapped(a + 0x9000, 0x1000));
    CHECK(own_watch(a + 0x9000, 0x1000) == 0);
    CHECK(GP(a)[0x9010] == (u8)(0x11 + 0x9010));
    g_busy_tid = 0;
    CHECK(!own_trapped(a + 0x9000, 0x1000) && g_resolves == resolves + 2);
    /* a watch and an owner on one page: the watch goes into the gap before the owner, and leaves it whole. A read of
       the owner's page answers the watch first (whole, its other page too), then lands the owner */
    CHECK(own_begin(a + 0xB400, 0x800, r, 0) == 0);
    CHECK(own_watch(a + 0xA800, 0x1400) == 0);
    CHECK(own_lookup(a + 0xA000, 0x2000, o, 4) == 1 && o[0].addr == a + 0xB400 && o[0].size == 0x800);
    CHECK(own_watched(a + 0xA800, 0xC00) && !own_watched(a + 0xA800, 0xD00) && !own_watched(a + 0xBC00, 0x200));
    CHECK(GP(a)[0xB500] == (u8)(0xA0 + 0x100));
    {
        int left = refs(r), asked = g_resolves - resolves, trapped = own_trapped(a + 0xA000, 0x2000);
        if (left || asked != 3 || trapped)
            fprintf(stderr, "watch beside an owner: %d references left, %d resolves, pages trapped %d (A %d, B %d)\n",
                    left, asked, trapped, own_trapped(a + 0xA000, 0x1000), own_trapped(a + 0xB000, 0x1000));
        CHECK(left == 0);
        CHECK(asked == 3);
        CHECK(!trapped);
    }
    CHECK(GP(a)[0xA900] == (u8)(0x11 + 0xA900) && GP(a)[0xBD00] == (u8)(0x11 + 0xBD00));
    /* a later owner over a watch replaces it; mem_unmap drops what is left */
    CHECK(own_watch(a + 0xD000, 0x2000) == 0 && own_begin(a + 0xD800, 0x800, r, 0) == 0);
    CHECK(own_lookup(a + 0xD000, 0x2000, o, 4) == 1 && !own_watched(a + 0xD000, 0x2000));
    CHECK(own_watched(a + 0xD000, 0x800) && own_watched(a + 0xE000, 0x1000));
    CHECK(mem_unmap(a, 0x10000) == 0);
    CHECK(refs(r) == 0 && g_resolves == resolves + 3);
    res_free(r);
}

/* watches past the end of a lookup, and one across its end, with an owner after them: the walk over watches stops at
   the end of the range (next_real_nl), and the results are those of a walk over every entry. A lookup or a stage that
   ends before the owner finds no owner; one that reaches it finds it, after the watches before it */
static void test_watch_bound(void) {
    u64 a = region(R0 + 0xF00000, 0x40000);
    Res* r = res_new(0x1000, 0x50);
    OwnRange o[4];
    u8 buf[0x8000];
    CHECK(own_watch(a + 0x7C00, 0x800) == 0); /* across the end of [a, a + 0x8000) */
    for (u64 k = 0; k < 32; k++) CHECK(own_watch(a + 0x10000 + k * 0x1000, 0x800) == 0);
    CHECK(own_begin(a + 0x38000, 0x1000, r, 0) == 0);
    CHECK(own_lookup(a, 0x8000, o, 4) == 0);
    CHECK(own_lookup(a + 0x10000, 0x20000, o, 4) == 0);
    CHECK(own_lookup(a, 0x40000, o, 4) == 1 && o[0].addr == a + 0x38000 && o[0].size == 0x1000 && o[0].res == r &&
          o[0].res_off == 0);
    CHECK(own_lookup(a + 0x2F000, 0xA000, o, 4) == 1 && o[0].addr == a + 0x38000);
    CHECK(own_lookup(a + 0x38800, 0x10, o, 4) == 1 && o[0].addr == a + 0x38800 && o[0].res_off == 0x800);
    /* a stage of watched pages reads the CPU's bytes; a stage over the owner leaves the owner's part to the caller */
    CHECK(own_stage(a, 0x8000, buf, o, 4) == 0);
    for (u32 i = 0; i < 0x8000; i++) CHECK(buf[i] == (u8)(0x11 + i));
    CHECK(own_stage(a + 0x2F000, 0x8000, buf, o, 4) == 0);
    for (u32 i = 0; i < 0x8000; i++) CHECK(buf[i] == (u8)(0x11 + 0x2F000 + i));
    CHECK(own_stage(a + 0x37000, 0x3000, buf, o, 4) == 1 && o[0].addr == a + 0x38000 && o[0].size == 0x1000);
    for (u32 i = 0; i < 0x1000; i++)
        CHECK(buf[i] == (u8)(0x11 + 0x37000 + i) && buf[0x2000 + i] == (u8)(0x11 + 0x39000 + i));
    CHECK(mem_unmap(a, 0x40000) == 0);
    CHECK(refs(r) == 0);
    res_free(r);
}

/* a watch of many pages (more than the first slab of shadows holds, in place), with an owner's page inside it: the
   watch goes around the owner, each of its pages is staged with the CPU's bytes, and the owner's with its own. Let go
   and watched again after the CPU wrote, the pages are staged with the new bytes (a slot let go and taken again) */
static void test_watch_many(void) {
    const u64 size = 0x60000;
    u64 a = region(R0 + 0xD00000, size);
    Res* r = res_new(0x1000, 0x30);
    OwnRange o[4];
    u8* buf = (u8*)malloc((size_t)size);
    CHECK(buf && own_begin(a + 0x20000, 0x1000, r, 0) == 0);
    CHECK(own_watch(a, size) == 0);
    CHECK(own_watched(a, 0x20000) && own_watched(a + 0x21000, size - 0x21000) && !own_watched(a, size));
    CHECK(own_stage(a, size, buf, o, 4) == 1 && o[0].addr == a + 0x20000 && o[0].size == 0x1000);
    for (u64 i = 0; i < size; i++)
        if (i < 0x20000 || i >= 0x21000) CHECK(buf[i] == (u8)(0x11 + i));
    own_unwatch(a, size);
    own_acquire(a + 0x20000, 0x1000);
    CHECK(refs(r) == 0 && !own_trapped(a, size));
    for (u64 i = 0; i < size; i += 0x1000) GP(a)[i + 7] = (u8)(0xC0 + (i >> 12));
    CHECK(own_watch(a, size) == 0 && own_stage(a, size, buf, o, 4) == 0);
    for (u64 i = 0; i < size; i++) { /* the owner's page has its bytes, landed */
        u8 was = i >= 0x20000 && i < 0x21000 ? (u8)(0x30 + (i - 0x20000)) : (u8)(0x11 + i);
        CHECK(buf[i] == ((i & 0xFFF) == 7 ? (u8)(0xC0 + (i >> 12)) : was));
    }
    CHECK(mem_unmap(a, size) == 0);
    CHECK(!own_trapped(a, size));
    free(buf);
    res_free(r);
}

/* many threads fault on many owned pages at once while the owners change between rounds. In place (no mirror:
   Windows) a page is open while a landing fills it, so a thread that reaches it then without faulting (a reader
   started late, its first read on a page already filling) can read its old bytes: those reads are noted, and each
   such byte must be the owner's once the landing is done. Through a mirror no read may */
#define THREADS   8
#define STALE_MAX 4096
static u64 g_sa;
static int g_round;
static u64 g_stale[STALE_MAX]; /* the offsets read stale, in place */
static int g_nstale;           /* atomic */
static int g_stale_total;
static void* reader(void* arg) {
    u32 seed = (u32)(uintptr_t)arg * 2654435761u + (u32)g_round;
    int i;
    for (i = 0; i < 2000; i++) {
        u64 off;
        seed = seed * 1103515245u + 12345u;
        off = (seed >> 4) % 0x20000;
        if (GP(g_sa)[off] == (u8)(g_round * 7 + off)) continue;
        CHECK(!plat_alias_mirror()); /* a mirror's landing is never seen half written */
        int k = __atomic_fetch_add(&g_nstale, 1, __ATOMIC_RELAXED);
        if (k < STALE_MAX) g_stale[k] = off;
    }
    return NULL;
}
/* Readers that start while the publish a fault asked for arms the watched pages: each waits for the resolve, then a
   spin longer than the one before, so that some reach a page while own_begin arms it. A page armed already must stay
   armed through it: userfaultfd arms a page again by letting it go first (plat_trap_arm unregisters it before it
   registers it), and a reader there maps the page with the bytes it had, the round before's. Each round's publish is
   ready before its watch: a fault of the round before's readers can come late, and with none it would drop the watch */
#define REARM_READERS 16
static u64 g_rearm_at;
static int g_rearm_round, g_rearm_seen, g_rearm_stale; /* g_rearm_stale atomic */
static void* rearm_reader(void* arg) {
    u32 k = (u32)(uintptr_t)arg;
    while (__atomic_load_n(&g_resolves, __ATOMIC_ACQUIRE) == g_rearm_seen) {}
    for (volatile u32 spin = 0; spin < k * 400; spin++) {}
    for (u32 page = k % 4; page < 32; page += 4) {
        u64 off = page * 0x1000 + 0x80 + k;
        /* in place a page is open while the landing fills it, and its old byte can be read then (test_stress) */
        if (GP(g_rearm_at)[off] != (u8)(g_rearm_round * 5 + off) && plat_alias_mirror())
            __atomic_add_fetch(&g_rearm_stale, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
static void test_rearm(void) {
    pthread_t t[REARM_READERS];
    g_rearm_at = region(R0 + 0xE00000, 0x20000);
    for (g_rearm_round = 1; g_rearm_round <= 40; g_rearm_round++) {
        Res* r = res_new(0x20000, (u8)(g_rearm_round * 5));
        g_resolve_res = r;
        g_resolve_lo = g_rearm_at;
        g_resolve_size = 0x20000;
        g_rearm_seen = __atomic_load_n(&g_resolves, __ATOMIC_ACQUIRE);
        CHECK(own_watch(g_rearm_at, 0x20000) == 0);
        for (u32 i = 0; i < REARM_READERS; i++) pthread_create(&t[i], NULL, rearm_reader, (void*)(uintptr_t)i);
        CHECK(GP(g_rearm_at)[0x7F] == (u8)(g_rearm_round * 5 + 0x7F)); /* the fault that asks for the publish */
        for (u32 i = 0; i < REARM_READERS; i++) pthread_join(t[i], NULL);
        g_resolve_res = NULL;
        CHECK(refs(r) == 0 && !own_trapped(g_rearm_at, 0x20000));
        res_free(r);
    }
    if (g_rearm_stale) fprintf(stderr, "rearm: %d reads of the round before's bytes in 40 rounds\n", g_rearm_stale);
    CHECK(g_rearm_stale == 0);
    CHECK(mem_unmap(g_rearm_at, 0x20000) == 0);
}
static void test_stress(void) {
    pthread_t t[THREADS];
    u64 i;
    g_sa = region(R0 + 0x600000, 0x20000);
    for (g_round = 0; g_round < 20; g_round++) {
        Res* r = res_new(0x20000, (u8)(g_round * 7));
        /* in pieces, so pages hold several owners' bytes */
        for (i = 0; i < 0x20000; i += 0x1800) {
            u64 n = 0x20000 - i < 0x1800 ? 0x20000 - i : 0x1800;
            CHECK(own_begin(g_sa + i, n, r, i) == 0);
        }
        for (i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, reader, (void*)(uintptr_t)(i + 1));
        for (i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
        own_acquire(g_sa, 0x20000); /* what no reader touched, and the landing a reader may still be in */
        CHECK(refs(r) == 0);
        for (i = 0; i < (u64)g_nstale && i < STALE_MAX; i++)
            CHECK(GP(g_sa)[g_stale[i]] == (u8)(g_round * 7 + g_stale[i]));
        g_stale_total += g_nstale;
        g_nstale = 0;
        res_free(r);
    }
    /* the same over watches: the first fault publishes the round's owner over all of them, once */
    for (; g_round < 30; g_round++) {
        Res* r = res_new(0x20000, (u8)(g_round * 7));
        int resolves = g_resolves;
        /* the publish is ready before the watches: a fault of the round before's readers can come late, after them, and
           with none it would drop them */
        g_resolve_res = r;
        g_resolve_lo = g_sa;
        g_resolve_size = 0x20000;
        for (i = 0; i < 0x20000; i += 0x1800)
            CHECK(own_watch(g_sa + i, 0x20000 - i < 0x1800 ? 0x20000 - i : 0x1800) == 0);
        for (i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, reader, (void*)(uintptr_t)(i + 1));
        for (i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
        own_acquire(g_sa, 0x20000);
        g_resolve_res = NULL;
        CHECK(refs(r) == 0 && g_resolves == resolves + 1 && !own_trapped(g_sa, 0x20000));
        for (i = 0; i < (u64)g_nstale && i < STALE_MAX; i++)
            CHECK(GP(g_sa)[g_stale[i]] == (u8)(g_round * 7 + g_stale[i]));
        g_stale_total += g_nstale;
        g_nstale = 0;
        res_free(r);
    }
    CHECK(mem_unmap(g_sa, 0x20000) == 0);
    if (g_stale_total)
        printf("memory_own: %d of %d reads reached a page while a landing filled it in place, and read its old byte "
               "(each the owner's once landed)\n",
               g_stale_total, 30 * THREADS * 2000);
}

int main(int argc, char** argv) {
    OwnHooks h = {.land = land,
                  .retain = retain,
                  .release = release,
                  .busy_tid = busy_tid,
                  .lock = producer_lock,
                  .unlock = producer_unlock,
                  .resolve = resolve};
    pthread_mutexattr_t recursive;
    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_producer, &recursive);
    pthread_mutexattr_destroy(&recursive);
    for (int i = 1; i < argc; i++) {
        const char* eq = strchr(argv[i], '=');
        char key[64];
        if (!eq) continue; /* --busy and the others, below */
        CHECK(!strncmp(argv[i], "--", 2) && (size_t)(eq - argv[i] - 2) < sizeof key);
        CHECK(g_settings_count < (int)(sizeof g_settings_given / sizeof *g_settings_given));
        snprintf(key, sizeof key, "%.*s", (int)(eq - argv[i] - 2), argv[i] + 2);
        CHECK(ini_find(g_debug_ini, key));
        ini_read(ini_find(g_debug_ini, key), eq + 1);
        g_settings_given[g_settings_count++] = argv[i];
    }
#if !defined(_WIN32)
    if (sysconf(_SC_PAGESIZE) != 4096) {
        printf("skipped: this host's pages are %ld bytes, and the pool needs 4 KiB ones\n", sysconf(_SC_PAGESIZE));
        return 77;
    }
#endif
    mem_init();
    if (own_init(&h)) {
        /* none asked for, or none the protection can make without the pool: the log says which, with the value */
        if (g_settings.memory_traps == 2 ||
            (g_settings.memory_traps == 1 && !g_settings.alias_pool && !SNX_PLAT_TRAPS_IN_PLACE)) {
            CHECK(strstr(g_log, g_settings.memory_traps == 2
                                    ? "memory: no traps (memory_traps=2): GPU engines land"
                                    : "memory: no traps (memory_traps=1, page protection, has none on this host)"));
            printf("memory_own: memory_traps=%d runs without traps, and the log says so\n", g_settings.memory_traps);
            return 0;
        }
        printf("skipped: no traps on this host (userfaultfd minor faults on shared memory)\n");
        return 77;
    }
    CHECK(g_settings.memory_traps != 2);
    g_log_on = 0; /* threads log from here */
    if (argc > 1 && !strcmp(argv[1], "--busy")) return busy_child(0, 0);
    if (argc > 1 && !strcmp(argv[1], "--busy-write")) return busy_child(1, 0);
    if (argc > 1 && !strcmp(argv[1], "--busy-private")) return busy_child(0, 1);
    test_read_write();
    test_freeze();
    test_syscall();
    test_drop();
    test_drop_sparse();
    test_stage();
    test_stage_dropped();
    test_view();
#if defined(_WIN32)
    {
        static char self[MAX_PATH];
        CHECK(GetModuleFileNameA(NULL, self, sizeof self) > 0);
        test_busy(self, 0, 0);
        test_busy(self, 1, 0);
    }
#else
    { /* the system's name for this program: /proc/self/exe is Linux's, and FreeBSD mounts no /proc of itself */
        char* self = snx_executable(argv[0]);
        CHECK(self != NULL);
        test_busy(self, 0, 0);
        test_busy(self, 1, 0);
        test_busy(self, 0, 1);
        free(self);
    }
#endif
    test_release_order();
    test_republish_after_landing();
    test_busy_unowned();
    test_scan_owned();
    test_let_go_watched();
    test_watch();
    test_watch_bound();
    test_watch_many();
    test_rearm();
    test_stress();
    CHECK(!own_any());
    printf("memory_own: %u checks passed (%d landings, %d bytes)\n", checks, g_lands, g_landed_bytes);
    return 0;
}
