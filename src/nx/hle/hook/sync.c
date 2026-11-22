/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C++ library's mutexes, condition variable, clocks and sleep the game's SDK exports (hooks.hook's sync family for
   the clocks and the sleep, and its cond_var family for the mutexes and the condition variable, which native_cond_var=0
   leaves to the game), run on the host's own: a std::mutex, std::recursive_mutex or std::condition_variable of the
   game's is a host mutex or condition variable (PMutex, PCond: the host's pthreads, SRW locks on Windows) found by the
   guest object's address, made at its first use (std::mutex's constructor is the compiler's, inlined) and let go of at
   its destructor. The guest object's own bytes go unread and unwritten: every function that reads them is hooked, and
   the others (timed_mutex's, shared_timed_mutex's and condition_variable_any's, whose templates the game's code inlines
   over their members) are left to the game's own code. libc++'s steady_clock waits are inlined too, and call the C
   library's pthread_cond_clockwait with the std::mutex's words, which the SDK's code reads as a mutex nobody holds and
   aborts on: the C library's waits are hooked as well, and wait on the host where the mutex is one of the family's.

   A thread that waits (a lock someone holds, a condition variable, a sleep) leaves its core first, as a waiting SVC
   does (hook_wait_begin): the core's other threads run meanwhile, the holder among them.

   steady_clock's now is the game's clock (guest_ns), as the SDK's reads it through clock_gettime's CLOCK_MONOTONIC and
   nn::os's system tick: it stops while the game is paused and takes a save state's time, as every other clock of the
   game does. system_clock's now is the host's real time, which is all __do_timed_wait compares a deadline with */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <stdatomic.h>

/* off the core while the thread waits, counted as stopped for a pause, as nvn_object.c's nvn_wait_begin */
void hook_wait_begin(void) {
    KThread* t = kthread_cur();
    if (!t) return;
    core_leave(t);
    kthread_block(t);
}
void hook_wait_end(void) {
    KThread* t = kthread_cur();
    if (!t) return;
    kthread_unblock(t);
    core_enter(t);
}
typedef struct SyncObj {
    struct SyncObj* next;
    u64 addr; /* the guest object's address */
    PMutex m;
    PCond c;
    _Atomic u32 owner;
    u32 depth; /* a recursive mutex's holder (plat_tid) and how often it holds it */
} SyncObj;
#define SYNC_BUCKETS 4096
static struct {
    PMutex lk; /* the table */
    SyncObj* bucket[SYNC_BUCKETS];
} g_sync = {PMUTEX_INIT, {0}};

static u32 bucket_of(u64 a) { return (u32)((a >> 3) * 0x9E3779B97F4A7C15ull >> 52) & (SYNC_BUCKETS - 1); }
/* the host object of the guest object at addr, made when there is none */
static SyncObj* obj(u64 addr) {
    u32 b = bucket_of(addr);
    pm_lock(&g_sync.lk);
    SyncObj* o = g_sync.bucket[b];
    while (o && o->addr != addr) o = o->next;
    if (!o) {
        o = calloc(1, sizeof *o);
        if (!o) fatal("hooks: no host memory for a mutex or condition variable of the game's");
        o->addr = addr;
        pm_init(&o->m);
        pc_init(&o->c);
        o->next = g_sync.bucket[b];
        g_sync.bucket[b] = o;
    }
    pm_unlock(&g_sync.lk);
    return o;
}
/* the guest object at addr destroyed: its host object goes. Objects live as long as the run otherwise */
static void drop(u64 addr) {
    u32 b = bucket_of(addr);
    pm_lock(&g_sync.lk);
    for (SyncObj** p = &g_sync.bucket[b]; *p; p = &(*p)->next)
        if ((*p)->addr == addr) {
            SyncObj* o = *p;
            *p = o->next;
            pm_unlock(&g_sync.lk);
            pm_destroy(&o->m);
            free(o);
            return;
        }
    pm_unlock(&g_sync.lk);
}
/* the host object of the guest object at addr, NULL when there is none (a mutex this family never locked) */
static SyncObj* find(u64 addr) {
    pm_lock(&g_sync.lk);
    SyncObj* o = g_sync.bucket[bucket_of(addr)];
    while (o && o->addr != addr) o = o->next;
    pm_unlock(&g_sync.lk);
    return o;
}
/* o's mutex, waited for off the core when another thread holds it */
static void lock(SyncObj* o) {
    if (pm_trylock(&o->m)) return;
    hook_wait_begin();
    pm_lock(&o->m);
    hook_wait_end();
}

/* ---- std::mutex */
void sync_mutex_lock(CPU* c, u64 m) {
    (void)c;
    lock(obj(m));
}
int sync_mutex_try_lock(CPU* c, u64 m) {
    (void)c;
    return pm_trylock(&obj(m)->m) != 0;
}
void sync_mutex_unlock(CPU* c, u64 m) {
    (void)c;
    pm_unlock(&obj(m)->m);
}
void sync_drop(CPU* c, u64 o) {
    (void)c;
    drop(o);
}

