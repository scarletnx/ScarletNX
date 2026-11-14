/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C++ library's mutexes, condition variable, clocks and sleep the game's SDK exports (hooks.hook's sync family),
   run on the host's own: a std::mutex, std::recursive_mutex or std::condition_variable of the game's is a host mutex
   or condition variable (PMutex, PCond: the host's pthreads, SRW locks on Windows) found by the guest object's
   address, made at its first use (std::mutex's constructor is the compiler's, inlined) and let go of at its
   destructor. The guest object's own bytes go unread and unwritten: every function that reads them is hooked, and
   the others (timed_mutex's, shared_timed_mutex's and condition_variable_any's, whose templates the game's code
   inlines over their members) are left to the game's own code.

   A thread that waits (a lock someone holds, a condition variable, a sleep) leaves its core first, as a waiting SVC
   does (hook_wait_begin): the core's other threads run meanwhile, the holder among them.

   The clocks are the host's too, system_clock's and steady_clock's now: a timed wait's deadline, which the game makes
   from them, is then on the host's clock, and waits as long as it says */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>

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
    u32 owner, depth; /* a recursive mutex's holder (plat_tid) and how often it holds it */
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
    if (__atomic_load_n(&o->owner, __ATOMIC_RELAXED) == me) {
        o->depth++;
        return;
    }
    lock(o);
    __atomic_store_n(&o->owner, me, __ATOMIC_RELAXED);
    o->depth = 1;
}
int sync_recursive_try_lock(CPU* c, u64 m) {
    (void)c;
    SyncObj* o = obj(m);
    u32 me = plat_tid();
    if (__atomic_load_n(&o->owner, __ATOMIC_RELAXED) == me) {
        o->depth++;
        return 1;
    }
    if (!pm_trylock(&o->m)) return 0;
    __atomic_store_n(&o->owner, me, __ATOMIC_RELAXED);
    o->depth = 1;
    return 1;
}
void sync_recursive_unlock(CPU* c, u64 m) {
    (void)c;
    SyncObj* o = obj(m);
    if (--o->depth) return;
    __atomic_store_n(&o->owner, 0, __ATOMIC_RELAXED);
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

/* ---- the clocks: system_clock's microseconds since 1970, steady_clock's nanoseconds */
i64 sync_system_now(CPU* c) {
    (void)c;
    return plat_unix_ns() / 1000;
}
i64 sync_steady_now(CPU* c) {
    (void)c;
    return (i64)plat_ns();
}
/* this_thread::sleep_for: the duration's nanoseconds at d, slept off the core */
void sync_sleep_for(CPU* c, u64 d) {
    i64 ns = (i64)snx_ld_le64(hook_host(c, d));
    if (ns <= 0) return;
    hook_wait_begin();
    plat_sleep_ns(ns);
    hook_wait_end();
}
