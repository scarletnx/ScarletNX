/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The platform layer on Windows: threads, condition variables, the guest's address space with its write watch, file
   reads and maps, the environment and the crash handler (plat_sdl.c has the clocks, sleeping,
   libraries and path queries). Paths are UTF-8, converted to wide for the W functions (the
   manifest's UTF-8 code page makes the A functions and the CRT take UTF-8 too) */
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <SDL3/SDL_thread.h>
#include <errno.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STACK_GUARANTEE 32768 /* room for the crash report after a stack overflow */
#define GUARANTEE_MIN_THREAD ((size_t)256 << 10)

/* ================================================================ timer resolution */
/* The scheduler's default tick is 15.6 ms: asked once for 1 ms before the first timed wait (plat_sleep_ns's
   high-resolution timer needs none). winmm is loaded here so the runtime doesn't link it */
static int g_period_asked;
static void timer_period_1ms(void) {
    HMODULE winmm;
    if (__atomic_load_n(&g_period_asked, __ATOMIC_ACQUIRE) || __atomic_exchange_n(&g_period_asked, 1, __ATOMIC_ACQ_REL))
        return;
    winmm = LoadLibraryW(L"winmm.dll");
    if (winmm) {
        FARPROC f = GetProcAddress(winmm, "timeBeginPeriod");
        UINT(WINAPI * begin)(UINT);
        if (f) {
            memcpy(&begin, &f, sizeof begin);
            begin(1);
        }
    }
}

/* ================================================================ condition variables */
void pc_init(PCond* c) { InitializeConditionVariable(&c->c); }
void pc_signal(PCond* c) { WakeConditionVariable(&c->c); }
void pc_broadcast(PCond* c) { WakeAllConditionVariable(&c->c); }
static int cond_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    DWORD ms = INFINITE;
    if (timeout_ns >= 0) {
        /* whole milliseconds rounded up, in u64 so a timeout near INT64_MAX can't overflow, and kept below INFINITE
           so a long timeout neither wraps in the DWORD nor becomes a wait without end */
        u64 r = (u64)timeout_ns / 1000000u + ((u64)timeout_ns % 1000000u != 0);
        ms = r >= (u64)INFINITE ? INFINITE - 1 : (DWORD)r;
        timer_period_1ms();
    }
    if (SleepConditionVariableSRW(&c->c, &m->l, ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT;
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
    int guarantee;
} ThreadStart;

static DWORD WINAPI thread_start(LPVOID p) {
    ThreadStart s = *(ThreadStart*)p;
    free(p);
    if (s.guarantee) {
        ULONG g = STACK_GUARANTEE;
        SetThreadStackGuarantee(&g);
    }
    s.fn(s.arg);
    SDL_CleanupTLS(); /* SDL's per-thread state, plat_sleep_ns's waitable timer among it */
    return 0;
}

int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    ThreadStart* s = malloc(sizeof *s);
    HANDLE h;
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    s->guarantee = stack == 0 || stack >= GUARANTEE_MIN_THREAD;
    h = CreateThread(NULL, stack, thread_start, s, stack ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, NULL);
    if (!h) {
        DWORD why = GetLastError(); /* the reason, for what the caller says */
        free(s);
        SetLastError(why);
        return -1;
    }
    if (g_settings.profiler) plat_prof_add_thread(h); /* the profiler takes its own handle */
    CloseHandle(h);
    return 0;
}
void plat_thread_exit(void) {
    SDL_CleanupTLS();
    ExitThread(0);
}
int plat_cpus(int* ids, int max) {
    (void)ids;
    (void)max;
    return 0;
}
void plat_thread_cpus(const int* ids, int n) {
    (void)ids;
    (void)n;
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
void plat_yield(void) { SwitchToThread(); }
void plat_exit_now(int code) {
    fflush(NULL);
    /* not ExitProcess: DLL detach code (the video driver's) can wait on a lock one of the stopped threads held */
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    _exit(code);
}

/* ================================================================ the guest's address space */
/* The reservation carries a write watch unless SNX_NO_WW is set: without it the caches hash guest memory to find
   changes (a third of Super Mario Odyssey's GPU thread) */
static u8* g_ww_base;
static u64 g_ww_size;

u64 plat_vm_page(void) {
    u64 page = 4096; /* VirtualAlloc commits and protects 4 KiB pages on every Windows system */
    const char* forced = getenv("SNX_HOST_PAGE");
    if (forced) { /* a larger power of two only: the host's pages can't be split */
        u64 v = strtoull(forced, NULL, 0);
        if (v > page && v <= (1u << 20) && !(v & (v - 1))) page = v;
    }
    return page;
}
/* VirtualAlloc places a reservation at its 64 KiB allocation granularity, which covers SNX_HOST_PAGE up to that */
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
u8* plat_vm_reserve(u64 size) {
    void* p = NULL;
    if (!getenv("SNX_NO_WW")) p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (p) {
        g_ww_base = (u8*)p; /* mem_init runs before any thread that scans */
        g_ww_size = size;
        return (u8*)p;
    }
    p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) { /* the reason in errno, as mmap's */
        DWORD why = GetLastError();
        errno = why == ERROR_NOT_ENOUGH_MEMORY || why == ERROR_COMMITMENT_LIMIT ? ENOMEM : EINVAL;
    }
    return (u8*)p;
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
        for (k = 0; k < n; k++) cb((u8*)pages[k], gran);
        if (n < sizeof pages / sizeof *pages) return 0;
    }
    return -1; /* pages may be left unreported: the caller compares contents */
}

