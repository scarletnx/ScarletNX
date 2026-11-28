/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sync.c's clocks against the game's (runtime/clock.c): steady_clock::now is guest time, which a game pause stops and a
   save state's load sets, as the SDK's own steady_clock reads it; system_clock::now is the host's real time.
   And the cond_var family in place, on the SDK's pthread_mutex_t and pthread_cond_t as its C library lays them out
   (from 14.3.0): a lock's word at 4 the holder's handle, as the SDK's IsLockedByCurrentThread reads it, its count at 2
   how often it holds it; a normal mutex, a recursive one held again, try_lock busy and free, unlock to 0; a
   condition variable's wait letting the mutex go (count 0, the word another thread's) until notified, and holding it
   again; and each call the SDK answers with an error, a throw or an abort running the function's own code, the
   object untouched. Host threads stand for the game's, each with its own thread record, through os_sync.c's
   handlers */
#include "nx/nn/hook.h"
#include "nn/hooks.hook.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/kernel/kernel_int.h"
#include "nx/runtime/clock.h"
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/* the kernel's side of a wait, which these calls never make in the kernel */
Hle g_hle;
int g_lock_futex = 1;
static _Thread_local KThread* t_self;
KThread* kthread_cur(void) { return t_self; }
void core_leave(KThread* t) { (void)t; }
void core_enter(KThread* t) { (void)t; }
void core_yield(KThread* t, int migrate) { (void)t, (void)migrate; }
void kthread_block(KThread* t) { (void)t; }
void kthread_unblock(KThread* t) { (void)t; }
u64 kernel_find_symbol_exact(const char* name) {
    (void)name;
    return 0;
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}
u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count) {
    (void)addr, (void)type, (void)value, (void)count;
    abort();
}
u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i64 value, i64 timeout) {
    (void)t, (void)addr, (void)type, (void)value, (void)timeout;
    abort();
}
static u8 g_guest[0x1000];
/* the kernel's hand over with no thread waiting in the kernel: to the tag given, keeping the waiters bit, else freed */
u32 svc_arbitrate_unlock(u64 addr) {
    (void)addr;
    abort();
}
u32 svc_arbitrate_unlock_to(u64 addr, u32 tag, int* handed) {
    _Atomic u32* p = (_Atomic u32*)(void*)(g_guest + addr);
    *handed = tag != 0;
    atomic_store(p, snx_le32(tag ? tag | HANDLE_WAIT : 0u));
    if (tag)
        plat_wake_word(p);
    else
        plat_wake_word_one(p);
    return 0;
}
/* the hooked function's own code: counted, its result a mark */
#define OWN_RESULT 22
static int g_original;
void hook_run_original(CPU* c, u64 addr) {
    (void)addr;
    g_original++;
    c->x[0] = OWN_RESULT;
}

