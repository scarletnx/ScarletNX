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
#include "nx/nn/hook.h"
#include "nn/hooks.hook.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel_int.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include <stdatomic.h>
#include <string.h>

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

static _Atomic u32* word(CPU* c, u64 a) { return (_Atomic u32*)(void*)hook_host(c, a); }
static u32 ld(const _Atomic u32* p) { return snx_le32(atomic_load_explicit(p, memory_order_acquire)); }
static int cas(_Atomic u32* p, u32* cur, u32 nv) {
    u32 e = snx_le32(*cur);
    int ok = atomic_compare_exchange_strong_explicit(p, &e, snx_le32(nv), memory_order_acq_rel, memory_order_acquire);
    *cur = snx_le32(e);
    return ok;
}
/* the guest word plus d, the new value */
static u32 add(_Atomic u32* p, u32 d) {
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
#if defined(SNX_TRACY)
    u64 waited; /* only the sleep-time plot reads the clock */
#endif
    (void)c;
    if (ns == 0 || ns == -1 || ns == -2) {
        yield();
        return;
    }
    if (ns < 0) {
        SNX_ZONE_T(z, "hook os sleep skip", "negative"); /* rare decision */
        return;
    }
#if defined(SNX_TRACY)
    waited = plat_ns();
#endif
    hook_wait_begin();
    plat_sleep_ns(ns);
    hook_wait_end();
#if defined(SNX_TRACY)
    SNX_PLOT("hook os sleep ms", (double)(plat_ns() - waited) / 1e6);
#endif
}
void os_yield_thread(CPU* c) {
    (void)c;
    yield();
}
void os_yield_impl(CPU* c, u64 self) {
    (void)c, (void)self;
    yield();
}

/* ---- the core the calling thread is scheduled on, as svcGetCurrentProcessorNumber answers (os_core) */
i32 os_core_number(CPU* c) {
    KThread* t = kthread_cur();
    (void)c;
    return t ? t->core : 0;
}
i32 os_core_number_impl(CPU* c, u64 self) {
    (void)self;
    return os_core_number(c);
}

/* ---- the threads waiting on a lock's word, oldest first, which a Leave hands the lock to as the console's kernel
   hands a mutex to a waiter (svcArbitrateUnlock). Freed instead, the lock went back at once to the thread that let it
   go: one game's movie library holds its mutex while it handles an event and lets go only while it waits, and took it
   again each time before the game's thread waiting on it in a call ran, so that thread saw two of the library's
   events at once and took them in the wrong order. A lock is in one of the table's slots while threads wait on it; one
   that finds no room waits as before, with no place in the order */
