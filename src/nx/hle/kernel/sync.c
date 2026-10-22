/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/kernel/k_condition_variable.cpp, k_address_arbiter.cpp, k_synchronization_object.cpp,
   k_thread.cpp, svc/svc_lock.cpp, svc/svc_condition_variable.cpp, svc/svc_address_arbiter.cpp,
   svc/svc_synchronization.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project */
/* mutexes with priority inheritance, condition variables, address arbitration and WaitSynchronization */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/hle.h"

#define R_BADPOINTER 0xE601 /* InvalidPointer */

/* The threads blocked in each kind of wait, in arrival order, linked by next_waiter. Each wakes the best priority
   first, the earliest among equals; a thread whose priority changes while it waits goes to the back, behind the
   waiters of its new priority, as the console reinserts it in its ordered queue. Condition variable and address
   waiters are in a list per bucket of the word they wait on, so a signal looks only at the threads that may wait on its
   word: idle workers park there for good, and a list of them all made every signal walk every parked thread (dozens in
   one game), each a cache miss. Mutex waits are short, and a priority change looks for the waiters of an
   owner among all of them, so they stay in one list. The lists are g_hle.sync's (kernel_int.h). A mutex waiter
   holds a reference to its lock_owner while it names it, so the owner can't be freed under it if it exits holding the
   mutex */
static unsigned bucket(u64 word) { return (unsigned)(((word >> 2) * 0x9E3779B97F4A7C15ull) >> 56); }
/* the list a thread waiting in that kind on that word is in (its wait_key for a condition variable, else wait_addr) */
static u32* wait_list_for(int kind, u64 word) {
    switch (kind) {
        case 2: return &g_hle.sync.mwait;
        case 3: return &g_hle.sync.cvwait[bucket(word)];
        case 4: return &g_hle.sync.arbwait[bucket(word)];
        default: return NULL;
    }
}
static u32* wait_list(const KThread* t) {
    return wait_list_for(t->wait_kind, t->wait_kind == 3 ? t->wait_key : t->wait_addr);
}
static void list_add(u32* l, KThread* t) {
    while (*l) l = &kt_at(*l)->next_waiter;
    t->next_waiter = 0;
    *l = kt_ix(t);
}
static void list_del(u32* l, KThread* t) {
    u32 i = kt_ix(t);
    while (*l && *l != i) l = &kt_at(*l)->next_waiter;
    if (*l) *l = t->next_waiter;
    t->next_waiter = 0;
}
/* the waiter to wake first among those whose wait_addr (or wait_key, by_key) is key */
static KThread* list_best(u32 l, int by_key, u64 key) {
    KThread *b = NULL, *w;
    for (w = kt_at(l); w; w = kt_at(w->next_waiter))
        if ((by_key ? w->wait_key : w->wait_addr) == key && (!b || w->prio < b->prio)) b = w;
    return b;
}

/* The words the game and the kernel both change: guest threads run in parallel with this one and change them with
   exclusive accesses, which the recompiler makes compare-and-swaps, so the kernel changes them with atomics too. The
   console checks an address before it touches it: InvalidCurrentMemory outside the address space (its kernel address
   check, and the user access that would fail), InvalidAddress when it isn't word-aligned */
static u32 check_word(u64 addr) {
    R_UNLESS(addr <= AS_SIZE - 4, R_BADMEMSTATE);
    R_UNLESS(!(addr & 3), R_BADADDR);
    R_SUCCEED();
}
static u32* uword(u64 addr) { return (u32*)(void*)GP(addr); }
/* The words are little-endian, the guest's order: these take and give host numbers, and convert at the access (nothing
   on a little-endian host). A compare-and-swap's expected value is converted in and out, so a failed one gives the
   word's current value as a host number */