/* ---- std::recursive_mutex: the host mutex taken once, by the thread that holds it however often it locked it */
void sync_recursive_init(CPU* c, u64 m) {
    (void)c;
    drop(m); /* a new object where an old one was */
    obj(m);
}
void sync_recursive_lock(CPU* c, u64 m) {
    (void)c;
    SyncObj* o = obj(m);
    u32 me = plat_tid();
    if (atomic_load_explicit(&o->owner, memory_order_relaxed) == me) {
        o->depth++;
        return;
    }
    lock(o);
    atomic_store_explicit(&o->owner, me, memory_order_relaxed);
    o->depth = 1;
}
int sync_recursive_try_lock(CPU* c, u64 m) {
    (void)c;
    SyncObj* o = obj(m);
    u32 me = plat_tid();
    if (atomic_load_explicit(&o->owner, memory_order_relaxed) == me) {
        o->depth++;
        return 1;
    }
    if (!pm_trylock(&o->m)) return 0;
    atomic_store_explicit(&o->owner, me, memory_order_relaxed);
    o->depth = 1;
    return 1;
}
void sync_recursive_unlock(CPU* c, u64 m) {
    (void)c;
    SyncObj* o = obj(m);
    if (--o->depth) return;
    atomic_store_explicit(&o->owner, 0, memory_order_relaxed);
    pm_unlock(&o->m);
}

/* ---- std::condition_variable: a unique_lock<mutex> is the mutex's address, then whether it owns it */
void sync_cond_notify_one(CPU* c, u64 cv) {
    (void)c;
    pc_signal(&obj(cv)->c);
}
void sync_cond_notify_all(CPU* c, u64 cv) {
    (void)c;
    pc_broadcast(&obj(cv)->c);
}
void sync_cond_wait(CPU* c, u64 cv, u64 lk) {
    SyncObj* o = obj(cv);
    SyncObj* m = obj(snx_ld_le64(hook_host(c, lk)));
    hook_wait_begin();
    pc_wait(&o->c, &m->m, -1);
    hook_wait_end();
}
/* until the system_clock time point at ns nanoseconds since 1970 (system_clock::now's host clock) */
void sync_cond_timed_wait(CPU* c, u64 cv, u64 lk, i64 ns) {
    SyncObj* o = obj(cv);
    SyncObj* m = obj(snx_ld_le64(hook_host(c, lk)));
    i64 left = ns - plat_unix_ns();
    if (left <= 0) return;
    hook_wait_begin();
    pc_wait(&o->c, &m->m, left);
    hook_wait_end();
}

/* ---- the C library's waits on a std::mutex of the family's (libc++'s inlined waits), on the host as the condition
   variable's own: 0 when woken (or for no reason, as a condition variable may be), else ETIMEDOUT as the console's C
   library numbers it. A wait on any other mutex runs the function's own code */
#define GUEST_ETIMEDOUT  110
#define GUEST_CLOCK_MONO 1
static i32 host_wait(u64 cv, SyncObj* m, i64 left) {
    SyncObj* o = obj(cv);
    int out;
    if (!left) return GUEST_ETIMEDOUT;
    hook_wait_begin();
    out = pc_wait(&o->c, &m->m, left);
    hook_wait_end();
    return out ? GUEST_ETIMEDOUT : 0;
}
/* the nanoseconds left until the timespec at abstime on the clock (at least 0): CLOCK_MONOTONIC's is steady_clock's,
   the game's clock, as the SDK reads it; any other the host's real time, as system_clock's now */
static i64 left_until(CPU* c, i32 clock, u64 abstime) {
    const u8* t = hook_host(c, abstime);
    i64 at = (i64)snx_ld_le64(t) * 1000000000 + (i64)snx_ld_le64(t + 8);
    i64 left = at - (clock == GUEST_CLOCK_MONO ? (i64)guest_ns() : plat_unix_ns());
    return left > 0 ? left : 0;
}
static i32 run_own(CPU* c) {
    hook_run_original(c, c->pc);
    return (i32)c->x[0];
}
i32 sync_pthread_cond_wait(CPU* c, u64 cv, u64 m) {
    SyncObj* o = find(m);
    return o ? host_wait(cv, o, -1) : run_own(c);
}
/* a std::condition_variable has no clock attribute: the real time's, CLOCK_REALTIME */
i32 sync_pthread_cond_timedwait(CPU* c, u64 cv, u64 m, u64 abstime) {
    SyncObj* o = find(m);
    return o ? host_wait(cv, o, left_until(c, 0, abstime)) : run_own(c);
}
i32 sync_pthread_cond_clockwait(CPU* c, u64 cv, u64 m, i32 clock, u64 abstime) {
    SyncObj* o = find(m);
    return o ? host_wait(cv, o, left_until(c, clock, abstime)) : run_own(c);
}

/* ---- the clocks: system_clock's microseconds since 1970, steady_clock's nanoseconds of the game's clock */
i64 sync_system_now(CPU* c) {
    (void)c;
    return plat_unix_ns() / 1000;
}
i64 sync_steady_now(CPU* c) {
    (void)c;
    return (i64)guest_ns();
}
/* this_thread::sleep_for: the duration's nanoseconds at d, slept off the core */
void sync_sleep_for(CPU* c, u64 d) {
    i64 ns = (i64)snx_ld_le64(hook_host(c, d));
    if (ns <= 0) return;
    hook_wait_begin();
    plat_sleep_ns(ns);
    hook_wait_end();
}
