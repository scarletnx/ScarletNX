/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The POSIX platform layer (platform/plat_posix.c, with plat_sdl.c): written-page tracking, decommit zeroing, pread,
   file maps, replace, folders, timed waits and threads. Written-page tracking needs Linux 6.7 for userfaultfd's;
   SNX_WW=signal (and every host without it) tracks by protection, and SNX_NO_WW must make the scan say -1 */
#include "nx/runtime/platform/plat_posix.c"
#include <stdarg.h>
#include <stdio.h>
#include <sys/wait.h>

void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
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
    CHECK(!plat_vm_commit(g_base, 64 * 4096), "commit failed");
    if (getenv("SNX_NO_WW") || plat_ww_scan(g_base, 4096, on_written) < 0) {
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
