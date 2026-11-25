/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sync.c's clocks against the game's (runtime/clock.c): steady_clock::now is guest time, which a game pause stops and a
   save state's load sets, as the SDK's own steady_clock reads it; system_clock::now is the host's real time.
   And the C library's condition variable waits on a std::mutex the family holds (libc++'s inlined steady_clock waits
   call pthread_cond_clockwait with its words): the host's wait, ETIMEDOUT at a deadline on steady_clock's or the real
   time's clock, 0 when the condition variable is notified; on any other mutex the function's own code runs */
#include "nx/nn/hook.h"
#include "nn/hooks.hook.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/clock.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* the kernel's side of a wait, which these calls never make */
KThread* kthread_cur(void) { return NULL; }
void core_leave(KThread* t) { (void)t; }
void core_enter(KThread* t) { (void)t; }
void kthread_block(KThread* t) { (void)t; }
void kthread_unblock(KThread* t) { (void)t; }
void fatal(const char* format, ...) {
    (void)format;
    abort();
}
/* the hooked function's own code: counted, its result a mark */
#define OWN_RESULT 22
static int g_original;
void hook_run_original(CPU* c, u64 addr) {
    (void)addr;
    g_original++;
    c->x[0] = OWN_RESULT;
}

/* guest memory: a std::mutex, a std::condition_variable, another mutex, a timespec */
#define MUTEX 0x100u
#define CV    0x140u
#define OTHER 0x180u
#define TS    0x1c0u
static u8 g_guest[0x1000];
static void deadline(u64 ns) {
    snx_st_le64(g_guest + TS, ns / 1000000000u);
    snx_st_le64(g_guest + TS + 8, ns % 1000000000u);
}
/* another thread: after 20 ms, the mutex taken, the condition variable notified, the mutex let go */
static void notifier(void* arg) {
    CPU* c = arg;
    plat_sleep_ns(20000000);
    sync_mutex_lock(c, MUTEX);
    sync_cond_notify_all(c, CV);
    sync_mutex_unlock(c, MUTEX);
}
static void test_c_waits(void) {
    CPU c = {.mem = g_guest}, other = {.mem = g_guest};
    /* a mutex the family never locked: the function's own code */
    deadline(guest_ns());
    assert(sync_pthread_cond_clockwait(&c, CV, OTHER, 1, TS) == OWN_RESULT && g_original == 1);
    assert(sync_pthread_cond_wait(&c, CV, OTHER) == OWN_RESULT && g_original == 2);
    /* the family's std::mutex: a deadline past runs out at once, one 30 ms on after them, the mutex held again */
    sync_mutex_lock(&c, MUTEX);
    deadline(guest_ns() - 1000000);
    assert(sync_pthread_cond_clockwait(&c, CV, MUTEX, 1, TS) == 110);
    u64 t0 = guest_ns();
    deadline(t0 + 30000000);
    assert(sync_pthread_cond_clockwait(&c, CV, MUTEX, 1, TS) == 110 && guest_ns() - t0 >= 25000000);
    assert(!sync_mutex_try_lock(&other, MUTEX) || (sync_mutex_unlock(&other, MUTEX), 0));
    deadline((u64)plat_unix_ns() + 30000000);
    assert(sync_pthread_cond_timedwait(&c, CV, MUTEX, TS) == 110);
    /* notified by another thread before the deadline: 0, and the mutex held again */
    assert(plat_thread(notifier, &other, 0) == 0);
    deadline(guest_ns() + 5000000000ull);
    assert(sync_pthread_cond_clockwait(&c, CV, MUTEX, 1, TS) == 0);
    assert(plat_thread(notifier, &other, 0) == 0);
    assert(sync_pthread_cond_wait(&c, CV, MUTEX) == 0);
    sync_mutex_unlock(&c, MUTEX);
    assert(g_original == 2);
    plat_sleep_ns(20000000); /* the last notifier lets go of the mutex */
}

int main(void) {
    CPU c = {0};
    clock_start();
    plat_sleep_ns(20000000);
    i64 before = sync_steady_now(&c);
    assert(before >= 20000000 && before < 1000000000ll); /* time since the run's start, not the host's uptime */
    /* paused for 200 ms: the game's steady_clock stands still, and goes on from there */
    clock_pause();
    i64 frozen = sync_steady_now(&c);
    plat_sleep_ns(200000000);
    assert(sync_steady_now(&c) == frozen && frozen >= before);
    clock_resume();
    i64 after = sync_steady_now(&c);
    assert(after >= frozen && after - frozen < 150000000);
    /* a save state's load: the state's guest time */
    clock_set(5000000000ull);
    i64 set = sync_steady_now(&c);
    assert(set >= 5000000000ll && set < 5150000000ll);
    /* system_clock stays the host's real time, in microseconds */
    i64 unix_us = plat_unix_ns() / 1000, now_us = sync_system_now(&c);
    assert(now_us >= unix_us && now_us - unix_us < 1000000);
    test_c_waits();
    puts("PASS sync.c's clocks: steady_clock the game's (stopped by a pause, set by a state's load), system_clock the "
         "host's; the C library's waits on the family's std::mutex on the host, on any other mutex their own code");
    return 0;
}
