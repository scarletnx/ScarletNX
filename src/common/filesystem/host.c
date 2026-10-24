/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host side of the indexed filesystem: mutexes, folders, UTF-8 paths on Windows, and files shared read-only with
   the host's page cache for the whole run */
#include "common/filesystem/host.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

/* descriptors a shared file may not take: they stay for the files opened on demand (saves, the SD card, unmapped
   RomFS files) */
#define SHARE_RESERVE 256

/* The waits pm_lock and pc_wait note while they block: a slot per thread, taken the first time it waits, read by the
   hang report while the threads run (each field alone is atomic; a report may catch a wait as it changes) */
#define PM_WAITS 128
static struct {
    uint64_t tid;
    const void* m;
    const void* at;
    int cond;
} g_pm_wait[PM_WAITS];
/* Written once per thread, on its first wait, and read by the hang report. */
static unsigned g_pm_nwait;
/* This thread's own state in the host's lock bookkeeping: the hang report's slot, and the id the named tables are
   searched by. One struct, so the thread takes a single TLS slot for all of it. */
typedef struct {
    int slot;  /* the hang report's, -1 until this thread waits once (pm_wait_note) */
    uint64_t tid; /* pm_tid, taken once: it is a syscall, and pm_lock reads it on every call */
} PmThread;
static _Thread_local PmThread t_pm = {-1, 0};
static uint64_t pm_tid(void);
void pm_wait_note(const void* m, const void* at, int cond) {
    int i = t_pm.slot;
    if (i < 0) {
        unsigned n = __atomic_fetch_add(&g_pm_nwait, 1, __ATOMIC_RELAXED);
        i = t_pm.slot = n < PM_WAITS ? (int)n : PM_WAITS;
        if (i < PM_WAITS) __atomic_store_n(&g_pm_wait[i].tid, pm_tid(), __ATOMIC_RELAXED);
    }
    if (i >= PM_WAITS) return;
    __atomic_store_n(&g_pm_wait[i].at, at, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pm_wait[i].cond, cond, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pm_wait[i].m, m, __ATOMIC_RELEASE);
}
int pm_waits(PmWait* out, int max) {
    unsigned n = __atomic_load_n(&g_pm_nwait, __ATOMIC_RELAXED), i;
    int k = 0;
    if (n > PM_WAITS) n = PM_WAITS;
    for (i = 0; i < n && k < max; i++) {
        const void* m = __atomic_load_n(&g_pm_wait[i].m, __ATOMIC_ACQUIRE);
        if (!m) continue;
        out[k].tid = __atomic_load_n(&g_pm_wait[i].tid, __ATOMIC_RELAXED);
        out[k].m = m;
        out[k].at = __atomic_load_n(&g_pm_wait[i].at, __ATOMIC_RELAXED);
        out[k].cond = __atomic_load_n(&g_pm_wait[i].cond, __ATOMIC_RELAXED);
        k++;
    }
    return k;
}
const void* pm_wait_base(void) { return g_pm_wait; }

/* The profiler's reporters, set once while a run has one attached; the named locks report through them. The
   server's lock pane holds 64 threads a lock, with the guard a mere assert, so a 65th would corrupt it.
   Nothing here is paid for until a reporter registers: the gates of pm_lock, pm_unlock and pm_trylock read one
   relaxed flag and nothing else. */
#define PM_TRACY_THREADS 64
typedef struct {
    void* l; /* the profiler's lock context, from make */
    uint32_t nthreads;
    uint64_t tid[PM_TRACY_THREADS];
} PmNamed;
static const PmTracy* g_pm_tracy;
static int g_pm_tracy_on; /* the one flag pm_lock reads; set once, never cleared */
/* locks named before a reporter registered (a client started later, F10's): attached when one registers */
#define PM_PENDING 16
static struct {
    PMutex* m;
    const char* name;
} g_pm_pending[PM_PENDING];
static unsigned g_pm_npending;
static uint64_t pm_tid(void);
static uint64_t pm_tid_once(void) {
    if (!t_pm.tid) t_pm.tid = pm_tid(); /* pm_tid is a syscall, and pm_lock reads it on every call */
    return t_pm.tid;
}