#define HAND_SLOTS   512
#define HAND_PROBE   16
#define HAND_WAITERS 16
static struct {
    u64 addr;
    u32 n, tag[HAND_WAITERS];
} g_hand[HAND_SLOTS];
static PMutex g_hand_lk = PMUTEX_INIT;
static u32 hand_slot(u64 addr, u32 k) { return (u32)(((addr >> 2) * 0x9E3779B1u + k) % HAND_SLOTS); }
/* tag queued on the lock at addr: 1, or 0 when there was no room */
static int hand_add(u64 addr, u32 tag) {
    int done = 0;
    pm_lock(&g_hand_lk);
    for (u32 k = 0, free_slot = HAND_SLOTS; k < HAND_PROBE && !done; k++) {
        u32 i = hand_slot(addr, k);
        if (g_hand[i].n && g_hand[i].addr == addr) {
            if (g_hand[i].n < HAND_WAITERS) g_hand[i].tag[g_hand[i].n++] = tag, done = 1;
            break;
        }
        if (!g_hand[i].n && free_slot == HAND_SLOTS) free_slot = i;
        if (k == HAND_PROBE - 1 && free_slot < HAND_SLOTS) {
            g_hand[free_slot].addr = addr;
            g_hand[free_slot].tag[0] = tag;
            g_hand[free_slot].n = 1;
            done = 1;
        }
    }
    pm_unlock(&g_hand_lk);
    return done;
}
static void hand_del(u64 addr, u32 tag) {
    pm_lock(&g_hand_lk);
    for (u32 k = 0; k < HAND_PROBE; k++) {
        u32 i = hand_slot(addr, k);
        if (!g_hand[i].n || g_hand[i].addr != addr) continue;
        for (u32 j = 0; j < g_hand[i].n; j++)
            if (g_hand[i].tag[j] == tag) {
                memmove(&g_hand[i].tag[j], &g_hand[i].tag[j + 1], sizeof(u32) * (g_hand[i].n - j - 1));
                g_hand[i].n--;
                break;
            }
        break;
    }
    pm_unlock(&g_hand_lk);
}
/* the oldest thread waiting on the lock's word, 0 for none */
static u32 hand_first(u64 addr) {
    u32 tag = 0;
    pm_lock(&g_hand_lk);
    for (u32 k = 0; k < HAND_PROBE && !tag; k++) {
        u32 i = hand_slot(addr, k);
        if (g_hand[i].n && g_hand[i].addr == addr) tag = g_hand[i].tag[0];
    }
    pm_unlock(&g_hand_lk);
    return tag;
}

/* ---- InternalCriticalSection: the word at cs */
static void cs_enter(CPU* c, u64 cs) {
    _Atomic u32* p = word(c, cs);
    u32 tag = me(), v = 0;
    int waited = 0, queued = 0;
#if defined(SNX_TRACY)
    u64 t0 = 0; /* only the contention plot reads the clock */
#endif
    if (cas(p, &v, tag)) return;
    if ((v & ~HANDLE_WAIT) == tag) return; /* the SDK's Enter takes a lock its thread holds as held */
    for (;;) {
        if (!v) { /* free: taken with the waiters bit, as others may still wait */
            if (cas(p, &v, tag | HANDLE_WAIT)) break;
            continue;
        }
        if ((v & ~HANDLE_WAIT) == tag) break; /* handed to this thread by a Leave (cs_leave) */
        if (!(v & HANDLE_WAIT) && !cas(p, &v, v | HANDLE_WAIT)) continue;
        if (!queued) queued = hand_add(cs, tag) ? 1 : -1;
        if (!waited) {
            hook_wait_begin();
            waited = 1;
#if defined(SNX_TRACY)
            t0 = plat_ns();
#endif
        }
        plat_wait_word(p, snx_le32(v | HANDLE_WAIT), -1);
        v = ld(p);
    }
    if (queued > 0) hand_del(cs, tag);
    if (waited) {
        hook_wait_end();
        /* the wait left the core, so it is wall time, never a zone: the plot says how long contention blocked the
           guest thread, measured after it runs again */
#if defined(SNX_TRACY)
        SNX_PLOT("hook os cs contention ms", (double)(plat_ns() - t0) / 1e6);
#endif
    }
}
static int cs_try_enter(CPU* c, u64 cs) {
    u32 v = 0;
    return cas(word(c, cs), &v, me());
}
/* as the SDK's Leave: free at once unless someone waits, else the kernel's svcArbitrateUnlock, here with no SVC, hands
   it to the best thread waiting there, else to the oldest waiting on the word, else frees it (g_lock_futex) */
static void cs_leave(CPU* c, u64 cs) {
    u32 v = me();
    int handed;
    if (!cas(word(c, cs), &v, 0)) svc_arbitrate_unlock_to(cs, hand_first(cs), &handed);
}
static int cs_mine(CPU* c, u64 cs) { return (ld(word(c, cs)) & ~HANDLE_WAIT) == me(); }

