/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The POSIX platform layer (platform/plat_posix.c, with plat_sdl.c): written-page tracking, decommit zeroing, pread,
   file maps, replace, folders, timed waits and threads. Written-page tracking needs Linux 6.7; without it the scan must say -1 */
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
        /* the kernel writing guest memory (read() into it) counts too */
        snprintf(path, sizeof path, "%s/platform_posix.bin", dir);
        f = fopen(path, "wb");
        fwrite("0123456789", 1, 10, f);
        fclose(f);
        fd = open(path, O_RDONLY);
        CHECK(read(fd, g_base + 7 * 4096, 10) == 10, "read into guest memory failed");
        close(fd);
        CHECK(scan() == 1ull << 7, "a read() into page 7 was not reported");
    }
    printf("decommit\n");
    g_base[9 * 4096] = 9;
    plat_vm_decommit(g_base, 64 * 4096);
    CHECK(!plat_vm_commit(g_base, 64 * 4096) && g_base[9 * 4096] == 0, "memory not zero after decommit");

    printf("populate\n");
    {
        u8* p = g_base + (1 << 20);
        CHECK(!plat_vm_commit(p, 16 * 4096), "commit failed");
#ifdef MADV_POPULATE_WRITE /* Linux, whose mincore takes unsigned chars (other hosts' take char) */
        unsigned char in[16];
        CHECK(!mincore(p, 16 * 4096, in) && !(in[0] & 1) && !(in[15] & 1), "committed pages resident before a write");
        if (plat_vm_populate(p, 16 * 4096)) { /* a kernel before 5.14 */
            printf("populate unavailable here (%s): only checking that it says so\n", strerror(errno));
            CHECK(errno == EINVAL, "populate failed with another error");
        } else {
            int all = 1;
            CHECK(!mincore(p, 16 * 4096, in), "mincore failed");
            for (int i = 0; i < 16; i++) all &= in[i] & 1;
            CHECK(all && p[5 * 4096] == 0, "populated pages not resident, or not zero");
        }
#else
        CHECK(plat_vm_populate(p, 16 * 4096) == -1 && errno == ENOSYS, "populate did not say this host can't");
#endif
    }
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
