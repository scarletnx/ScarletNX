/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's thread primitives, the same for every driver: a mutex the profiler and the hang report can see, condition
   variables, waits on a word, yielding and the thread's id. No guest type and no setting enters here, so sizes are
   <stdint.h>'s, and the target (scarletnx-platform) links nothing but the system's threads */
#ifndef SNX_PLATFORM_THREAD_H
#define SNX_PLATFORM_THREAD_H
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX /* or windows.h defines min and max as macros, which any later use of those names trips over */
#endif
#include <windows.h>
/* A mutex with a static initializer on both hosts; the member is l on both */
typedef struct {
    SRWLOCK l;
    void* tracy; /* the profiler's context for a named lock (pm_lock_announce), null otherwise */
} PMutex;
#define PMUTEX_INIT {SRWLOCK_INIT, NULL}
#else
#include <pthread.h>
typedef struct {
    pthread_mutex_t l;
    void* tracy; /* the profiler's context for a named lock (pm_lock_announce), null otherwise */
} PMutex;
#define PMUTEX_INIT {PTHREAD_MUTEX_INITIALIZER, NULL}
#endif

void pm_init(PMutex* m);
void pm_destroy(PMutex* m);
void pm_lock(PMutex* m);
void pm_unlock(PMutex* m);
int pm_trylock(PMutex* m); /* 1 when it took the lock, 0 when another thread holds it */
/* The profiler's view of the locks, registered while a run has one attached: pm_lock, pm_unlock and pm_trylock
   report the named locks through it, and its pane shows who held which and who waited how long. Nothing registers
   it on a run without the profiler, so a lock unnamed costs a null check. The reporter's context is the raw
   profiler lock; the admission (who reports on a lock at all) is host.c's, which owns the mutex. */
typedef struct {
    void* (*make)(const char* name);
    void (*free)(void* l);
    void (*wait)(void* l);
    void (*lock)(void* l);
    void (*unlock)(void* l);
    void (*try_lock)(void* l, int acquired);
} PmTracy;
void pm_tracy_register(const PmTracy* t);
void pm_lock_announce(PMutex* m, const char* name); /* names a lock; from then on its locks report */
/* A thread blocked in pm_lock on a mutex another thread holds, or waiting on a condition variable with it (cond 1), for
   a hang report: the host thread, the mutex and the caller's address. A lock taken at once notes nothing */
typedef struct {
    uint64_t tid;
    const void* m;
    const void* at;
    int cond;
} PmWait;
void pm_wait_note(const void* m, const void* at, int cond); /* m NULL: the wait is over */
int pm_waits(PmWait* out, int max);                          /* the waits now, at most max */
const void* pm_wait_base(void); /* an address in this module's data, to name a mutex by its offset from */

/* ---- condition variables, used with a PMutex */
typedef struct {
#if defined(_WIN32)
    CONDITION_VARIABLE c;
#else
    pthread_cond_t c;
#endif
} PCond;
void pc_init(PCond* c);
void pc_signal(PCond* c);
void pc_broadcast(PCond* c);
/* m held. timeout_ns < 0 waits without limit. Returns 1 when the time ran out, 0 when woken (or spuriously) */
int pc_wait(PCond* c, PMutex* m, int64_t timeout_ns);

/* ---- waiting on a word, with no lock (a futex on Linux): plat_wait_word blocks while *w holds v, for at most
   timeout_ns (negative: no limit), and may also return early for no reason; plat_wake_word wakes every thread waiting
   on w. The waker changes *w before it wakes them */
void plat_wait_word(uint32_t* w, uint32_t v, int64_t timeout_ns);
void plat_wake_word(uint32_t* w);

/* ---- threads */
typedef void (*PThreadFn)(void* arg);
/* the alternate signal stack of a thread: the crash report needs about 12 KiB, and a thread whose own stack overflowed
   has none left */
#define PLAT_ALT_STACK_SIZE 32768
/* the room Windows keeps for the crash report after a thread's stack overflowed (SetThreadStackGuarantee) */
#define PLAT_STACK_GUARANTEE 32768
/* What a driver does on each thread plat_thread starts: start before fn, end after it (and in plat_thread_exit), on
   the thread itself; created (Windows) on the starting thread with the new thread's handle, which it must not keep
   without its own copy. Any member may be NULL. Set once, before the first thread; the table must outlive them */
typedef struct {
    void (*start)(void);
    void (*end)(void);
#if defined(_WIN32)
    void (*created)(HANDLE h);
#endif
} PThreadHooks;
void plat_thread_hooks(const PThreadHooks* hooks);
/* A detached thread running fn(arg) on a stack of that many bytes (0: the system's default). A thread with the
   default stack, or one of 256 KiB or more, gets room for a crash report after its stack overflows (an alternate
   signal stack, Windows' stack guarantee). 0, or -1 with the reason in errno (GetLastError on Windows) */
int plat_thread(PThreadFn fn, void* arg, size_t stack);
void plat_thread_exit(void); /* ends the calling thread (one plat_thread started) */
/* the host CPUs the process may run on, at most max of them, into ids; 0 where the runtime doesn't choose them (all
   but Linux; Android's CPUs differ in speed). On Linux each thread plat_thread starts may then run on all of them */
int plat_cpus(int* ids, int max);
/* the calling thread runs only on the n host CPUs in ids: 0, or -1 with errno */
int plat_thread_cpus(const int* ids, int n);
/* the calling thread runs only where a CPU has nothing else to run: background work the game must not wait for (the
   BC7 encodes of nx/gpu/vk/astc.c). Linux's and Android's SCHED_IDLE, macOS's and iOS's background QoS class, Windows'
   idle priority, nothing elsewhere: 0, or -1 with errno (GetLastError on Windows) */
int plat_thread_idle(void);
/* the calling thread's id, as the system numbers threads (Linux's gettid, kept per thread). Safe in a signal handler
   once the thread has called it one time */
uint32_t plat_tid(void);
#if defined(_WIN32)
/* asks Windows one time for a 1 ms scheduler tick (its default is 15.6 ms), before the first timed wait */
void plat_timer_1ms(void);
#endif
#endif
