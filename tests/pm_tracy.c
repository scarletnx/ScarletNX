/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The profiler's view of a named lock (common/platform/thread.c, the PmTracy a run registers): pm_unlock reports the
   release while the lock is still held, as Tracy's own Lockable does since 0.15 (BeforeUnlock), so no other thread's
   lock of it reaches the profiler before the release does. A release reported after the mutex is let go read as two
   threads holding the lock at once */
#include "common/platform/thread.h"
#include <stdio.h>
#if !defined(_WIN32)
#include <pthread.h>
#endif

static struct {
    PMutex m;
    int unlocks, held; /* the releases reported, and those reported while m was held */
} g_t;

/* 1 while another holder has m: this thread's own try of it fails then */
static int held(void) {
#if defined(_WIN32)
    if (!TryAcquireSRWLockExclusive(&g_t.m.l)) return 1;
    ReleaseSRWLockExclusive(&g_t.m.l);
#else
    if (pthread_mutex_trylock(&g_t.m.l)) return 1;
    pthread_mutex_unlock(&g_t.m.l);
#endif
    return 0;
}
static void* made(const char* name) {
    (void)name;
    return &g_t;
}
static void none(void* l) { (void)l; }
static void unlocked(void* l) {
    (void)l;
    g_t.unlocks++;
    g_t.held += held();
}
static void tried(void* l, int acquired) { (void)l, (void)acquired; }

int main(void) {
    static const PmTracy reporter = {made, none, none, none, unlocked, tried};
    pm_init(&g_t.m);
    pm_tracy_register(&reporter);
    pm_lock_announce(&g_t.m, "pm_tracy");
    pm_lock(&g_t.m);
    pm_unlock(&g_t.m);
    if (pm_trylock(&g_t.m)) pm_unlock(&g_t.m);
    if (g_t.unlocks != 2 || g_t.held != 2) {
        fprintf(stderr, "FAIL: %d releases reported, %d of them while the lock was held: 2 and 2 expected\n",
                g_t.unlocks, g_t.held);
        return 1;
    }
    puts("pm_tracy: a named lock's release is reported while it is held");
    return 0;
}
