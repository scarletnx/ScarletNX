/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The platform layer on Windows: threads, condition variables, the guest's address space with its write watch, the
   environment and the crash handler (plat_sdl.c has the libraries and path queries, common/platform the clocks,
   sleeping and files). Paths are UTF-8, converted to wide for the W functions (the manifest's UTF-8 code page makes
   the A functions and the CRT take UTF-8 too) */
#include "nx/runtime/platform/plat.h"
#include "common/utilities/run_mark.h"
#include "common/platform/vm.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include <SDL3/SDL_thread.h>
#include <errno.h>
#include <io.h>
#include <powersetting.h>
#include <powrprof.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ threads */
/* what each thread plat_thread starts (common/platform/thread.c) does here: SDL's per-thread state (plat_sleep_ns's
   waitable timer among it) ended with it, and the sampling profiler given it. A constructor, so it is in place before
   any thread, in the runtime and in each test that compiles this file */
static void sdl_thread_end(void) { SDL_CleanupTLS(); }
static void profile_thread(HANDLE h) {
    if (g_settings.profiler) plat_prof_add_thread(h);
}
__attribute__((constructor)) static void plat_win32_threads(void) {
    static const PThreadHooks hooks = {NULL, sdl_thread_end, profile_thread};
    plat_thread_hooks(&hooks);
}

/* ================================================================ the system's low memory */
/* The memory resource notifications: low while the available physical memory is under the system's mark (32 MiB per
   4 GiB, 64 MiB at most), high once it is about three times that. A line at each fall to the low mark, then a wait for
   the high one, so memory that stays low makes one line */
static struct {
    HANDLE low, high;
    int (*say)(const char* why);
} g_memory_watch;
static void memory_watch_thread(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("memory pressure");
    for (;;) {
        if (WaitForSingleObject(g_memory_watch.low, INFINITE) != WAIT_OBJECT_0) return;
        if (!g_memory_watch.say(
                "the system's available memory fell to its low mark (Windows' low memory notification)"))
            return;
        if (WaitForSingleObject(g_memory_watch.high, INFINITE) != WAIT_OBJECT_0) return;
    }
}
int plat_memory_watch(int (*say)(const char* why)) {
    char why[256];
    if (g_memory_watch.say) return 0;
    g_memory_watch.low = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    g_memory_watch.high = g_memory_watch.low ? CreateMemoryResourceNotification(HighMemoryResourceNotification) : NULL;
    if (!g_memory_watch.high) {
        snx_os_error(why, sizeof why);
        LOG("memory: no watch of the system's low memory: CreateMemoryResourceNotification: %s\n", why);
        if (g_memory_watch.low) CloseHandle(g_memory_watch.low);
        g_memory_watch.low = NULL;
        return -1;
    }
    g_memory_watch.say = say;
    if (plat_thread(memory_watch_thread, NULL, 0)) {
        snx_os_error(why, sizeof why);
        LOG("memory: no watch of the system's low memory: its thread does not start: %s\n", why);
        g_memory_watch.say = NULL;
        return -1;
    }
    return 0;
}

/* ================================================================ the host's power source */
/* the notification marks this; the main thread takes it, so only there does the runtime ask SDL for the new state */
static int g_power_changed;
void plat_power_mark_changed(void) { __atomic_store_n(&g_power_changed, 1, __ATOMIC_RELEASE); }
int plat_power_take_change(void) { return __atomic_exchange_n(&g_power_changed, 0, __ATOMIC_ACQ_REL); }
/* The system's power setting notification for the AC/DC source: the charger in or out calls this on a system thread.
   The SDK's DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS is not in every _WIN32_WINNT's powrprof.h, so this is the same two
   pointers, and DEVICE_NOTIFY_CALLBACK is 2 (winuser.h) */