/* 1 when this thread may report on the lock: its id is in the table, or a slot took it. The table is read and
   filled without a lock (each slot alone is atomic; a duplicate of one thread's id only wastes a slot). */
static int pm_named_admit(PmNamed* c) {
    uint64_t t = pm_tid_once(), none;
    uint32_t i, n = __atomic_load_n(&c->nthreads, __ATOMIC_ACQUIRE);
    if (n > PM_TRACY_THREADS) n = PM_TRACY_THREADS;
    for (i = 0; i < n; i++)
        if (__atomic_load_n(&c->tid[i], __ATOMIC_RELAXED) == t) return 1;
    if (n >= PM_TRACY_THREADS) return 0; /* the pane's cap */
    for (; i < PM_TRACY_THREADS; i++) {
        none = 0;
        if (__atomic_compare_exchange_n(&c->tid[i], &none, t, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (i >= n) __atomic_store_n(&c->nthreads, i + 1, __ATOMIC_RELEASE);
            return 1;
        }
        if (none == t) return 1; /* a racing insert of this thread's own id took the slot first */
    }
    return 0;
}
static void pm_named_attach(PMutex* m, const char* name, const PmTracy* t) {
    PmNamed* c;
    if (__atomic_load_n(&m->tracy, __ATOMIC_ACQUIRE)) return;
    c = malloc(sizeof *c);
    if (!c) return;
    c->l = t->make(name);
    c->nthreads = 0;
    memset(c->tid, 0, sizeof c->tid);
    __atomic_store_n(&m->tracy, c, __ATOMIC_RELEASE);
}
void pm_tracy_register(const PmTracy* t) {
    unsigned n, i;
    __atomic_store_n(&g_pm_tracy, t, __ATOMIC_RELEASE);
    n = __atomic_load_n(&g_pm_npending, __ATOMIC_ACQUIRE);
    for (i = 0; i < n && i < PM_PENDING; i++)
        pm_named_attach(g_pm_pending[i].m, g_pm_pending[i].name, t);
    __atomic_store_n(&g_pm_npending, 0, __ATOMIC_RELEASE);
    /* last, so a pm_lock that finds the flag open sees a reporter and every lock already attached */
    __atomic_store_n(&g_pm_tracy_on, 1, __ATOMIC_RELEASE);
}
void pm_lock_announce(PMutex* m, const char* name) {
    const PmTracy* t = __atomic_load_n(&g_pm_tracy, __ATOMIC_ACQUIRE);
    unsigned n;
    if (__atomic_load_n(&m->tracy, __ATOMIC_ACQUIRE)) return; /* once-only: one context a lock */
    if (t) {
        pm_named_attach(m, name, t);
        return;
    }
    /* claimed by the count the add returns, never a re-read: two threads announcing at once each keep their
       own slot, and no slot is written that the register would then walk past */
    n = __atomic_fetch_add(&g_pm_npending, 1, __ATOMIC_ACQ_REL);
    if (n < PM_PENDING) {
        g_pm_pending[n].m = m;
        g_pm_pending[n].name = name;
        return;
    }
    /* the runtime names five locks in all; the count is clamped so the register stops with the table's own */
    __atomic_store_n(&g_pm_npending, PM_PENDING, __ATOMIC_RELEASE);
}
static PmNamed* pm_named(const PMutex* m) { return __atomic_load_n(&m->tracy, __ATOMIC_ACQUIRE); }

#if defined(_WIN32)
void pm_init(PMutex* m) {
    m->tracy = NULL;
    InitializeSRWLock(&m->l);
}
/* The mutex must be quiescent when this runs (see pm_destroy below). */
void pm_destroy(PMutex* m) {
    PmNamed* c = __atomic_exchange_n(&m->tracy, NULL, __ATOMIC_ACQ_REL);
    if (c) {
        const PmTracy* t = __atomic_load_n(&g_pm_tracy, __ATOMIC_ACQUIRE);
        if (t) t->free(c->l);
        free(c); /* the wrapper is this module's, the lock context the profiler's */
    }
}
static uint64_t pm_tid(void) { return GetCurrentThreadId(); }
/* The three gates read g_pm_tracy_on once and take the plain path while it is closed. An announce that lands
   while a lock is being taken is read again after it is held, so the release never finds no obtain to pair
   with. */
void pm_lock(PMutex* m) {
    PmNamed* c;
    if (!__atomic_load_n(&g_pm_tracy_on, __ATOMIC_RELAXED)) {
        AcquireSRWLockExclusive(&m->l);
        return;
    }
    c = pm_named(m);
    if (TryAcquireSRWLockExclusive(&m->l)) {
        if (!c) c = pm_named(m);
        if (c && pm_named_admit(c)) g_pm_tracy->try_lock(c->l, 1); /* the count the release balances */
        return;
    }
    if (c && pm_named_admit(c)) g_pm_tracy->wait(c->l);
    pm_wait_note(m, __builtin_return_address(0), 0);
    AcquireSRWLockExclusive(&m->l);
    if (c && pm_named_admit(c)) g_pm_tracy->lock(c->l);
    pm_wait_note(NULL, NULL, 0);
}
void pm_unlock(PMutex* m) {
    PmNamed* c = __atomic_load_n(&g_pm_tracy_on, __ATOMIC_RELAXED) ? pm_named(m) : NULL;
    ReleaseSRWLockExclusive(&m->l); /* the release first, as the profiler's own Lockable does */
    if (c && pm_named_admit(c)) g_pm_tracy->unlock(c->l);
}
int pm_trylock(PMutex* m) {
    int got = TryAcquireSRWLockExclusive(&m->l) != 0;
    if (!got || !__atomic_load_n(&g_pm_tracy_on, __ATOMIC_RELAXED)) return got;
    PmNamed* c = pm_named(m);
    if (c && pm_named_admit(c)) g_pm_tracy->try_lock(c->l, 1);
    return got;
}

/* paths are UTF-8 everywhere else. A path that isn't valid UTF-8 names nothing: NULL, rather than a path with
   replacement characters that could name another file. Freed with free */
wchar_t* snx_host_wpath(const char* text) {
    wchar_t* wide;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (n <= 0) return NULL;
    wide = malloc((size_t)n * sizeof *wide);
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n) != n) {
        free(wide);
        return NULL;
    }
    return wide;
}

