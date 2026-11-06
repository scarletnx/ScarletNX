/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's thread primitives (thread.h): the mutexes with the profiler's and the hang report's view of them,
   condition variables, waits on a word, yielding and thread ids, on Windows and on the POSIX systems */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "common/platform/thread.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/umtx.h>
#include <pthread_np.h>
#endif
#if defined(__NetBSD__)
#include <sys/futex.h>
#include <sys/syscall.h>
#endif
#endif

/* The waits pm_lock and pc_wait note while they block: a slot per thread, taken the first time it waits, read by the
   hang report while the threads run (each field alone is atomic; a report may catch a wait as it changes) */
#define PM_WAITS 512 /* a game's guest threads and the runtime's own: one large game has over 200 */
static struct {
    uint64_t tid;
    const void* m;
    const void* at;
    int cond;
} g_pm_wait[PM_WAITS];
/* Written once per thread, on its first wait, and read by the hang report. */
static unsigned g_pm_nwait;
/* This thread's own state in the host's lock bookkeeping: the hang report's slot, and its id. One struct, so the
   thread takes a single TLS slot for all of it. On Darwin the first access may allocate, which a signal handler must
   not, so each thread touches it (plat_tid) before it can fault */
typedef struct {
    int slot;     /* the hang report's, -1 until this thread waits once (pm_wait_note) */
    uint32_t tid; /* plat_tid's, taken once: it is a system call, and pm_lock reads it on every call */
} PmThread;
static _Thread_local PmThread t_pm = {-1, 0};
void pm_wait_note(const void* m, const void* at, int cond) {
    int i = t_pm.slot;
    if (i < 0) {
        unsigned n = __atomic_fetch_add(&g_pm_nwait, 1, __ATOMIC_RELAXED);
        i = t_pm.slot = n < PM_WAITS ? (int)n : PM_WAITS;
        if (i < PM_WAITS) __atomic_store_n(&g_pm_wait[i].tid, plat_tid(), __ATOMIC_RELAXED);
    }
    if (i >= PM_WAITS) return;
    __atomic_store_n(&g_pm_wait[i].at, at, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pm_wait[i].cond, cond, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pm_wait[i].m, m, __ATOMIC_RELEASE);
}
int pm_waits(PmWait* out, int max) {
    unsigned n = __atomic_load_n(&g_pm_nwait, __ATOMIC_RELAXED);
    int k = 0;
    if (n > PM_WAITS) n = PM_WAITS;
    for (unsigned i = 0; i < n && k < max; i++) {
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

/* 1 when this thread may report on the lock: its id is in the table, or a slot took it. The table is read and
   filled without a lock (each slot alone is atomic; a duplicate of one thread's id only wastes a slot). */
static int pm_named_admit(PmNamed* c) {
    uint64_t t = plat_tid(), none;
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
    __atomic_store_n(&g_pm_tracy, t, __ATOMIC_RELEASE);
    unsigned n = __atomic_load_n(&g_pm_npending, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n && i < PM_PENDING; i++)
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
/* ================================================================ mutexes */
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
/* The three gates read g_pm_tracy_on once and take the plain path while it is closed. An announce that lands
   while a lock is being taken is read again after it is held, so the release never finds no obtain to pair
   with. */
void pm_lock(PMutex* m) {
    if (!__atomic_load_n(&g_pm_tracy_on, __ATOMIC_RELAXED)) {
        AcquireSRWLockExclusive(&m->l);
        return;
    }
    PmNamed* c = pm_named(m);
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

/* ================================================================ timer resolution */
/* The scheduler's default tick is 15.6 ms: asked once for 1 ms before the first timed wait (plat_sleep_ns's
   high-resolution timer needs none). winmm is loaded here so the runtime doesn't link it */
static int g_period_asked;
void plat_timer_1ms(void) {
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
static int cond_wait(PCond* c, PMutex* m, int64_t timeout_ns) {
    DWORD ms = INFINITE;
    if (timeout_ns >= 0) {
        /* whole milliseconds rounded up, in uint64_t so a timeout near INT64_MAX can't overflow, and kept below INFINITE
           so a long timeout neither wraps in the DWORD nor becomes a wait without end */
        uint64_t r = (uint64_t)timeout_ns / 1000000u + ((uint64_t)timeout_ns % 1000000u != 0);
        ms = r >= (uint64_t)INFINITE ? INFINITE - 1 : (DWORD)r;
        plat_timer_1ms();
    }
    if (SleepConditionVariableSRW(&c->c, &m->l, ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT;
}
/* the wait noted while it lasts, for a hang report (pm_waits) */
int pc_wait(PCond* c, PMutex* m, int64_t timeout_ns) {
    int r;
    pm_wait_note(m, __builtin_return_address(0), 1);
    r = cond_wait(c, m, timeout_ns);
    pm_wait_note(NULL, NULL, 0);
    return r;
}

void plat_wait_word(uint32_t* w, uint32_t v, int64_t timeout_ns) {
    DWORD ms = INFINITE;
    if (timeout_ns >= 0) {
        uint64_t r = (uint64_t)timeout_ns / 1000000u + ((uint64_t)timeout_ns % 1000000u != 0); /* as cond_wait rounds it */
        ms = r >= (uint64_t)INFINITE ? INFINITE - 1 : (DWORD)r;
        plat_timer_1ms();
    }
    WaitOnAddress(w, &v, sizeof v, ms);
}
void plat_wake_word(uint32_t* w) { WakeByAddressAll(w); }

uint32_t plat_tid(void) { return (uint32_t)GetCurrentThreadId(); }
#else
/* ================================================================ mutexes */
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
void pm_lock(PMutex* m) {
    if (!__atomic_load_n(&g_pm_tracy_on, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&m->l);
        return;
    }
    PmNamed* c = pm_named(m);
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
static int cond_wait(PCond* c, PMutex* m, int64_t timeout_ns) {
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
        uint64_t sec;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        sec = (uint64_t)timeout_ns / 1000000000u;
        ts.tv_nsec += (long)((uint64_t)timeout_ns % 1000000000u);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            sec++;
        }
        ts.tv_sec = (time_t)((uint64_t)ts.tv_sec + sec);
    }
    r = pthread_cond_timedwait(&c->c, &m->l, &ts);
#endif
    return r == ETIMEDOUT;
}
/* the wait noted while it lasts, for a hang report (pm_waits) */
int pc_wait(PCond* c, PMutex* m, int64_t timeout_ns) {
    int r;
    pm_wait_note(m, __builtin_return_address(0), 1);
    r = cond_wait(c, m, timeout_ns);
    pm_wait_note(NULL, NULL, 0);
    return r;
}

#if defined(__APPLE__)
/* libSystem's, which libc++'s atomic waits use too: the public os_sync_wait_on_address needs macOS 14.4 */
extern int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
#define SNX_UL_COMPARE_AND_WAIT 1u
#define SNX_ULF_WAKE_ALL        0x100u
#define SNX_ULF_NO_ERRNO        0x1000000u
#endif
void plat_wait_word(uint32_t* w, uint32_t v, int64_t timeout_ns) {
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
#elif defined(__NetBSD__)
    /* NetBSD's __futex takes Linux's operations, a relative timeout for FUTEX_WAIT included */
    struct timespec ts;
    if (timeout_ns >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000);
        ts.tv_nsec = (long)(timeout_ns % 1000000000);
    }
    syscall(SYS___futex, w, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, v, timeout_ns >= 0 ? &ts : NULL, NULL, 0, 0);
#else
    /* 0 waits without limit: a timeout is at least a microsecond, and at most what the 32 bits hold */
    uint64_t us = timeout_ns < 0 ? 0 : (uint64_t)timeout_ns / 1000u + 1u;
    __ulock_wait(SNX_UL_COMPARE_AND_WAIT | SNX_ULF_NO_ERRNO, w, v, us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)us);
#endif
}
void plat_wake_word(uint32_t* w) {
#if defined(__linux__)
    syscall(SYS_futex, w, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#elif defined(__FreeBSD__)
    _umtx_op(w, UMTX_OP_WAKE_PRIVATE, INT_MAX, NULL, NULL);
#elif defined(__NetBSD__)
    syscall(SYS___futex, w, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, INT_MAX, NULL, NULL, 0, 0);
#else
    __ulock_wake(SNX_UL_COMPARE_AND_WAIT | SNX_ULF_WAKE_ALL | SNX_ULF_NO_ERRNO, w, 0);
#endif
}

uint32_t plat_tid(void) {
    /* kept per thread: vk_lock asks it on every acquire, thousands of times a frame in one game, and on Linux each was
       a system call */
    if (!t_pm.tid) {
#if defined(__linux__)
        t_pm.tid = (uint32_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
        uint64_t t = 0;
        pthread_threadid_np(NULL, &t);
        t_pm.tid = (uint32_t)t;
#elif defined(__FreeBSD__)
        t_pm.tid = (uint32_t)pthread_getthreadid_np();
#else
        t_pm.tid = (uint32_t)(uintptr_t)pthread_self();
#endif
    }
    return t_pm.tid;
}
#endif