#ifndef DEVICE_NOTIFY_CALLBACK
#define DEVICE_NOTIFY_CALLBACK 2
#endif
typedef ULONG(CALLBACK* SnxPowerCallback)(PVOID, ULONG, PVOID);
typedef struct {
    SnxPowerCallback Callback;
    PVOID Context;
} SnxPowerSubscribe;
static ULONG CALLBACK power_changed(PVOID context, ULONG type, PVOID setting) {
    (void)context;
    (void)type;
    (void)setting;
    plat_power_mark_changed();
    return ERROR_SUCCESS;
}
static SnxPowerSubscribe g_power_sub = {power_changed, NULL};
static HPOWERNOTIFY g_power_notify;
int plat_power_watch(void) {
    /* GUID_ACDC_POWER_SOURCE: 5D3E9A59-E9D5-4B00-A6BD-FF34FF516548 */
    static const GUID acdc = {0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
    if (g_power_notify) return 0;
    if (PowerSettingRegisterNotification(&acdc, DEVICE_NOTIFY_CALLBACK, &g_power_sub, &g_power_notify) !=
        ERROR_SUCCESS)
        return -1;
    return 0;
}
typedef LONG(WINAPI* RtlGetVersionFn)(RTL_OSVERSIONINFOW*);
void plat_system_info(char* out, size_t cap) {
    static const char key[] = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    char os[128], release[32] = {0}, cpu[128] = {0}, *c = cpu;
    DWORD size;
    size_t n;
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    /* GetVersionEx answers what the manifest declares; ntdll's RtlGetVersion the real version */
    RTL_OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    RtlGetVersionFn get =
        (RtlGetVersionFn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!get || get(&v)) memset(&v, 0, sizeof v);
    size = sizeof release; /* "23H2", from Windows 10 20H2 on */
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "DisplayVersion", RRF_RT_REG_SZ, NULL, release, &size)) release[0] = 0;
    /* Windows 11 kept the version 10.0, and its registry's ProductName still says Windows 10 */
    if (v.dwMajorVersion == 10)
        snprintf(os, sizeof os, "Windows %s%s%s (10.0.%lu)", v.dwBuildNumber >= 22000 ? "11" : "10",
                 release[0] ? " " : "", release, (unsigned long)v.dwBuildNumber);
    else
        snprintf(os, sizeof os, "Windows %lu.%lu.%lu", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
                 (unsigned long)v.dwBuildNumber);
    size = sizeof cpu;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, NULL, cpu, &size))
        cpu[0] = 0;
    while (*c == ' ') c++; /* some CPUs pad their brand string */
    for (n = strlen(c); n && c[n - 1] == ' '; n--) c[n - 1] = 0;
    if (!GlobalMemoryStatusEx(&m)) m.ullTotalPhys = 0;
    snprintf(out, cap, "%s%s%s, %lu threads, %llu GB RAM", os, *c ? ", " : "", c,
             (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
             (unsigned long long)((m.ullTotalPhys + (512ull << 20)) >> 30));
}
void plat_exit_now(int code) {
    fflush(NULL);
    snx_run_mark_end(); /* an exit: TerminateProcess runs no atexit handler */
    /* not ExitProcess: DLL detach code (the video driver's) can wait on a lock one of the stopped threads held */
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    _exit(code);
}

/* ================================================================ the guest's address space */
/* The reservation carries a write watch, which plat_ww_scan reads unless write_tracking=2: without it the caches hash
   guest memory to find changes (a third of Super Mario Odyssey's GPU thread). The watch is chosen as the reservation
   is made, before the settings are read */
static u8* g_ww_base;
static u64 g_ww_size;
static u8* g_ww_watched; /* plat_vm_reserve's reservation, when it has the watch */
/* plat_vm_track's reservation, whose pages the traps arm (below) */
static u8* g_vm_base;
static u64 g_vm_size;
static int trap_armed_at(const void* a);
static void trap_forget(u8* p, u64 size);

