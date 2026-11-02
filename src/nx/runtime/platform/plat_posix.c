/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The platform layer on Linux, Android, FreeBSD and Apple systems: threads, condition variables, the guest's address
   space with written-page tracking, file reads and maps, the environment and the crash handler
   (plat_sdl.c has the clocks, sleeping, libraries and path queries) */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* REG_RIP, pipe2 */
#endif
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include <SDL3/SDL_thread.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#if defined(__linux__)
#include <linux/falloc.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#if defined(__aarch64__)
#include <asm/sigcontext.h> /* the signal frame's ESR record (context_write) */
#endif
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/umtx.h>
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0 /* FreeBSD never reserves swap for a mapping */
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* the crash report needs about 12 KiB of stack; a thread whose own stack overflowed has none left */
#define ALT_STACK_SIZE 32768
/* below this a thread is a small helper that gets no alternate signal stack */
#define ALT_STACK_MIN_THREAD ((size_t)256 << 10)
/* the least stack a thread gets: FreeBSD's PTHREAD_STACK_MIN (2 KiB on amd64) is less than its dynamic linker takes
   to bind the thread's first call, so never under glibc's 16 KiB */
#define THREAD_STACK_FLOOR   ((size_t)PTHREAD_STACK_MIN > 16384 ? (size_t)PTHREAD_STACK_MIN : (size_t)16384)

/* ================================================================ condition variables */
void pc_init(PCond* c) {
#if defined(__APPLE__)
    pthread_cond_init(&c->c, NULL); /* Darwin has no clock attribute: timed waits are relative there */
#else
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC); /* deadlines don't move when the wall clock is set */
    pthread_cond_init(&c->c, &a);
    pthread_condattr_destroy(&a);
#endif
}
void pc_signal(PCond* c) { pthread_cond_signal(&c->c); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
static int cond_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    struct timespec ts;
    int r;
    if (timeout_ns < 0) {
        pthread_cond_wait(&c->c, &m->l);
        return 0;
    }
#if defined(__APPLE__)
    ts.tv_sec = (time_t)(timeout_ns / 1000000000);
    ts.tv_nsec = (long)(timeout_ns % 1000000000);
    r = pthread_cond_timedwait_relative_np(&c->c, &m->l, &ts);
#else
    {
        u64 sec;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        sec = (u64)timeout_ns / 1000000000u;
        ts.tv_nsec += (long)((u64)timeout_ns % 1000000000u);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            sec++;
        }
        ts.tv_sec = (time_t)((u64)ts.tv_sec + sec);
    }
    r = pthread_cond_timedwait(&c->c, &m->l, &ts);
#endif
    return r == ETIMEDOUT;
}
/* the wait noted while it lasts, for a hang report (pm_waits) */
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    int r;
    pm_wait_note(m, __builtin_return_address(0), 1);
    r = cond_wait(c, m, timeout_ns);
    pm_wait_note(NULL, NULL, 0);
    return r;
}

#if defined(__APPLE__)
/* libSystem's, which libc++'s atomic waits use too: the public os_sync_wait_on_address needs macOS 14.4 */
extern int __ulock_wait(u32 operation, void* addr, u64 value, u32 timeout_us);
extern int __ulock_wake(u32 operation, void* addr, u64 wake_value);
#define SNX_UL_COMPARE_AND_WAIT 1u
#define SNX_ULF_WAKE_ALL        0x100u
#define SNX_ULF_NO_ERRNO        0x1000000u
#endif
void plat_wait_word(u32* w, u32 v, i64 timeout_ns) {
#if defined(__linux__)
    struct timespec ts;
    if (timeout_ns >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000);
        ts.tv_nsec = (long)(timeout_ns % 1000000000);
    }
    syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, v, timeout_ns >= 0 ? &ts : NULL, NULL, 0);
#elif defined(__FreeBSD__)
    struct _umtx_time ut = {0};
    if (timeout_ns < 0) {
        _umtx_op(w, UMTX_OP_WAIT_UINT_PRIVATE, v, NULL, NULL);
        return;
    }
    ut._timeout.tv_sec = (time_t)(timeout_ns / 1000000000);
    ut._timeout.tv_nsec = (long)(timeout_ns % 1000000000);
    ut._clockid = CLOCK_MONOTONIC;
    _umtx_op(w, UMTX_OP_WAIT_UINT_PRIVATE, v, (void*)(uintptr_t)sizeof ut, &ut);
#else
    /* 0 waits without limit: a timeout is at least a microsecond, and at most what the 32 bits hold */
    u64 us = timeout_ns < 0 ? 0 : (u64)timeout_ns / 1000u + 1u;
    __ulock_wait(SNX_UL_COMPARE_AND_WAIT | SNX_ULF_NO_ERRNO, w, v, us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)us);
#endif
}
void plat_wake_word(u32* w) {
#if defined(__linux__)
    syscall(SYS_futex, w, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#elif defined(__FreeBSD__)
    _umtx_op(w, UMTX_OP_WAKE_PRIVATE, INT_MAX, NULL, NULL);
#else
    __ulock_wake(SNX_UL_COMPARE_AND_WAIT | SNX_ULF_WAKE_ALL | SNX_ULF_NO_ERRNO, w, 0);
#endif
}

/* ================================================================ threads */
typedef struct {
    PThreadFn fn;
    void* arg;
    int alt_stack;
} ThreadStart;

static void alt_stack_off(void) {
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
}

/* the alternate stack lives in this frame for as long as fn runs, and is switched off before the frame goes. Not
   inlined: in thread_start's frame the array would take its room on every thread, the small ones too */
__attribute__((noinline)) static void run_with_alt_stack(PThreadFn fn, void* arg) {
    char stack[ALT_STACK_SIZE];
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = stack;
    ss.ss_size = sizeof stack;
    sigaltstack(&ss, NULL);
    fn(arg);
    alt_stack_off();
}

static void wp_thread_start(void);
static void* thread_start(void* p) {
    ThreadStart s = *(ThreadStart*)p;
    free(p);
    wp_thread_start();
    if (s.alt_stack)
        run_with_alt_stack(s.fn, s.arg);
    else
        s.fn(s.arg);
    SDL_CleanupTLS(); /* SDL's per-thread state (plat_sdl.c's calls set its error text) */
    return NULL;
}

#if defined(__linux__) && !defined(__ANDROID__)
/* every CPU the process may use (plat_cpus), which plat_thread gives each thread it starts: one a game thread started
   would otherwise run only on the share of that thread's core (plat_thread_cpus) */
static cpu_set_t g_all_cpus;
#endif
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    ThreadStart* s = malloc(sizeof *s);
    pthread_attr_t a;
    pthread_t t;
    int r;
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    s->alt_stack = stack == 0 || stack >= ALT_STACK_MIN_THREAD;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
#if defined(__linux__) && !defined(__ANDROID__)
    if (CPU_COUNT(&g_all_cpus)) pthread_attr_setaffinity_np(&a, sizeof g_all_cpus, &g_all_cpus);
#endif
    if (stack) {
        /* smaller than the minimum, setstacksize fails and the default would be used silently */
        if (stack < THREAD_STACK_FLOOR) stack = THREAD_STACK_FLOOR;
#if defined(__APPLE__)
        {
            size_t page = (size_t)sysconf(_SC_PAGESIZE); /* 16 KiB on Apple silicon; Darwin wants whole pages */
            stack = (stack + page - 1) / page * page;
        }
#endif
        pthread_attr_setstacksize(&a, stack);
    }
    r = pthread_create(&t, &a, thread_start, s);
    pthread_attr_destroy(&a);
    if (r) {
        free(s);
        errno = r; /* the reason, for what the caller says */
        return -1;
    }
    return 0;
}
void plat_thread_exit(void) {
    /* pthread_exit leaves run_with_alt_stack's frame without returning through it */
    alt_stack_off();
    SDL_CleanupTLS();
    pthread_exit(NULL);
}
/* Linux only: Android's CPUs differ in speed, so a fixed share of them would make a core slow */
#if defined(__linux__) && !defined(__ANDROID__)
int plat_cpus(int* ids, int max) {
    int c, n = 0;
    if (sched_getaffinity(0, sizeof g_all_cpus, &g_all_cpus)) {
        CPU_ZERO(&g_all_cpus);
        return 0;
    }
    for (c = 0; c < CPU_SETSIZE && n < max; c++)
        if (CPU_ISSET(c, &g_all_cpus)) ids[n++] = c;
    return n;
}
void plat_thread_cpus(const int* ids, int n) {
    cpu_set_t s;
    int i;
    CPU_ZERO(&s);
    for (i = 0; i < n; i++) CPU_SET(ids[i], &s);
    if (sched_setaffinity(0, sizeof s, &s))
        WARN_LIMIT(1, "Could not move a game thread to its CPUs: %s\n", strerror(errno));
}
#else
int plat_cpus(int* ids, int max) {
    (void)ids;
    (void)max;
    return 0;
}
void plat_thread_cpus(const int* ids, int n) {
    (void)ids;
    (void)n;
}
#endif
#if defined(__linux__) && !defined(__ANDROID__)
/* the text after "key" (and a '=' or ':' with the spaces around it) on the first line of path that starts with it,
   unquoted; out empty when there's none */