static u32 word_load(const u32* p) { return snx_le32(__atomic_load_n(p, __ATOMIC_SEQ_CST)); }
static void word_store(u32* p, u32 v) { __atomic_store_n(p, snx_le32(v), __ATOMIC_SEQ_CST); }
static int word_cas(u32* p, u32* cur, u32 nv, int weak) {
    u32 e = snx_le32(*cur);
    int ok = __atomic_compare_exchange_n(p, &e, snx_le32(nv), weak, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    *cur = snx_le32(e);
    return ok;
}

/* ============================================================ priority inheritance */
/* Eden's KThread::RestorePriority: t runs at the best of its base priority and the priorities of the threads waiting
   on the mutexes it holds, and a change goes on up to the owner of the mutex t waits on itself */
static void prio_restore_nl(KThread* t) {
    while (t) {
        int p = t->base_prio;
        KThread* w;
        u32* l;
        for (w = kt_at(g_hle.sync.mwait); w; w = kt_at(w->next_waiter))
            if (w->lock_owner == kt_ix(t) && w->prio < p) p = w->prio;
        if (p == t->prio) return;
        core_set_prio(t, p);
        l = wait_list(t);
        if (l) {
            list_del(l, t);
            list_add(l, t);
        }
        t = t->wait_kind == 2 ? kt_at(t->lock_owner) : NULL;
    }
}
void prio_set_base_nl(KThread* t, int prio) {
    t->base_prio = prio;
    prio_restore_nl(t);
}
/* t waits for owner to hand over the mutex at t->wait_addr */
static void mutex_wait_nl(KThread* owner, KThread* t) {
    t->wait_kind = 2;
    kobj_ref(&owner->o);
    t->lock_owner = kt_ix(owner);
    list_add(&g_hle.sync.mwait, t);
    if (t->prio < owner->prio) prio_restore_nl(owner);
}
/* t stops waiting on its mutex without getting it (its condition variable wait timed out) */
static void mutex_unwait_nl(KThread* t) {
    KThread* owner = kt_at(t->lock_owner);
    list_del(&g_hle.sync.mwait, t);
    t->lock_owner = 0;
    t->wait_kind = 0;
    if (owner) prio_restore_nl(owner);
    kobj_unref(owner ? &owner->o : NULL);
}
/* Eden's KThread::RemoveWaiterByKey: the next owner of the mutex at addr, out of its waiters, and the waiters left
   (*more) wait for it now, so their priorities go with them. NULL when nobody waits. The console looks among the
   calling thread's waiters, the mutex's owner in a game that uses it correctly: here the waiters name their owner */
static KThread* mutex_next_nl(u64 addr, int* more) {
    KThread* n = list_best(g_hle.sync.mwait, 0, addr);
    KThread* old;
    KThread* w;
    *more = 0;
    if (!n) return NULL;
    old = kt_at(n->lock_owner);
    list_del(&g_hle.sync.mwait, n);
    n->lock_owner = 0;
    n->wait_kind = 0;
    for (w = kt_at(g_hle.sync.mwait); w; w = kt_at(w->next_waiter))
        if (w->wait_addr == addr && w->lock_owner == kt_ix(old)) {
            kobj_ref(&n->o);
            w->lock_owner = kt_ix(n);
            kobj_unref(old ? &old->o : NULL); /* n's reference to old, dropped below, keeps it */
            *more = 1;
        }
    if (old) prio_restore_nl(old);
    prio_restore_nl(n);
    kobj_unref(old ? &old->o : NULL);
    return n;
}
/* the word that hands the mutex to n: its tag, with the waiters bit while others still wait */
static u32 mutex_tag(const KThread* n, int more) { return n ? n->wait_tag | (more ? HANDLE_WAIT : 0u) : 0u; }

/* ============================================================ mutexes */
/* svcArbitrateLock (Eden's KConditionVariable::WaitForAddress): the game found the mutex held, set the waiters bit, and
   asks to wait for owner. If the word changed meanwhile (it was unlocked) the game tries again itself */
u32 svc_arbitrate_lock(KThread* t, u32 owner, u64 addr, u32 tag) {
    KObj* o;
    u32 r;
    R_TRY(check_word(addr));
    pm_lock(&g_hle.kernel.klock);
    if (word_load(uword(addr)) != (owner | HANDLE_WAIT)) {
        pm_unlock(&g_hle.kernel.klock);
        R_SUCCEED();
    }
    o = h_get_nl(owner);
    if (!o || o->type != KO_THREAD) {
        pm_unlock(&g_hle.kernel.klock);
        R_THROW(R_BADHANDLE);
    }
    t->woken = 0;
    t->wait_addr = addr;
    t->wait_tag = tag;
    mutex_wait_nl((KThread*)o, t);
    sleep_nl(t, -1); /* until it is handed the mutex: nothing cancels this wait */
    r = (u32)t->wait_result;
    pm_unlock(&g_hle.kernel.klock);
    return r;
}
/* svcArbitrateUnlock (Eden's KConditionVariable::SignalToAddress): the owner unlocks a mutex with waiters, the best of
   which gets it */
u32 svc_arbitrate_unlock(u64 addr) {
    KThread* n;
    int more;
    R_TRY(check_word(addr));
    pm_lock(&g_hle.kernel.klock);
    n = mutex_next_nl(addr, &more);
    word_store(uword(addr), mutex_tag(n, more));
    if (n) wake(n, 0);
    pm_unlock(&g_hle.kernel.klock);
    R_SUCCEED();
}

/* ============================================================ condition variables */
/* Eden's KConditionVariable::SignalImpl: a signaled waiter takes its mutex if it is free, or waits for its owner */
static void cv_take_mutex_nl(KThread* w) {
    u32* p = uword(w->wait_addr);
    u32 old = word_load(p), nv;
    KObj* o;
    do
        nv = old ? old | HANDLE_WAIT : w->wait_tag;
    while (!word_cas(p, &old, nv, 1));
    if (!old) {
        wake(w, 0);
        return;
    }
    o = h_get_nl(old & ~HANDLE_WAIT);
    if (o && o->type == KO_THREAD)
        mutex_wait_nl((KThread*)o, w);
    else
        wake(w, R_BADSTATE); /* the mutex names no thread */
}
/* svcWaitProcessWideKeyAtomic (Eden's KConditionVariable::Wait): t unlocks the mutex at maddr, handing it to its best
   waiter, marks the condition variable as waited on, and waits for a signal and then for the mutex. The mark is set
   before the mutex changes hands, so the thread that takes the mutex sees it and signals */
u32 svc_wait_cv(KThread* t, u64 maddr, u64 key, u32 tag, i64 timeout) {
    KThread* n;
    int more;
    u32 r;
    R_TRY(check_word(maddr));
    key &= ~3ull;
    R_UNLESS(key <= AS_SIZE - 4, R_BADMEMSTATE);
    pm_lock(&g_hle.kernel.klock);
    n = mutex_next_nl(maddr, &more);
    word_store(uword(key), 1u);
    word_store(uword(maddr), mutex_tag(n, more));
    if (n) wake(n, 0);
    if (!timeout) {
        pm_unlock(&g_hle.kernel.klock);
        R_THROW(R_TIMEOUT);
    }
    t->woken = 0;
    t->wait_kind = 3;
    t->wait_addr = maddr;
    t->wait_key = key;
    t->wait_tag = tag;
    list_add(wait_list(t), t);
    if (sleep_nl(t, timeout))
        r = (u32)t->wait_result;
    else { /* timed out waiting for the signal, or after it for the mutex: the game takes the mutex itself then */
        if (t->wait_kind == 3) {
            list_del(wait_list(t), t);
            t->wait_kind = 0;
        } else if (t->wait_kind == 2)
            mutex_unwait_nl(t);
        r = R_TIMEOUT;
    }
    pm_unlock(&g_hle.kernel.klock);
    return r;
}
/* svcSignalProcessWideKey (Eden's KConditionVariable::Signal): wakes count waiters (all of them when count <= 0), and
   clears the mark once nobody waits */
u32 svc_signal_cv(u64 key, i32 count) {
    KThread* w;
    u32* l;
    i32 n = 0;
    key &= ~3ull;
    l = wait_list_for(3, key);
    pm_lock(&g_hle.kernel.klock);
    while ((count <= 0 || n < count) && (w = list_best(*l, 1, key)) != NULL) {
        list_del(l, w);
        w->wait_kind = 0;
        cv_take_mutex_nl(w);
        n++;
    }
    if (!list_best(*l, 1, key) && key <= AS_SIZE - 4) word_store(uword(key), 0u);
    pm_unlock(&g_hle.kernel.klock);
    R_SUCCEED();
}

/* ============================================================ address arbitration */
/* svcWaitForAddress (Eden's KAddressArbiter::WaitIfLessThan and WaitIfEqual). type 0: wait if the word is less than
   value; 1: the same, decrementing it first; 2: wait if it equals value */
u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i32 value, i64 timeout) {
    u32* p;
    u32 cur;
    u32 r;
    R_TRY(check_word(addr));
    R_UNLESS(type <= 2, R_BADENUM);
    p = uword(addr);
    pm_lock(&g_hle.kernel.klock);
    cur = word_load(p);
    if (type == 1)
        while ((i32)cur < value && !word_cas(p, &cur, cur - 1u, 1)) {}
    if (type == 2 ? (i32)cur != value : (i32)cur >= value) {
        pm_unlock(&g_hle.kernel.klock);
        R_THROW(R_BADSTATE);
    }
    if (!timeout) {
        pm_unlock(&g_hle.kernel.klock);
        R_THROW(R_TIMEOUT);
    }
    t->woken = 0;
    t->wait_kind = 4;
    t->wait_addr = addr;
    list_add(wait_list(t), t);
    if (sleep_nl(t, timeout))
        r = (u32)t->wait_result;
    else {
        if (t->wait_kind == 4) {
            list_del(wait_list(t), t);
            t->wait_kind = 0;
        }
        r = R_TIMEOUT;
    }
    pm_unlock(&g_hle.kernel.klock);
    return r;
}
/* svcSignalToAddress (Eden's KAddressArbiter::Signal, SignalAndIncrementIfEqual and
   SignalAndModifyByWaitingCountIfEqual). type 0: wake count waiters (all when count <= 0); 1: the same if the word
   equals value, which it increments; 2: the same if the word equals value, which it changes by whether waiters remain:
   value + 1 with none, value - 1 when all of them wake (value - 2 with count <= 0), value when some stay */