static int g_host_page_said; /* plat_vm_page told why it refused host_page */
u64 plat_vm_page(void) {
    u64 page = 4096; /* VirtualAlloc commits and protects 4 KiB pages on every Windows system */
    u64 v = g_settings.host_page_size > 0 ? (u64)g_settings.host_page_size : 0;
    if (!v) return page;
    if (v >= page && v <= SNX_PLAT_PAGE_MAX && !(v & (v - 1))) return v; /* the host's pages can't be split */
    if (!g_host_page_said) { /* mem_init asks first, on the main thread at startup */
        g_host_page_said = 1;
        WARN("memory: host_page=%llu is not a power of two from the host's page, %llu bytes, to %llu: the runtime "
             "works in %llu-byte pages\n",
             (unsigned long long)v, (unsigned long long)page, (unsigned long long)SNX_PLAT_PAGE_MAX,
             (unsigned long long)page);
    }
    return page;
}
int plat_address_space(u64* mapped, u64* largest_free) {
    SYSTEM_INFO system;
    MEMORY_BASIC_INFORMATION region;
    const u8* at;
    u64 total = 0, hole = 0;
    GetSystemInfo(&system);
    at = (const u8*)system.lpMinimumApplicationAddress;
    while (at < (const u8*)system.lpMaximumApplicationAddress &&
           VirtualQuery(at, &region, sizeof region) == sizeof region) {
        if (region.State == MEM_FREE) {
            if (region.RegionSize > hole) hole = region.RegionSize;
        } else
            total += region.RegionSize; /* reserved or committed */
        at = (const u8*)region.BaseAddress + region.RegionSize;
    }
    if (!total) return 0;
    *mapped = total;
    *largest_free = hole;
    return 1;
}
/* the reason in errno, as mmap's */
static u8* vm_reserved(void* p) {
    if (!p) {
        DWORD why = GetLastError();
        errno = why == ERROR_NOT_ENOUGH_MEMORY || why == ERROR_COMMITMENT_LIMIT ? ENOMEM : EINVAL;
    }
    return (u8*)p;
}
u8* plat_vm_reserve(u64 size) {
    u8* p = vm_reserved(VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS));
    if (p) g_ww_watched = p;
    return p;
}
/* at the 64 KiB allocation granularity, which commits any 4 KiB page */
u8* plat_vm_reserve_unwatched(u64 size) { return (u8*)plat_mem_reserve(size, 0); }
void plat_vm_track(u8* p, u64 size) {
    g_vm_base = p; /* mem_init runs before any thread that traps */
    g_vm_size = size;
    if (g_settings.write_tracking == 1)
        WARN("memory: write_tracking=1 (page protection) has no Windows version: %s\n",
             p == g_ww_watched ? "the reservation's write watch tracks writes, as write_tracking=0 does"
                               : "the reservation has no write watch, so the caches compare contents");
    if (g_settings.write_tracking_hot_pages == 1)
        WARN("memory: write_tracking_hot=1 leaves pages hot only where page protection tracks the writes, which has "
             "no Windows version: no page is hot this run\n");
    if (p != g_ww_watched || g_settings.write_tracking == 2) return;
    g_ww_base = p; /* mem_init runs before any thread that scans */
    g_ww_size = size;
}

int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    void* pages[1024]; /* on the stack: two threads may scan at once */
    u64 start, end, calls, i;
    if (!g_ww_base || !size) return g_ww_base ? 0 : -1;
    start = (u64)(uintptr_t)p & ~(u64)4095;
    end = ((u64)(uintptr_t)p + size + 4095) & ~(u64)4095;
    if (start < (u64)(uintptr_t)g_ww_base || end > (u64)(uintptr_t)g_ww_base + g_ww_size) return -1;
    /* each call reports and resets up to 1024 pages, so the range needs at most this many */
    calls = (end - start) / (4u << 20) + 2;
    for (i = 0; i < calls; i++) {
        ULONG_PTR n = sizeof pages / sizeof *pages, k;
        ULONG gran = 4096;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void*)(uintptr_t)start, (SIZE_T)(end - start), pages, &n, &gran))
            return -1;
        /* a page a trap holds is not reported: its consumers look the ownership up, and it reads as written once the
           trap lets it go (trap_open writes it) */
        for (k = 0; k < n; k++)
            if (!trap_armed_at(pages[k])) cb((u8*)pages[k], gran);
        if (n < sizeof pages / sizeof *pages) return 0;
    }
    return -1; /* pages may be left unreported: the caller compares contents */
}

