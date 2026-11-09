/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The POSIX platform layer (platform/plat_posix.c, with plat_sdl.c): written-page tracking, decommit zeroing, pread,
   file maps, replace, folders, timed waits and threads, the watch of the system's memory pressure. Written-page
   tracking needs Linux 6.7 for userfaultfd's; write_tracking=1 (and every host without it) tracks by protection, and
   write_tracking=2 must make the scan say -1. The arguments: the folder for its files, then Debug settings as the
   runtime takes them, --key=value; --bench-ww runs the tracking's benchmark alone (bench_ww). And the look for a
   library the process has loaded already (plat_dl_loaded), with a library of the tests' own, SNX_TEST_DL_PROBE */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>

/* Intercept only this test's alias operation. Simulate a driver that rejects nonzero mmap offsets, then another
   thread that immediately maps the hole left by mremap. All other calls reach the real system calls. */
typedef struct {
    int armed, fd, rejected, moved, sentinel_error;
    void* destination;
    unsigned char* sentinel;
    size_t sentinel_size;
} AliasCleanup;
static AliasCleanup g_alias_cleanup;
static void* alias_test_mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
    if (g_alias_cleanup.armed && fd == g_alias_cleanup.fd && address == g_alias_cleanup.destination && offset) {
        g_alias_cleanup.rejected++;
        errno = EINVAL;
        return MAP_FAILED;
    }
    return mmap(address, length, protection, flags, fd, offset);
}
static void* alias_test_mremap(void* address, size_t old_size, size_t new_size, int flags, ...) {
    void* destination = NULL;
    void* result;
    if (flags & MREMAP_FIXED) {
        va_list args;
        va_start(args, flags);
        destination = va_arg(args, void*);
        va_end(args);
        result = mremap(address, old_size, new_size, flags, destination);
    } else
        result = mremap(address, old_size, new_size, flags);
    if (g_alias_cleanup.armed && g_alias_cleanup.rejected && destination == g_alias_cleanup.destination &&
        result != MAP_FAILED) {
        g_alias_cleanup.moved++;
        void* sentinel = mmap(address, old_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (sentinel == MAP_FAILED)
            g_alias_cleanup.sentinel_error = errno;
        else {
            memset(sentinel, 0xa7, old_size);
            g_alias_cleanup.sentinel = sentinel;
            g_alias_cleanup.sentinel_size = old_size;
        }
    }
    return result;
}
#define mmap alias_test_mmap
#define mremap alias_test_mremap
#endif
#include "nx/runtime/platform/plat_posix.c"
#if defined(__linux__)
#undef mmap
#undef mremap
#endif
#include "nx/runtime/ini.h"
#include <stdarg.h>
#include <stdio.h>
#include <sys/resource.h> /* RLIMIT_NPROC: a thread the system refuses */
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
#if defined(__linux__)
static int g_reports; /* crash reports made: only the checks of signals sent to a thread read them */
#endif
static int g_report_exit; /* a child's: a report ends it with 42, which its parent checks */
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
#if defined(__linux__)
    g_reports++;
#endif
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
/* the marks that let a scan skip a group of unwritten pages: a whole group's scan takes its mark, and a scan of a part
   of it leaves the mark for the pages it didn't read. The first 64 pages are the reservation's first group */
static void test_scan_marks(void) {
    printf("written-page tracking by protection: the marks of groups of pages\n");
    CHECK(g_wp_marks != NULL && g_base == g_wp_base, "no marks, or the test's pages are not the first group");
    scan();
    CHECK(!__atomic_load_n(&g_wp_marks[0], __ATOMIC_RELAXED), "a whole group's scan left its mark");
    g_base[3 * 4096] = 1;
    CHECK(__atomic_load_n(&g_wp_marks[0], __ATOMIC_RELAXED), "a written page left no mark");
    __atomic_store_n(&g_seen, 0, __ATOMIC_RELAXED);
    plat_ww_scan(g_base + 8 * 4096, 8 * 4096, on_seen);
    CHECK(!__atomic_load_n(&g_seen, __ATOMIC_RELAXED), "a scan of other pages reported a page");
    CHECK(__atomic_load_n(&g_wp_marks[0], __ATOMIC_RELAXED), "a scan of a part of a group took its mark");
    CHECK(scan() == 1ull << 3, "the written page was lost to the scan of a part of its group");
    CHECK(!__atomic_load_n(&g_wp_marks[0], __ATOMIC_RELAXED) && scan() == 0, "the page was reported twice");
    /* a scan that starts inside a group takes the written page it reads, and the next finds nothing */
    g_base[60 * 4096] = 2;
    __atomic_store_n(&g_seen, 0, __ATOMIC_RELAXED);
    plat_ww_scan(g_base + 56 * 4096, 8 * 4096, on_seen);
    CHECK(__atomic_load_n(&g_seen, __ATOMIC_RELAXED) == 1ull << 60, "a scan of a part of a group missed its page");
    CHECK(scan() == 0, "a page that a scan of a part of its group took was reported again");
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

/* page n's hot count, and whether the tracking holds it read only */
static u8 hot_count(u64 n) { return __atomic_load_n(&g_wp_hot[n], __ATOMIC_RELAXED); }
static int held(u64 n) { return (__atomic_load_n(&g_wp[n], __ATOMIC_RELAXED) & WP_PROT) != 0; }
/* page n written, then scanned, k times: 1 when every scan reported it */
static int written_scans(u64 n, int k) {
    int all = 1;
    while (k--) {
        g_base[n * 4096 + 1]++;
        all &= (scan() >> n & 1) != 0;
    }
    return all;
}
/* ---- hot pages (write_tracking_hot=1): a page written in WP_HOT_AFTER scans in a row stays writable and is reported
   at each scan, until its WP_HOT_AFTER + WP_HOT_FOR-th report; plat_ww_cool makes its reports exact again. Run on a
   table of counts of its own when the setting left none */
static void test_hot_pages(void) {
    static u8 before[32 * 4096];
    u8* own = NULL;
    pthread_t t[WRITERS];
    u64 missed = 0, inexact = 0, scans = 0, cools = 0, most_hot = 0, iter, k;
    int i;
    printf("hot pages%s\n", g_wp_hot ? "" : " (on a table of counts of the test's own)");
    if (!g_wp_hot) {
        own = (u8*)mmap(NULL, (size_t)g_wp_pages, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                        -1, 0);
        CHECK(own != MAP_FAILED, "no table of counts");
        if (own == MAP_FAILED) return;
        g_wp_hot = own;
    }
    scan();
    CHECK(scan() == 0, "pages reported written before the hot pages' checks");
    /* reported once: protected; twice in a row: left writable, and reported with no write */
    CHECK(written_scans(6, 1) && hot_count(6) == 1 && held(6), "a page written once was not protected");
    CHECK(written_scans(6, 1) && hot_count(6) == WP_HOT_AFTER && !held(6), "a page written twice did not go hot");
    CHECK(scan() == 1ull << 6, "a hot page was not reported by a scan with no write");
    CHECK(plat_ww_hot_in(g_base + 6 * 4096, 4096) && !plat_ww_hot_in(g_base + 7 * 4096, 4096) &&
              plat_ww_hot_in(g_base, 64 * 4096),
          "plat_ww_hot_in did not find the one hot page");
    /* cooled: reported, protected, and exact from then on */
    g_written = 0;
    plat_ww_cool(g_base, 64 * 4096, on_written);
    CHECK(g_written == 1ull << 6, "the cool did not report the hot page alone");
    CHECK(hot_count(6) == 0 && held(6) && !plat_ww_hot_in(g_base, 64 * 4096), "the cool left the page hot");
    CHECK(scan() == 0, "a cooled page was reported with no write");
    CHECK(written_scans(6, 1) && held(6), "a write after the cool was not reported, or the page went hot at once");
    /* a scan that finds it unwritten starts its count over */
    CHECK(written_scans(8, 1) && scan() == 0 && written_scans(8, 1) && hot_count(8) == 1 && held(8),
          "a page written in two scans not in a row went hot");
    /* written at each scan: hot until its WP_HOT_AFTER + WP_HOT_FOR-th report, then protected and counted over */
    CHECK(written_scans(9, WP_HOT_AFTER + WP_HOT_FOR - 1) && hot_count(9) == WP_HOT_AFTER + WP_HOT_FOR - 1 && !held(9),
          "a page written at each scan did not stay hot");
    CHECK(written_scans(9, 1) && hot_count(9) == 0 && held(9), "a hot page was not protected again after its run");
    CHECK(scan() == 0, "a page protected again after its run was reported with no write");
    /* hot, then left alone: reported until its run ends, then quiet */
    CHECK(written_scans(11, WP_HOT_AFTER), "page 11 not reported");
    for (k = 0; k < WP_HOT_FOR + 1 && scan() >> 11 & 1; k++) {}
    CHECK(k == WP_HOT_FOR && held(11) && scan() == 0, "a hot page left alone did not go quiet after its run");
    /* a mapping change starts its count over; so does a trap's arming (wp_armed), which scans skip */
    CHECK(written_scans(13, WP_HOT_AFTER) && hot_count(13) == WP_HOT_AFTER, "page 13 did not go hot");
    plat_vm_decommit(g_base + 13 * 4096, 4096);
    CHECK(!plat_vm_commit(g_base + 13 * 4096, 4096) && hot_count(13) == 0, "a decommit left the page's count");
    CHECK(scan() == 1ull << 13 && scan() == 0, "a page committed again was not reported once");
    CHECK(written_scans(14, WP_HOT_AFTER), "page 14 not reported");
    wp_armed(g_base + 14 * 4096, 4096, 1);
    CHECK(scan() == 0 && hot_count(14) == 0, "a page armed for a trap kept its count, or was reported");
    wp_armed(g_base + 14 * 4096, 4096, 0);
    CHECK(scan() == 1ull << 14 && held(14), "a page let go by its trap was not reported once and protected");
    /* the kernel writing a hot page: the scans and the cool leave it writable until plat_ww_close, then a scan
       reports it */
    CHECK(written_scans(15, WP_HOT_AFTER) && !held(15), "page 15 did not go hot");
    scan();
    plat_ww_open(g_base + 15 * 4096, 100);
    CHECK(!(scan() >> 15 & 1), "a page the kernel is writing was reported");
    g_written = 0;
    plat_ww_cool(g_base + 15 * 4096, 4096, on_written);
    CHECK(!g_written && !held(15), "the cool protected a page the kernel is writing");
    memset(g_base + 15 * 4096, 0x5a, 100); /* as the system call would */
    plat_ww_close(g_base + 15 * 4096, 100);
    CHECK(scan() >> 15 & 1, "a page the kernel wrote was not reported after it was done");
    scan();
    /* writers race scans and cools: no write is lost, and right after a cool with the writers held a scan reports
       nothing (what a target's baseline needs) */
    memcpy(before, g_base + 32 * 4096, sizeof before);
    g_seen = 0;
    __atomic_store_n(&g_writers_stop, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_writers_run, 1, __ATOMIC_SEQ_CST);
    for (i = 0; i < WRITERS; i++) { /* the last writers ended idle */
        __atomic_store_n(&g_writer_idle[i], 0, __ATOMIC_SEQ_CST);
        CHECK(!pthread_create(&t[i], NULL, writer, (void*)(intptr_t)i), "no writer");
    }
    for (iter = 0; iter < 200; iter++) {
        u64 from = __atomic_load_n(&g_writes, __ATOMIC_RELAXED), hot;
        while (__atomic_load_n(&g_writes, __ATOMIC_RELAXED) - from < 4000) {
            plat_ww_scan(g_base + 32 * 4096, 32 * 4096, on_seen);
            if (++scans % 8 == 0) { /* a target's baseline over them, with the writers going */
                plat_ww_cool(g_base + 32 * 4096, 32 * 4096, on_seen);
                cools++;
            }
            hot = __atomic_load_n(&g_wp_hot_n, __ATOMIC_RELAXED);
            if (hot > most_hot) most_hot = hot;
        }
        writers_pause();
        plat_ww_scan(g_base + 32 * 4096, 32 * 4096, on_seen);
        for (k = 0; k < 32; k++)
            if (memcmp(before + k * 4096, g_base + (32 + k) * 4096, 4096) && !(g_seen >> (32 + k) & 1)) missed++;
        plat_ww_cool(g_base + 32 * 4096, 32 * 4096, on_seen);
        g_written = 0;
        plat_ww_scan(g_base + 32 * 4096, 32 * 4096, on_written);
        if (g_written) inexact++;
        memcpy(before, g_base + 32 * 4096, sizeof before);
        g_seen = 0;
        __atomic_store_n(&g_writers_run, 1, __ATOMIC_SEQ_CST);
        for (i = 0; i < WRITERS; i++)
            while (__atomic_load_n(&g_writer_idle[i], __ATOMIC_SEQ_CST)) sched_yield();
    }
    __atomic_store_n(&g_writers_stop, 1, __ATOMIC_SEQ_CST);
    for (i = 0; i < WRITERS; i++) pthread_join(t[i], NULL);
    CHECK(missed == 0, "a scan or a cool missed a page another thread wrote, with hot pages");
    CHECK(inexact == 0, "a scan right after a cool, with the writers held, reported pages");
    CHECK(most_hot > 0, "the writers' pages never went hot");
    printf("  %llu scans and %llu cools while the writers wrote, up to %llu pages hot at once, %llu checks: %llu pages "
           "missed, %llu scans after a cool not exact\n",
           (unsigned long long)scans, (unsigned long long)cools, (unsigned long long)most_hot, (unsigned long long)iter,
           (unsigned long long)missed, (unsigned long long)inexact);
    plat_ww_cool(g_base, 64 * 4096, on_written); /* exact again for what follows */
    if (own) {
        g_wp_hot = NULL;
        __atomic_store_n(&g_wp_hot_n, 0, __ATOMIC_RELAXED);
        munmap(own, (size_t)g_wp_pages);
    }
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
    g_log[0] = 0;
    g_log_on = 1;
    plat_vm_track(g_base, (u64)1 << 30);
    g_log_on = 0;
    /* write_tracking_hot=1 reaches the tracking by protection, and is said to do nothing elsewhere */
    if (g_settings.write_tracking_hot_pages == 1 && g_wp_track)
        CHECK(g_wp_hot != NULL, "write_tracking_hot=1 left the tracking by protection without hot pages");
    else if (g_settings.write_tracking_hot_pages == 1)
        CHECK(strstr(g_log, "memory: write_tracking_hot=1 leaves pages hot only where page protection tracks the "
                            "writes, and ") &&
                  strstr(g_log, ": no page is hot this run\n"),
              "write_tracking_hot=1 where nothing protects pages for the tracking was not said");
    else
        CHECK(g_wp_hot == NULL, "hot pages without write_tracking_hot=1");
    CHECK(!plat_vm_commit(g_base, 64 * 4096), "commit failed");
    if (g_settings.write_tracking == 2 || plat_ww_scan(g_base, 4096, on_written) < 0) {
        printf("written-page tracking unavailable here: only checking that it says so\n");
        CHECK(scan() == ~0ull, "a scan without tracking did not fail");
    } else {
        u8* hot = g_wp_hot; /* the checks below are of exact reports: hot pages have theirs (test_hot_pages) */
        g_wp_hot = NULL;
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
        if (g_wp && g_wp_track) test_scan_marks();
        if (g_wp) {
            test_protection_tracking();
            test_kernel_writes(dir);
            test_open_frozen();
            test_shed();
        }
        if (hot) { /* back, counted from zero: the scans without it left its counts behind */
            memset(hot, 0, (size_t)g_wp_pages);
            __atomic_store_n(&g_wp_hot_n, 0, __ATOMIC_RELAXED);
            g_wp_hot = hot;
        }
        if (g_wp && g_wp_track) test_hot_pages();
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
        u8* alias = g + 0x10000;
        g[0xffff] = 0x55;
        g[0x12000] = 0x66;
        v[0x3011] = 0x77;
        CHECK(!plat_vm_map_fd(alias, 0x2000, fd, 0x3000, 2u << 20) && alias[17] == 0x77,
              "a file range did not map its source offset");
        alias[18] = 0x88;
        CHECK(v[0x3012] == 0x88 && g[0xffff] == 0x55 && g[0x12000] == 0x66,
              "a file range did not share its pages or changed its neighbors");
        CHECK(plat_vm_map_fd(alias, 0x2000, fd, (2u << 20) - 0x1000, 2u << 20) == -1 && alias[18] == 0x88,
              "an out-of-bounds file range replaced the destination");
        g_alias_cleanup = (AliasCleanup){.armed = 1, .fd = fd, .destination = alias};
        int remapped = plat_vm_map_fd(alias, 0x2000, fd, 0x3000, 2u << 20);
        g_alias_cleanup.armed = 0;
        CHECK(!remapped && g_alias_cleanup.rejected == 1 && g_alias_cleanup.moved == 1 &&
                  !g_alias_cleanup.sentinel_error && g_alias_cleanup.sentinel,
              "the alias cleanup race did not reach its forced mmap fallback and sentinel mapping");
        /* mincore tests the mapping before any read. The old whole-allocation munmap removed this sentinel. */
        unsigned char residency[2];
        int retained = g_alias_cleanup.sentinel &&
                       !mincore(g_alias_cleanup.sentinel, g_alias_cleanup.sentinel_size, residency);
        CHECK(retained, "alias cleanup removed another thread's mapping in the old mremap hole");
        if (retained) {
            CHECK(g_alias_cleanup.sentinel[17] == 0xa7 &&
                      g_alias_cleanup.sentinel[g_alias_cleanup.sentinel_size - 1] == 0xa7,
                  "alias cleanup changed another thread's sentinel bytes");
            munmap(g_alias_cleanup.sentinel, g_alias_cleanup.sentinel_size);
        }
        if (!remapped)
            CHECK(alias[17] == 0x77 && alias[18] == 0x88 && g[0xffff] == 0x55 && g[0x12000] == 0x66,
                  "the mmap fallback changed its file offset, shared bytes or destination neighbors");
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

/* the system's warnings of low memory: on Linux the watch starts when this process may set the pressure trigger itself
   (a kernel with PSI, from 6.5 for one without privileges), and the log says why it does not; elsewhere it does not
   start here (macOS's comes from the backend, FreeBSD has none). A watch whose thread the system refused (past
   RLIMIT_NPROC, which root does not have) says so and starts at the next call */
static int test_say(const char* why) {
    (void)why;
    return 1;
}
#if defined(__linux__) && !defined(__ANDROID__)
static void* test_no_op(void* unused) { return unused; } /* a thread for the refusal's probe */
#endif
static void test_memory_watch(void) {
    int can = 0;
    printf("memory watch\n");
#if defined(__linux__) && !defined(__ANDROID__)
    {
        int fd = open(PSI_FILE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        can = fd >= 0 && write(fd, PSI_TRIGGER, sizeof PSI_TRIGGER) > 0;
        if (fd >= 0) close(fd);
    }
#endif
    g_log[0] = 0;
    g_log_on = 1;
#if defined(__linux__) && !defined(__ANDROID__)
    if (can) {
        struct rlimit was, none;
        pthread_t probe;
        int refused = 0;
        CHECK(!getrlimit(RLIMIT_NPROC, &was), "RLIMIT_NPROC");
        none = was;
        none.rlim_cur = 0;
        CHECK(!setrlimit(RLIMIT_NPROC, &none), "RLIMIT_NPROC down");
        refused = pthread_create(&probe, NULL, test_no_op, NULL) != 0;
        if (!refused) pthread_join(probe, NULL);
        if (refused)
            CHECK(plat_memory_watch(test_say) == -1 &&
                      strstr(g_log, "memory: no watch of the system's memory pressure: its thread does not start: "),
                  "a memory watch whose thread did not start");
        CHECK(!setrlimit(RLIMIT_NPROC, &was), "RLIMIT_NPROC back");
        printf("memory watch: %s\n", refused ? "a refused thread said" : "no thread refused here (root)");
        g_log[0] = 0;
    }
#endif
    CHECK(plat_memory_watch(test_say) == (can ? 0 : -1), "the memory watch does not start as the trigger allows");
#if defined(__linux__) && !defined(__ANDROID__)
    CHECK(can ? !g_log[0] : strstr(g_log, "memory: no watch of the system's memory pressure: " PSI_FILE) != NULL,
          "the memory watch's log line");
    CHECK(plat_memory_watch(test_say) == (can ? 0 : -1), "a second start of the memory watch");
    if (can) {
        /* one thread runs, by its name: a refused start before left nothing that keeps the watch from starting, and
           the second start started none. Each names itself as it starts, so the count waits a little for them */
        static SnxThreadInfo list[256];
        struct timespec pause = {0, 10000000};
        int running = 0;
        for (int tries = 0; tries < 300 && (tries < 20 || !running); tries++) {
            int n = snx_threads(list, 256);
            running = 0;
            for (int i = 0; i < n && i < 256; i++) running += !strcmp(list[i].name, "memory pressure");
            nanosleep(&pause, NULL);
        }
        if (running != 1) printf("memory watch: %d threads named \"memory pressure\"\n", running);
        CHECK(running == 1, "the memory watch does not run one thread");
    }
#endif
    printf("memory watch: %s\n", can ? "started" : "none here");
    g_log_on = 0;
}

/* ---- --bench-ww: what the written-page tracking costs on this host, with no game: page protection without and with
   hot pages (write_tracking_hot, #1237), through the real scan and fault handler, or the host's own way where it has
   one. A region of BENCH_PAGES pages; each epoch writes a byte on each of hot pages (the same ones every epoch, every
   other page, so each is a run of its own to protect) and on cold ones picked anew, then one scan of the region takes
   in what was written, as a cache's check does. Per pattern and arm: the faults the handler took and its time (a
   handler around the process's own counts and times them), the kernel's minor faults, the writes' and the scans' time,
   per epoch and per second of the run. The timings are the host's at that moment, printed and not checked; what is
   checked is that each scan reported every page written before it, and that hot pages took fewer faults */
#define BENCH_PAGES 4096u
static struct {
    struct sigaction segv, bus; /* the process's own, which bench_signal calls */
    u64 faults, handler_ns;
    u8* lo;
    u8 seen[BENCH_PAGES], written[BENCH_PAGES];
} g_bench;
static void bench_signal(int sig, siginfo_t* si, void* ctx) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    (sig == SIGBUS ? g_bench.bus : g_bench.segv).sa_sigaction(sig, si, ctx);
    clock_gettime(CLOCK_MONOTONIC, &b);
    g_bench.faults++;
    g_bench.handler_ns += (u64)(b.tv_sec - a.tv_sec) * 1000000000ull + (u64)b.tv_nsec - (u64)a.tv_nsec;
}
static void bench_seen(u8* p, u64 size) {
    for (u64 a = (u64)(p - g_bench.lo) >> 12, e = (u64)(p - g_bench.lo + size) >> 12; a < e && a < BENCH_PAGES; a++)
        g_bench.seen[a] = 1;
}
typedef struct {
    u64 faults, handler_ns, minflt, write_ns, scan_ns, reported, missed, ns;
} BenchArm;
static void bench_arm(BenchArm* r, u32 hot, u32 cold, u32 epochs) {
    struct rusage ru0, ru1;
    u32 x = 0x2545f491u; /* the same cold pages in each arm */
    memset(r, 0, sizeof *r);
    plat_ww_scan(g_bench.lo, BENCH_PAGES * 4096ull, bench_seen); /* everything taken in: the arm starts clean */
    g_bench.faults = g_bench.handler_ns = 0;
    getrusage(RUSAGE_SELF, &ru0);
    u64 start = plat_ns();
    for (u32 e = 0; e < epochs; e++) {
        memset(g_bench.written, 0, sizeof g_bench.written);
        u64 t0 = plat_ns();
        for (u32 i = 0; i < hot; i++) {
            g_bench.lo[2 * i * 4096ull + (e & 4095)]++;
            g_bench.written[2 * i] = 1;
        }
        for (u32 i = 0; i < cold; i++) {
            x = x * 1664525u + 1013904223u;
            u32 pg = 2 * hot + (x >> 8) % (BENCH_PAGES - 2 * hot);
            g_bench.lo[pg * 4096ull + (e & 4095)]++;
            g_bench.written[pg] = 1;
        }
        u64 t1 = plat_ns();
        memset(g_bench.seen, 0, sizeof g_bench.seen);
        plat_ww_scan(g_bench.lo, BENCH_PAGES * 4096ull, bench_seen);
        u64 t2 = plat_ns();
        r->write_ns += t1 - t0;
        r->scan_ns += t2 - t1;
        for (u32 p = 0; p < BENCH_PAGES; p++) {
            r->missed += g_bench.written[p] && !g_bench.seen[p];
            r->reported += g_bench.seen[p];
        }
    }
    r->ns = plat_ns() - start;
    getrusage(RUSAGE_SELF, &ru1);
    r->minflt = (u64)(ru1.ru_minflt - ru0.ru_minflt);
    r->faults = g_bench.faults;
    r->handler_ns = g_bench.handler_ns;
}
static void bench_print(const char* arm, u32 hot, u32 cold, u32 epochs, const BenchArm* r) {
    double sec = (double)r->ns / 1e9;
    printf(
        "  %-7s hot %4u cold %2u: an epoch %6.1f faults (handler %4.0f ns each), %6.1f minor faults, writes %7.1f us, "
        "scan %6.1f us, %6.1f pages reported; a second: %7.0f faults, handler %5.1f ms, %5.0f scans taking %5.1f ms\n",
        arm, hot, cold, (double)r->faults / epochs, r->faults ? (double)r->handler_ns / (double)r->faults : 0.0,
        (double)r->minflt / epochs, (double)r->write_ns / epochs / 1e3, (double)r->scan_ns / epochs / 1e3,
        (double)r->reported / epochs, (double)r->faults / sec, (double)r->handler_ns / sec / 1e6, epochs / sec,
        (double)r->scan_ns / sec / 1e6);
}
static void bench_ww(void) {
    static const u32 patterns[][2] = {{64, 16}, {256, 64}, {1024, 64}}; /* hot, cold pages an epoch */
    const u32 epochs = 200;
    struct sigaction mine;
    u8 *hot_table = g_wp_hot, *own = NULL;
    int wrapped = 0;
    printf("written-page tracking, benchmarked: %u epochs a pattern over %u pages\n", epochs, BENCH_PAGES);
    u8* base = plat_vm_reserve((u64)1 << 30);
    CHECK(base != NULL, "no reservation");
    if (!base) return;
    plat_vm_track(base, (u64)1 << 30);
    CHECK(!plat_vm_commit(base, BENCH_PAGES * 4096ull), "commit failed");
    g_bench.lo = base;
    if (g_settings.write_tracking == 2 || plat_ww_scan(base, 4096, bench_seen) < 0) {
        printf("  no written-page tracking here (write_tracking=%d): nothing to measure\n", g_settings.write_tracking);
        return;
    }
    /* where the tracking protects pages its faults go through the process's handler (fault_signal), which a handler
       of the bench's own wraps; userfaultfd's stay in the kernel, and the minor faults count them */
    sigaction(SIGSEGV, NULL, &g_bench.segv);
    sigaction(SIGBUS, NULL, &g_bench.bus);
    if ((g_bench.segv.sa_flags & SA_SIGINFO) && (g_bench.bus.sa_flags & SA_SIGINFO)) {
        mine = g_bench.segv;
        mine.sa_sigaction = bench_signal;
        sigaction(SIGSEGV, &mine, NULL);
        mine = g_bench.bus;
        mine.sa_sigaction = bench_signal;
        sigaction(SIGBUS, &mine, NULL);
        wrapped = 1;
    }
    if (g_wp_track && !hot_table) {
        own = (u8*)mmap(NULL, (size_t)g_wp_pages, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                        -1, 0);
        CHECK(own != MAP_FAILED, "no table of counts");
        if (own == MAP_FAILED) own = NULL;
        hot_table = own;
    }
    printf("  %s\n", g_wp_track ? "page protection: arm protect is write_tracking_hot=0, arm hot is 1"
                                : "the host's own tracking (userfaultfd): hot pages are protection's alone");
    for (size_t k = 0; k < sizeof patterns / sizeof *patterns; k++) {
        u32 hot = patterns[k][0], cold = patterns[k][1];
        BenchArm prot, warm;
        g_wp_hot = NULL;
        bench_arm(&prot, hot, cold, epochs);
        bench_print(g_wp_track ? "protect" : "own", hot, cold, epochs, &prot);
        CHECK(prot.missed == 0, "a scan missed a page written before it");
        if (!g_wp_track || !hot_table) continue;
        memset(hot_table, 0, (size_t)g_wp_pages); /* counted from zero */
        __atomic_store_n(&g_wp_hot_n, 0, __ATOMIC_RELAXED);
        g_wp_hot = hot_table;
        bench_arm(&warm, hot, cold, epochs);
        g_wp_hot = NULL;
        bench_print("hot", hot, cold, epochs, &warm);
        CHECK(warm.missed == 0, "a scan missed a page written before it, with hot pages");
        CHECK(warm.faults < prot.faults, "hot pages took no fewer faults than protection alone");
        printf("  hot against protect: %.0f%% of the faults, %.0f%% of the run's time\n",
               100.0 * (double)warm.faults / (double)(prot.faults ? prot.faults : 1),
               100.0 * (double)warm.ns / (double)(prot.ns ? prot.ns : 1));
    }
    if (hot_table) memset(hot_table, 0, (size_t)g_wp_pages);
    __atomic_store_n(&g_wp_hot_n, 0, __ATOMIC_RELAXED);
    g_wp_hot = own ? NULL : hot_table; /* the setting's table, if any, counted from zero */
    if (own) munmap(own, (size_t)g_wp_pages);
    if (wrapped) {
        sigaction(SIGSEGV, &g_bench.segv, NULL);
        sigaction(SIGBUS, &g_bench.bus, NULL);
    }
}

/* a start's mark that an earlier run of this test left: nothing to check of it */
static void mark_seen(void* data, const SnxRunMark* mark) {
    (void)data;
    (void)mark;
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
    int i, arg = 7, bench = 0;
    u64 t;

    setvbuf(stdout, NULL, _IONBF, 0); /* each section says it started, so a crash shows where it happened */
    for (i = 2; i < argc; i++) {
        const char* eq = strchr(argv[i], '=');
        char key[64];
        if (!strcmp(argv[i], "--bench-ww")) { /* the benchmark alone (bench_ww) */
            bench = 1;
            continue;
        }
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
    if (bench) {
        if (sysconf(_SC_PAGESIZE) == 4096)
            bench_ww();
        else
            printf("benchmark skipped: this host's pages are %ld bytes, guest memory's 4096\n", sysconf(_SC_PAGESIZE));
        printf("%u checks, %u failed\n", checks, failures);
        return failures != 0;
    }
    test_host_page();
    test_foreign();
    test_memory_watch();
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
    /* replace: dst gets tmp's contents; a failing replace leaves tmp for the caller, dst untouched */
    CHECK(!plat_replace_file(tmp, path) && !stat(path, &st) && st.st_size == 0 && !fopen(tmp, "rb"), "replace");
    fclose(fopen(tmp, "wb"));
    snprintf(sub, sizeof sub, "%s/platform_posix.none/x", dir);
    CHECK(plat_replace_file(tmp, sub) == -1 && !stat(tmp, &st) && stat(sub, &st), "a failed replace kept tmp");
    remove(tmp);

    printf("folders\n");
    snprintf(sub, sizeof sub, "%s/platform_posix.dir", dir);
    plat_mkdir(sub);
    CHECK(plat_is_dir(sub) && !plat_is_dir(path), "plat_is_dir");

    printf("libraries\n");
    /* a library the process hasn't loaded isn't found, and isn't loaded by the look (twice: a look that loaded it
       finds it the second time); once opened it is, and its function through that */
    CHECK(!plat_dl_loaded(SNX_TEST_DL_PROBE) && !plat_dl_loaded(SNX_TEST_DL_PROBE), "a library not loaded was found");
    {
        void* opened = plat_dl_open(SNX_TEST_DL_PROBE);
        void* found = plat_dl_loaded(SNX_TEST_DL_PROBE);
        int (*probe)(void) = NULL;
        void* sym = found ? plat_dl_sym(found, "snx_test_dl_probe") : NULL;
        memcpy(&probe, &sym, sizeof probe);
        CHECK(opened && found && probe && probe() == 42, "a library opened was not found, or its function");
    }

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

    /* an exit through plat_exit_now deletes the start's mark (common/utilities/run_mark.h), which _exit would leave */
    printf("start mark\n");
    {
        pid_t child = fork();
        int status = 0;
        char why[1300];
        if (!child) {
            if (snx_run_mark_start(dir, mark_seen, NULL, why, sizeof why)) _exit(9);
            plat_exit_now(5);
        }
        CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 5,
              "the child did not make its mark and exit through plat_exit_now");
        snprintf(path, sizeof path, "%s/.running-%ld", dir, (long)child);
        CHECK(access(path, F_OK) && errno == ENOENT, "plat_exit_now left the start's mark");
    }

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
