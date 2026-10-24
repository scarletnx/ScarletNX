/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The platform layer on Windows: threads, time, the guest's address space with its write watch, files, libraries,
   the text entry dialog and the crash handler. Paths are UTF-8, converted to wide for the W functions (the
   manifest's UTF-8 code page makes the A functions and the CRT take UTF-8 too) */
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#define STACK_GUARANTEE 32768 /* room for the crash report after a stack overflow */
#define GUARANTEE_MIN_THREAD ((size_t)256 << 10)

/* ================================================================ timer resolution */
/* The scheduler's default tick is 15.6 ms: asked once for 1 ms before the first sleep or timed wait. winmm is
   loaded here so the runtime doesn't link it */
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
/* each thread's waitable timer for plat_sleep_ns, made on its first sleep and closed when a plat_thread ends */
static __thread HANDLE t_sleep_timer;

static void sleep_timer_close(void) {
    if (t_sleep_timer) CloseHandle(t_sleep_timer);
    t_sleep_timer = NULL;
}

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
    sleep_timer_close();
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
    sleep_timer_close();
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
void plat_sleep_ns(i64 ns) {
    LARGE_INTEGER due;
    if (ns <= 0) {
        SwitchToThread();
        return;
    }
    timer_period_1ms();
    if (!t_sleep_timer) {
        t_sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t_sleep_timer) t_sleep_timer = CreateWaitableTimerW(NULL, TRUE, NULL); /* before Windows 10 1803 */
    }
    due.QuadPart = -(LONGLONG)((u64)ns / 100u + ((u64)ns % 100u != 0)); /* relative, in 100 ns units */
    if (t_sleep_timer && SetWaitableTimer(t_sleep_timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(t_sleep_timer, INFINITE);
    else {
        u64 ms = (u64)ns / 1000000u + ((u64)ns % 1000000u != 0);
        Sleep(ms >= (u64)INFINITE ? INFINITE - 1 : (DWORD)ms);
    }
}
void plat_exit_now(int code) {
    fflush(NULL);
    /* not ExitProcess: DLL detach code (the video driver's) can wait on a lock one of the stopped threads held */
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    _exit(code);
}

/* ================================================================ time */
u64 plat_ns(void) {
    static LARGE_INTEGER freq; /* set once, before any other thread runs: the first log line asks */
    LARGE_INTEGER c;
    u64 f, n;
    if (!__atomic_load_n(&freq.QuadPart, __ATOMIC_RELAXED)) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        __atomic_store_n(&freq.QuadPart, q.QuadPart, __ATOMIC_RELAXED);
    }
    QueryPerformanceCounter(&c);
    f = (u64)__atomic_load_n(&freq.QuadPart, __ATOMIC_RELAXED);
    n = (u64)c.QuadPart;
    /* split so n * 1e9 can't overflow */
    return n / f * 1000000000u + n % f * 1000000000u / f;
}
i64 plat_unix_time(void) {
    FILETIME ft;
    u64 t;
    GetSystemTimeAsFileTime(&ft);
    t = (u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return (i64)((t - 116444736000000000ull) / 10000000u); /* 100 ns units since 1601 */
}
i64 plat_tz_offset(void) {
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    LONG bias = tz.Bias;
    if (r == TIME_ZONE_ID_INVALID) return 0;
    bias += r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias;
    return -(i64)bias * 60; /* Bias is UTC minus local, in minutes */
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
u8* plat_vm_reserve(u64 size) {
    void* p = NULL;
    if (!getenv("SNX_NO_WW")) p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (p) {
        g_ww_base = (u8*)p; /* mem_init runs before any thread that scans */
        g_ww_size = size;
        return (u8*)p;
    }
    return (u8*)VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
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

/* ================================================================ libraries */
void* plat_dl_open(const char* path) {
    wchar_t* w = snx_host_wpath(path);
    HMODULE h;
    if (!w) {
        SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        return NULL;
    }
    h = LoadLibraryW(w);
    free(w);
    return (void*)h;
}
void* plat_dl_sym(void* h, const char* name) {
    FARPROC f = GetProcAddress((HMODULE)h, name);
    void* p;
    memcpy(&p, &f, sizeof p);
    return p;
}
int plat_setenv(const char* name, const char* value) { return _putenv_s(name, value) ? -1 : 0; }
const char* plat_dl_error(void) {
    static __thread char msg[512];
    DWORD e = GetLastError(), n;
    if (!e) return "unknown error";
    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, 400, NULL);
    while (n && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' ')) n--;
    if (!n) {
        snprintf(msg, sizeof msg, "unknown error (error %lu)", (unsigned long)e);
        return msg;
    }
    snprintf(msg + n, sizeof msg - n, " (error %lu)", (unsigned long)e);
    return msg;
}

/* ================================================================ files and folders */
static int file_attributes(const char* path, WIN32_FILE_ATTRIBUTE_DATA* a) {
    wchar_t* w = snx_host_wpath(path);
    int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, a);
    free(w);
    return ok;
}
int plat_is_dir(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return file_attributes(path, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
u64 plat_file_size(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!file_attributes(path, &a)) return 0;
    return (u64)a.nFileSizeHigh << 32 | a.nFileSizeLow;
}

typedef struct {
    HANDLE find;
    WIN32_FIND_DATAW data;
} DirState;
static void dir_entry(PDir* d, const WIN32_FIND_DATAW* f) {
    if (!WideCharToMultiByte(CP_UTF8, 0, f->cFileName, -1, d->name, (int)sizeof d->name, NULL, NULL)) d->name[0] = 0;
    d->is_dir = (f->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    d->size = d->is_dir ? 0 : (u64)f->nFileSizeHigh << 32 | f->nFileSizeLow;
}
static int dir_skip(const WIN32_FIND_DATAW* f) {
    return !wcscmp(f->cFileName, L".") || !wcscmp(f->cFileName, L"..");
}
int plat_dir_open(PDir* d, const char* path) {
    char pattern[1024];
    wchar_t* w;
    DirState* s;
    memset(d, 0, sizeof *d);
    /* a cut pattern would list some other folder */
    if ((size_t)snprintf(pattern, sizeof pattern, "%s\\*", path) >= sizeof pattern) return -1;
    w = snx_host_wpath(pattern);
    s = w ? malloc(sizeof *s) : NULL;
    if (!s) {
        free(w);
        return -1;
    }
    s->find = FindFirstFileW(w, &s->data);
    free(w);
    if (s->find == INVALID_HANDLE_VALUE) {
        free(s);
        return -1;
    }
    d->h = s;
    d->first = 1;
    return 0;
}
int plat_dir_next(PDir* d) {
    DirState* s = (DirState*)d->h;
    if (!s) return 0;
    for (;;) {
        if (d->first)
            d->first = 0;
        else if (!FindNextFileW(s->find, &s->data))
            return 0;
        if (dir_skip(&s->data)) continue;
        dir_entry(d, &s->data);
        if (d->name[0]) return 1;
    }
}
void plat_dir_close(PDir* d) {
    DirState* s = (DirState*)d->h;
    if (s) {
        FindClose(s->find);
        free(s);
    }
    d->h = NULL;
}

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
int plat_replace_file(const char* tmp, const char* dst) {
    wchar_t* wt = snx_host_wpath(tmp);
    wchar_t* wd = snx_host_wpath(dst);
    /* rename() fails when dst exists, and deleting dst first would lose it to a crash in between */
    int ok = wt && wd && MoveFileExW(wt, wd, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        if (wt)
            DeleteFileW(wt);
        else
            remove(tmp);
    }
    free(wt);
    free(wd);
    return ok ? 0 : -1;
}

/* ================================================================ text entry */
enum { TI_EDIT = 100, TI_W = 420, TI_H = 150 };
typedef struct {
    HWND edit;
    char* out;
    size_t cap;
    int result; /* -2 while open */
} TextInput;

static void text_input_ok(HWND wnd, TextInput* t) {
    int n = GetWindowTextLengthW(t->edit), bytes;
    wchar_t* w = malloc(((size_t)n + 1) * sizeof *w);
    if (!w) {
        t->result = -1;
        return;
    }
    GetWindowTextW(t->edit, w, n + 1);
    bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0 || (size_t)bytes > t->cap) {
        free(w);
        MessageBoxW(wnd, L"Text is too long.", L"ScarletNX", MB_OK | MB_ICONWARNING);
        return; /* the dialog stays */
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, t->out, bytes, NULL, NULL);
    free(w);
    t->result = 0;
}

static LRESULT CALLBACK text_input_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    TextInput* t = (TextInput*)GetWindowLongPtrW(wnd, GWLP_USERDATA);
    if (t && msg == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) text_input_ok(wnd, t);
        if (LOWORD(wp) == IDCANCEL) t->result = 1;
        return 0;
    }
    if (t && msg == WM_CLOSE) {
        t->result = 1;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int plat_text_input(char* out, size_t cap, const char* initial) {
    static const wchar_t cls[] = L"ScarletNXTextInput";
    WNDCLASSW wc;
    TextInput t;
    HINSTANCE inst = GetModuleHandleW(NULL);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND wnd, child[4];
    wchar_t* w;
    MSG m;
    int x, y, i;
    if (!cap) return -1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = text_input_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
    x = (GetSystemMetrics(SM_CXSCREEN) - TI_W) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - TI_H) / 2;
    wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, cls, L"ScarletNX text entry",
                          WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, TI_W, TI_H, NULL, NULL, inst, NULL);
    if (!wnd) return -1;
    memset(&t, 0, sizeof t);
    t.out = out;
    t.cap = cap;
    t.result = -2;
    w = snx_host_wpath(initial ? initial : "");
    child[0] = CreateWindowExW(0, L"STATIC", L"Enter text:", WS_CHILD | WS_VISIBLE, 12, 12, 380, 20, wnd, NULL, inst,
                               NULL);
    child[1] = t.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", w ? w : L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 36, 380, 24, wnd,
                                        (HMENU)(INT_PTR)TI_EDIT, inst, NULL);
    child[2] = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 212, 72, 85,
                               26, wnd, (HMENU)(INT_PTR)IDOK, inst, NULL);
    child[3] = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 307, 72, 85, 26, wnd,
                               (HMENU)(INT_PTR)IDCANCEL, inst, NULL);
    free(w);
    if (!t.edit) {
        DestroyWindow(wnd);
        return -1;
    }
    for (i = 0; i < 4; i++)
        if (child[i]) SendMessageW(child[i], WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)&t);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(t.edit);
    SendMessageW(t.edit, EM_SETSEL, 0, -1);
    while (t.result == -2) {
        BOOL r = GetMessageW(&m, NULL, 0, 0);
        if (r == -1) {
            t.result = -1;
            break;
        }
        if (!r) { /* the application is quitting: leave the quit for the loop that owns it */
            PostQuitMessage((int)m.wParam);
            t.result = 1;
            break;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && m.hwnd == t.edit) {
            text_input_ok(wnd, &t);
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && (m.hwnd == wnd || IsChild(wnd, m.hwnd))) {
            t.result = 1;
            continue;
        }
        if (IsDialogMessageW(wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    SetWindowLongPtrW(wnd, GWLP_USERDATA, 0);
    DestroyWindow(wnd);
    return t.result;
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
