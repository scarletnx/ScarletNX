/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The POSIX platform layer (platform/plat_posix.c, with plat_sdl.c): written-page tracking, decommit zeroing, pread,
   file maps, replace, folders, timed waits and threads. Written-page tracking needs Linux 6.7 for userfaultfd's;
   write_tracking=1 (and every host without it) tracks by protection, and write_tracking=2 must make the scan say -1.
   The arguments: the folder for its files, then Debug settings as the runtime takes them, --key=value */
#include "nx/runtime/platform/plat_posix.c"
#include "nx/runtime/ini.h"
#include <stdarg.h>
#include <stdio.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/prctl.h> /* PR_SET_VMA_ANON_NAME, for test_foreign */
#endif

static char g_log[1024]; /* the log's lines while g_log_on, as far as they fit */
static int g_log_on;
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    size_t n = g_log_on ? strlen(g_log) : 0;
    (void)lvl;
    if (g_log_on) {
        va_start(ap, fmt);
        vsnprintf(g_log + n, sizeof g_log - n, fmt, ap);
        va_end(ap);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
void log_crash_mode(void) {}
/* nce.c, whose NCE no game here is of: the crash handler gives it the signal's context */
void nce_crash_context(const void* context, const void* fault) {
    (void)context;
    (void)fault;
}
static int g_reports;     /* crash reports made */
static int g_report_exit; /* a child's: a report ends it with 42, which its parent checks */
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
    g_reports++;
    if (g_report_exit) _exit(42);
}

static unsigned checks, failures;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

static u8* g_base;
static u64 g_written; /* bit n: page n reported written */
static void on_written(u8* p, u64 size) {
    u64 a;
    for (a = (u64)(p - g_base) >> 12; a < (u64)(p - g_base + size) >> 12; a++)
        if (a < 64) g_written |= 1ull << a;
}
static u64 scan(void) {
    g_written = 0;
    return plat_ww_scan(g_base, 64 * 4096, on_written) ? ~0ull : g_written;
}

/* ---- written-page tracking by protection (SNX_WW=signal, or a host without Linux's): mapping changes, the freeze,
   and writers on other threads racing the scans */
