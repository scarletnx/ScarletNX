/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GPU-owned guest memory (memory/own.c) over the real alias pool and traps (platform/plat_posix.c: userfaultfd
   minor faults), with owners whose "GPU copy" is host memory, so no GPU is needed: a read and a write of an owned
   byte land the owner first, the CPU's bytes on a page it shares with an owner stay, a neighbour writing during the
   move into the pool loses nothing, a system call fails on owned memory until it is acquired, dropping and unmapping
   land nothing, a view of owned memory lands it first, a fault on the thread holding the producer's lock ends the
   process, the write watch's scan skips owned spans, and threads faulting at once on many pages while owners come
   and go read every owner's bytes. Skips (77) where the host has no traps */
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/plat.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>

static unsigned checks;
static int diagnostic_log;
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
    (void)level;
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
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
}
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
static void land(const OwnRange* r, u8* const* dst, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        const Res* res = (const Res*)r[i].res;
        memcpy(dst[i], res->data + r[i].res_off, r[i].size);
        __atomic_add_fetch(&g_landed_bytes, (int)r[i].size, __ATOMIC_RELAXED);
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
/* the producer's lock: recursive, as the renderer's */
static pthread_mutex_t g_producer;
static void producer_lock(void) { pthread_mutex_lock(&g_producer); }
static void producer_unlock(void) { pthread_mutex_unlock(&g_producer); }
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

/* a second mapping of owned memory lands it first: owned memory has one */
static void test_view(void) {
    u64 a = region(R0 + 0x400000, 0x4000), v = STACK_BASE + 0x100000;
    Res* r = res_new(0x1000, 0x90);
    int lands = g_lands;
    CHECK(own_begin(a + 0x1000, 0x1000, r, 0) == 0);
    CHECK(mem_alias_begin(a, 0x4000) == 0);
    CHECK(mem_alias_map(v, a, 0x4000, MS_STACK, PERM_RW) == 0);
    CHECK(g_lands == lands + 1 && refs(r) == 0 && GP(v)[0x1001] == 0x91 && GP(a)[0x1001] == 0x91);
    /* nor is a view's memory owned: own_begin refuses it, and the producer lands it itself */
    CHECK(own_begin(a + 0x1000, 0x1000, r, 0) == -1 && own_begin(v, 0x1000, r, 0) == -1);
    CHECK(mem_alias_unmap(v, 0x4000, 0) == 0);
    mem_alias_end(a, 0x4000);
    CHECK(mem_unmap(a, 0x4000) == 0);
    res_free(r);
}

/* a fault on the thread holding the producer's lock is the runtime's bug: it ends the process, not hangs. In a
   process of its own (this test run again: a forked one has no traps, which aren't inherited) */
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
    fprintf(stderr, "(an abort with the thread and address is expected next)\n");
    if (write) *(volatile u8*)GP(a) = 0x42;
    if (GP(a)[3] == 0x13) return 3; /* the fault must not be answered */
    return 4;
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
        if (dup2(fileno(output), STDERR_FILENO) < 0) _exit(6);
        execl(self, self, restricted ? "--busy-private" : write ? "--busy-write" : "--busy", (char*)NULL);
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
}

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
    if (plat_ww_scan(GP(a), 0x8000, on_scan)) { /* no write tracking here (SNX_NO_WW, or before Linux 6.7) */
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

/* many threads fault on many owned pages at once while the owners change between rounds */
#define THREADS 8
static u64 g_sa;
static int g_round;
static void* reader(void* arg) {
    u32 seed = (u32)(uintptr_t)arg * 2654435761u + (u32)g_round;
    int i;
    for (i = 0; i < 2000; i++) {
        u64 off;
        seed = seed * 1103515245u + 12345u;
        off = (seed >> 4) % 0x20000;
        CHECK(GP(g_sa)[off] == (u8)(g_round * 7 + off));
    }
    return NULL;
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
        own_acquire(g_sa, 0x20000); /* what no reader touched */
        CHECK(refs(r) == 0);
        res_free(r);
    }
    CHECK(mem_unmap(g_sa, 0x20000) == 0);
}

int main(int argc, char** argv) {
    OwnHooks h = {.land = land,
                  .retain = retain,
                  .release = release,
                  .busy_tid = busy_tid,
                  .lock = producer_lock,
                  .unlock = producer_unlock};
    pthread_mutexattr_t recursive;
    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_producer, &recursive);
    pthread_mutexattr_destroy(&recursive);
    if (sysconf(_SC_PAGESIZE) != 4096) {
        printf("skipped: this host's pages are %ld bytes, and the pool needs 4 KiB ones\n", sysconf(_SC_PAGESIZE));
        return 77;
    }
    mem_init();
    if (own_init(&h)) {
        printf("skipped: no traps on this host (userfaultfd minor faults on shared memory)\n");
        return 77;
    }
    if (argc > 1 && !strcmp(argv[1], "--busy")) return busy_child(0, 0);
    if (argc > 1 && !strcmp(argv[1], "--busy-write")) return busy_child(1, 0);
    if (argc > 1 && !strcmp(argv[1], "--busy-private")) return busy_child(0, 1);
    test_read_write();
    test_freeze();
    test_syscall();
    test_drop();
    test_stage();
    test_view();
    test_busy("/proc/self/exe", 0, 0);
    test_busy("/proc/self/exe", 1, 0);
    test_busy("/proc/self/exe", 0, 1);
    test_release_order();
    test_busy_unowned();
    test_scan_owned();
    test_stress();
    CHECK(!g_own_any);
    printf("memory_own: %u checks passed (%d landings, %d bytes)\n", checks, g_lands, g_landed_bytes);
    return 0;
}