/* the write watch sees the kernel's writes (ReadFile, recv) as it sees any other */
void plat_ww_open(u8* p, u64 size) {
    (void)p;
    (void)size;
}
void plat_ww_close(u8* p, u64 size) {
    (void)p;
    (void)size;
}
/* no hot pages: the write watch takes no faults to spare */
int plat_ww_hot_in(const u8* p, u64 size) {
    (void)p;
    (void)size;
    return 0;
}
void plat_ww_cool(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    (void)p;
    (void)size;
    (void)cb;
}

int plat_vm_commit(u8* p, u64 size) {
    trap_forget(p, size); /* readable and writable from here: no trap's (mem.c lets go of owners first) */
    /* MEM_COMMIT charges the whole range to the commit limit at once */
    if (!plat_mem_protect(p, size, PLAT_MEM_READ | PLAT_MEM_WRITE)) return 0;
    WARN("memory: could not commit %llu KB (error %lu): out of memory or page file\n", (unsigned long long)(size >> 10),
         (unsigned long)GetLastError());
    return -1;
}
void plat_vm_decommit(u8* p, u64 size) {
    (void)plat_mem_decommit(p, size);
    trap_forget(p, size);
}
int plat_vm_share(u8* p, int fd, u64 size) {
    (void)p, (void)fd, (void)size;
    errno = ENOSYS;
    return -1;
}
int plat_vm_unshare(u8* p, u64 size) {
    (void)p, (void)size;
    errno = ENOSYS;
    return -1;
}
int plat_gpu_buffer(u64 size, int* fd, void** handle) {
    (void)size;
    *fd = -1;
    *handle = NULL;
    errno = ENOSYS;
    return -1;
}
void plat_gpu_buffer_free(int fd, void* handle, u64 size) { (void)fd, (void)handle, (void)size; }

/* No alias pool yet: views of the same pages need the reservation made of placeholders (VirtualAlloc2 and
   MapViewOfFile3, Windows 10 1803), and whether the write watch survives that is untested. mem.c copies instead */
int plat_alias_init(u64 size) {
    (void)size;
    return -1;
}
int plat_alias_map(u8* p, u64 off, u64 size) {
    (void)p;
    (void)off;
    (void)size;
    return -1;
}
int plat_alias_unmap(u8* p, u64 size) {
    (void)p;
    (void)size;
    return -1;
}
int plat_vm_map_fd(u8* p, u64 size, int fd, u64 off, u64 allocation_size) {
    (void)p;
    (void)size;
    (void)fd;
    (void)off;
    (void)allocation_size;
    return -1;
}
void plat_vm_fd_close(int fd) { (void)fd; }
int plat_alias_write(u64 off, const void* src, u64 size) {
    (void)off;
    (void)src;
    (void)size;
    return -1;
}
int plat_alias_read(u64 off, void* dst, u64 size) {
    (void)off;
    (void)dst;
    (void)size;
    return -1;
}
void plat_alias_free(u64 off, u64 size) {
    (void)off;
    (void)size;
}
u8* plat_alias_host(u64 off, u64 size) {
    (void)off;
    (void)size;
    return NULL;
}
void plat_alias_host_free(u8* p, u64 size) {
    (void)p;
    (void)size;
}
/* no pool, so no mirror: the traps land in place (SNX_PLAT_TRAPS_IN_PLACE, plat_trap_fill) */
u8* plat_alias_mirror(void) { return NULL; }
u32 plat_tid(void) { return (u32)GetCurrentThreadId(); }

/* ---------------------------------------------------------------- traps (memory/own.c, docs/GPU_OWNERSHIP.md) */
/* Traps by protection, as Linux's without userfaultfd's minor faults (plat_posix.c), on the reservation's own pages:
   plat_trap_arm makes them inaccessible (VirtualProtect) and marks them in a bitmap, a bit per 4 KiB page. A guest
   access to one raises an access violation, which the vectored exception handler (trap_fault) takes first, on the
   faulting thread in its ordinary context: it posts the page to the trap thread, which calls the fault callback, and
   waits (WaitOnAddress) until some trap lets pages go (g_trap_gen moves); then the access goes again, and faults again
   if its page is still held. The callback lands the page (plat_trap_fill writes the GPU's bytes in place, there being
   no mirror) and calls plat_trap_continue, which opens the pages and wakes the waiters. A system call given an armed
   page fails (ERROR_NOACCESS, WSAEFAULT) rather than faulting: the runtime lands what it hands the kernel first
   (own_acquire). The handler runs nothing but atomics and the waits */
