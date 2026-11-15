/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::os's sleeps, yields, locks and condition variables (hooks.hook's os_sync family), which the game calls tens of
   thousands of times a second, run here with no SVC: the SDK's own function would make one (svcSleepThread,
   svcArbitrateLock and Unlock, svcWaitProcessWideKeyAtomic and svcSignalProcessWideKey), and every SVC is a trip out
   of the recompiled code into the kernel's. The family hooks the functions the game calls (nn::os::SleepThread,
   YieldThread and the SDK's own SdkMutexType, SdkRecursiveMutexType and SdkConditionVariableType) and the ones every
   other nn::os object locks and waits through (InternalCriticalSectionImplByHorizon and
   InternalConditionVariableImplByHorizon: MutexType's, ConditionVariableType's, the semaphores', the events' and the
   message queues').

   A lock's word is the SDK's, in guest memory as the SDK leaves it: 0 when free, else the owner's handle (KThread's
   own_handle, which the SDK tags the word with too), with HANDLE_WAIT set while threads wait. They wait on the word
   itself, the host's futex (plat_wait_word), instead of in the kernel's lists. Some of the SDK's own code takes and
   leaves those words itself, with the kernel's waits (ReaderWriterLockImplByHorizon, which inlines them): so a holder
   with waiters lets go through the kernel's svcArbitrateUnlock, which hands the lock to a thread waiting in the
   kernel, and the kernel wakes one waiting on the word whenever it hands one on or frees it (g_lock_futex). A condition variable's word, which the
   SDK has the kernel set while threads wait, counts its waiters here in its low 16 bits and its signals in the high
   16, and the waiters wait on it the same way: the family owns that word, so every function of the SDK's that writes
   a condition variable's word, and the critical section's own functions, are hooked, or none is (os_sync_ready).

   A save state can't hold a thread waiting on the host, so native_os_sync=0 leaves the family to the game's own code,
   and save states are refused while it runs (hook_states_refused), as for the cond_var family. A waiting thread
   leaves its core first (hook_wait_begin), as in a waiting SVC: the core's other threads run meanwhile, the holder
   among them. The console's kernel raises a lock holder's priority to its waiters'; the host's futex doesn't */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel_int.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"

/* the functions that write the two words, which the family takes all of or none of */
static const char* const k_word_writers[] = {
    "_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon5EnterEv",
    "_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon8TryEnterEv",
    "_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon5LeaveEv",
    "_ZN2nn2os6detail38InternalConditionVariableImplByHorizon6SignalEv",
    "_ZN2nn2os6detail38InternalConditionVariableImplByHorizon9BroadcastEv",
    "_ZN2nn2os6detail38InternalConditionVariableImplByHorizon4WaitEPNS1_23InternalCriticalSectionE",
    "_ZN2nn2os6detail38InternalConditionVariableImplByHorizon9TimedWaitEPNS1_23InternalCriticalSectionERKNS1_"
    "13TimeoutHelperE",
};
int os_sync_ready(void) {
    for (u32 i = 0; i < sizeof k_word_writers / sizeof *k_word_writers; i++)
        if (!kernel_find_symbol_exact(k_word_writers[i])) return 0;
    g_lock_futex = 1; /* asked as the hooks are installed, before the game's first thread */
    return 1;
}