void os_cs_init(CPU* c, u64 cs) { atomic_store_explicit(word(c, cs), 0u, memory_order_release); }
void os_cs_enter(CPU* c, u64 cs) { cs_enter(c, cs); }
int os_cs_try_enter(CPU* c, u64 cs) { return cs_try_enter(c, cs); }
void os_cs_leave(CPU* c, u64 cs) { cs_leave(c, cs); }
int os_cs_mine(CPU* c, u64 cs) { return cs_mine(c, cs); }

/* ---- InternalConditionVariable: the word at cv, waiters in its low 16 bits, signals in its high 16 */
#define CV_WAITERS 0xFFFFu
#define CV_SIGNAL  0x10000u
static void cv_wake(CPU* c, u64 cv, int all) {
    _Atomic u32* p = word(c, cv);
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
    _Atomic u32* p = word(c, cv);
    u32 v = add(p, 1u), now;
    int got = 0;
#if defined(SNX_TRACY)
    u64 t0; /* only the wait-time plot reads the clock */
#endif
    cs_leave(c, cs);
#if defined(SNX_TRACY)
    t0 = plat_ns();
#endif
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
    /* measured after the thread runs again: a zone here would time the core it had left, not CPU work */
#if defined(SNX_TRACY)
    SNX_PLOT("hook os cv wait ms", (double)(plat_ns() - t0) / 1e6);
#endif
    if (!got) SNX_MSG("hook os cv timeout", sizeof("hook os cv timeout") - 1); /* rare outcome */
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

void os_cv_init(CPU* c, u64 cv) { atomic_store_explicit(word(c, cv), 0u, memory_order_release); }
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
    if (left < 1) {
        SNX_ZONE_T(z, "hook os cv no wait", "expired"); /* rare decision: no blocking at all */
        return 0;
    }
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
    atomic_store_explicit(word(c, m), 0u, memory_order_release);
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
    if (ns < 0) {
        SNX_ZONE_T(z, "hook os cv no wait", "negative span"); /* rare decision */
        return 0;
    }
    if (!ns) {
        SNX_ZONE_T(z, "hook os cv no wait", "zero span"); /* rare decision: the lock is let go and taken again */
        cs_leave(c, m);
        SNX_ZONE_END(z); /* the re-lock can block; it is not the no-wait decision */
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

/* ---- the light objects: LightEvent, LightSemaphore and LightMessageQueue, whose threads wait on a word of three
   states (the SDK's LightWaiter: 0 nobody waits, 1 threads wait, 2 signaled) with svcWaitForAddress and
   svcSignalToAddress. Their functions run here whole, the address arbiter's waits and signals the kernel's own
   (svc_wait_addr, svc_signal_addr) with no SVC, so a light object's words follow the SDK's protocol and a function of
   theirs the family leaves to the game's own code still works with them. A light object's spinlock is taken as the
   SDK takes it, less its preemption counter in TLS, which only the console's scheduler reads */
#define LW_SIGNALED 2u
#define RC_DESC(rc) ((rc) & 0x3FFFFFu)
static void lw_check(u32 rc, const char* what) {
    if (rc) fatal("nn::os: %s: the address arbiter returned %#x (the SDK aborts here)", what, rc);
}
static int lw_retry(u32 rc) { return RC_DESC(rc) == RC_DESC(SNX_RC_SVC_INVALID_STATE); }
/* the word set to signaled: every waiter woken (all) or one, by SignalAndIncrementIfEqual or
   SignalAndModifyByWaitingCountIfEqual, as the SDK's manual and automatic clears signal */
static void lw_signal(CPU* c, u64 a, int all) {
    _Atomic u32* p = word(c, a);
    for (;;) {
        u32 v = ld(p);
        if (v == LW_SIGNALED) return;
        if (v == 0) {
            if (cas(p, &v, LW_SIGNALED)) return;
            continue;
        }
        if (v != 1) misuse("a light object's waiter word holds neither 0, 1 nor 2");
        const u32 rc = svc_signal_addr(a, all ? 1u : 2u, 1, all ? -1 : 1);
        if (!lw_retry(rc)) return lw_check(rc, "a light object's signal");
    }
}
/* a wait on the word while nobody signaled it: 1 when woken, 2 when the word changed before the wait began (the
   arbiter's InvalidState), 0 when it was signaled already */
static int lw_wait(CPU* c, u64 a) {
    _Atomic u32* p = word(c, a);
    for (;;) {
        u32 v = ld(p);
        if (v == LW_SIGNALED) return 0;
        if (v == 0 && !cas(p, &v, 1u)) continue;
        if (v != 0 && v != 1) misuse("a light object's waiter word holds neither 0, 1 nor 2");
        KThread* t = kthread_cur();
        kthread_block(t); /* outside an SVC: a pause asked during the wait settles, and stops the thread after it */
        const u32 rc = svc_wait_addr(t, a, 2, 1, -1);
        kthread_unblock(t);
        if (lw_retry(rc)) return 2;
        lw_check(rc, "a light object's wait");
        return 1;
    }
}
static void lw_clear(CPU* c, u64 a) {
    u32 v = LW_SIGNALED;
    cas(word(c, a), &v, 0);
}
/* a light object's spinlock word */
static void spin_lock(CPU* c, u64 a) {
    _Atomic u32* p = word(c, a);
    for (u32 n = 0;; n++) {
        u32 v = 0;
        if (cas(p, &v, 1u)) return;
        if (n >= 64) plat_yield();
    }
}
static void spin_unlock(CPU* c, u64 a) { atomic_store_explicit(word(c, a), 0u, memory_order_release); }

/* LightEventType (hooks.hook gives its layout per SDK): whether its clear is automatic, that it is initialized, and the
   waiter word. Its functions keep to the layout of the SDK that built the game, as the SDK's own TimedWait, which the
   family leaves to the game's code, reads it: an SDK before 13 found 0x101 where its waiter word is and aborted */
typedef const HookLayout_nn_os_LightEventType LightEventLayout;
static void light_event_init(CPU* c, LightEventLayout* L, u64 e, int signaled, u32 mode) {
    hook_put_bool(c, L->auto_clear, e, mode == 1);
    hook_put_bool(c, L->initialized, e, 1);
    atomic_store_explicit(word(c, e + L->waiter.offset), snx_le32(signaled ? LW_SIGNALED : 0u), memory_order_release);
}
void os_light_event_init(CPU* c, LightEventLayout* L, u64 e, int signaled, u32 mode) {
    light_event_init(c, L, e, signaled, mode);
}
/* the SDK's before 13 clears all of it first, the word after the flags too */
void os_light_event_init_old(CPU* c, LightEventLayout* L, u64 e, int signaled, u32 mode) {
    memset(hook_host(c, e), 0, L->size);
    light_event_init(c, L, e, signaled, mode);
}
void os_light_event_signal(CPU* c, LightEventLayout* L, u64 e) {
    lw_signal(c, e + L->waiter.offset, !hook_get_bool(c, L->auto_clear, e));
}
void os_light_event_clear(CPU* c, LightEventLayout* L, u64 e) { lw_clear(c, e + L->waiter.offset); }
/* an automatic one's signal is taken: the wake of one waiter, or the word's signaled state cleared */
void os_light_event_wait(CPU* c, LightEventLayout* L, u64 e) {
    const u64 a = e + L->waiter.offset;
    _Atomic u32* p = word(c, a);
    if (!hook_get_bool(c, L->auto_clear, e)) { /* manual: a wake, or the word found changed, ends it */
        lw_wait(c, a);
        return;
    }
    for (;;) {
        u32 v = LW_SIGNALED;
        if (cas(p, &v, 0)) return;
        /* woken: SignalAndModifyByWaitingCountIfEqual handed the signal to this thread */
        if (lw_wait(c, a) == 1) return;
    }
}
int os_light_event_try_wait(CPU* c, LightEventLayout* L, u64 e) {
    u32 v = LW_SIGNALED;
    _Atomic u32* p = word(c, e + L->waiter.offset);
    if (hook_get_bool(c, L->auto_clear, e)) return cas(p, &v, 0);
    return ld(p) == LW_SIGNALED;
}

/* LightSemaphoreType: its count at 4, its spinlock at 0x10, the waiter word at 0x18 */
/* one taken if the count has one; the waiter word's signal cleared where it hasn't, but by TryAcquire */
static int sem_take(CPU* c, u64 s, int clear) {
    spin_lock(c, s + 0x10);
    const i32 n = (i32)snx_ld_le32(hook_host(c, s + 4));
    if (n > 0)
        snx_st_le32(hook_host(c, s + 4), (u32)(n - 1));
    else if (clear)
        lw_clear(c, s + 0x18);
    spin_unlock(c, s + 0x10);
    return n > 0;
}
void os_light_sem_acquire(CPU* c, u64 s) {
    while (!sem_take(c, s, 1)) lw_wait(c, s + 0x18);
}
int os_light_sem_try_acquire(CPU* c, u64 s) { return sem_take(c, s, 0); }
void os_light_sem_release_n(CPU* c, u64 s, i32 k) {
    spin_lock(c, s + 0x10);
    snx_st_le32(hook_host(c, s + 4), snx_ld_le32(hook_host(c, s + 4)) + (u32)k);
    spin_unlock(c, s + 0x10);
    lw_signal(c, s + 0x18, 1);
}
void os_light_sem_release(CPU* c, u64 s) { os_light_sem_release_n(c, s, 1); }

/* LightMessageQueueType: its buffer of 64-bit messages at 0, their room at 8, how many it holds at 0xc, the first's
   index at 0x10, its spinlock at 0x18, the waiter words of room (0x20, senders wait) and of messages (0x28) */
static int lmq_take(CPU* c, u64 out, u64 q) {
    spin_lock(c, q + 0x18);
    const u32 n = snx_ld_le32(hook_host(c, q + 0xc));
    if (n) {
        const u32 cap = snx_ld_le32(hook_host(c, q + 8));
        u32 head = snx_ld_le32(hook_host(c, q + 0x10));
        const u64 msg = snx_ld_le64(hook_host(c, snx_ld_le64(hook_host(c, q)) + (u64)head * 8));
        head = head + 1 >= cap ? head + 1 - cap : head + 1;
        snx_st_le32(hook_host(c, q + 0xc), n - 1);
        snx_st_le32(hook_host(c, q + 0x10), head);
        snx_st_le64(hook_host(c, out), msg);
    } else
        lw_clear(c, q + 0x28);
    spin_unlock(c, q + 0x18);
    if (n) lw_signal(c, q + 0x20, 1);
    return n != 0;
}
int os_lmq_try_receive(CPU* c, u64 out, u64 q) { return lmq_take(c, out, q); }
void os_lmq_receive(CPU* c, u64 out, u64 q) {
    while (!lmq_take(c, out, q)) lw_wait(c, q + 0x28);
}
int os_lmq_try_send(CPU* c, u64 q, u64 msg) {
    spin_lock(c, q + 0x18);
    const u32 cap = snx_ld_le32(hook_host(c, q + 8)), n = snx_ld_le32(hook_host(c, q + 0xc));
    if ((i32)n < (i32)cap) {
        u32 at = snx_ld_le32(hook_host(c, q + 0x10)) + n;
        if (at >= cap) at -= cap;
        snx_st_le64(hook_host(c, snx_ld_le64(hook_host(c, q)) + (u64)at * 8), msg);
        snx_st_le32(hook_host(c, q + 0xc), n + 1);
    }
    spin_unlock(c, q + 0x18);
    if ((i32)n >= (i32)cap) return 0;
    lw_signal(c, q + 0x28, 1);
    return 1;
}