static u64* g_trap_bits; /* atomic: a bit per 4 KiB page of [g_vm_base, g_vm_base + g_vm_size), set while armed */
static void (*g_trap_cb)(u8* p, int write, u32 tid);
static void (*g_trap_told)(u32 tid); /* plat_trap_waiting's */
static u32 g_trap_thread;            /* atomic: the trap thread's id, which never waits on a trap */
#define TRAP_RING 256u
typedef struct {
    u8* p;
    u32 tid, seq; /* seq: the Vyukov queue's turn */
    int write;
} TrapMsg;
static TrapMsg g_trap_ring[TRAP_RING];
static u32 g_trap_head;   /* atomic: the next slot a fault claims */
static u32 g_trap_tail;   /* the trap thread's */
static u32 g_trap_posted; /* atomic, a word to wait on: moves with each message */
static u32 g_trap_gen;    /* atomic, a word to wait on: moves when a trap lets pages go */
static u32 g_trap_opened; /* atomic: moves before pages a trap held become accessible (trap_fault's retry) */
#define TRAP_WAITERS 256
static u32 g_trap_waiting[TRAP_WAITERS]; /* atomic: the threads waiting in a trap, for plat_thread_in_fault */
static u32 g_trap_unlisted;              /* atomic: threads waiting with no room in that table, which makes it unsure */
/* what each listed waiter faulted at, for plat_dump_fault_thread: its code's address, its stack's and the access's */
static const void* g_trap_wait_pc[TRAP_WAITERS];
static const void* g_trap_wait_sp[TRAP_WAITERS];
static const void* g_trap_wait_at[TRAP_WAITERS];
/* the address a fault on a page no trap held was let go again at, and g_trap_opened then: the same fault again with
   no page opened since is no trap's. Pages opened by other traps in between let it go again, but not past
   TRAP_RETRIES times in a row: a page inaccessible with no trap on it (a bug) is reported then, not spun on */
#define TRAP_RETRIES 4096u
static _Thread_local const void* t_trap_retried;
static _Thread_local u32 t_trap_retried_at;
static _Thread_local u32 t_trap_retries;
static int g_trap_stray_said;

/* the bit of the page at a: 1 with its index in *i, or 0 outside the reservation (or without traps) */
static int trap_index(const void* a, u64* i) {
    const u64* bits = __atomic_load_n(&g_trap_bits, __ATOMIC_ACQUIRE);
    if (!bits || (const u8*)a < g_vm_base || (u64)((const u8*)a - g_vm_base) >= g_vm_size) return 0;
    *i = (u64)((const u8*)a - g_vm_base) >> 12;
    return 1;
}
static int trap_bit(u64 i) { return (int)(__atomic_load_n(&g_trap_bits[i >> 6], __ATOMIC_SEQ_CST) >> (i & 63) & 1); }
static int trap_armed_at(const void* a) {
    u64 i;
    return trap_index(a, &i) && trap_bit(i);
}
/* the pages [*lo, *hi) of [p, p + size), whole 4 KiB pages in the reservation: 1, or 0 when they aren't */
static int trap_pages(const u8* p, u64 size, u64* lo, u64* hi) {
    if (!size || ((uintptr_t)p | size) & 4095 || !trap_index(p, lo) || size > g_vm_size - (u64)(p - g_vm_base))
        return 0;
    *hi = *lo + (size >> 12);
    return 1;
}
static void trap_wake(void) {
    __atomic_fetch_add(&g_trap_gen, 1, __ATOMIC_SEQ_CST);
    WakeByAddressAll(&g_trap_gen);
}
/* the armed pages of [lo, hi) open (readable and writable), written once so the write watch reports them (a page a
   trap lets go reads as written, as on the other hosts), let go, and their waiters woken. Under own.c's trap lock, or
   on one thread (a test's) */