int plat_mkdir(const char* path) {
    wchar_t* wide = snx_host_wpath(path);
    int ok = wide && CreateDirectoryW(wide, NULL);
    free(wide);
    return ok ? 0 : -1;
}

/* the CRT has room for 8192 descriptors, handed out lowest first */
#define CRT_DESCRIPTORS 8192
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data) {
    wchar_t* wide;
    HANDLE file, section;
    void* view = NULL;
    int d;
    *fd = -1;
    *data = NULL;
    if ((uint64_t)(size_t)size != size || size > (uint64_t)INT64_MAX) return -1;
    wide = snx_host_wpath(path);
    if (!wide) return -1;
    /* others may read it, and delete or rename it as on POSIX: the view stays valid */
    file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return -1;
    d = _open_osfhandle((intptr_t)file, _O_RDONLY | _O_BINARY);
    if (d < 0) {
        CloseHandle(file);
        return -1;
    }
    if (d + 1 + SHARE_RESERVE > CRT_DESCRIPTORS) {
        _close(d); /* closes the handle with it */
        return -1;
    }
    if (size) {
        section = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (section) {
            view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, (SIZE_T)size);
            CloseHandle(section); /* the view keeps it */
        }
        if (!view) {
            _close(d);
            return -1;
        }
    }
    *fd = d;
    *data = view;
    return 0;
}
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size) {
    (void)size;
    if (data) UnmapViewOfFile(data);
    if (fd >= 0) _close(fd);
}
#else
void pm_init(PMutex* m) {
    m->tracy = NULL;
    pthread_mutex_init(&m->l, NULL);
}
/* The mutex must be quiescent when this runs: a thread inside pm_lock reads m->tracy and the context it points
   at, and this frees both. Every lock is destroyed at exit, after the threads that used it have joined. */