u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count) {
    KThread* w;
    u32* l;
    i32 n = 0;
    u32* p;
    R_TRY(check_word(addr));
    R_UNLESS(type <= 2, R_BADENUM);
    p = uword(addr);
    l = wait_list_for(4, addr);
    pm_lock(&g_hle.kernel.klock);
    if (type) {
        u32 cur = (u32)value, nv = (u32)value + 1u;
        if (type == 2) {
            i32 waiting = 0;
            for (w = kt_at(*l); w; w = kt_at(w->next_waiter))
                if (w->wait_addr == addr) waiting++;
            if (!waiting)
                nv = (u32)value + 1u;
            else if (count <= 0)
                nv = (u32)value - 2u;
            else
                nv = waiting <= count ? (u32)value - 1u : (u32)value;
        }
        if (nv != (u32)value)
            word_cas(p, &cur, nv, 0);
        else
            cur = word_load(p);
        if (cur != (u32)value) {
            pm_unlock(&g_hle.kernel.klock);
            R_THROW(R_BADSTATE);
        }
    }
    while ((count <= 0 || n < count) && (w = list_best(*l, 0, addr)) != NULL) {
        list_del(l, w);
        w->wait_kind = 0;
        wake(w, 0);
        n++;
    }
    pm_unlock(&g_hle.kernel.klock);
    R_SUCCEED();
}

