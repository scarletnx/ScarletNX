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

static void* thread_start(void* p) {
    ThreadStart s = *(ThreadStart*)p;
    free(p);
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
   minor faults (before Linux 5.13) it does the tracking alone; SNX_NO_WW leaves it the traps alone */
static void ww_init(u8* p, u64 size) {
#if SNX_UFFD
    int no_ww = getenv("SNX_NO_WW") != NULL, fd = -1, pm;
    u64 wp = no_ww ? 0 : UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
#if SNX_UFFD_MINOR
    fd = uffd_open(wp | UFFD_FEATURE_MINOR_SHMEM | UFFD_FEATURE_THREAD_ID);
    g_uffd_minor = fd >= 0;
#endif
    if (fd < 0 && wp) fd = uffd_open(wp);
    if (fd < 0) return;
    g_uffd = fd; /* before any other thread exists: mem_init runs on the main thread at startup */
    if (no_ww) return;
    if (ww_register(p, size)) return;
    pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm < 0) {
        struct uffdio_range r = {(u64)(uintptr_t)p, size};
        ioctl(g_uffd, UFFDIO_UNREGISTER, &r);
        return;
    }
    g_pagemap = pm;
#else
    (void)p;
    (void)size;
#endif
}
/* a range mapped again: registered again, or tracking off for the run (the pagemap stays open, as a scan on another
   thread may be using it) */
static void ww_remapped(u8* p, u64 size) {
    if (__atomic_load_n(&g_pagemap, __ATOMIC_ACQUIRE) < 0 || !ww_register(p, size)) return;
    __atomic_store_n(&g_pagemap, -1, __ATOMIC_RELEASE);
    WARN("memory: could not watch remapped guest memory for writes (error %d): caches compare contents now\n", errno);
}

/* the pagemap stays open, as a scan on another thread may be using it (ww_remapped); with it gone, ranges mapped
   again aren't registered and traps let go of theirs (plat_trap_disarm). Pages the registration watches already
   take one asynchronous write fault each and are writable from then on, as nothing protects them again */
void plat_ww_off(void) { __atomic_store_n(&g_pagemap, -1, __ATOMIC_RELEASE); }
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
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
int plat_vm_commit(u8* p, u64 size) { return mprotect(p, (size_t)size, PROT_READ | PROT_WRITE) ? -1 : 0; }
void plat_vm_decommit(u8* p, u64 size) {
#if defined(__linux__)
    madvise(p, (size_t)size, MADV_DONTNEED);
    mprotect(p, (size_t)size, PROT_NONE);
#else
    /* only Linux's MADV_DONTNEED drops the contents (Darwin's may keep them, FreeBSD's does): fresh pages over the
       range are certain to read zero */
    mmap(p, (size_t)size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
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
    if (g_alias < 0 ||
        mmap(p, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_alias, (off_t)off) == MAP_FAILED)
        return -1;
    ww_remapped(p, size);
    return 0;
}
int plat_alias_unmap(u8* p, u64 size) {
    if (mmap(p, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED)
        return -1;
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
    static _Thread_local u32 t_tid;
    if (!t_tid) t_tid = (u32)syscall(SYS_gettid);
    return t_tid;
#else
    return (u32)(uintptr_t)pthread_self();
#endif
}
/* /proc's view of the thread: running (R) is no wait, and a sleep's wait channel names the kernel function it sleeps
   in, handle_userfault for a trap. "0" or nothing when the kernel won't name it (no kallsyms): unknown */
int plat_thread_in_fault(u32 tid) {
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
int plat_trap_init(void (*fault)(u8* p, int write, u32 tid)) {
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
    (void)fault;
    return -1;
#endif
}
int plat_trap_arm(u8* p, u64 size) {
#if SNX_UFFD_MINOR
    struct uffdio_register reg;
    struct uffdio_range unreg;
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
static u8 *g_frz_lo, *g_frz_hi;
static int g_frozen; /* a futex word: 1 while frozen */
static _Thread_local void* t_frz_retried; /* the address a thawed write was let go again at once */
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

/* a process without the crash handler (a test): faults outside a frozen range end it as they would have */
static void freeze_signal(int sig, siginfo_t* si, void* ctx) {
    int saved = errno; /* the interrupted code's, which frozen_wait's futex changes */
    (void)ctx;
    if (sent_signal(si)) {
        note_sent(sig, si);
        errno = saved;
        return;
    }
    if (frozen_wait(si->si_addr, access_fault(sig, si))) {
        errno = saved;
        return;
    }
    signal(sig, SIG_DFL);
}
static int g_fault_handler; /* plat_install_crash_handler's, or freeze_signal, catches SIGSEGV and SIGBUS */
void plat_freeze(u8* p, u64 size) {
    if (!__atomic_load_n(&g_fault_handler, __ATOMIC_ACQUIRE)) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = freeze_signal;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        __atomic_store_n(&g_fault_handler, 1, __ATOMIC_RELEASE);
    }
    g_frz_lo = p;
    g_frz_hi = p + size;
    __atomic_store_n(&g_frozen, 1, __ATOMIC_SEQ_CST);
    mprotect(p, (size_t)size, PROT_READ);
}
void plat_thaw(void) {
    /* readable and writable whatever is mapped there now (the copy's pages, or the old ones when it failed) */
    mprotect(g_frz_lo, (size_t)(g_frz_hi - g_frz_lo), PROT_READ | PROT_WRITE);
    __atomic_store_n(&g_frozen, 0, __ATOMIC_SEQ_CST);
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
        if (!access || t_frz_retried == a) return 0;
        t_frz_retried = a;
        return 1;
    }
    if (!__atomic_load_n(&g_frozen, __ATOMIC_SEQ_CST)) {
        if (t_frz_retried == a) return 0; /* faulted again after the thaw: not the freeze's */
        t_frz_retried = a;
        return 1;
    }
    t_frz_retried = NULL;
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
static _Thread_local int t_crashed; /* this thread faulted already */
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
    /* a write to memory frozen while it moves (plat_freeze): it goes on once the move is done */
    if ((sig == SIGSEGV || sig == SIGBUS) && frozen_wait(si->si_addr, access_fault(sig, si))) {
        errno = saved;
        return;
    }
    /* a fault inside the report itself */
    if (t_crashed) _exit(1);
    t_crashed = 1;
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