void pm_destroy(PMutex* m) {
    PmNamed* c = __atomic_exchange_n(&m->tracy, NULL, __ATOMIC_ACQ_REL);
    if (c) {
        const PmTracy* t = __atomic_load_n(&g_pm_tracy, __ATOMIC_ACQUIRE);
        if (t) t->free(c->l);
        free(c);
    }
    pthread_mutex_destroy(&m->l);
}
static uint64_t pm_tid(void) {
#if defined(__APPLE__)
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return t;
#elif defined(__linux__)
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}
void pm_lock(PMutex* m) {
    PmNamed* c;
    if (!__atomic_load_n(&g_pm_tracy_on, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&m->l);
        return;
    }
    c = pm_named(m);
    if (pthread_mutex_trylock(&m->l) == 0) {
        if (!c) c = pm_named(m);
        if (c && pm_named_admit(c)) g_pm_tracy->try_lock(c->l, 1); /* the count the release balances */
        return;
    }
    if (c && pm_named_admit(c)) g_pm_tracy->wait(c->l);
    pm_wait_note(m, __builtin_return_address(0), 0);
    pthread_mutex_lock(&m->l);
    if (c && pm_named_admit(c)) g_pm_tracy->lock(c->l);
    pm_wait_note(NULL, NULL, 0);
}
void pm_unlock(PMutex* m) {
    PmNamed* c = __atomic_load_n(&g_pm_tracy_on, __ATOMIC_RELAXED) ? pm_named(m) : NULL;
    pthread_mutex_unlock(&m->l); /* the release first, as the profiler's own Lockable does */
    if (c && pm_named_admit(c)) g_pm_tracy->unlock(c->l);
}
int pm_trylock(PMutex* m) {
    int got = pthread_mutex_trylock(&m->l) == 0;
    if (!got || !__atomic_load_n(&g_pm_tracy_on, __ATOMIC_RELAXED)) return got;
    PmNamed* c = pm_named(m);
    if (c && pm_named_admit(c)) g_pm_tracy->try_lock(c->l, 1);
    return got;
}

int plat_mkdir(const char* path) { return mkdir(path, 0777) ? -1 : 0; }

/* the descriptors the shared files may use: the soft limit, and on macOS no more than OPEN_MAX (10240). Its hard limit is
   "unlimited" and kern.maxfilesperproc (61440) is what stops a process, but a game with 300000 files that mapped up to
   that left SDL unable to open its Vulkan window, with the descriptors still free */
static rlim_t descriptor_limit(const struct rlimit* rl) {
    rlim_t limit = rl->rlim_cur;
#if defined(__APPLE__)
    if (limit > 10240) limit = 10240;
#endif
    return limit;
}
/* a game's RomFS can have more files than the default soft limit of descriptors: raise it to the hard limit, once.
   If that fails the old limit stays, and the reserve check below refuses what doesn't fit */
static void raise_descriptor_limit(void) {
    static int done;
    struct rlimit rl;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    if (getrlimit(RLIMIT_NOFILE, &rl) || rl.rlim_cur == rl.rlim_max) return;
    rl.rlim_cur = rl.rlim_max;
    (void)setrlimit(RLIMIT_NOFILE, &rl);
}
/* whether keeping descriptor fd leaves the reserve free (descriptors are handed out lowest first) */
static int descriptor_fits(int fd) {
    struct rlimit rl;
    rlim_t limit;
    if (getrlimit(RLIMIT_NOFILE, &rl)) return 1;
    limit = descriptor_limit(&rl);
    if (limit == RLIM_INFINITY) return 1;
    return (rlim_t)fd + 1 + SHARE_RESERVE <= limit;
}
int plat_file_share(const char* path, uint64_t size, int* fd, const uint8_t** data) {
    void* view = NULL;
    int d;
    *fd = -1;
    *data = NULL;
    if ((uint64_t)(size_t)size != size || size > (uint64_t)INT64_MAX) return -1;
    raise_descriptor_limit();
    d = open(path, O_RDONLY | O_CLOEXEC);
    if (d < 0) return -1;
    if (!descriptor_fits(d)) {
        close(d);
        return -1;
    }
    if (size) { /* paged in as the game reads it */
        view = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, d, 0);
        if (view == MAP_FAILED) {
            close(d);
            return -1;
        }
    }
    *fd = d;
    *data = view;
    return 0;
}
void plat_file_unshare(int fd, const uint8_t* data, uint64_t size) {
    if (data) munmap((void*)data, (size_t)size);
    if (fd >= 0) close(fd);
}
#endif