/* guest memory: two mutexes, a condition variable and a unique_lock, as the SDK lays them out */
#define MUTEX  0x100u
#define RMUTEX 0x140u
#define CV     0x180u
#define LOCK   0x1c0u
#define ME     0x1234u /* this thread's handle */
#define OTHER  0x5678u /* the other thread's */
static KThread g_thread[2];
/* the lock word's holder, less the bit that says threads wait on it (which a hand over keeps) */
static u32 word_at(u64 m) { return snx_ld_le32(g_guest + m + 4) & ~HANDLE_WAIT; }
static u32 count_at(u64 m) { return snx_ld_le16(g_guest + m + 2); }
static void lock_holds(u64 m, int owns) {
    snx_st_le64(g_guest + LOCK, m);
    g_guest[LOCK + 8] = (u8)owns;
}
/* what the other thread saw of the mutex once it held it during the wait: its count and word */
static u32 g_seen_count, g_seen_word;
static void notifier(void* arg) {
    CPU* c = arg;
    t_self = &g_thread[1];
    plat_sleep_ns(20000000);
    sync_mutex_lock(c, MUTEX);
    g_seen_count = count_at(MUTEX);
    g_seen_word = word_at(MUTEX);
    sync_cond_notify_all(c, CV);
    sync_mutex_unlock(c, MUTEX);
}
static void test_in_place(void) {
    CPU c = {.mem = g_guest}, other = {.mem = g_guest};
    g_thread[0].own_handle = ME;
    g_thread[1].own_handle = OTHER;
    t_self = &g_thread[0];
    /* a std::mutex as its constexpr constructor leaves it, all zero: taken and let go on the SDK's words */
    sync_mutex_lock(&c, MUTEX);
    assert(word_at(MUTEX) == ME && count_at(MUTEX) == 1);
    t_self = &g_thread[1];
    assert(!sync_mutex_try_lock(&other, MUTEX) && count_at(MUTEX) == 1); /* busy for another thread */
    t_self = &g_thread[0];
    assert(!sync_mutex_try_lock(&c, MUTEX) && g_original == 0); /* EBUSY for its holder, a normal mutex */
    sync_mutex_lock(&c, MUTEX); /* locked again by its holder: the SDK's abort, the function's own code */
    assert(g_original == 1 && word_at(MUTEX) == ME && count_at(MUTEX) == 1);
    sync_mutex_unlock(&c, MUTEX);
    assert(word_at(MUTEX) == 0 && count_at(MUTEX) == 0);
    sync_mutex_unlock(&c, MUTEX); /* not held: the function's own code */
    assert(g_original == 2 && word_at(MUTEX) == 0);
    assert(sync_mutex_try_lock(&c, MUTEX) && word_at(MUTEX) == ME && count_at(MUTEX) == 1);
    t_self = &g_thread[1];
    sync_mutex_unlock(&other, MUTEX); /* by a thread that doesn't hold it: the function's own code */
    assert(g_original == 3 && word_at(MUTEX) == ME && count_at(MUTEX) == 1);
    t_self = &g_thread[0];
    sync_mutex_unlock(&c, MUTEX);
    /* a recursive mutex (pthread_mutexattr_settype's 1 in its type byte): held again, let go at its count's 0 */
    g_guest[RMUTEX + 1] = 1;
    sync_mutex_lock(&c, RMUTEX);
    sync_mutex_lock(&c, RMUTEX);
    assert(sync_mutex_try_lock(&c, RMUTEX) && word_at(RMUTEX) == ME && count_at(RMUTEX) == 3);
    sync_mutex_unlock(&c, RMUTEX);
    sync_mutex_unlock(&c, RMUTEX);
    assert(word_at(RMUTEX) == ME && count_at(RMUTEX) == 1);
    sync_mutex_unlock(&c, RMUTEX);
    assert(word_at(RMUTEX) == 0 && count_at(RMUTEX) == 0 && g_original == 3);
    /* a count that would wrap: EAGAIN, the function's own code for lock and busy for try_lock */
    sync_mutex_lock(&c, RMUTEX);
    snx_st_le16(g_guest + RMUTEX + 2, 0xFFFF);
    sync_mutex_lock(&c, RMUTEX);
    assert(g_original == 4 && !sync_mutex_try_lock(&c, RMUTEX) && count_at(RMUTEX) == 0xFFFF);
    snx_st_le16(g_guest + RMUTEX + 2, 1);
    sync_mutex_unlock(&c, RMUTEX);
    /* a type the C library doesn't have: EINVAL, the function's own code for every call */
    g_guest[RMUTEX + 1] = 3;
    sync_mutex_lock(&c, RMUTEX);
    assert(sync_mutex_try_lock(&c, RMUTEX) == OWN_RESULT);
    sync_mutex_unlock(&c, RMUTEX);
    assert(g_original == 7 && word_at(RMUTEX) == 0 && count_at(RMUTEX) == 0);
    /* a notify with no waiter leaves the condition variable's word as it was */
    sync_cond_notify_one(&c, CV);
    sync_cond_notify_all(&c, CV);
    assert(snx_ld_le32(g_guest + CV + 4) == 0);
    /* a wait: the mutex let go (count 0, another thread takes it) until notified, then held again once */
    sync_mutex_lock(&c, MUTEX);
    lock_holds(MUTEX, 1);
    assert(plat_thread(notifier, &other, 0) == 0);
    sync_cond_wait(&c, CV, LOCK);
    assert(g_seen_count == 1 && g_seen_word == OTHER); /* the other thread's lock, count 1, while this one waited */
    assert(word_at(MUTEX) == ME && count_at(MUTEX) == 1 && g_original == 7);
    /* a unique_lock that doesn't own its mutex, a mutex held twice: the function's own code */
    lock_holds(MUTEX, 0);
    sync_cond_wait(&c, CV, LOCK);
    g_guest[RMUTEX + 1] = 1;
    sync_mutex_lock(&c, RMUTEX);
    sync_mutex_lock(&c, RMUTEX);
    lock_holds(RMUTEX, 1);
    sync_cond_wait(&c, CV, LOCK);
    assert(g_original == 9 && count_at(RMUTEX) == 2 && word_at(MUTEX) == ME);
    sync_mutex_unlock(&c, MUTEX);
    plat_sleep_ns(20000000); /* the notifier lets go of the mutex */
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
    test_in_place();
    puts("PASS sync.c's clocks: steady_clock the game's (stopped by a pause, set by a state's load), system_clock the "
         "host's; std::mutex, recursive_mutex and condition_variable on the SDK's own words, its errors its own code");
    return 0;
}