int plat_vm_commit(u8* p, u64 size) {
    /* MEM_COMMIT charges the whole range to the commit limit at once */
    if (VirtualAlloc(p, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE)) return 0;
    WARN("memory: could not commit %llu KB (error %lu): out of memory or page file\n", (unsigned long long)(size >> 10),
         (unsigned long)GetLastError());
    return -1;
}
void plat_vm_decommit(u8* p, u64 size) { VirtualFree(p, (SIZE_T)size, MEM_DECOMMIT); }

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
/* no pool, so no mirror and no traps yet: a GPU result lands at its own fence (docs/GPU_OWNERSHIP.md) */
u8* plat_alias_mirror(void) { return NULL; }
u32 plat_tid(void) { return (u32)GetCurrentThreadId(); }
int plat_thread_in_fault(u32 tid) {
    (void)tid;
    return -1; /* no traps here */
}
void plat_dump_fault_thread(u32 tid) {
    WARN("ownership-fault: host context unavailable for thread %u on this platform\n", tid);
}
int plat_trap_init(void (*fault)(u8* p, int write, u32 tid)) {
    (void)fault;
    return -1;
}
int plat_trap_arm(u8* p, u64 size) {
    (void)p;
    (void)size;
    return -1;
}
int plat_trap_continue(u8* p, u64 size) {
    (void)p;
    (void)size;
    return -1;
}
void plat_trap_disarm(u8* p, u64 size) {
    (void)p;
    (void)size;
}
void plat_freeze(u8* p, u64 size) {
    (void)p;
    (void)size;
}
void plat_thaw(void) {}

/* ================================================================ environment */
int plat_setenv(const char* name, const char* value) { return _putenv_s(name, value) ? -1 : 0; }

/* ================================================================ files */
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off) {
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    u64 got = 0;
    if (h == INVALID_HANDLE_VALUE || size > (u64)INT64_MAX || off > (u64)INT64_MAX - size) return -1;
    while (got < size) {
        OVERLAPPED o;
        DWORD want = size - got > (1u << 30) ? (1u << 30) : (DWORD)(size - got), n = 0;
        u64 at = off + got;
        memset(&o, 0, sizeof o);
        o.Offset = (DWORD)at;
        o.OffsetHigh = (DWORD)(at >> 32);
        if (!ReadFile(h, (u8*)buf + got, want, &n, &o)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return -1;
        }
        if (!n) break;
        got += n;
    }
    return (i64)got;
}

static void file_map_clear(PFileMap* m) {
    m->data = NULL;
    m->size = 0;
    m->borrowed = 0;
    m->heap = 0;
}
static int file_map(PFileMap* m, const char* path, int ask_size, u64 size) {
    wchar_t* w;
    HANDLE file, section;
    void* view;
    file_map_clear(m);
    w = snx_host_wpath(path); /* NULL for a path that isn't UTF-8 */
    if (!w) return -1;
    /* a folder doesn't open without FILE_FLAG_BACKUP_SEMANTICS, so this is a file. Others may read it, or delete or
       rename it as on POSIX */
    file = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (file == INVALID_HANDLE_VALUE) return -1;
    if (ask_size) {
        LARGE_INTEGER s;
        if (!GetFileSizeEx(file, &s) || s.QuadPart < 0) {
            CloseHandle(file);
            return -1;
        }
        size = (u64)s.QuadPart;
    }
    if (size > (u64)SIZE_MAX || size > (u64)INT64_MAX) {
        CloseHandle(file);
        return -1;
    }
    if (!size) {
        CloseHandle(file);
        return 0;
    }
    section = CreateFileMappingW(file, NULL, PAGE_READONLY, (DWORD)(size >> 32), (DWORD)size, NULL);
    view = section ? MapViewOfFile(section, FILE_MAP_READ, 0, 0, (SIZE_T)size) : NULL;
    if (section) CloseHandle(section); /* the view keeps the section and the file */
    CloseHandle(file);
    if (!view) return -1;
    m->data = (const u8*)view;
    m->size = size;
    return 0;
}
int plat_file_map(PFileMap* m, const char* path) { return file_map(m, path, 1, 0); }
int plat_file_map_sized(PFileMap* m, const char* path, u64 size) { return file_map(m, path, 0, size); }
void plat_file_unmap(PFileMap* m) {
    if (m->heap)
        free((void*)m->data);
    else if (m->data && !m->borrowed) UnmapViewOfFile(m->data);
    file_map_clear(m);
}
/* ================================================================ the crash handler */
static LONG g_crash_reporting;
static LONG g_shutdown_said;

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    void* fault = NULL;
    /* one report: it ends the process, so another crashing thread waits for that */
    if (InterlockedCompareExchange(&g_crash_reporting, 1, 0)) Sleep(INFINITE);
    log_crash_mode();
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        r->NumberParameters >= 2)
        fault = (void*)r->ExceptionInformation[1];
    crash_report(r->ExceptionAddress, fault, (int)r->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Failures that may never reach the filter (the CRT fails fast on a buffer overrun or heap corruption): say so in the
   log first, once */
static LONG WINAPI shutdown_note(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000409u || code == 0xC0000374u) &&
        !InterlockedExchange(&g_shutdown_said, 1)) {
        log_crash_mode();
        logf_(0, "*** Shutdown: exception %08lx at %p (stack/heap) ***\n", (unsigned long)code,
              e->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void plat_install_crash_handler(void) {
    ULONG g = STACK_GUARANTEE; /* the main thread's room for a report after a stack overflow */
    SetThreadStackGuarantee(&g);
    AddVectoredExceptionHandler(1, shutdown_note);
    SetUnhandledExceptionFilter(crash_filter);
}

u64 plat_monotonic_ns(void) { return 0; }
