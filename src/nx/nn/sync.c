/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C++ library's mutexes, condition variable, clocks and sleep the game's SDK exports (hooks.hook's sync family for
   the clocks and the sleep, and its cond_var family for the mutexes and the condition variable, which native_cond_var=0
   leaves to the game).

   The cond_var family keeps a std::mutex, std::recursive_mutex or std::condition_variable of the game's in place, as
   the SDK's C library does, which libc++ makes them of (pthread_mutex_lock, _trylock, _unlock, pthread_cond_wait,
   _signal and _broadcast, from 14.3.0 to 20.5.17, the SDKs looked at): a mutex is a pthread_mutex_t, its byte 1 the
   type (0 normal, 1 recursive, 2 error checking), its u16 at 2 how often its holder holds it, and at 4 an SdkMutexType's
   lock word; a condition variable is a pthread_cond_t with an SdkConditionVariableType's word at 4. Those words go
   through the os_sync family's handlers (os_sync.c), so the SDK's own code (the constructors, the timed waits, and
   the waits the game's code inlines: libc++'s steady_clock waits call pthread_cond_clockwait) works on the same words
   with no table beside them and nothing to allocate. A call the SDK answers with an error, a throw or an abort (a
   mutex its holder locks again, an unknown type, a count that would wrap, a wait without the lock) runs the
   function's own code, which gives it. Before 14.3.0 a std::mutex is a nn::os::MutexType, whose functions run the
   game's own code on os_sync's critical section.

   A thread that waits (a lock someone holds, a condition variable, a sleep) leaves its core first, as a waiting SVC
   does (hook_wait_begin): the core's other threads run meanwhile, the holder among them.

   steady_clock's now is the game's clock (guest_ns), as the SDK's reads it through clock_gettime's CLOCK_MONOTONIC and
   nn::os's system tick: it stops while the game is paused and takes a save state's time, as every other clock of the
   game does. system_clock's now is the host's real time */
#include "nx/nn/hook.h"
#include "nn/hooks.hook.h"
#include "common/platform/time.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/tracy.h"

/* off the core while the thread waits, and counted as stopped for a pause (kthread_block) */
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
/* ---- the SDK's pthread_mutex_t and pthread_cond_t, kept as its C library's functions keep them */
#define PT_TYPE  1u /* the mutex's type: PT_NORMAL, PT_RECURSIVE or PT_ERRORCHECK */
#define PT_COUNT 2u /* the u16 count of the mutex's holder */
#define PT_WORD  4u /* the SdkMutexType's lock word, and the SdkConditionVariableType's word */
enum { PT_NORMAL, PT_RECURSIVE, PT_ERRORCHECK };
#define PT_COUNT_MAX 0xFFFFu /* past it pthread_mutex_lock answers EAGAIN */

static u32 type_of(CPU* c, u64 m) { return hook_host(c, m + PT_TYPE)[0]; }
static u32 count_of(CPU* c, u64 m) { return snx_ld_le16(hook_host(c, m + PT_COUNT)); }
static void count_set(CPU* c, u64 m, u32 n) { snx_st_le16(hook_host(c, m + PT_COUNT), (u16)n); }
/* the hooked function's own code, which answers the call: its result */
static int run_own(CPU* c) {
    hook_run_original(c, c->pc);
    return (int)c->x[0];
}

/* std::mutex::lock and std::recursive_mutex::lock, pthread_mutex_lock's success: the lock taken (or held again, a
   recursive one), its holder's count one more */
void sync_mutex_lock(CPU* c, u64 m) {
    u32 type = type_of(c, m);
    if (type > PT_ERRORCHECK) {
        run_own(c); /* EINVAL, which the caller throws */
        return;
    }
    if (os_cs_mine(c, m + PT_WORD)) {
        if (type != PT_RECURSIVE || count_of(c, m) == PT_COUNT_MAX) {
            run_own(c); /* the SDK's abort, EDEADLK or EAGAIN */
            return;
        }
    } else
        os_cs_enter(c, m + PT_WORD);
    count_set(c, m, count_of(c, m) + 1);
}
/* their try_lock: pthread_mutex_trylock's 0, or its EBUSY or EAGAIN */
int sync_mutex_try_lock(CPU* c, u64 m) {
    u32 type = type_of(c, m);
    int got;
    if (type > PT_ERRORCHECK) return run_own(c);
    if (os_cs_mine(c, m + PT_WORD))
        got = type == PT_RECURSIVE && count_of(c, m) != PT_COUNT_MAX;
    else
        got = os_cs_try_enter(c, m + PT_WORD);
    SNX_ZONE_V(z, "hook mutex try", (u64)got); /* frequent decision: 1 acquired, 0 busy */
    if (got) count_set(c, m, count_of(c, m) + 1);
    return got;
}
/* their unlock, by the holder: the count one less, the lock let go at 0 */
void sync_mutex_unlock(CPU* c, u64 m) {
    u32 n = count_of(c, m);
    if (type_of(c, m) > PT_ERRORCHECK || !n || !os_cs_mine(c, m + PT_WORD)) {
        run_own(c); /* EINVAL, EPERM, or the SDK's abort for a lock its thread doesn't hold */
        return;
    }
    count_set(c, m, n - 1);
    if (n == 1) os_cs_leave(c, m + PT_WORD);
}

/* std::condition_variable: notify_one and notify_all are pthread_cond_signal and _broadcast */
void sync_cond_notify_one(CPU* c, u64 cv) { os_cv_signal(c, cv + PT_WORD); }
void sync_cond_notify_all(CPU* c, u64 cv) { os_cv_broadcast(c, cv + PT_WORD); }
/* wait(unique_lock<mutex>&), a unique_lock being the mutex's address and then whether it owns it: pthread_cond_wait,
   whose mutex is held once, its count 0 while the wait has let it go */
void sync_cond_wait(CPU* c, u64 cv, u64 lk) {
    const u8* l = hook_host(c, lk);
    u64 m = snx_ld_le64(l);
    if (!l[8] || !m || count_of(c, m) != 1 || !os_cs_mine(c, m + PT_WORD)) {
        run_own(c); /* the throw for a lock it doesn't own, EPERM, or the SDK's abort */
        return;
    }
    count_set(c, m, 0);
    os_sdk_cv_wait(c, cv + PT_WORD, m + PT_WORD);
    count_set(c, m, count_of(c, m) + 1);
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
#if defined(SNX_TRACY)
    u64 waited; /* only the sleep-time plot reads the clock */
#endif
    i64 ns = (i64)snx_ld_le64(hook_host(c, d));
    if (ns <= 0) {
        SNX_MSG("hook sleep none", sizeof("hook sleep none") - 1); /* rare decision */
        return;
    }
#if defined(SNX_TRACY)
    waited = plat_ns(); /* a duration: the monotonic clock */
#endif
    hook_wait_begin();
    plat_sleep_ns(ns);
    hook_wait_end();
#if defined(SNX_TRACY)
    SNX_PLOT("hook sleep ms", (double)(plat_ns() - waited) / 1e6);
#endif
}