static void trap_open(u64 lo, u64 hi) {
    u64 i = lo, run, k;
    int any = 0;
    while (i < hi) {
        DWORD old;
        u8* a;
        if (!trap_bit(i)) {
            i++;
            continue;
        }
        for (run = i + 1; run < hi && trap_bit(run); run++) {}
        a = g_vm_base + (i << 12);
        if (!VirtualProtect(a, (SIZE_T)((run - i) << 12), PAGE_READWRITE, &old)) {
            /* given back meanwhile (decommitted): nothing to open, and the bits go below */
            if (GetLastError() != ERROR_INVALID_ADDRESS)
                fatal("Could not let trapped guest memory go at %p (Windows error %lu)", (void*)a,
                      (unsigned long)GetLastError());
        } else
            for (k = i; k < run; k++) __atomic_fetch_or(g_vm_base + (k << 12), (u8)0, __ATOMIC_RELAXED);
        /* before the bits go: a fault that raced this finds the count moved, and goes again (trap_fault) */
        __atomic_fetch_add(&g_trap_opened, 1, __ATOMIC_SEQ_CST);
        for (k = i; k < run; k++) __atomic_fetch_and(&g_trap_bits[k >> 6], ~(1ull << (k & 63)), __ATOMIC_SEQ_CST);
        any = 1;
        i = run;
    }
    if (any) trap_wake();
}
/* [p, p + size) committed or decommitted by mem.c: none of it is armed any more (its owners were let go first; a
   page left marked would keep a waiter waiting, and keep it from the scans) */