static u32* word(CPU* c, u64 a) { return (u32*)(void*)hook_host(c, a); }
static u32 ld(const u32* p) { return snx_le32(__atomic_load_n(p, __ATOMIC_ACQUIRE)); }
static int cas(u32* p, u32* cur, u32 nv) {
    u32 e = snx_le32(*cur);
    int ok = __atomic_compare_exchange_n(p, &e, snx_le32(nv), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    *cur = snx_le32(e);
    return ok;
}
/* the guest word plus d, the new value */
static u32 add(u32* p, u32 d) {
    u32 v = ld(p);
    while (!cas(p, &v, v + d)) {}
    return v + d;
}
/* the calling thread's tag; one no handle is for a thread the game got none for (it never takes a lock) */
static u32 me(void) {
    KThread* t = kthread_cur();
    return t && t->own_handle ? t->own_handle : t ? 0x3FFF0000u | kt_ix(t) : 0x3FFFFFFFu;
}
/* the game's NN_ABORT for a lock used wrong, which its own code would stop on */
static void misuse(const char* what) { fatal("nn::os: %s (the SDK aborts here)", what); }

/* ---- sleeps and yields: svcSleepThread's, with no SVC */
static void yield(void) {
    KThread* t = kthread_cur();
    core_yield(t, 2); /* the SDK's yield is svcSleepThread(-1): to any core */
    if (t && (++t->yields & 63) == 0) plat_yield();
}
/* TimeoutHelperImplByHorizon::Sleep, which nn::os::SleepThread is: 0 yields */
void os_sleep_thread(CPU* c, i64 ns) {
    (void)c;
    if (ns == 0 || ns == -1 || ns == -2) {
        yield();
        return;
    }
    if (ns < 0) return;
    hook_wait_begin();
    plat_sleep_ns(ns);
    hook_wait_end();
}
void os_yield_thread(CPU* c) {
    (void)c;
    yield();
}
void os_yield_impl(CPU* c, u64 self) {
    (void)c, (void)self;
    yield();
}

/* ---- InternalCriticalSection: the word at cs */
static void cs_enter(CPU* c, u64 cs) {
    u32* p = word(c, cs);
    u32 tag = me(), v = 0;
    int waited = 0;
    if (cas(p, &v, tag)) return;
    if ((v & ~HANDLE_WAIT) == tag) return; /* the SDK's Enter takes a lock its thread holds as held */
    for (;;) {
        if (!v) { /* free: taken with the waiters bit, as others may still wait */
            if (cas(p, &v, tag | HANDLE_WAIT)) break;
            continue;
        }
        if (!(v & HANDLE_WAIT) && !cas(p, &v, v | HANDLE_WAIT)) continue;
        if (!waited) hook_wait_begin(), waited = 1;
        plat_wait_word(p, snx_le32(v | HANDLE_WAIT), -1);
        v = ld(p);
    }
    if (waited) hook_wait_end();
}
static int cs_try_enter(CPU* c, u64 cs) {
    u32 v = 0;
    return cas(word(c, cs), &v, me());
}
/* as the SDK's Leave: free at once unless someone waits, else the kernel's svcArbitrateUnlock, here with no SVC, hands
   it to the best thread waiting there, or frees it, and wakes one waiting on the word (g_lock_futex) */
static void cs_leave(CPU* c, u64 cs) {
    u32 v = me();
    if (!cas(word(c, cs), &v, 0)) svc_arbitrate_unlock(cs);
}
static int cs_mine(CPU* c, u64 cs) { return (ld(word(c, cs)) & ~HANDLE_WAIT) == me(); }

void os_cs_init(CPU* c, u64 cs) { __atomic_store_n(word(c, cs), 0u, __ATOMIC_RELEASE); }
void os_cs_enter(CPU* c, u64 cs) { cs_enter(c, cs); }
int os_cs_try_enter(CPU* c, u64 cs) { return cs_try_enter(c, cs); }
void os_cs_leave(CPU* c, u64 cs) { cs_leave(c, cs); }
int os_cs_mine(CPU* c, u64 cs) { return cs_mine(c, cs); }

/* ---- InternalConditionVariable: the word at cv, waiters in its low 16 bits, signals in its high 16 */
#define CV_WAITERS 0xFFFFu
#define CV_SIGNAL  0x10000u
static void cv_wake(CPU* c, u64 cv, int all) {
    u32* p = word(c, cv);
    if (!(ld(p) & CV_WAITERS)) return;
    add(p, CV_SIGNAL);
    if (all)
        plat_wake_word(p);
    else
        plat_wake_word_one(p);
}
/* cs held by the caller, which it lets go of while it waits: until a signal (1), or until the host's clock reaches
   end (plat_ns; 0 for none) first (0). It holds cs again either way */
static int cv_wait(CPU* c, u64 cv, u64 cs, u64 end) {
    u32* p = word(c, cv);
    u32 v = add(p, 1u), now;
    int got = 0;
    cs_leave(c, cs);
    hook_wait_begin();
    for (;;) {
        i64 left = -1;
        if (end) {
            u64 t = plat_ns();
            if (t >= end) break;
            left = (i64)(end - t);
        }
        plat_wait_word(p, snx_le32(v), left);
        now = ld(p);
        if ((now ^ v) & ~CV_WAITERS) {
            got = 1;
            break;
        }
        v = now; /* another waiter came or went: wait on with its count */
    }
    add(p, (u32)-1);
    hook_wait_end();
    cs_enter(c, cs);
    return got;
}
/* the host's deadline for ns nanoseconds from now; 0 for none */
static u64 deadline_ns(i64 ns) { return plat_ns() + (u64)ns; }
/* the host's deadline for a guest system tick (CNTPCT_EL0's, as the SDK's TimeoutHelper keeps it) */
static i64 ticks_left_ns(u64 tick) {
    u64 now = guest_ticks();
    ClockRate r = clock_rate();
    u64 d, s;
    if (tick <= now) return 0;
    d = tick - now;
    s = d / r.freq;
    if (s >= (u64)INT64_MAX / 1000000000u / 2) return INT64_MAX / 2;
    return (i64)(s * 1000000000u + (d % r.freq) * 1000000000u / r.freq);
}

void os_cv_init(CPU* c, u64 cv) { __atomic_store_n(word(c, cv), 0u, __ATOMIC_RELEASE); }
void os_cv_signal(CPU* c, u64 cv) { cv_wake(c, cv, 0); }
void os_cv_broadcast(CPU* c, u64 cv) { cv_wake(c, cv, 1); }
/* the SDK checks the caller holds cs, and aborts otherwise */
void os_cv_wait(CPU* c, u64 cv, u64 cs) {
    if (!cs_mine(c, cs)) misuse("InternalConditionVariable::Wait without its lock");
    cv_wait(c, cv, cs, 0);
}
/* until the system tick the TimeoutHelper at helper holds (its first 8 bytes; 0 for none left): 1 when signaled, 0
   when the time ran out. With no time left the SDK returns 0 at once, holding the lock all along */
int os_cv_timed_wait(CPU* c, u64 cv, u64 cs, u64 helper) {
    u64 tick = snx_ld_le64(hook_host(c, helper));
    i64 left;
    if (!cs_mine(c, cs)) misuse("InternalConditionVariable::TimedWait without its lock");
    left = tick ? ticks_left_ns(tick) : 0;
    if (left < 1) return 0;
    return cv_wait(c, cv, cs, deadline_ns(left));
}

/* ---- the SDK's own types: SdkMutexType and SdkConditionVariableType are a word each, SdkRecursiveMutexType a lock
   word then its count, as their own functions use them (with the aborts they check for) */
void os_sdk_mutex_lock(CPU* c, u64 m) {
    if (cs_mine(c, m)) misuse("SdkMutexType::Lock by its holder");
    cs_enter(c, m);
}
int os_sdk_mutex_try_lock(CPU* c, u64 m) {
    if (cs_mine(c, m)) misuse("SdkMutexType::TryLock by its holder");
    return cs_try_enter(c, m);
}
void os_sdk_mutex_unlock(CPU* c, u64 m) {
    if (!cs_mine(c, m)) misuse("SdkMutexType::Unlock by a thread that doesn't hold it");
    cs_leave(c, m);
}
void os_sdk_recursive_init(CPU* c, u64 m) {
    __atomic_store_n(word(c, m), 0u, __ATOMIC_RELEASE);
    snx_st_le32(hook_host(c, m + 4), 0);
}
static void recursive_more(CPU* c, u64 m) {
    u32 n = snx_ld_le32(hook_host(c, m + 4)) + 1;
    if (!n) misuse("SdkRecursiveMutexType locked 2^32 times");
    snx_st_le32(hook_host(c, m + 4), n);
}
void os_sdk_recursive_lock(CPU* c, u64 m) {
    if (!cs_mine(c, m)) cs_enter(c, m);
    recursive_more(c, m);
}
int os_sdk_recursive_try_lock(CPU* c, u64 m) {
    if (!cs_mine(c, m) && !cs_try_enter(c, m)) return 0;
    recursive_more(c, m);
    return 1;
}
void os_sdk_recursive_unlock(CPU* c, u64 m) {
    u32 n;
    if (!cs_mine(c, m)) misuse("SdkRecursiveMutexType::Unlock by a thread that doesn't hold it");
    n = snx_ld_le32(hook_host(c, m + 4)) - 1;
    snx_st_le32(hook_host(c, m + 4), n);
    if (!n) cs_leave(c, m);
}
void os_sdk_cv_wait(CPU* c, u64 cv, u64 m) {
    if (!cs_mine(c, m)) misuse("SdkConditionVariableType::Wait without its mutex");
    cv_wait(c, cv, m, 0);
}
/* a TimeSpan of 0 lets the mutex go and takes it again, and times out; a negative one times out holding it */
int os_sdk_cv_timed_wait(CPU* c, u64 cv, u64 m, i64 ns) {
    if (!cs_mine(c, m)) misuse("SdkConditionVariableType::TimedWait without its mutex");
    if (ns < 0) return 0;
    if (!ns) {
        cs_leave(c, m);
        cs_enter(c, m);
        return 0;
    }
    return cv_wait(c, cv, m, deadline_ns(ns));
}
/* the recursive mutex held once, its count 0 while the wait has let it go */
void os_sdk_cv_wait_recursive(CPU* c, u64 cv, u64 m) {
    if (!cs_mine(c, m) || snx_ld_le32(hook_host(c, m + 4)) != 1)
        misuse("SdkConditionVariableType::Wait without its recursive mutex held once");
    snx_st_le32(hook_host(c, m + 4), 0);
    cv_wait(c, cv, m, 0);
    snx_st_le32(hook_host(c, m + 4), 1);
}