static void file_value(const char* path, const char* key, char* out, size_t cap) {
    char line[512];
    size_t k = strlen(key), n;
    FILE* f = fopen(path, "r");
    out[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char* v = line + k;
        if (strncmp(line, key, k)) continue;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '=' && *v != ':') continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '"') v++;
        n = strcspn(v, "\"\n");
        if (n >= cap) n = cap - 1;
        memcpy(out, v, n);
        out[n] = 0;
        break;
    }
    fclose(f);
}
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
static void sysctl_text(const char* name, char* out, size_t cap) {
    size_t n = cap;
    if (sysctlbyname(name, out, &n, NULL, 0) || !n) n = 1;
    out[n < cap ? n - 1 : cap - 1] = 0;
}
#endif
void plat_system_info(char* out, size_t cap) {
    char os[128] = {0}, cpu[128] = {0}, *c = cpu;
    struct utsname u;
    unsigned long long ram = 0;
    long threads = sysconf(_SC_NPROCESSORS_ONLN);
    size_t n;
    if (uname(&u)) memset(&u, 0, sizeof u);
#if defined(__APPLE__)
    {
        char v[32];
        size_t len = sizeof ram;
        sysctl_text("kern.osproductversion", v, sizeof v);
        snprintf(os, sizeof os, "%s %s (Darwin %s)", TARGET_OS_IPHONE ? "iOS" : "macOS", v, u.release);
        sysctl_text("machdep.cpu.brand_string", cpu, sizeof cpu);
        if (sysctlbyname("hw.memsize", &ram, &len, NULL, 0)) ram = 0;
    }
#else
#if defined(__ANDROID__)
    {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("ro.build.version.release", v);
        snprintf(os, sizeof os, "Android %s (Linux %s)", v, u.release);
        __system_property_get("ro.soc.model", cpu);
    }
#elif defined(__linux__)
    {
        char name[96];
        file_value("/etc/os-release", "PRETTY_NAME", name, sizeof name);
        if (!name[0]) file_value("/usr/lib/os-release", "PRETTY_NAME", name, sizeof name);
        snprintf(os, sizeof os, "%s (Linux %s)", name[0] ? name : "Linux", u.release);
        file_value("/proc/cpuinfo", "model name", cpu, sizeof cpu); /* x86's; Arm's /proc/cpuinfo names no model */
    }
#else
    snprintf(os, sizeof os, "%s %s", u.sysname, u.release);
#if defined(__FreeBSD__)
    sysctl_text("hw.model", cpu, sizeof cpu);
#endif
#endif
    {
        long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page > 0) ram = (unsigned long long)pages * (unsigned long long)page;
    }
#endif
    while (*c == ' ') c++; /* some CPUs pad their brand string */
    for (n = strlen(c); n && c[n - 1] == ' '; n--) c[n - 1] = 0;
    snprintf(out, cap, "%s%s%s, %ld threads, %llu GB RAM", os, *c ? ", " : "", c, threads > 0 ? threads : 0,
             (ram + (512ull << 20)) >> 30);
}
void plat_yield(void) { sched_yield(); }
void plat_exit_now(int code) {
    fflush(NULL);
    _exit(code);
}

/* ================================================================ the guest's address space */
/* Written-page tracking (Linux 6.7 and later): the reservation is registered with a userfaultfd in asynchronous
   write-protect mode, so the kernel unprotects a page on its first write (the guest's, or a read() into guest memory)
   with no handler thread, and PAGEMAP_SCAN lists the written pages and protects them again in one call. The
   userfaultfd is kept open for the whole run; only the ranges a cache scans are ever protected again. Registration
   belongs to a mapping: a range mapped again with MAP_FIXED (the alias pool's, below) is registered again, or a scan
   over it fails. g_pagemap goes back to -1 when that fails, so every scan says -1 and the caches compare contents */
static int g_pagemap = -1;
#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY)
#define SNX_UFFD 1
static int g_uffd = -1;
#if defined(UFFD_FEATURE_MINOR_SHMEM) && defined(UFFDIO_CONTINUE)
#define SNX_UFFD_MINOR 1
#endif
#endif

#if SNX_UFFD_MINOR
static int g_uffd_minor; /* the userfaultfd takes minor faults on shared memory: traps work (plat_trap_init) */
#endif
static int g_uffd_minor_on(void) {
#if SNX_UFFD_MINOR
    return g_uffd_minor;
#else
    return 0;
#endif
}

static int ww_register(u8* p, u64 size) {
#if SNX_UFFD
    struct uffdio_register reg;
    if (g_uffd < 0) return 0;
    memset(&reg, 0, sizeof reg);
    reg.range.start = (u64)(uintptr_t)p;
    reg.range.len = size;
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    return ioctl(g_uffd, UFFDIO_REGISTER, &reg) ? -1 : 0;
#else
    (void)p;
    (void)size;
    return 0;
#endif
}

#if SNX_UFFD
/* a userfaultfd with these features (faults from user mode only: the unprivileged kind), or -1 */
static int uffd_open(u64 features) {
    struct uffdio_api api;
    long fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (fd < 0) return -1;
    memset(&api, 0, sizeof api);
    api.api = UFFD_API;
    api.features = features;
    if (ioctl((int)fd, UFFDIO_API, &api)) {
        close((int)fd);
        return -1;
    }
    return (int)fd;
}
#endif

/* One userfaultfd serves the written-page tracking and the traps (a mapping is registered with one at most). Without
   minor faults (before Linux 5.13) it does the tracking alone; SNX_NO_WW leaves it the traps alone. Where it can't
   track writes, protection does (wp_init, below) */
static void wp_init(u8* p, u64 size, int track);
/* SNX_TRAPS=signal: traps by protection (plat_trap_init) on a Linux whose userfaultfd could take them */
static int traps_by_signal(void) {
    const char* how = getenv("SNX_TRAPS");
    return how && !strcmp(how, "signal");
}
static int g_uffd_minor_on(void);
static void ww_init(u8* p, u64 size) {
    const char* how = getenv("SNX_WW");
    int no_ww = getenv("SNX_NO_WW") != NULL;
#if SNX_UFFD
    int fd = -1, pm;
    u64 wp = no_ww || (how && !strcmp(how, "signal")) ? 0 : UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
#if SNX_UFFD_MINOR
    fd = uffd_open(wp | UFFD_FEATURE_MINOR_SHMEM | UFFD_FEATURE_THREAD_ID);
    g_uffd_minor = fd >= 0;
#endif
    if (fd < 0 && wp) fd = uffd_open(wp);
    if (fd >= 0) {
        g_uffd = fd; /* before any other thread exists: mem_init runs on the main thread at startup */
        if (wp && !ww_register(p, size)) {
            pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
            if (pm >= 0) {
                g_pagemap = pm;
                if (traps_by_signal()) wp_init(p, size, 0); /* the states for the traps alone */
                return;
            }
            {
                struct uffdio_range r = {(u64)(uintptr_t)p, size};
                ioctl(g_uffd, UFFDIO_UNREGISTER, &r);
            }
        }
    }
#endif
    (void)how;
    /* the states also serve traps by protection, where userfaultfd's minor faults don't (plat_trap_init) */
    if (!no_ww || !g_uffd_minor_on() || traps_by_signal()) wp_init(p, size, !no_ww);
}
/* a range mapped again: registered again, or tracking off for the run (the pagemap stays open, as a scan on another
   thread may be using it) */
static void ww_remapped(u8* p, u64 size) {
    if (__atomic_load_n(&g_pagemap, __ATOMIC_ACQUIRE) < 0 || !ww_register(p, size)) return;
    __atomic_store_n(&g_pagemap, -1, __ATOMIC_RELEASE);
    WARN("memory: could not watch remapped guest memory for writes (error %d): caches compare contents now\n", errno);
}

/* Written-page tracking by protection, where the kernel tracks no writes (macOS, FreeBSD, Android, Linux before 6.7;
   SNX_WW=signal takes it on a later Linux, to test it): a scan makes the written pages it reports read only, and the
   first write to one faults. The fault handler (wp_fault, on the faulting thread) makes the page writable again and
   notes it written, and the write goes on. Each host page of the reservation has a state byte: WP_MAPPED while the
   reservation's own protection lets it be written (committed, or a view of the pool), WP_CLEAN while no write came
   since a scan reported it, WP_PROT while the tracking holds it read only, and WP_LOCK while one thread changes its
   protection: a scan, a fault, or a mapping change (plat_vm_commit and the others), which the tracking's protection
   must not undo nor outlive. A page without WP_PROT is never WP_CLEAN. WP_ARMED marks a page a trap holds
   (plat_trap_arm): scans skip it, as Linux's skip a trapped range, since its consumers look the ownership up instead,
   and it reads as written once the trap lets it go. A system call doesn't fault into the handler:
   it fails with EFAULT on a read-only page, so the runtime opens what the kernel will write first (plat_ww_open), and
   scans leave those pages alone until it is done (plat_ww_close). Each run of protected pages is a mapping of its own
   to the kernel, and Linux limits a process's mappings (vm.max_map_count): there the tracking and the traps hold no
   more pages than half what the limit leaves free (a trap's pages count too, as their runs split mappings as well),
   and a written page past that reads as written at every scan. When a protection change fails for want of mappings
   anyway (ENOMEM: the rest of the process took more than the limit assumed), the budget comes down to half what is
   held then and the tracking lets every page it holds go (wp_shed), which only ever merges mappings */
#define WP_MAPPED 1u
#define WP_CLEAN  2u
#define WP_PROT   4u
#define WP_LOCK   8u
#define WP_ARMED  16u
#define WP_TRAP   32u /* inaccessible for a trap by protection (plat_trap_arm): a fault waits for its landing */
static u8* g_wp; /* the states, a byte per host page of the reservation; NULL without this tracking or the traps' */
static int g_wp_track; /* scans are the states' (written-page tracking by protection) */
static u8* g_wp_base;
static u64 g_wp_pages;
static unsigned g_wp_shift; /* the host's page, log2 */
static u64 g_wp_budget;     /* atomic: pages the tracking and the traps may hold at once */
static u64 g_wp_held;       /* atomic: pages they hold now (WP_PROT or WP_TRAP) */
static u32 g_wp_opened;     /* atomic: moves whenever pages become writable (a fault's, a mapping change's) */
/* this thread's state. The fault handlers read it, so wp_thread_start touches it before the thread can fault: on
   Darwin a thread's first access to a _Thread_local variable may allocate, which a signal handler mustn't */