static u64 g_seen; /* bit n: page n reported by a scan since the last check */
static void on_seen(u8* p, u64 size) {
    u64 a;
    for (a = (u64)(p - g_base) >> 12; a < (u64)(p - g_base + size) >> 12; a++)
        if (a < 64) __atomic_fetch_or(&g_seen, 1ull << a, __ATOMIC_RELAXED);
}
#define WRITERS 4
static int g_writers_run = 1, g_writers_stop, g_writer_idle[WRITERS];
static u64 g_writes;
static void* writer(void* arg) {
    int id = (int)(intptr_t)arg;
    u32 x = 0x9e3779b9u * (u32)(id + 1);
    while (!__atomic_load_n(&g_writers_stop, __ATOMIC_SEQ_CST)) {
        if (!__atomic_load_n(&g_writers_run, __ATOMIC_SEQ_CST)) {
            __atomic_store_n(&g_writer_idle[id], 1, __ATOMIC_SEQ_CST);
            while (!__atomic_load_n(&g_writers_run, __ATOMIC_SEQ_CST) &&
                   !__atomic_load_n(&g_writers_stop, __ATOMIC_SEQ_CST))
                sched_yield();
            __atomic_store_n(&g_writer_idle[id], 0, __ATOMIC_SEQ_CST);
            continue;
        }
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        /* pages 32 to 63, each writer its own bytes of them, and one counter they all add to: the threads fault on
           its page at once, again and again, and only one of them opens it each time */
        __atomic_fetch_add(&g_base[(32 + (x & 31)) * 4096 + ((x >> 5) & 1023) * 4 + (u32)id], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add((u32*)(void*)&g_base[33 * 4096 + 4092], 1, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&g_writes, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_writer_idle[id], 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void writers_pause(void) {
    int i;
    __atomic_store_n(&g_writers_run, 0, __ATOMIC_SEQ_CST);
    for (i = 0; i < WRITERS; i++)
        while (!__atomic_load_n(&g_writer_idle[i], __ATOMIC_SEQ_CST)) sched_yield();
}
/* system calls that write guest memory while another thread scans it: the pages plat_ww_open opened stay writable
   until plat_ww_close, so the kernel's write never meets a page a scan protected again (EFAULT, a short read) */
static int g_kernel_fd = -1, g_kernel_stop;
static u64 g_kernel_reads, g_kernel_failed;
static void* kernel_reader(void* arg) {
    (void)arg;
    while (!__atomic_load_n(&g_kernel_stop, __ATOMIC_SEQ_CST)) {
        u8* at = g_base + 16 * 4096 + (g_kernel_reads % 8) * 4096 + 4000; /* over two pages */
        plat_ww_open(at, 1000);
        if (pread(g_kernel_fd, at, 1000, 0) != 1000) g_kernel_failed++;
        plat_ww_close(at, 1000);
        g_kernel_reads++;
    }
    return NULL;
}
static void test_kernel_writes(const char* dir) {
    static u8 data[1000];
    char path[1200];
    pthread_t t;
    u64 scans = 0;
    printf("written-page tracking by protection: the kernel's writes against scans\n");
    snprintf(path, sizeof path, "%s/platform_posix.kernel", dir);
    g_kernel_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(g_kernel_fd >= 0 && write(g_kernel_fd, data, sizeof data) == (ssize_t)sizeof data, "no file to read");
    unlink(path);
    CHECK(!pthread_create(&t, NULL, kernel_reader, NULL), "no reader");
    while (__atomic_load_n(&g_kernel_reads, __ATOMIC_RELAXED) < 20000) {
        plat_ww_scan(g_base + 16 * 4096, 16 * 4096, on_seen);
        scans++;
    }
    __atomic_store_n(&g_kernel_stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(t, NULL);
    close(g_kernel_fd);
    CHECK(g_kernel_failed == 0, "a read into opened guest memory failed");
    printf("  %llu reads, %llu failed, %llu scans\n", (unsigned long long)g_kernel_reads,
           (unsigned long long)g_kernel_failed, (unsigned long long)scans);
}
/* a system call into memory being moved (plat_freeze) waits for the move: opened before it, the kernel would write
   the old pages, which the move has copied already */
static int g_open_done;
static void* frozen_opener(void* arg) {
    (void)arg;
    plat_ww_open(g_base + 12 * 4096, 100);
    __atomic_store_n(&g_open_done, 1, __ATOMIC_SEQ_CST);
    g_base[12 * 4096] = 7; /* as the kernel's write would */
    plat_ww_close(g_base + 12 * 4096, 100);
    return NULL;
}
static void test_open_frozen(void) {
    pthread_t t;
    printf("written-page tracking by protection: a system call into memory being moved\n");
    scan();
    plat_freeze(g_base + 12 * 4096, 2 * 4096);
    CHECK(!pthread_create(&t, NULL, frozen_opener, NULL), "no opener");
    plat_sleep_ns(50000000);
    CHECK(!__atomic_load_n(&g_open_done, __ATOMIC_SEQ_CST), "plat_ww_open did not wait for the thaw");
    plat_thaw();
    pthread_join(t, NULL);
    CHECK(g_open_done && g_base[12 * 4096] == 7, "the write after the thaw did not land");
    CHECK(scan() == 3ull << 12, "the thawed and written pages were not reported");
}
/* a protection change that fails for want of mappings lets every held page go (wp_shed): writable without a fault,
   written for the next scan, and the budget halved */
static void test_shed(void) {
    u64 budget = __atomic_load_n(&g_wp_budget, __ATOMIC_RELAXED);
    printf("written-page tracking by protection: letting every page go\n");
    scan();
    g_base[0] = 1;
    g_base[2 * 4096] = 1;
    scan();
    CHECK(__atomic_load_n(&g_wp_held, __ATOMIC_RELAXED) >= 64, "a scan held no pages");
    wp_shed();
    CHECK(__atomic_load_n(&g_wp_held, __ATOMIC_RELAXED) == 0, "pages still held after the shed");
    CHECK(scan() == ~0ull >> (64 - 64), "the pages let go were not reported written");
    __atomic_store_n(&g_wp_budget, budget, __ATOMIC_RELAXED); /* as it was, for what follows */
}
static void test_protection_tracking(void) {
    static u8 before[32 * 4096];
    pthread_t t[WRITERS];
    u64 missed = 0, reported = 0, scans = 0, iter, k;
    int i;
    printf("written-page tracking by protection\n");
    scan();
    /* a page written, decommitted and committed again reads as written, and takes writes */
    g_base[4 * 4096] = 1;
    plat_vm_decommit(g_base + 4 * 4096, 2 * 4096);
    CHECK(!plat_vm_commit(g_base + 4 * 4096, 2 * 4096), "commit failed");
    CHECK(scan() == (3ull << 4), "pages committed again were not reported");
    g_base[5 * 4096] = 1;
    CHECK(scan() == 1ull << 5, "a write after the commit was not reported");
    /* a frozen range reads as written after the thaw, and a write waits for it */
    plat_freeze(g_base + 8 * 4096, 2 * 4096);
    CHECK(g_base[8 * 4096] == 0, "a frozen page could not be read");
    plat_thaw();
    CHECK(scan() == (3ull << 8), "a thawed range was not reported");
    g_base[9 * 4096] = 1;
    CHECK(scan() == 1ull << 9, "a write after the thaw was not reported");
    /* writers race scans: every page whose bytes changed between two checks was reported by a scan in between */
    scan();
    memcpy(before, g_base + 32 * 4096, sizeof before);
    g_seen = 0;
    for (i = 0; i < WRITERS; i++) CHECK(!pthread_create(&t[i], NULL, writer, (void*)(intptr_t)i), "no writer");
    for (iter = 0; iter < 200; iter++) {
        u64 from = __atomic_load_n(&g_writes, __ATOMIC_RELAXED);
        /* scans while the writers write some thousands of times */
        while (__atomic_load_n(&g_writes, __ATOMIC_RELAXED) - from < 4000) {
            u64 had = g_seen;
            g_seen = 0;
            plat_ww_scan(g_base + 32 * 4096, 32 * 4096, on_seen);
            reported += snx_popcount64(g_seen); /* each a page the scan protected, written again */
            g_seen |= had;
            scans++;
        }
        writers_pause();
        plat_ww_scan(g_base + 32 * 4096, 32 * 4096, on_seen);
        for (k = 0; k < 32; k++)
            if (memcmp(before + k * 4096, g_base + (32 + k) * 4096, 4096) && !(g_seen >> (32 + k) & 1)) missed++;
        memcpy(before, g_base + 32 * 4096, sizeof before);
        g_seen = 0;
        __atomic_store_n(&g_writers_run, 1, __ATOMIC_SEQ_CST);
        for (i = 0; i < WRITERS; i++) /* every writer going again before the next check */
            while (__atomic_load_n(&g_writer_idle[i], __ATOMIC_SEQ_CST)) sched_yield();
    }
    __atomic_store_n(&g_writers_stop, 1, __ATOMIC_SEQ_CST);
    for (i = 0; i < WRITERS; i++) pthread_join(t[i], NULL);
    CHECK(missed == 0, "a scan missed a page another thread wrote");
    CHECK(__atomic_load_n(&g_wp_held, __ATOMIC_RELAXED) <= g_wp_pages, "the held pages' count went wrong");
    CHECK(reported > 32 * iter, "the writers' pages were not reported again and again");
    printf("  %llu writes, %llu scans reporting %llu pages, %llu checks: %llu pages missed\n",
           (unsigned long long)g_writes, (unsigned long long)scans, (unsigned long long)reported,
           (unsigned long long)iter, (unsigned long long)missed);
}

static int g_ran; /* the sum of what the threads read: both must finish before main returns, since they read its arg */
static void thread_fn(void* arg) { __atomic_fetch_add(&g_ran, *(int*)arg, __ATOMIC_RELEASE); }

/* written-page tracking and decommit, in a reservation of guest memory */
static void test_guest_memory(const char* dir) {
    char path[1200];
    FILE* f;
    int fd;

    printf("written-page tracking\n");
    g_base = plat_vm_reserve((u64)1 << 30);
    CHECK(g_base != NULL, "no reservation");
    CHECK(!((uintptr_t)g_base & (SNX_PLAT_PAGE_MAX - 1)), "the reservation is not aligned to SNX_PLAT_PAGE_MAX");
    plat_vm_track(g_base, (u64)1 << 30);
    CHECK(!plat_vm_commit(g_base, 64 * 4096), "commit failed");
    if (g_settings.write_tracking == 2 || plat_ww_scan(g_base, 4096, on_written) < 0) {
        printf("written-page tracking unavailable here: only checking that it says so\n");
        CHECK(scan() == ~0ull, "a scan without tracking did not fail");
    } else {
        scan();
        CHECK(scan() == 0, "pages reported written twice");
        g_base[3 * 4096] = 1;
        g_base[10 * 4096 + 77] = 2;
        CHECK(scan() == (1ull << 3 | 1ull << 10), "pages 3 and 10 not reported alone");
        CHECK(scan() == 0, "a scan did not protect its pages again");
        (void)g_base[20 * 4096]; /* a read is no write */
        g_base[5 * 4096] = 3;
        CHECK(scan() == 1ull << 5, "page 5 not reported alone after a read elsewhere");
        /* the kernel writing guest memory (read() into it) counts too, once the runtime opened it for the kernel */
        snprintf(path, sizeof path, "%s/platform_posix.bin", dir);
        f = fopen(path, "wb");
        fwrite("0123456789", 1, 10, f);
        fclose(f);
        fd = open(path, O_RDONLY);
        if (g_wp) /* the protected page fails the system call: it doesn't fault into the handler */
            CHECK(read(fd, g_base + 7 * 4096, 10) == -1 && errno == EFAULT, "read() into a protected page worked");
        plat_ww_open(g_base + 7 * 4096, 10);
        CHECK(pread(fd, g_base + 7 * 4096, 10, 0) == 10, "read into guest memory failed");
        plat_ww_close(g_base + 7 * 4096, 10);
        close(fd);
        CHECK(scan() == 1ull << 7, "a read() into page 7 was not reported");
        if (g_wp) {
            test_protection_tracking();
            test_kernel_writes(dir);
            test_open_frozen();
            test_shed();
        }
    }
    printf("decommit\n");
    g_base[9 * 4096] = 9;
    plat_vm_decommit(g_base, 64 * 4096);
    CHECK(!plat_vm_commit(g_base, 64 * 4096) && g_base[9 * 4096] == 0, "memory not zero after decommit");

    printf("a GPU's buffer\n");
    {
        void* handle;
#if defined(__linux__) && !defined(__ANDROID__) /* a memfd, and its pages mapped: what a driver imports here */
        u8* g = g_base + (1u << 20);
        CHECK(!plat_gpu_buffer(2u << 20, &fd, &handle) && fd >= 0 && handle, "no memfd buffer");
        u8* v = (u8*)handle;
        CHECK(lseek(fd, 0, SEEK_END) == 2 << 20, "the buffer isn't 2 MiB");
        CHECK(!plat_vm_commit(g, 2u << 20), "commit failed");
        g[12345] = 0x5A;
        CHECK(!plat_vm_share(g, fd, 2u << 20) && v[12345] == 0x5A, "guest memory did not move into it with its bytes");
        g[777] = 0x11;
        v[778] = 0x22;
        CHECK(v[777] == 0x11 && g[778] == 0x22, "guest memory moved into it and its mapping aren't one memory");
        CHECK(!plat_vm_unshare(g, 2u << 20) && g[12345] == 0x5A && g[778] == 0x22, "guest memory did not move back");
        g[999] = 0x33;
        CHECK(v[999] != 0x33, "guest memory moved back is still the buffer's");
        plat_vm_decommit(g, 2u << 20);
        plat_gpu_buffer_free(fd, handle, 2u << 20);
#elif !defined(__ANDROID__)
        CHECK(plat_gpu_buffer(2u << 20, &fd, &handle) == -1 && errno == ENOSYS && fd == -1 && !handle,
              "a buffer this host can't make");
#endif
    }
}

#if defined(__linux__)
static struct {
    u64 lo, hi;
} g_own; /* the range test_foreign's caller claims as its own */
static int own_claims(u64 a, u64 b) { return g_own.lo <= a && b <= g_own.hi; }
/* a memfd named name, size bytes, written with text when there is one and read from its start: -1 when not */
static int memfd_with(const char* name, u64 size, const char* text) {
    int fd = (int)syscall(SYS_memfd_create, name, 1u /* MFD_CLOEXEC */);
    size_t n = text ? strlen(text) : 0;
    if (fd < 0) return -1;
    if ((size && ftruncate(fd, (off_t)size)) || (n && write(fd, text, n) != (ssize_t)n) || lseek(fd, 0, SEEK_SET)) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif
/* plat_vm_foreign over a reservation of its own: its own pages and the alias pool's are the guest's, and so is a
   mapping the caller claims (a GPU's buffer guest memory moved into); another owner's memfd mapped over it (as a JIT's
   code cache would be), a run given back and a named anonymous mapping (as ART and the allocator name theirs) are not,
   each named with its addresses. A failed read of the maps is no answer, and a run they list as empty that is mapped
   now (it changed while they were read) is no gap. Linux, where /proc/self/maps says; elsewhere -1 */
static void test_foreign(void) {
    char out[512];
    printf("what is not the guest's in its range\n");
#if defined(__linux__)
    const u64 pg = (u64)sysconf(_SC_PAGESIZE);
    u8* r = (u8*)mmap(NULL, 64 * pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    u64 lo = (u64)(uintptr_t)r, hi = lo + 64 * pg;
    char want[256], small[24];
    int alias, jit, gpu, maps, dir, n;
    if (r == (u8*)MAP_FAILED) {
        CHECK(0, "no reservation for the foreign mappings");
        return;
    }
    CHECK(plat_vm_foreign(lo, hi, NULL, out, sizeof out) == 0 && !out[0], "a bare reservation held something");
    CHECK(!mprotect(r, 4 * pg, PROT_READ | PROT_WRITE), "four pages not committed");
    r[0] = 1;
    CHECK(plat_vm_foreign(lo, hi, NULL, out, sizeof out) == 0, "its committed pages were not its own");
    alias = memfd_with("snx-alias", 2 * pg, NULL);
    jit = memfd_with("jit-cache", 2 * pg, NULL);
    gpu = memfd_with("gpu-buffer", 2 * pg, NULL);
    CHECK(alias >= 0 && jit >= 0 && gpu >= 0, "no memfds");
    CHECK(mmap(r + 8 * pg, 2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, alias, 0) == r + 8 * pg &&
              plat_vm_foreign(lo, hi, NULL, out, sizeof out) == 0,
          "the alias pool's pages were not the guest's");
    g_own.lo = lo + 24 * pg, g_own.hi = lo + 26 * pg;
    CHECK(mmap(r + 24 * pg, 2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, gpu, 0) == r + 24 * pg &&
              plat_vm_foreign(lo, hi, own_claims, out, sizeof out) == 0,
          "a mapping its caller claims was named");
    CHECK(plat_vm_foreign(lo, hi, NULL, out, sizeof out) == 1 && strstr(out, " rw-s /memfd:gpu-buffer (deleted)"),
          "the same mapping, which no caller claims, was not named");
    CHECK(mmap(r + 24 * pg, 2 * pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) ==
              r + 24 * pg,
          "the reservation not mapped back");
    CHECK(mmap(r + 16 * pg, 2 * pg, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, jit, 0) == r + 16 * pg,
          "no memfd of another owner over it");
    n = plat_vm_foreign(lo, hi, own_claims, out, sizeof out);
    snprintf(want, sizeof want, "mapped at %#llx-%#llx r-xs /memfd:jit-cache (deleted)",
             (unsigned long long)(lo + 16 * pg), (unsigned long long)(lo + 18 * pg));
    CHECK(n == 1 && !strcmp(out, want), "another owner's mapping not named with its range, permissions and name");
    CHECK(!munmap(r + 32 * pg, 2 * pg), "no run given back");
    n = plat_vm_foreign(lo, hi, own_claims, out, sizeof out);
    snprintf(want, sizeof want, "; nothing mapped at %#llx-%#llx", (unsigned long long)(lo + 32 * pg),
             (unsigned long long)(lo + 34 * pg));
    CHECK(n == 2 && strstr(out, want), "the run given back not named with its range");
    CHECK(plat_vm_foreign(lo + 18 * pg, lo + 32 * pg, NULL, out, sizeof out) == 0, "a range read past its ends");
    snprintf(want, sizeof want, "nothing mapped at %#llx-%#llx", (unsigned long long)(lo + 33 * pg),
             (unsigned long long)(lo + 34 * pg));
    CHECK(plat_vm_foreign(lo + 33 * pg, lo + 34 * pg, NULL, out, sizeof out) == 1 && !strcmp(out, want),
          "a run the range lies in not cut to the range");
    CHECK(plat_vm_foreign(lo, hi, NULL, small, sizeof small) == 2 && strlen(small) == sizeof small - 1,
          "a small buffer did not get the whole count and the names cut where it was full");
#if defined(PR_SET_VMA) && defined(PR_SET_VMA_ANON_NAME)
    if (!prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (unsigned long)(uintptr_t)(r + 40 * pg), (unsigned long)(2 * pg),
               (unsigned long)(uintptr_t)"dalvik-jit-code-cache")) {
        n = plat_vm_foreign(lo, hi, NULL, out, sizeof out);
        CHECK(n == 3 && strstr(out, " [anon:dalvik-jit-code-cache]"), "a named anonymous mapping not named");
    } else
        printf("  a named anonymous mapping skipped: this kernel names none (%s)\n", strerror(errno));
#endif
    /* a read that fails is no answer, not the end of the maps: nothing past it reads as unmapped */
    dir = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    errno = 0;
    CHECK(dir >= 0 && foreign_scan(dir, lo, hi, NULL, out, sizeof out) == -1 && errno == EISDIR && !out[0],
          "a failed read of the maps read as a run nothing maps");
    if (dir >= 0) close(dir);
    /* maps read while another thread changed the range: they list [4, 8) empty, which is the reservation's, and
       [32, 34), which is given back. Only that one is named */
    snprintf(want, sizeof want,
             "%llx-%llx rw-p 00000000 00:00 0\n%llx-%llx ---p 00000000 00:00 0\n%llx-%llx ---p 00000000 00:00 0\n",
             (unsigned long long)lo, (unsigned long long)(lo + 4 * pg), (unsigned long long)(lo + 8 * pg),
             (unsigned long long)(lo + 32 * pg), (unsigned long long)(lo + 34 * pg), (unsigned long long)hi);
    maps = memfd_with("maps", 0, want);
    CHECK(maps >= 0, "no copy of the maps");
    n = maps >= 0 ? foreign_scan(maps, lo, hi, NULL, out, sizeof out) : -1;
    snprintf(want, sizeof want, "nothing mapped at %#llx-%#llx", (unsigned long long)(lo + 32 * pg),
             (unsigned long long)(lo + 34 * pg));
    CHECK(n == 1 && !strcmp(out, want), "a run the maps listed empty, mapped when asked again, was named");
    if (maps >= 0) close(maps);
    munmap(r, 64 * pg);
    close(alias);
    close(jit);
    close(gpu);
#else
    errno = 0;
    CHECK(plat_vm_foreign(0x1000, 0x2000, NULL, out, sizeof out) == -1 && errno == ENOSYS && !out[0],
          "a system with no /proc/self/maps said what lies in a range");
#endif
}

/* host_page: the host's page, or a larger power of two up to SNX_PLAT_PAGE_MAX; another value is said once, with the
   page the runtime works in instead */
static void test_host_page(void) {
    u64 host = (u64)sysconf(_SC_PAGESIZE);
    int given = g_settings.host_page_size;
    char want[256];
    printf("host_page\n");
    g_settings.host_page_size = 0;
    CHECK(plat_vm_page() == host, "host_page=0 is not the host's page");
    g_settings.host_page_size = (int)host;
    CHECK(plat_vm_page() == host, "host_page as the host's page is not that page");
    if (host * 16 <= SNX_PLAT_PAGE_MAX) {
        g_settings.host_page_size = (int)(host * 16);
        CHECK(plat_vm_page() == host * 16, "a larger power of two is not the page");
    }
    g_log_on = 1;
    g_settings.host_page_size = (int)(host * 3);
    CHECK(plat_vm_page() == host, "a page of three host pages is taken");
    snprintf(want, sizeof want,
             "memory: host_page=%llu is not a power of two from the host's page, %llu bytes, to 1048576: the runtime "
             "works in %llu-byte pages\n",
             (unsigned long long)host * 3, (unsigned long long)host, (unsigned long long)host);
    CHECK(!strcmp(g_log, want), "host_page's refusal does not name the value given and the page in effect");
    g_log[0] = 0;
    CHECK(plat_vm_page() == host && !g_log[0], "host_page's refusal is said again");
    g_log_on = 0;
    g_settings.host_page_size = given;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    char path[1200], tmp[1200], sub[1200];
    u8 buf[64];
    FILE* f;
    PFileMap m;
    struct stat st;
    PCond c;
    PMutex mu = PMUTEX_INIT;
    int i, arg = 7;
    u64 t;

    setvbuf(stdout, NULL, _IONBF, 0); /* each section says it started, so a crash shows where it happened */
    for (i = 2; i < argc; i++) {
        const char* eq = strchr(argv[i], '=');
        char key[64];
        if (strncmp(argv[i], "--", 2) || !eq || (size_t)(eq - argv[i] - 2) >= sizeof key) {
            printf("not --key=value: %s\n", argv[i]);
            return 2;
        }
        snprintf(key, sizeof key, "%.*s", (int)(eq - argv[i] - 2), argv[i] + 2);
        if (!ini_find(g_debug_ini, key)) {
            printf("not a Debug setting: %s\n", argv[i]);
            return 2;
        }
        ini_read(ini_find(g_debug_ini, key), eq + 1);
    }
    test_host_page();
    test_foreign();
    if (sysconf(_SC_PAGESIZE) == 4096)
        test_guest_memory(dir);
    else /* plat_vm_reserve refuses the host: guest memory needs 4 KiB pages, and Apple silicon's are 16 KiB */
        printf("guest memory skipped: this host's pages are %ld bytes\n", sysconf(_SC_PAGESIZE));

    printf("pread and file maps\n");
    /* pread: a short count at the end, the position untouched */
    snprintf(path, sizeof path, "%s/platform_posix.bin", dir);
    f = fopen(path, "wb");
    fwrite("0123456789", 1, 10, f);
    fclose(f);
    f = fopen(path, "rb");
    CHECK(plat_pread(f, buf, 4, 3) == 4 && !memcmp(buf, "3456", 4), "pread in the middle");
    CHECK(plat_pread(f, buf, 20, 6) == 4 && !memcmp(buf, "6789", 4), "pread past the end");
    CHECK(ftell(f) == 0, "pread moved the position");
    fclose(f);

    CHECK(!plat_file_map(&m, path) && m.size == 10 && !memcmp(m.data, "0123456789", 10), "map");
    plat_file_unmap(&m);
    CHECK(!m.data && !m.size, "unmap did not clear");
    CHECK(!plat_file_map_sized(&m, path, 4) && m.size == 4 && !memcmp(m.data, "0123", 4), "sized map");
    plat_file_unmap(&m);
    CHECK(plat_file_map(&m, dir) == -1 && !m.data, "a folder mapped");
    snprintf(tmp, sizeof tmp, "%s/platform_posix.empty", dir);
    fclose(fopen(tmp, "wb"));
    CHECK(!plat_file_map(&m, tmp) && !m.data && !m.size, "an empty file did not map to nothing");
    plat_file_unmap(&m);

    printf("replace\n");
    /* replace: dst gets tmp's contents; a failing replace removes tmp */
    CHECK(!plat_replace_file(tmp, path) && !stat(path, &st) && st.st_size == 0 && !fopen(tmp, "rb"), "replace");
    fclose(fopen(tmp, "wb"));
    snprintf(sub, sizeof sub, "%s/platform_posix.none/x", dir);
    CHECK(plat_replace_file(tmp, sub) == -1 && !fopen(tmp, "rb"), "a failed replace kept tmp");

    printf("folders\n");
    snprintf(sub, sizeof sub, "%s/platform_posix.dir", dir);
    plat_mkdir(sub);
    CHECK(plat_is_dir(sub) && !plat_is_dir(path), "plat_is_dir");

    printf("timed waits and threads\n");
    /* a timed wait times out, roughly on time */
    pc_init(&c);
    pm_lock(&mu);
    t = plat_ns();
    CHECK(pc_wait(&c, &mu, 20000000) == 1, "a timed wait not reported as timed out");
    CHECK(plat_ns() - t >= 19000000, "a timed wait came back early");
    pm_unlock(&mu);

    CHECK(!plat_thread(thread_fn, &arg, 0) && !plat_thread(thread_fn, &arg, 1024), "threads did not start");
    for (i = 0; i < 5000 && __atomic_load_n(&g_ran, __ATOMIC_ACQUIRE) != 14; i++) plat_sleep_ns(1000000);
    CHECK(__atomic_load_n(&g_ran, __ATOMIC_ACQUIRE) == 14, "a thread did not run");

    /* last: the crash handler stays for the rest of the process. A SIGSEGV or SIGBUS sent (a debugger's leftover, a
       kill) is ignored where the kernel says it was sent (Linux: sent_signal), and a real fault is still reported */
    printf("sent signals\n");
    plat_install_crash_handler();
    {
        union sigval v;
        pid_t child;
        int status = 0;
        v.sival_int = 0;
#if defined(__linux__)
        CHECK(raise(SIGSEGV) == 0 && g_reports == 0, "a raised SIGSEGV was reported");
        CHECK(pthread_kill(pthread_self(), SIGBUS) == 0 && g_reports == 0, "a SIGBUS sent to the thread was reported");
        CHECK(sigqueue(getpid(), SIGSEGV, v) == 0 && g_reports == 0, "a queued SIGSEGV was reported");
#else
        (void)v;
#endif
        child = fork();
        if (!child) {
            volatile u8* p = (volatile u8*)mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            g_report_exit = 1;
            p[0] = 1; /* faults: the handler reports, which ends the child with 42 */
            _exit(3);
        }
        CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 42,
              "a real fault in a child was not reported");
    }

    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
