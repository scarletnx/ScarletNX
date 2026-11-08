/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The threads plat_thread starts (thread.h), with the hooks a driver gives them, and the host CPUs they run on. A file
   of its own, so a test that stubs these links the rest of thread.c without them */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* pthread_attr_setaffinity_np, CPU_SET */
#endif
#include "common/platform/thread.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#endif

/* ================================================================ threads */
static const PThreadHooks* g_hooks; /* plat_thread_hooks's */
void plat_thread_hooks(const PThreadHooks* hooks) { __atomic_store_n(&g_hooks, hooks, __ATOMIC_RELEASE); }

#if defined(_WIN32)
#define STACK_GUARANTEE 32768 /* room for the crash report after a stack overflow */
#define GUARANTEE_MIN_THREAD ((size_t)256 << 10)

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
    const PThreadHooks* hk = __atomic_load_n(&g_hooks, __ATOMIC_ACQUIRE);
    if (hk && hk->start) hk->start();
    s.fn(s.arg);
    if (hk && hk->end) hk->end();
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
    const PThreadHooks* hk = __atomic_load_n(&g_hooks, __ATOMIC_ACQUIRE);
    if (hk && hk->created) hk->created(h); /* it takes its own handle when it keeps one */
    CloseHandle(h);
    return 0;
}
void plat_thread_exit(void) {
    const PThreadHooks* hk = __atomic_load_n(&g_hooks, __ATOMIC_ACQUIRE);
    if (hk && hk->end) hk->end();
    ExitThread(0);
}

int plat_cpus(int* ids, int max) {
    (void)ids;
    (void)max;
    return 0;
}
int plat_thread_cpus(const int* ids, int n) {
    (void)ids;
    (void)n;
    return 0;
}
#else
/* below this a thread is a small helper that gets no alternate signal stack */
#define ALT_STACK_MIN_THREAD ((size_t)256 << 10)
/* the least stack a thread gets: FreeBSD's PTHREAD_STACK_MIN (2 KiB on amd64) is less than its dynamic linker takes
   to bind the thread's first call, so never under glibc's 16 KiB */
#define THREAD_STACK_FLOOR   ((size_t)PTHREAD_STACK_MIN > 16384 ? (size_t)PTHREAD_STACK_MIN : (size_t)16384)

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
    char stack[PLAT_ALT_STACK_SIZE];
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
    const PThreadHooks* h = __atomic_load_n(&g_hooks, __ATOMIC_ACQUIRE);
    if (h && h->start) h->start();
    if (s.alt_stack)
        run_with_alt_stack(s.fn, s.arg);
    else
        s.fn(s.arg);
    if (h && h->end) h->end();
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
    const PThreadHooks* h = __atomic_load_n(&g_hooks, __ATOMIC_ACQUIRE);
    alt_stack_off();
    if (h && h->end) h->end();
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
int plat_thread_cpus(const int* ids, int n) {
    cpu_set_t s;
    int i;
    CPU_ZERO(&s);
    for (i = 0; i < n; i++) CPU_SET(ids[i], &s);
    return sched_setaffinity(0, sizeof s, &s) ? -1 : 0;
}
#else
int plat_cpus(int* ids, int max) {
    (void)ids;
    (void)max;
    return 0;
}
int plat_thread_cpus(const int* ids, int n) {
    (void)ids;
    (void)n;
    return 0;
}
#endif
#endif