static _Thread_local struct {
    /* the address a fault on a writable page was let go again at, and g_wp_opened then */
    void* wp_retried;
    u32 wp_retried_at;
    void* frz_retried; /* the address a thawed write was let go again at once (frozen_wait) */
    int crashed;       /* this thread faulted already */
#if defined(__linux__)
    u32 tid; /* plat_tid's */
#endif
} t_plat_posix;
/* a range frozen while it is copied (plat_freeze, below) */
static u8 *g_frz_lo, *g_frz_hi;
static int g_frozen; /* a futex word: 1 while frozen */
static void fault_handler_install(void);
/* Ranges the kernel is writing now (plat_ww_open to plat_ww_close), which scans don't protect again. A slot is claimed
   (busy 1), filled, then published (busy 2) before any of its pages is opened, so a scan that locks such a page finds
   it published */
#define WW_OPEN_N 32
static int g_ww_open_busy[WW_OPEN_N]; /* atomic */
static u8* g_ww_open_lo[WW_OPEN_N];
static u8* g_ww_open_hi[WW_OPEN_N];
static int g_ww_open_n; /* atomic: slots claimed, to skip the search */

/* touches t_plat_posix, which the fault handlers use, before the thread can fault */
static void wp_thread_start(void) {
    __atomic_store_n(&t_plat_posix.wp_retried, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&t_plat_posix.wp_retried_at, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&t_plat_posix.frz_retried, NULL, __ATOMIC_RELAXED);
    (void)plat_tid();
}