/* ============================================================ WaitSynchronization */
/* svcWaitSynchronization (Eden's svc WaitSynchronization and KSynchronizationObject::Wait): up to 64 handles, none
   being a plain sleep. The lowest index already signaled returns at once; otherwise a timeout of 0 times out, a
   CancelSynchronization that came before the wait cancels it, and the thread waits on every object for the first to be
   signaled, whose index it returns (-1 when the wait timed out or was cancelled). Only synchronization objects (the
   ones with is_signaled) can be waited on, and pseudo-handles can't. Each object keeps a reference for the wait, so
   closing its handle meanwhile doesn't free it under the waiter; the references go after g_hle.kernel.klock is let go, as the
   last one destroys the object */
u32 svc_wait_sync(KThread* t, u64 hptr, u32 n, i64 timeout, u32* out_idx) {
    u32 hs[64];
    KObj* objs[64];
    u32 i, r = 0;
    R_UNLESS(n <= 64, R_OUTOFRANGE);
    if (n && (hptr > AS_SIZE || AS_SIZE - hptr < 4ull * n)) R_THROW(R_BADPOINTER);
    if (n) memcpy(hs, GP(hptr), 4 * (size_t)n);
    snx_le32_array(hs, n);
    pm_lock(&g_hle.kernel.klock);
    for (i = 0; i < n; i++) {
        objs[i] = h_get_nl(hs[i]);
        if (!objs[i] || !kobj_waitable(objs[i])) {
            u32 k = g_hle.sync.bad_waits++, bad = hs[i];
            pm_unlock(&g_hle.kernel.klock);
            while (i--) kobj_unref(objs[i]);
            if (k < 20 || !(k & 0xFFFF))
                WARN("kernel: WaitSynchronization on %x, which names nothing that can be waited on: InvalidHandle "
                     "(%u times)\n",
                     bad, k + 1);
            R_THROW(R_BADHANDLE);
        }
        kobj_ref(objs[i]);
    }
    for (i = 0; i < n; i++)
        if (kobj_signaled(objs[i])) break;
    if (i < n)
        *out_idx = i;
    else if (!timeout)
        r = R_TIMEOUT;
    else if (t->cancel) {
        t->cancel = 0;
        r = R_CANCELLED;
    } else {
        int woke;
        t->woken = 0;
        t->wait_kind = 1;
        t->wait_key = 0; /* kobj_signal_nl names the object here */
        snprintf(t->wait_name, sizeof t->wait_name, "%s",
                 n == 1 ? objs[0]->name
                 : n    ? "several objects"
                        : "no handles");
        t->wait_tid = n && objs[0]->type == KO_THREAD ? ((KThread*)objs[0])->id : 0; /* a join */
        for (i = 0; i < n; i++) waiter_add(objs[i], t);
        woke = sleep_nl(t, timeout);
        for (i = 0; i < n; i++) waiter_del(objs[i], t);
        t->wait_kind = 0;
        t->wait_tid = 0;
        if (!woke)
            r = R_TIMEOUT;
        else {
            r = (u32)t->wait_result;
            if (r == R_CANCELLED) t->cancel = 0; /* CancelSynchronization latches it too, for a thread not waiting */
        }
        i = n;
        if (R_SUCCEEDED(r)) {
            for (i = 0; i < n && kobj_id(objs[i]) != t->wait_key; i++) {}
            if (i == n) /* woken without being told by which object */
                for (i = 0; i < n && !kobj_signaled(objs[i]); i++) {}
        }
        *out_idx = i < n ? i : ~0u;
    }
    pm_unlock(&g_hle.kernel.klock);
    for (i = 0; i < n; i++) kobj_unref(objs[i]);
    return r;
}