static void trap_forget(u8* p, u64 size) {
    u64 lo, hi, i;
    int any = 0;
    if (!__atomic_load_n(&g_trap_bits, __ATOMIC_ACQUIRE) || !size) return;
    if (!trap_index(p, &lo)) return;
    if (size > g_vm_size - (u64)(p - g_vm_base)) size = g_vm_size - (u64)(p - g_vm_base);
    hi = (u64)(p + size - g_vm_base + 4095) >> 12;
    for (i = lo; i < hi; i++)
        if (trap_bit(i)) {
            if (!any) __atomic_fetch_add(&g_trap_opened, 1, __ATOMIC_SEQ_CST);
            __atomic_fetch_and(&g_trap_bits[i >> 6], ~(1ull << (i & 63)), __ATOMIC_SEQ_CST);
            any = 1;
        }
    if (any) trap_wake();
}
/* in the handler: page i is held by a trap. Posts it and waits for a trap to let pages go */
static void trap_wait(u64 i, int write, const void* at, const CONTEXT* c) {
    u32 gen = __atomic_load_n(&g_trap_gen, __ATOMIC_SEQ_CST), tid = (u32)GetCurrentThreadId(), pos, k, none = 0;
    int slot = -1;
    TrapMsg* m;
    for (k = 0; k < TRAP_WAITERS && slot < 0; k++, none = 0)
        if (__atomic_compare_exchange_n(&g_trap_waiting[k], &none, tid, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            slot = (int)k;
    if (slot >= 0) {
#if defined(_M_X64) || defined(__x86_64__)
        g_trap_wait_pc[slot] = (const void*)(uintptr_t)c->Rip;
        g_trap_wait_sp[slot] = (const void*)(uintptr_t)c->Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
        g_trap_wait_pc[slot] = (const void*)(uintptr_t)c->Pc;
        g_trap_wait_sp[slot] = (const void*)(uintptr_t)c->Sp;
#else
        (void)c;
#endif
        g_trap_wait_at[slot] = at;
    } else
        __atomic_fetch_add(&g_trap_unlisted, 1, __ATOMIC_ACQ_REL);
    for (;;) { /* a slot whose turn it is: a full ring waits for the trap thread */
        pos = __atomic_load_n(&g_trap_head, __ATOMIC_RELAXED);
        m = &g_trap_ring[pos % TRAP_RING];
        k = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
        if (k == pos) {
            if (__atomic_compare_exchange_n(&g_trap_head, &pos, pos + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
        } else if ((i32)(k - pos) < 0)
            SwitchToThread();
    }
    m->p = g_vm_base + (i << 12);
    m->tid = tid;
    m->write = write;
    __atomic_store_n(&m->seq, pos + 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_trap_posted, 1, __ATOMIC_SEQ_CST);
    WakeByAddressSingle(&g_trap_posted);
    while (trap_bit(i) && __atomic_load_n(&g_trap_gen, __ATOMIC_SEQ_CST) == gen)
        WaitOnAddress(&g_trap_gen, &gen, sizeof gen, INFINITE);
    if (slot >= 0)
        __atomic_store_n(&g_trap_waiting[slot], 0, __ATOMIC_RELEASE);
    else
        __atomic_fetch_sub(&g_trap_unlisted, 1, __ATOMIC_RELEASE);
}
/* The vectored exception handler, ahead of the crash handlers (code_fault, crash_filter): an access violation in the
   reservation on an armed page waits for its landing and goes again. One on a page armed when it faulted but let go
   since goes again at once; the same fault again with no page opened meanwhile is no trap's, and goes on to the crash
   handlers. ExceptionInformation[0] is 0 for a read, 1 for a write, 8 for an execution */
static LONG WINAPI trap_fault(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    const void* a;
    u64 i;
    u32 opened;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    a = (const void*)r->ExceptionInformation[1];
    if (!trap_index(a, &i)) return EXCEPTION_CONTINUE_SEARCH;
    /* the trap thread's own fault would wait for itself: a bug in a landing, reported as a crash */
    if ((u32)GetCurrentThreadId() == __atomic_load_n(&g_trap_thread, __ATOMIC_ACQUIRE))
        return EXCEPTION_CONTINUE_SEARCH;
    if (trap_bit(i)) {
        trap_wait(i, r->ExceptionInformation[0] == 1, a, e->ContextRecord);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    opened = __atomic_load_n(&g_trap_opened, __ATOMIC_SEQ_CST);
    if (t_trap_retried == a) {
        if (t_trap_retried_at == opened) return EXCEPTION_CONTINUE_SEARCH;
        if (++t_trap_retries >= TRAP_RETRIES) {
            MEMORY_BASIC_INFORMATION m;
            if (!InterlockedExchange((LONG*)&g_trap_stray_said, 1) && VirtualQuery(a, &m, sizeof m) == sizeof m)
                WARN("memory: %p faulted %u times in a row with no trap armed on its page (state %lx, protection %lx), "
                     "while other traps let pages go: no trap's\n",
                     a, t_trap_retries, (unsigned long)m.State, (unsigned long)m.Protect);
            t_trap_retries = 0;
            return EXCEPTION_CONTINUE_SEARCH;
        }
    } else
        t_trap_retries = 0;
    t_trap_retried = a;
    t_trap_retried_at = opened;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void trap_thread(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("memory traps");
    __atomic_store_n(&g_trap_thread, (u32)GetCurrentThreadId(), __ATOMIC_RELEASE);
    for (;;) {
        u32 seen = __atomic_load_n(&g_trap_posted, __ATOMIC_SEQ_CST);
        for (;;) {
            TrapMsg* m = &g_trap_ring[g_trap_tail % TRAP_RING];
            TrapMsg got;
            void (*told)(u32);
            if (__atomic_load_n(&m->seq, __ATOMIC_ACQUIRE) != g_trap_tail + 1) break;
            got = *m;
            __atomic_store_n(&m->seq, g_trap_tail + TRAP_RING, __ATOMIC_RELEASE);
            g_trap_tail++;
            told = __atomic_load_n(&g_trap_told, __ATOMIC_ACQUIRE);
            if (told) told(got.tid);
            g_trap_cb(got.p, got.write, got.tid);
        }
        WaitOnAddress(&g_trap_posted, &seen, sizeof seen, INFINITE);
    }
}
void plat_trap_waiting(void (*waiting)(u32 tid)) { __atomic_store_n(&g_trap_told, waiting, __ATOMIC_RELEASE); }
/* a thread waiting in a trap says where; the system tells nothing else here without a debugger */