static u8 wp_lock(u64 i) {
    u8 s = __atomic_load_n(&g_wp[i], __ATOMIC_RELAXED);
    for (;;) {
        if (s & WP_LOCK) { /* held by a scan, a mapping change or another fault, for one system call */
            sched_yield();
            s = __atomic_load_n(&g_wp[i], __ATOMIC_RELAXED);
        } else if (__atomic_compare_exchange_n(&g_wp[i], &s, (u8)(s | WP_LOCK), 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return s;
    }
}
static void wp_unlock(u64 i, u8 s) { __atomic_store_n(&g_wp[i], s, __ATOMIC_RELEASE); }
/* the host pages [*lo, *hi) of [p, p + size), within the reservation: 0, or -1 when there are none */
static int wp_pages(const u8* p, u64 size, u64* lo, u64* hi) {
    u64 mask = ((u64)1 << g_wp_shift) - 1, a, e, end = g_wp_pages << g_wp_shift;
    if (!g_wp || !size || p >= g_wp_base + end || p + size <= g_wp_base) return -1;
    a = p > g_wp_base ? (u64)(p - g_wp_base) : 0;
    e = (u64)(p + size - g_wp_base);
    if (e > end) e = end;
    *lo = a >> g_wp_shift;
    *hi = (e + mask) >> g_wp_shift;
    return 0;
}
/* locks the states of [p, p + size) for a change of the pages' protection that leaves them in state to (wp_let). A
   page without a state that gets none isn't locked: only mapping changes give one, and mem.c makes one at a time */
static void wp_hold(const u8* p, u64 size, u8 to) {
    u64 i, lo, hi;
    if (wp_pages(p, size, &lo, &hi)) return;
    for (i = lo; i < hi; i++)
        if (to || __atomic_load_n(&g_wp[i], __ATOMIC_RELAXED)) wp_lock(i);
}
/* the change wp_hold locked them for is done: the pages are in state to now (changed), or as they were but written:
   a change that failed partway (an mprotect, a mapping) may have made some writable */
static void trap_wake(void);
static void wp_let(const u8* p, u64 size, u8 to, int changed) {
    u64 i, lo, hi, freed = 0;
    int woken = 0;
    if (wp_pages(p, size, &lo, &hi)) return;
    /* before the first page is let go: a fault that finds one writable must find the count moved (wp_fault) */
    if (changed && to) __atomic_fetch_add(&g_wp_opened, 1, __ATOMIC_SEQ_CST);
    for (i = lo; i < hi; i++) {
        u8 s = __atomic_load_n(&g_wp[i], __ATOMIC_RELAXED);
        if (!to && !s) continue;
        if (changed && (s & (WP_PROT | WP_TRAP))) freed++;
        if (changed && (s & WP_TRAP)) woken = 1;
        wp_unlock(i, changed ? to : (u8)(s & ~(WP_LOCK | WP_CLEAN)));
    }
    if (freed) __atomic_fetch_sub(&g_wp_held, freed, __ATOMIC_RELAXED);
    if (woken) trap_wake();
}
/* whether a is in the range frozen now: the freeze's to answer (frozen_wait) */
static int frozen_at(const void* a) {
    return __atomic_load_n(&g_frozen, __ATOMIC_SEQ_CST) &&
           (const u8*)a >= __atomic_load_n(&g_frz_lo, __ATOMIC_ACQUIRE) &&
           (const u8*)a < __atomic_load_n(&g_frz_hi, __ATOMIC_ACQUIRE);
}
/* whether the kernel is writing page i now (plat_ww_open) */
static int wp_kernel_writing(u64 i) {
    u8* a = g_wp_base + (i << g_wp_shift);
    u8* e = a + ((u64)1 << g_wp_shift);
    int k;
    if (!__atomic_load_n(&g_ww_open_n, __ATOMIC_SEQ_CST)) return 0;
    for (k = 0; k < WW_OPEN_N; k++)
        if (__atomic_load_n(&g_ww_open_busy[k], __ATOMIC_SEQ_CST) == 2 && g_ww_open_lo[k] < e && g_ww_open_hi[k] > a)
            return 1;
    return 0;
}
/* a protection change failed for want of mappings: the budget comes down to half what is held, and every page the
   tracking holds read only is let go, which merges mappings and splits none (a run of them is whole mappings, but
   where it meets a frozen page: a run that fails is tried again once the others merged). Runs only atomics and
   mprotect, so a fault handler may call it, holding no page's lock */
static void wp_shed(void) {
    u64 i, run = 0, n = 0, k, pass;
    __atomic_store_n(&g_wp_budget, __atomic_load_n(&g_wp_held, __ATOMIC_RELAXED) / 2, __ATOMIC_RELAXED);
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i <= g_wp_pages; i++) {
            if (i < g_wp_pages && (__atomic_load_n(&g_wp[i], __ATOMIC_RELAXED) & WP_PROT)) {
                if (wp_lock(i) & WP_PROT) {
                    if (!n) run = i;
                    n++;
                    continue;
                }
                wp_unlock(i, (u8)(__atomic_load_n(&g_wp[i], __ATOMIC_RELAXED) & ~WP_LOCK));
            }
            if (!n) continue;
            if (!mprotect(g_wp_base + (run << g_wp_shift), (size_t)(n << g_wp_shift), PROT_READ | PROT_WRITE)) {
                __atomic_fetch_sub(&g_wp_held, n, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_wp_opened, 1, __ATOMIC_SEQ_CST);
                for (k = run; k < run + n; k++)
                    wp_unlock(k, (u8)((__atomic_load_n(&g_wp[k], __ATOMIC_RELAXED) & WP_ARMED) | WP_MAPPED));
            } else
                for (k = run; k < run + n; k++)
                    wp_unlock(k, (u8)(__atomic_load_n(&g_wp[k], __ATOMIC_RELAXED) & ~(WP_LOCK | WP_CLEAN)));
            n = 0;
        }
}
/* n more pages held, within the budget: 1, or 0 when it has no room for them */
static int wp_reserve(u64 n) {
    u64 held = __atomic_load_n(&g_wp_held, __ATOMIC_RELAXED);
    do {
        if (held + n > __atomic_load_n(&g_wp_budget, __ATOMIC_RELAXED)) return 0;
    } while (!__atomic_compare_exchange_n(&g_wp_held, &held, held + n, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return 1;
}
/* Traps by protection (Linux without userfaultfd's minor faults, or SNX_TRAPS=signal): plat_trap_arm makes the
   pool pages inaccessible (WP_TRAP), and a guest access faults into the handler (wp_fault) on the faulting thread.
   That thread posts the page to the trap thread, which calls the fault callback in ordinary thread context, and waits
   in the handler, on a futex, until some trap lets pages go (g_trap_gen moves); then the access goes again, and faults
   again if its page is still held. The callback lands the page through the mirror and calls plat_trap_continue, which
   opens the pages. Nothing but atomics, mprotect and futexes runs in the handler */
static int g_trap_signal; /* plat_trap_* are this backend's */
static void (*g_trap_cb)(u8* p, int write, u32 tid);
#define TRAP_RING 256u
typedef struct {
    u8* p;
    u32 tid, seq; /* seq: the Vyukov queue's turn */
    int write;
} TrapMsg;
static TrapMsg g_trap_ring[TRAP_RING];
static u32 g_trap_head;   /* atomic: the next slot a fault claims */
static u32 g_trap_tail;   /* the trap thread's */
static u32 g_trap_posted; /* atomic, a futex word: moves with each message */
static u32 g_trap_gen;    /* atomic, a futex word: moves when a trap lets pages go */
#define TRAP_WAITERS 256
static u32 g_trap_waiting[TRAP_WAITERS]; /* atomic: the threads waiting in a trap, for plat_thread_in_fault */
static u32 g_trap_unlisted;              /* atomic: threads waiting with no room in that table, which makes it unsure */

static void trap_futex_wait(u32* w, u32 v) {
#if defined(__linux__)
    syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
#else
    (void)w, (void)v;
    sched_yield();
#endif
}
static void trap_futex_wake(u32* w) {
#if defined(__linux__)
    syscall(SYS_futex, w, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#else
    (void)w;
#endif
}
/* in the handler: page i is held by a trap. Posts it and waits for a trap to let pages go */
static void trap_wait(u64 i, int write) {
    u32 gen = __atomic_load_n(&g_trap_gen, __ATOMIC_SEQ_CST), tid = plat_tid(), pos, k, none = 0;
    int slot = -1;
    TrapMsg* m;
    for (k = 0; k < TRAP_WAITERS && slot < 0; k++, none = 0)
        if (__atomic_compare_exchange_n(&g_trap_waiting[k], &none, tid, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            slot = (int)k;
    if (slot < 0) __atomic_fetch_add(&g_trap_unlisted, 1, __ATOMIC_ACQ_REL);
    for (;;) { /* a slot whose turn it is: a full ring waits for the trap thread */
        pos = __atomic_load_n(&g_trap_head, __ATOMIC_RELAXED);
        m = &g_trap_ring[pos % TRAP_RING];
        k = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
        if (k == pos) {
            if (__atomic_compare_exchange_n(&g_trap_head, &pos, pos + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
        } else if ((i32)(k - pos) < 0)
            sched_yield();
    }
    m->p = g_wp_base + (i << g_wp_shift);
    m->tid = tid;
    m->write = write;
    __atomic_store_n(&m->seq, pos + 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_trap_posted, 1, __ATOMIC_SEQ_CST);
    trap_futex_wake(&g_trap_posted);
    while ((__atomic_load_n(&g_wp[i], __ATOMIC_ACQUIRE) & WP_TRAP) &&
           __atomic_load_n(&g_trap_gen, __ATOMIC_SEQ_CST) == gen)
        trap_futex_wait(&g_trap_gen, gen);
    if (slot >= 0)
        __atomic_store_n(&g_trap_waiting[slot], 0, __ATOMIC_RELEASE);
    else
        __atomic_fetch_sub(&g_trap_unlisted, 1, __ATOMIC_RELEASE);
}
static void* trap_signal_thread(void* arg) {
    (void)arg;
    for (;;) {
        u32 seen = __atomic_load_n(&g_trap_posted, __ATOMIC_SEQ_CST);
        for (;;) {
            TrapMsg* m = &g_trap_ring[g_trap_tail % TRAP_RING];
            TrapMsg got;
            if (__atomic_load_n(&m->seq, __ATOMIC_ACQUIRE) != g_trap_tail + 1) break;
            got = *m;
            __atomic_store_n(&m->seq, g_trap_tail + TRAP_RING, __ATOMIC_RELEASE);
            g_trap_tail++;
            g_trap_cb(got.p, got.write, got.tid);
        }
        trap_futex_wait(&g_trap_posted, seen);
    }
    return NULL;
}

/* A fault at a, in the signal handler: 1 when it was the tracking's and the access may go again. A fault on a page the
   tracking doesn't hold but that is writable now was another thread's to open, between this fault and the lock: it
   goes again too. Only when it faults again at the same address with no page opened since is it not the
   tracking's (a page whose state is wrong, which must end the process rather than spin). A page that isn't mapped
   is never the tracking's */
static int wp_fault(void* a, int write) {
    u8* w = __atomic_load_n(&g_wp, __ATOMIC_ACQUIRE);
    u64 i, page = (u64)1 << g_wp_shift;
    u8 s;
    if (!w || (u8*)a < g_wp_base || (u64)((u8*)a - g_wp_base) >> g_wp_shift >= g_wp_pages) return 0;
    i = (u64)((u8*)a - g_wp_base) >> g_wp_shift;
    s = wp_lock(i);
    if (frozen_at(a)) { /* the freeze holds it read only */
        wp_unlock(i, s);
        return 0;
    }
    if (s & WP_TRAP) { /* a trap's: lands on the trap thread, then the access goes again */
        wp_unlock(i, s);
        trap_wait(i, write);
        return 1;
    }
    if (!(s & WP_PROT)) {
        u32 opened = __atomic_load_n(&g_wp_opened, __ATOMIC_SEQ_CST);
        wp_unlock(i, s);
        if (!(s & WP_MAPPED) || (t_plat_posix.wp_retried == a && t_plat_posix.wp_retried_at == opened)) return 0;
        t_plat_posix.wp_retried = a;
        t_plat_posix.wp_retried_at = opened;
        return 1;
    }
    if (mprotect(g_wp_base + (i << g_wp_shift), (size_t)page, PROT_READ | PROT_WRITE)) {
        int why = errno;
        wp_unlock(i, s);
        if (why != ENOMEM) return 0;
        wp_shed(); /* opening one page splits a mapping: open them all, which splits none, and go again */
        return 1;
    }
    __atomic_fetch_sub(&g_wp_held, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_wp_opened, 1, __ATOMIC_SEQ_CST);
    wp_unlock(i, (u8)((s & WP_ARMED) | WP_MAPPED)); /* written, and writable */
    return 1;
}
/* the locked written pages [i, i + n): reported, and held read only when the budget allows */
static void wp_protect(u64 i, u64 n, void (*cb)(u8* p, u64 size)) {
    u8* a = g_wp_base + (i << g_wp_shift);
    u64 k, add = 0, len = n << g_wp_shift;
    u8 to;
    int shed = 0;
    for (k = i; k < i + n; k++)
        if (!(__atomic_load_n(&g_wp[k], __ATOMIC_RELAXED) & WP_PROT)) add++;
    if (!wp_reserve(add))
        to = 0; /* left writable, and written: reported again by the next scan */
    else if (mprotect(a, (size_t)len, PROT_READ)) {
        /* it may have changed part of the run: held, but written, so a fault opens it and the next scan tries again */
        to = WP_MAPPED | WP_PROT;
        shed = errno == ENOMEM;
    } else
        to = WP_MAPPED | WP_PROT | WP_CLEAN;
    for (k = i; k < i + n; k++) wp_unlock(k, to ? to : (u8)(__atomic_load_n(&g_wp[k], __ATOMIC_RELAXED) & ~WP_LOCK));
    cb(a, len);
    if (shed) wp_shed();
}
static int wp_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    u64 i, lo, hi, run = 0, n = 0;
    if (wp_pages(p, size, &lo, &hi)) return size ? -1 : 0;
    for (i = lo; i < hi; i++) {
        u8 s = __atomic_load_n(&g_wp[i], __ATOMIC_RELAXED);
        if ((s & (WP_MAPPED | WP_CLEAN | WP_ARMED | WP_TRAP)) == WP_MAPPED) {
            s = wp_lock(i);
            /* written, and no system call writing it: locked until its run is protected */
            if ((s & (WP_MAPPED | WP_CLEAN | WP_ARMED | WP_TRAP)) == WP_MAPPED && !wp_kernel_writing(i)) {
                if (!n) run = i;
                n++;
                continue;
            }
            wp_unlock(i, s);
        }
        if (n) wp_protect(run, n, cb);
        n = 0;
    }
    if (n) wp_protect(run, n, cb);
    return 0;
}
/* [p, p + size) armed for a trap, or let go by it (armed 0): written then, through the mirror, which the tracking
   doesn't see */
static void wp_armed(const u8* p, u64 size, int armed) {
    u64 i, lo, hi;
    if (wp_pages(p, size, &lo, &hi)) return;
    for (i = lo; i < hi; i++) {
        u8 s = wp_lock(i);
        wp_unlock(i, (u8)(armed ? s | WP_ARMED : s & ~(WP_ARMED | WP_CLEAN)));
    }
}
static void wp_init(u8* p, u64 size, int track) {
    u64 page = (u64)sysconf(_SC_PAGESIZE), budget = ~(u64)0;
    unsigned shift = 0;
    void* s;
    while (((u64)1 << shift) < page) shift++;
    s = mmap(NULL, (size_t)(size >> shift), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (s == MAP_FAILED) return;
#if defined(__linux__)
    {
        FILE* f = fopen("/proc/sys/vm/max_map_count", "r");
        unsigned long long limit = 0;
        if (f) {
            if (fscanf(f, "%llu", &limit) != 1) limit = 0;
            fclose(f);
        }
        /* 65530 by default; SteamOS and Arch raise it past a million, where the tracking has no limit of its own */
        if (limit && limit < (1u << 20)) budget = limit > 16384 ? (limit - 16384) / 2 : 0;
    }
#endif
    fault_handler_install();
    /* mem_init, on the main thread before any other: a later reservation (a test's) takes the tracking over */
    g_wp_base = p;
    g_wp_pages = size >> shift;
    g_wp_shift = shift;
    __atomic_store_n(&g_wp_budget, budget, __ATOMIC_RELAXED);
    g_wp_track = track;
    wp_thread_start();
    __atomic_store_n(&g_wp, (u8*)s, __ATOMIC_RELEASE);
}
/* whether [p, e) meets the range frozen now */
static int frozen_over(const u8* p, const u8* e) {
    return __atomic_load_n(&g_frozen, __ATOMIC_SEQ_CST) && p < __atomic_load_n(&g_frz_hi, __ATOMIC_ACQUIRE) &&
           e > __atomic_load_n(&g_frz_lo, __ATOMIC_ACQUIRE);
}
static void frozen_until_thaw(const u8* p, const u8* e) {
    while (frozen_over(p, e)) {
#if defined(__linux__)
        syscall(SYS_futex, &g_frozen, FUTEX_WAIT_PRIVATE, 1, NULL, NULL, 0);
#else
        sched_yield();
#endif
    }
}
void plat_ww_open(u8* p, u64 size) {
    u64 i, lo, hi, run = 0, n = 0, k;
    int slot;
    if (!g_wp_track || wp_pages(p, size, &lo, &hi)) return;
    /* a slot of the ranges scans leave alone, published before any page opens */
    for (;;) {
        for (slot = 0; slot < WW_OPEN_N; slot++) {
            int free = 0;
            if (__atomic_compare_exchange_n(&g_ww_open_busy[slot], &free, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                break;
        }
        if (slot < WW_OPEN_N) break;
        sched_yield(); /* as many system calls writing guest memory at once as there are slots */
    }
    g_ww_open_lo[slot] = p;
    g_ww_open_hi[slot] = p + size;
    __atomic_fetch_add(&g_ww_open_n, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_ww_open_busy[slot], 2, __ATOMIC_SEQ_CST);
    /* a range being moved (plat_freeze) takes the write when the move is done: opened now, the kernel would write
       the old pages, which the move has copied already */
    frozen_until_thaw(p, p + size);
    for (i = lo; i <= hi; i++) {
        /* every page locked, not only those held now: a scan that locked one before the range was published may be
           about to protect it, and holds it until it has */
        if (i < hi) {
            u8 s = wp_lock(i);
            if ((s & WP_PROT) && !frozen_at(g_wp_base + (i << g_wp_shift))) {
                if (!n) run = i;
                n++;
                continue;
            }
            wp_unlock(i, s);
        }
        if (!n) continue;
        if (!mprotect(g_wp_base + (run << g_wp_shift), (size_t)(n << g_wp_shift), PROT_READ | PROT_WRITE)) {
            __atomic_fetch_sub(&g_wp_held, n, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_wp_opened, 1, __ATOMIC_SEQ_CST);
            for (k = run; k < run + n; k++)
                wp_unlock(k, (u8)((__atomic_load_n(&g_wp[k], __ATOMIC_RELAXED) & WP_ARMED) | WP_MAPPED));
        } else /* left for the system call to fail on, which it reports */
            for (k = run; k < run + n; k++)
                wp_unlock(k, (u8)(__atomic_load_n(&g_wp[k], __ATOMIC_RELAXED) & ~(WP_LOCK | WP_CLEAN)));
        n = 0;
    }
}
void plat_ww_close(u8* p, u64 size) {
    int k;
    if (!g_wp_track || !g_wp) return;
    for (k = 0; k < WW_OPEN_N; k++)
        if (__atomic_load_n(&g_ww_open_busy[k], __ATOMIC_SEQ_CST) == 2 && g_ww_open_lo[k] == p &&
            g_ww_open_hi[k] == p + size) {
            __atomic_store_n(&g_ww_open_busy[k], 0, __ATOMIC_SEQ_CST);
            __atomic_fetch_sub(&g_ww_open_n, 1, __ATOMIC_SEQ_CST);
            return;
        }
}

int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    if (__atomic_load_n(&g_wp, __ATOMIC_ACQUIRE) && g_wp_track) return wp_scan(p, size, cb);
#if defined(__linux__) && defined(PAGEMAP_SCAN)
    struct page_region regions[64];
    struct pm_scan_arg arg;
    u64 mask = (u64)sysconf(_SC_PAGESIZE) - 1; /* the kernel's pages: PAGEMAP_SCAN wants its bounds on them */
    u64 start = (u64)(uintptr_t)p & ~mask, end = ((u64)(uintptr_t)p + size + mask) & ~mask;
    int pm = __atomic_load_n(&g_pagemap, __ATOMIC_ACQUIRE);
    if (pm < 0) return -1;
    while (start < end) {
        int i, n;
        memset(&arg, 0, sizeof arg);
        arg.size = sizeof arg;
        /* no PM_SCAN_CHECK_WPASYNC: a range not registered for the write watch (an owned span, in minor mode for its
           trap: plat_trap_arm) is skipped rather than failing the scan with EPERM. Its consumers use the ownership
           map (docs/GPU_OWNERSHIP.md) */
        arg.flags = PM_SCAN_WP_MATCHING;
        arg.start = start;
        arg.end = end;
        arg.vec = (u64)(uintptr_t)regions;
        arg.vec_len = sizeof regions / sizeof *regions;
        arg.category_mask = PAGE_IS_WRITTEN;
        arg.return_mask = PAGE_IS_WRITTEN;
        n = ioctl(pm, PAGEMAP_SCAN, &arg);
        if (n < 0) return -1;
        for (i = 0; i < n; i++) cb((u8*)(uintptr_t)regions[i].start, regions[i].end - regions[i].start);
        /* a full vector may have stopped the walk early: go on from where it stopped */
        if ((u64)n < arg.vec_len || !arg.walk_end || arg.walk_end >= end) break;
        start = arg.walk_end;
    }
    return 0;
#else
    (void)p;
    (void)size;
    (void)cb;
    return -1;
#endif
}

u64 plat_vm_page(void) {
    long host = sysconf(_SC_PAGESIZE);
    u64 page = host > 0 ? (u64)host : 4096;
    const char* forced = getenv("SNX_HOST_PAGE");
    if (forced) { /* a larger power of two only: the host's pages can't be split */
        u64 v = strtoull(forced, NULL, 0);
        if (v > page && v <= (1u << 20) && !(v & (v - 1))) page = v;
    }
    return page;
}
/* mmap aligns to the host's own page: a larger plat_vm_page (SNX_HOST_PAGE) takes a page more and trims the rest, so
   guest addresses sit at the same place in a page as their host addresses */
int plat_address_space(u64* mapped, u64* largest_free) {
    u64 total = 0, hole = 0, last = 0;
#if defined(__linux__)
    /* each mapping's range, in address order, a line each */
    char line[PATH_MAX + 128];
    unsigned long long lo, hi;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%llx-%llx ", &lo, &hi) != 2 || hi <= lo) continue;
        total += hi - lo;
        if (last && lo > last && lo - last > hole) hole = lo - last;
        last = hi;
    }
    fclose(f);
#elif defined(__APPLE__)
    vm_address_t at = 0;
    for (;;) {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count,
                         &object) != KERN_SUCCESS)
            break;
        total += size;
        if (last && at > last && at - last > hole) hole = at - last;
        last = at + size;
        at = last;
    }
#else
    (void)last;
#endif
    if (!total) return 0;
    *mapped = total;
    *largest_free = hole;
    return 1;
}
u8* plat_vm_reserve(u64 size) {
    u64 page = plat_vm_page(), extra = page - (u64)sysconf(_SC_PAGESIZE), head;
    u8* p = (u8*)mmap(NULL, (size_t)(size + extra), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == (u8*)MAP_FAILED) return NULL;
    head = (page - (u64)(uintptr_t)p % page) % page;
    if (head) munmap(p, (size_t)head);
    if (extra > head) munmap(p + head + size, (size_t)(extra - head));
    p += head;
    ww_init(p, size);
    return p;
}
int plat_vm_commit(u8* p, u64 size) {
    int r;
    wp_hold(p, size, WP_MAPPED);
    r = mprotect(p, (size_t)size, PROT_READ | PROT_WRITE) ? -1 : 0;
    wp_let(p, size, WP_MAPPED, !r);
    return r;
}
void plat_vm_decommit(u8* p, u64 size) {
    wp_hold(p, size, 0);
#if defined(__linux__)
    madvise(p, (size_t)size, MADV_DONTNEED);
    mprotect(p, (size_t)size, PROT_NONE);
#else
    /* only Linux's MADV_DONTNEED drops the contents (Darwin's may keep them, FreeBSD's does): fresh pages over the
       range are certain to read zero */
    mmap(p, (size_t)size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
    wp_let(p, size, 0, 1);
}

/* The alias pool (Linux and Android): one memfd, as large as asked and sparse, whose pages mem.c maps at guest
   addresses with MAP_SHARED, so two addresses of the reservation (and a service's mapping outside it) are the same
   pages, and a freed range is punched out so it reads zero when handed out again. The other systems here keep mem.c's
   copies: the pool is untested there (FreeBSD has memfd_create and, from 14, fspacectl to free a range; Apple has
   shm_open, whose descriptors can't be read or written, and no way to free part of one) */
static int g_alias = -1;
static u64 g_alias_size;

int plat_alias_init(u64 size) {
#if defined(__linux__) && defined(SYS_memfd_create) && defined(FALLOC_FL_PUNCH_HOLE)
    long fd = syscall(SYS_memfd_create, "snx-alias", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return -1;
    if (ftruncate((int)fd, (off_t)size)) {
        close((int)fd);
        return -1;
    }
    g_alias = (int)fd; /* mem_init, before any other thread */
    g_alias_size = size;
    return 0;
#else
    (void)size;
    return -1;
#endif
}
int plat_alias_map(u8* p, u64 off, u64 size) {
    int r;
    if (g_alias < 0) return -1;
    wp_hold(p, size, WP_MAPPED);
    r = mmap(p, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_alias, (off_t)off) == MAP_FAILED;
    wp_let(p, size, WP_MAPPED, !r);
    if (r) return -1;
    ww_remapped(p, size);
    return 0;
}
int plat_alias_unmap(u8* p, u64 size) {
    int r;
    wp_hold(p, size, 0);
    r = mmap(p, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED;
    wp_let(p, size, 0, !r);
    if (r) return -1;
    ww_remapped(p, size); /* the anonymous mapping is a new one too */
    return 0;
}
int plat_alias_write(u64 off, const void* src, u64 size) {
    const u8* s = (const u8*)src;
    while (size) {
        ssize_t n = pwrite(g_alias, s, (size_t)size, (off_t)off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        s += n, off += (u64)n, size -= (u64)n;
    }
    return 0;
}
int plat_alias_read(u64 off, void* dst, u64 size) {
    u8* d = (u8*)dst;
    while (size) {
        ssize_t n = pread(g_alias, d, (size_t)size, (off_t)off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        d += n, off += (u64)n, size -= (u64)n;
    }
    return 0;
}
void plat_alias_free(u64 off, u64 size) {
#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE)
    if (g_alias >= 0 && fallocate(g_alias, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)size))
        fatal("Could not free %llu KiB of the alias pool (error %d)", (unsigned long long)(size >> 10), errno);
#else
    (void)off;
    (void)size;
#endif
}
u8* plat_alias_host(u64 off, u64 size) {
    void* p;
    if (g_alias < 0) return NULL;
    p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, g_alias, (off_t)off);
    return p == MAP_FAILED ? NULL : (u8*)p;
}
void plat_alias_host_free(u8* p, u64 size) { munmap(p, (size_t)size); }
static u64 g_alias_size;
u32 plat_tid(void) {
#if defined(__linux__)
    /* kept per thread: vk_lock asks it on every acquire, thousands of times a frame in one game, and each was a
       system call */
    if (!t_plat_posix.tid) t_plat_posix.tid = (u32)syscall(SYS_gettid);
    return t_plat_posix.tid;
#else
    return (u32)(uintptr_t)pthread_self();
#endif
}
/* /proc's view of the thread: running (R) is no wait, and a sleep's wait channel names the kernel function it sleeps
   in, handle_userfault for a trap. "0" or nothing when the kernel won't name it (no kallsyms): unknown */
int plat_thread_in_fault(u32 tid) {
    if (__atomic_load_n(&g_trap_signal, __ATOMIC_ACQUIRE)) { /* waiting in the handler (trap_wait) */
        u32 k;
        for (k = 0; k < TRAP_WAITERS; k++)
            if (__atomic_load_n(&g_trap_waiting[k], __ATOMIC_ACQUIRE) == tid) return 1;
        return __atomic_load_n(&g_trap_unlisted, __ATOMIC_ACQUIRE) ? -1 : 0; /* it may be one not listed */
    }
#if defined(__linux__)
    char path[64], buf[256];
    const char* state;
    ssize_t n;
    int fd;
    snprintf(path, sizeof path, "/proc/self/task/%u/stat", (unsigned)tid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    state = strrchr(buf, ')'); /* after the name, which may hold anything */
    if (!state || state[1] != ' ') return -1;
    if (state[2] == 'R') return 0;
    snprintf(path, sizeof path, "/proc/self/task/%u/wchan", (unsigned)tid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    if (!strcmp(buf, "0")) return -1;
    return !strcmp(buf, "handle_userfault");
#else
    (void)tid;
    return -1; /* traps are userfaultfd's, Linux's */
#endif
}

#if defined(__linux__)
/* Read proc metadata, never the stopped thread's guest memory. A blocked user-space fault has syscall=-1,
   followed by SP and PC; a blocked syscall has six arguments before them. */
static int fault_proc_line(u32 tid, const char* file, char* buf, size_t cap) {
    char path[96];
    ssize_t n;
    int fd, error;
    snprintf(path, sizeof path, "/proc/self/task/%u/%s", tid, file);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        WARN("ownership-fault: %s unavailable (errno=%d)\n", file, errno);
        return 0;
    }
    n = read(fd, buf, cap - 1);
    error = errno;
    close(fd);
    if (n <= 0) {
        WARN("ownership-fault: %s unavailable (read=%ld errno=%d)\n", file, (long)n, n < 0 ? error : 0);
        return 0;
    }
    buf[n] = 0;
    buf[strcspn(buf, "\n")] = 0;
    return 1;
}

/* Report mapping-relative and file offsets even in stripped builds, without dladdr's dynamic-loader lock.
   Stack words are only candidates, not unwound frames. */
static void fault_code_mapping(uintptr_t pc, const char* kind, size_t slot) {
    FILE* f = fopen("/proc/self/maps", "r");
    char line[1024], perms[5];
    unsigned long long lo, hi, off;
    int found = 0;
    if (!f) {
        WARN("ownership-fault: maps unavailable (errno=%d)\n", errno);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%llx-%llx %4s %llx", &lo, &hi, perms, &off) != 4 || pc < lo || pc >= hi || perms[2] != 'x')
            continue;
        line[strcspn(line, "\n")] = 0;
        WARN("ownership-fault: %s[%zu] host=%llx mapping+%llx file-offset=%llx map=%s\n", kind, slot,
             (unsigned long long)pc, (unsigned long long)pc - lo, off + (unsigned long long)pc - lo, line);
        found = 1;
        break;
    }
    fclose(f);
    if (!found && !strcmp(kind, "pc"))
        WARN("ownership-fault: pc=%llx has no executable mapping (possibly changed)\n", (unsigned long long)pc);
}

static void fault_stack_candidates(uintptr_t sp) {
    uintptr_t words[128];
    FILE* f = fopen("/proc/self/maps", "r");
    char line[1024], perms[5];
    unsigned long long lo, hi;
    size_t bytes = 0, i;
    ssize_t n;
    struct iovec local, remote;
    if (!f) {
        WARN("ownership-fault: stack candidates unavailable (maps errno=%d)\n", errno);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        int name = 0;
        if (sscanf(line, "%llx-%llx %4s %*s %*s %*s %n", &lo, &hi, perms, &name) != 3 || !name || sp < lo || sp >= hi)
            continue;
        /* Native stacks only: anonymous private RW or [stack]. Owned guest pool pages are shared mappings.
           Excluding them prevents the diagnostic itself blocking on a userfaultfd page. */
        if (!strcmp(perms, "rw-p") && (!line[name] || !strncmp(line + name, "[stack]", 7)))
            bytes = hi - sp < sizeof words ? (size_t)(hi - sp) : sizeof words;
        break;
    }
    fclose(f);
    if (!bytes) {
        WARN("ownership-fault: stack candidates unavailable (SP is not in a private native stack mapping)\n");
        return;
    }
    local.iov_base = words;
    local.iov_len = bytes;
    remote.iov_base = (void*)sp;
    remote.iov_len = bytes;
    /* Android API 21 has no libc declaration for process_vm_readv. Use the kernel interface, as the
       userfaultfd paths do, and keep the diagnostic optional if the syscall is unavailable or denied. */
    n = syscall(SYS_process_vm_readv, getpid(), &local, 1ul, &remote, 1ul, 0ul);
    if (n <= 0) {
        WARN("ownership-fault: stack candidates unavailable (read=%ld errno=%d)\n", (long)n, n < 0 ? errno : 0);
        return;
    }
    WARN("ownership-fault: scanning %ld native stack bytes for possible callers; candidates are NOT a backtrace\n",
         (long)n);
    for (i = 0; i < (size_t)n / sizeof words[0]; i++)
        fault_code_mapping(words[i], "stack-candidate", i * sizeof words[0]);
}
#endif

void plat_dump_fault_thread(u32 tid) {
#if defined(__linux__)
    char buf[512], *p, *end;
    unsigned long long regs[8];
    long call;
    unsigned n = 0, expected;
    if (fault_proc_line(tid, "comm", buf, sizeof buf)) WARN("ownership-fault: thread-name=%s\n", buf);
    if (fault_proc_line(tid, "wchan", buf, sizeof buf)) WARN("ownership-fault: wait-channel=%s\n", buf);
    if (!fault_proc_line(tid, "syscall", buf, sizeof buf)) return;
    call = strtol(buf, &p, 10);
    if (p == buf) {
        WARN("ownership-fault: host registers unavailable (%s)\n", buf);
        return;
    }
    while (n < 8) {
        regs[n] = strtoull(p, &end, 0);
        if (end == p) break;
        n++;
        p = end;
    }
    expected = call == -1 ? 2 : 8;
    if (n != expected) {
        WARN("ownership-fault: host registers unavailable (unexpected syscall record)\n");
        return;
    }
    WARN("ownership-fault: blocked-thread=%u syscall=%ld host-pc=%llx host-sp=%llx\n", tid, call, regs[n - 1],
         regs[n - 2]);
    fault_code_mapping((uintptr_t)regs[n - 1], "pc", 0);
    fault_stack_candidates((uintptr_t)regs[n - 2]);
#else
    WARN("ownership-fault: host context unavailable for thread %u on this platform\n", tid);
#endif
}
u8* plat_alias_mirror(void) {
    static u8* mirror;
    u8 *m = __atomic_load_n(&mirror, __ATOMIC_ACQUIRE), *none = NULL;
    void* v;
    if (m || g_alias < 0) return m;
    v = mmap(NULL, (size_t)g_alias_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, g_alias, 0);
    if (v == MAP_FAILED) return NULL;
    /* the first one made stays: a thread that made another at the same time unmaps its own */
    if (!__atomic_compare_exchange_n(&mirror, &none, (u8*)v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        munmap(v, (size_t)g_alias_size);
        return none;
    }
    return (u8*)v;
}

/* ---- traps: userfaultfd minor faults on the pool's pages, answered on a thread of their own */
#if SNX_UFFD_MINOR
static void (*g_trap_fault)(u8* p, int write, u32 tid);
static void* trap_thread(void* arg) {
    struct uffd_msg msg[16];
    u64 page = (u64)sysconf(_SC_PAGESIZE);
    (void)arg;
    for (;;) {
        struct pollfd pf;
        ssize_t n;
        int i;
        pf.fd = g_uffd;
        pf.events = POLLIN;
        pf.revents = 0;
        if (poll(&pf, 1, -1) <= 0) continue;
        n = read(g_uffd, msg, sizeof msg);
        if (n <= 0) continue;
        for (i = 0; i < (int)(n / (ssize_t)sizeof *msg); i++)
            if (msg[i].event == UFFD_EVENT_PAGEFAULT)
                g_trap_fault((u8*)(uintptr_t)(msg[i].arg.pagefault.address & ~(page - 1)),
                             (msg[i].arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) != 0,
                             msg[i].arg.pagefault.feat.ptid);
    }
    return NULL;
}
#endif
/* the pages of [p, p + size) a trap by protection holds, readable and writable again, and the threads waiting on
   them woken */
static void trap_open(u8* p, u64 size) {
    u64 i, lo, hi, run = 0, n = 0, k;
    if (wp_pages(p, size, &lo, &hi)) return;
    for (i = lo; i <= hi; i++) {
        if (i < hi && (__atomic_load_n(&g_wp[i], __ATOMIC_RELAXED) & WP_TRAP)) {
            if (wp_lock(i) & WP_TRAP) {
                if (!n) run = i;
                n++;
                continue;
            }
            wp_unlock(i, (u8)(__atomic_load_n(&g_wp[i], __ATOMIC_RELAXED) & ~WP_LOCK));
        }
        if (!n) continue;
        if (mprotect(g_wp_base + (run << g_wp_shift), (size_t)(n << g_wp_shift), PROT_READ | PROT_WRITE)) {
            if (errno != ENOMEM) fatal("Could not let trapped guest memory go (error %d)", errno);
            wp_shed(); /* mappings to spare, from the write tracking's */
            if (mprotect(g_wp_base + (run << g_wp_shift), (size_t)(n << g_wp_shift), PROT_READ | PROT_WRITE))
                fatal("Could not let trapped guest memory go (error %d)", errno);
        }
        __atomic_fetch_sub(&g_wp_held, n, __ATOMIC_RELAXED);
        /* before the pages go: a fault that raced this finds them writable, and the count moved (wp_fault) */
        __atomic_fetch_add(&g_wp_opened, 1, __ATOMIC_SEQ_CST);
        /* written: what landed came through the mirror */
        for (k = run; k < run + n; k++)
            wp_unlock(k, (u8)((__atomic_load_n(&g_wp[k], __ATOMIC_RELAXED) & WP_ARMED) | WP_MAPPED));
        n = 0;
    }
    trap_wake();
}
static void trap_wake(void) {
    __atomic_fetch_add(&g_trap_gen, 1, __ATOMIC_SEQ_CST);
    trap_futex_wake(&g_trap_gen);
}
static int trap_signal_init(void (*fault)(u8* p, int write, u32 tid)) {
    pthread_t t;
    pthread_attr_t a;
    u32 k;
    if (!__atomic_load_n(&g_wp, __ATOMIC_ACQUIRE) || g_alias < 0 || !fault) return -1;
    for (k = 0; k < TRAP_RING; k++) g_trap_ring[k].seq = k;
    g_trap_cb = fault;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, trap_signal_thread, NULL)) {
        pthread_attr_destroy(&a);
        return -1;
    }
    pthread_attr_destroy(&a);
    __atomic_store_n(&g_trap_signal, 1, __ATOMIC_RELEASE);
    return 0;
}
int plat_trap_init(void (*fault)(u8* p, int write, u32 tid)) {
    if (traps_by_signal() || !g_uffd_minor_on()) return trap_signal_init(fault);
#if SNX_UFFD_MINOR
    pthread_t t;
    pthread_attr_t a;
    if (!g_uffd_minor || g_alias < 0 || !fault) return -1;
    g_trap_fault = fault;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, trap_thread, NULL)) {
        pthread_attr_destroy(&a);
        return -1;
    }
    pthread_attr_destroy(&a);
    return 0;
#else
    return -1;
#endif
}
/* traps by protection: the pages inaccessible, under their states' locks */
static int trap_signal_arm(u8* p, u64 size) {
    u64 i, lo, hi, added = 0;
    int r;
#if SNX_UFFD
    if (__atomic_load_n(&g_pagemap, __ATOMIC_ACQUIRE) >= 0) { /* out of the written-page scans, as below */
        struct uffdio_range unreg = {(u64)(uintptr_t)p, size};
        if (ioctl(g_uffd, UFFDIO_UNREGISTER, &unreg) && errno != ENOENT) return -1;
    }
#endif
    if (wp_pages(p, size, &lo, &hi)) return -1;
    for (i = lo; i < hi; i++)
        if (!(wp_lock(i) & (WP_PROT | WP_TRAP))) added++;
    r = mprotect(p, (size_t)size, PROT_NONE) ? -1 : 0;
    if (r && errno == ENOMEM) { /* mappings to spare, from the write tracking's: its pages here stay locked */
        for (i = lo; i < hi; i++) wp_unlock(i, (u8)(__atomic_load_n(&g_wp[i], __ATOMIC_RELAXED) & ~WP_LOCK));
        wp_shed();
        added = 0;
        for (i = lo; i < hi; i++)
            if (!(wp_lock(i) & (WP_PROT | WP_TRAP))) added++;
        r = mprotect(p, (size_t)size, PROT_NONE) ? -1 : 0;
    }
    /* counted against the budget, which the tracking then has less of: they can't be refused */
    if (!r) __atomic_fetch_add(&g_wp_held, added, __ATOMIC_RELAXED);
    for (i = lo; i < hi; i++) {
        u8 s = (u8)(__atomic_load_n(&g_wp[i], __ATOMIC_RELAXED) & ~WP_LOCK);
        wp_unlock(i, r ? (u8)(s & ~WP_CLEAN) : (u8)((s & ~(WP_PROT | WP_CLEAN)) | WP_ARMED | WP_TRAP));
    }
    return r;
}
int plat_trap_arm(u8* p, u64 size) {
    if (__atomic_load_n(&g_trap_signal, __ATOMIC_ACQUIRE)) return trap_signal_arm(p, size);
#if SNX_UFFD_MINOR
    struct uffdio_register reg;
    struct uffdio_range unreg;
    wp_armed(p, size, 1); /* left out of scans from now: what the trap lands reads as written when it lets go */
    /* the write watch registered this range first; the kernel won't add minor mode to a WP registration (EBUSY), so
       let that go before the trap's own */
    unreg.start = (u64)(uintptr_t)p;
    unreg.len = size;
    if (ioctl(g_uffd, UFFDIO_UNREGISTER, &unreg) && errno != ENOENT) return -1;
    memset(&reg, 0, sizeof reg);
    reg.range.start = (u64)(uintptr_t)p;
    reg.range.len = size;
    reg.mode = UFFDIO_REGISTER_MODE_MINOR; /* in place of the write tracking's registration */
    if (ioctl(g_uffd, UFFDIO_REGISTER, &reg)) return -1;
    /* the pages leave the page tables and stay in the pool: the next access is a minor fault */
    return madvise(p, (size_t)size, MADV_DONTNEED) ? -1 : 0;
#else
    (void)p;
    (void)size;
    return -1;
#endif
}
int plat_trap_continue(u8* p, u64 size) {
    if (__atomic_load_n(&g_trap_signal, __ATOMIC_ACQUIRE)) {
        trap_open(p, size);
        return 0;
    }
#if SNX_UFFD_MINOR
    u64 page = (u64)sysconf(_SC_PAGESIZE), at = (u64)(uintptr_t)p, end = at + size;
    int one = 0; /* page by page: a range over two runs of pool pages is two mappings, which one call can't span */
    while (at < end) {
        struct uffdio_continue c;
        struct uffdio_range w;
        memset(&c, 0, sizeof c);
        c.range.start = at;
        c.range.len = one ? page : end - at;
        if (!ioctl(g_uffd, UFFDIO_CONTINUE, &c)) {
            at += c.range.len;
            one = 0;
            continue;
        }
        if (c.mapped > 0) { /* the kernel stopped partway (EAGAIN): the rest is asked for again */
            at += (u64)c.mapped;
            continue;
        }
        if (errno == EAGAIN) continue;
        if (!one) {
            one = 1;
            continue;
        }
        /* a page mapped already (EEXIST), registered no more (ENOENT), or not in the pool (a hole, which faults in as
           zero by itself): past it, waking whoever waits on it to fault again. A waiter left there sleeps for good */
        w.start = at;
        w.len = page;
        ioctl(g_uffd, UFFDIO_WAKE, &w);
        at += page;
        one = 0;
    }
    return 0;
#else
    (void)p;
    (void)size;
    return -1;
#endif
}
void plat_trap_disarm(u8* p, u64 size) {
    if (__atomic_load_n(&g_trap_signal, __ATOMIC_ACQUIRE)) {
        trap_open(p, size);   /* a page not let go yet reads the pool's contents */
        wp_armed(p, size, 0); /* landed through the mirror, which no protection watches */
        if (__atomic_load_n(&g_pagemap, __ATOMIC_ACQUIRE) >= 0) ww_remapped(p, size);
        return;
    }
    wp_armed(p, size, 0);
#if SNX_UFFD_MINOR
    struct uffdio_range r;
    if (__atomic_load_n(&g_pagemap, __ATOMIC_ACQUIRE) >= 0) {
        ww_remapped(p, size); /* written-page tracking again, in place of the trap */
        return;
    }
    r.start = (u64)(uintptr_t)p;
    r.len = size;
    ioctl(g_uffd, UFFDIO_UNREGISTER, &r);
#else
    (void)p;
    (void)size;
#endif
}

/* ---- a range frozen while it is copied: a write to it waits in the signal handler (fault_signal) for the thaw */
static int frozen_wait(void* a, int access);

/* A SIGSEGV or SIGBUS no access raised but sent (kill, tgkill, sigqueue), told by its code's name: si_code <= 0 is
   Linux's alone, and on Darwin x86_64 a real fault may carry 0. A debugger has one sent when it detaches passing a
   fault it held: the kernel then delivers it as SI_USER from the tracer, si_addr holding the sender's pid and uid
   instead of the fault's address. The access behind it, if any, faults again as the thread goes on. Darwin can't
   tell: its sendsig fills every SIGSEGV's and SIGBUS's code and address from the thread's last fault, sent or not
   (SEGV_ACCERR and BUS_ADRALN on arm64), so there one sent is reported as a crash. Its debuggers take faults as Mach exceptions
   and leave none behind */
static int sent_signal(const siginfo_t* si) {
    return si->si_code == SI_USER || si->si_code == SI_QUEUE
#ifdef SI_TKILL
           || si->si_code == SI_TKILL
#endif
        ;
}
/* the note of one ignored, on stderr: write(2) only, which a handler may call */
static void note_sent(int sig, const siginfo_t* si) {
    static const char head[] = "scarletnx: ignored a ", tail[] = " sent by process ";
    const char* name = sig == SIGBUS ? "SIGBUS" : "SIGSEGV";
    char msg[96], digits[24];
    size_t n, k = 0;
    unsigned long v = (unsigned long)(si->si_pid > 0 ? si->si_pid : 0);
    memcpy(msg, head, sizeof head - 1);
    n = sizeof head - 1;
    memcpy(msg + n, name, strlen(name));
    n += strlen(name);
    memcpy(msg + n, tail, sizeof tail - 1);
    n += sizeof tail - 1;
    do {
        digits[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k) msg[n++] = digits[--k];
    msg[n++] = '\n';
    if (write(STDERR_FILENO, msg, n) < 0) return;
}
/* SIGSEGV or SIGBUS of a write that may be the freeze's: a protection fault (Darwin reports one as SIGBUS) */
static int access_fault(int sig, const siginfo_t* si) { return sig == SIGBUS || si->si_code == SEGV_ACCERR; }

/* whether the fault the context holds was a write: 0 where the system doesn't tell */
static int context_write(void* ctx) {
    ucontext_t* uc = (ucontext_t*)ctx;
#if defined(__linux__) && defined(__x86_64__)
    return (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0; /* the page fault's error code: W/R */
#elif defined(__linux__) && defined(__aarch64__)
    /* the ESR record among the signal frame's extra records: WnR, of a data abort */
    const struct _aarch64_ctx* h = (const struct _aarch64_ctx*)(const void*)uc->uc_mcontext.__reserved;
    while (h->magic && h->size) {
        if (h->magic == ESR_MAGIC) return (((const struct esr_context*)(const void*)h)->esr >> 6 & 1) != 0;
        h = (const struct _aarch64_ctx*)(const void*)((const u8*)h + h->size);
    }
    return 0;
#else
    (void)uc;
    return 0;
#endif
}
/* a process without the crash handler (a test): faults that are neither the write tracking's nor the freeze's end it
   as they would have */
static void fault_signal(int sig, siginfo_t* si, void* ctx) {
    int saved = errno; /* the interrupted code's, which frozen_wait's futex and wp_fault's mprotect change */
    if (sent_signal(si)) {
        note_sent(sig, si);
        errno = saved;
        return;
    }
    if (wp_fault(si->si_addr, context_write(ctx)) || frozen_wait(si->si_addr, access_fault(sig, si))) {
        errno = saved;
        return;
    }
    signal(sig, SIG_DFL);
}
static int g_fault_handler; /* plat_install_crash_handler's, or fault_signal, catches SIGSEGV and SIGBUS */
static void fault_handler_install(void) {
    struct sigaction sa;
    if (__atomic_load_n(&g_fault_handler, __ATOMIC_ACQUIRE)) return;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = fault_signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    __atomic_store_n(&g_fault_handler, 1, __ATOMIC_RELEASE);
}
void plat_freeze(u8* p, u64 size) {
    fault_handler_install();
    __atomic_store_n(&g_frz_lo, p, __ATOMIC_RELEASE);
    __atomic_store_n(&g_frz_hi, p + size, __ATOMIC_RELEASE);
    __atomic_store_n(&g_frozen, 1, __ATOMIC_SEQ_CST);
    /* under the write tracking's locks: a fault it is opening a page for finishes before the freeze protects it */
    wp_hold(p, size, 0);
    mprotect(p, (size_t)size, PROT_READ);
    wp_let(p, size, 0, 0);
}
void plat_thaw(void) {
    u8* lo = g_frz_lo;
    u64 size = (u64)(g_frz_hi - lo);
    /* readable and writable whatever is mapped there now (the copy's pages, or the old ones when it failed), and
       written for the write tracking */
    wp_hold(lo, size, WP_MAPPED);
    mprotect(lo, (size_t)size, PROT_READ | PROT_WRITE);
    __atomic_store_n(&g_frozen, 0, __ATOMIC_SEQ_CST);
    wp_let(lo, size, WP_MAPPED, 1);
#if defined(__linux__)
    syscall(SYS_futex, &g_frozen, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#endif
}
/* 1 when the fault at a is a write to the frozen range, which may go on now (after waiting for the thaw). The range
   is kept after the thaw, so a write that faulted just before it is let go once more. So is a protection fault
   (access) outside the range: mem.c freezes its runs one after another, and a run can be thawed and the next frozen
   before the handler of a write that faulted on the first one reads the range (a debugger holding the fault widens
   that). A write that faults again at the same address is not the freeze's */
static int frozen_wait(void* a, int access) {
    u8* lo = __atomic_load_n(&g_frz_lo, __ATOMIC_ACQUIRE);
    if (!lo) return 0;
    if ((u8*)a < lo || (u8*)a >= __atomic_load_n(&g_frz_hi, __ATOMIC_ACQUIRE)) {
        if (!access || t_plat_posix.frz_retried == a) return 0;
        t_plat_posix.frz_retried = a;
        return 1;
    }
    if (!__atomic_load_n(&g_frozen, __ATOMIC_SEQ_CST)) {
        if (t_plat_posix.frz_retried == a) return 0; /* faulted again after the thaw: not the freeze's */
        t_plat_posix.frz_retried = a;
        return 1;
    }
    t_plat_posix.frz_retried = NULL;
    while (__atomic_load_n(&g_frozen, __ATOMIC_SEQ_CST)) {
#if defined(__linux__)
        syscall(SYS_futex, &g_frozen, FUTEX_WAIT_PRIVATE, 1, NULL, NULL, 0);
#else
        sched_yield();
#endif
    }
    return 1;
}

/* ================================================================ environment */
int plat_setenv(const char* name, const char* value) { return setenv(name, value, 1); }

/* ================================================================ files */
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off) {
    int fd = fileno(f);
    u64 got = 0;
    if (fd < 0 || size > (u64)INT64_MAX || off > (u64)INT64_MAX - size) return -1;
    /* pread may return less than asked (past 2 GiB on Linux, or interrupted): go on until done or at the end */
    while (got < size) {
        ssize_t r = pread(fd, (u8*)buf + got, (size_t)(size - got), (off_t)(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (u64)r;
    }
    return (i64)got;
}

static void file_map_clear(PFileMap* m) {
    m->data = NULL;
    m->size = 0;
    m->borrowed = 0;
    m->heap = 0;
}
/* stat_it: the size comes from fstat, and the file must be a regular one; else known is trusted */
static int file_map(PFileMap* m, const char* path, int stat_it, u64 known) {
    int fd;
    u64 size = known;
    void* p;
    file_map_clear(m);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (stat_it) {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
            close(fd);
            return -1;
        }
        size = (u64)st.st_size;
    }
    if (size > (u64)SIZE_MAX || size > (u64)INT64_MAX) {
        close(fd);
        return -1;
    }
    if (!size) {
        close(fd);
        return 0;
    }
    p = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* the mapping keeps the file */
    if (p == MAP_FAILED) return -1;
    m->data = (const u8*)p;
    m->size = size;
    return 0;
}
int plat_file_map(PFileMap* m, const char* path) { return file_map(m, path, 1, 0); }
int plat_file_map_sized(PFileMap* m, const char* path, u64 size) { return file_map(m, path, 0, size); }
void plat_file_unmap(PFileMap* m) {
    if (m->heap)
        free((void*)m->data);
    else if (m->data && !m->borrowed) munmap((void*)m->data, (size_t)m->size);
    file_map_clear(m);
}
/* ================================================================ the crash handler */
static int g_crash_reporting; /* a thread is reporting a crash */
static char g_main_alt_stack[ALT_STACK_SIZE];

static void* context_pc(void* ctx) {
    ucontext_t* uc = (ucontext_t*)ctx;
#if defined(__linux__) && defined(__x86_64__)
    return (void*)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
    return (void*)(uintptr_t)uc->uc_mcontext.pc;
#elif defined(__linux__) && defined(__powerpc64__)
    return (void*)(uintptr_t)uc->uc_mcontext.gp_regs[32]; /* PT_NIP (asm/ptrace.h), glibc's and musl's alike */
#elif defined(__FreeBSD__) && defined(__x86_64__)
    return (void*)(uintptr_t)uc->uc_mcontext.mc_rip;
#elif defined(__FreeBSD__) && defined(__aarch64__)
    return (void*)(uintptr_t)uc->uc_mcontext.mc_gpregs.gp_elr;
#elif defined(__APPLE__) && defined(__aarch64__)
    return (void*)(uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
    return (void*)(uintptr_t)uc->uc_mcontext->__ss.__rip;
#else
    (void)uc;
    return NULL;
#endif
}

static void crash_signal(int sig, siginfo_t* si, void* ctx) {
    int idle = 0, saved = errno; /* the interrupted code's, which frozen_wait's futex changes */
    void* fault;
    /* sent, not raised (sent_signal): a debugger's leftover of plat_freeze's fault, the runtime's one SIGSEGV trap
       (its guest-memory traps are userfaultfd's, which raise none) */
    if ((sig == SIGSEGV || sig == SIGBUS) && sent_signal(si)) {
        note_sent(sig, si);
        errno = saved;
        return;
    }
    /* a write the tracking protected the page against (wp_fault), or a write to memory frozen while it moves
       (plat_freeze), which goes on once the move is done */
    if ((sig == SIGSEGV || sig == SIGBUS) &&
        (wp_fault(si->si_addr, context_write(ctx)) || frozen_wait(si->si_addr, access_fault(sig, si)))) {
        errno = saved;
        return;
    }
    /* a fault inside the report itself */
    if (t_plat_posix.crashed) _exit(1);
    t_plat_posix.crashed = 1;
    /* one report at a time: it ends the process, so a second crashing thread just waits for that */
    if (!__atomic_compare_exchange_n(&g_crash_reporting, &idle, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        for (;;) pause();
    log_crash_mode();
    /* for SIGILL and SIGFPE si_addr is the instruction, not an address it touched */
    fault = sig == SIGSEGV || sig == SIGBUS ? si->si_addr : NULL;
    crash_report(context_pc(ctx), fault, sig);
    _exit(1);
}

/* abort() (a driver's assert, a failed allocation in a library): say so, then let it go on to end the process */
static void abort_signal(int sig) {
    log_crash_mode();
    logf_(0, "*** Shutdown: abort() (signal %d), probably the video driver or out of memory ***\n", sig);
}

void plat_install_crash_handler(void) {
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    struct sigaction sa;
    stack_t ss;
    size_t i;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = g_main_alt_stack;
    ss.ss_size = sizeof g_main_alt_stack;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = crash_signal;
    /* NODEFER: a fault during the report re-enters the handler, which ends the process, instead of hanging. RESTART:
       a sent signal it ignores (sent_signal) may land in a system call, which goes on instead of failing with EINTR;
       a fault interrupts none */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESTART;
    for (i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
    __atomic_store_n(&g_fault_handler, 1, __ATOMIC_RELEASE);
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = abort_signal;
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    sigaction(SIGABRT, &sa, NULL);
}

void plat_prof_start(void) {}

u64 plat_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
}
