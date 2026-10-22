/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/kernel/k_handle_table.h, k_handle_table.cpp, k_synchronization_object.cpp,
   k_readable_event.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project */
/* kernel objects and their references, the handle table, waiting on objects and waking, events */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/hle.h"
#include "nx/runtime/clock.h"

/* ============================================================ references */
void kobj_ref(KObj* o) {
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}
/* the last reference destroys the object. Never called with g_klock held where a destroy may take it (a session's
   service objects do) */
void kobj_unref(KObj* o) {
    if (o && __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0) kobj_destroy(o);
}
static void kevent_destroy(KObj* o) {
    pm_lock(&g_host.kernel.pool_lk);
    POOL_PUT((KEvent*)o);
    pm_unlock(&g_host.kernel.pool_lk);
}
/* the process and the resource limit are never destroyed */
void kobj_destroy(KObj* o) {
    switch (o->type) {
        case KO_EVENT: kevent_destroy(o); break;
        case KO_THREAD: kthread_destroy(o); break;
        case KO_SESSION: ksession_destroy(o); break;
        case KO_SHMEM: kshmem_destroy(o); break;
        case KO_TMEM: tmem_destroy(o); break;
        case KO_CODEMEM: kcodemem_destroy(o); break;
        case KO_WEVENT: kwevent_destroy(o); break;
        default: break;
    }
}
int kobj_waitable(const KObj* o) { return o->type == KO_EVENT || o->type == KO_THREAD || o->type == KO_PROCESS; }
int kobj_signaled(const KObj* o) {
    switch (o->type) {
        case KO_EVENT: return ((const KEvent*)o)->signaled;
        case KO_THREAD: return ((const KThread*)o)->state == 2;
        default: return 0;
    }
}

/* ============================================================ objects as numbers */
/* the type in the top byte, the slot below */
#define KOBJ_ID(type, slot) ((u32)(type) << 24 | (u32)(slot))
u32 kobj_id(const KObj* o) {
    if (!o) return 0;
    switch (o->type) {
        case KO_EVENT: return KOBJ_ID(KO_EVENT, POOL_INDEX(g_hle.kernel.events, (const KEvent*)o));
        case KO_THREAD: return KOBJ_ID(KO_THREAD, POOL_INDEX(g_hle.thread.threads, (const KThread*)o));
        case KO_SESSION: return KOBJ_ID(KO_SESSION, POOL_INDEX(g_hle.ipc.sessions, (const KSession*)o));
        case KO_SHMEM: return KOBJ_ID(KO_SHMEM, POOL_INDEX(g_hle.svcmem.shmems, (const KShmem*)o));
        case KO_TMEM: return KOBJ_ID(KO_TMEM, POOL_INDEX(g_hle.svcmem.tmems, (const KTmem*)o));
        case KO_CODEMEM: return KOBJ_ID(KO_CODEMEM, POOL_INDEX(g_hle.svcmem.codemems, (const KCodeMem*)o));
        case KO_WEVENT: return KOBJ_ID(KO_WEVENT, POOL_INDEX(g_hle.svc.wevents, (const KWEvent*)o));
        default: return KOBJ_ID(o->type, 0); /* the process and the resource limit, one of each */
    }
}
KObj* kobj_at(u32 id) {
    u32 slot = id & 0xFFFFFF;
    switch (id >> 24) {
        case KO_EVENT: return &g_hle.kernel.events[slot].o;
        case KO_THREAD: return &g_hle.thread.threads[slot].o;
        case KO_SESSION: return &g_hle.ipc.sessions[slot].o;
        case KO_SHMEM: return &g_hle.svcmem.shmems[slot].o;
        case KO_TMEM: return &g_hle.svcmem.tmems[slot].o;
        case KO_CODEMEM: return &g_hle.svcmem.codemems[slot].o;
        case KO_WEVENT: return &g_hle.svc.wevents[slot].o;
        case KO_PROCESS: return &g_hle.kernel.process;
        case KO_RESLIMIT: return &g_hle.svc.reslimit;
        default: return NULL;
    }
}

/* ============================================================ the handle table */
/* g_hle.kernel.htab, whose comment says how a handle is made: 1024 entries, the most the console's table holds */

u32 h_new(KObj* o) {
    int i;
    u16 lin;
    if (!o) return 0;
    pm_lock(&g_host.kernel.klock);
    if (g_hle.kernel.hfree >= 0) {
        i = g_hle.kernel.hfree;
        g_hle.kernel.hfree = g_hle.kernel.htab[i].next;
    } else if (g_hle.kernel.htop < H_MAX)
        i = g_hle.kernel.htop++;
    else {
        pm_unlock(&g_host.kernel.klock);
        WARN_LIMIT(4, "kernel: the handle table is full (%d handles), %s gets none\n", H_MAX, o->name);
        return 0;
    }
    lin = g_hle.kernel.hlinear;
    g_hle.kernel.hlinear = lin == 0x7FFF ? 1 : (u16)(lin + 1);
    g_hle.kernel.htab[i].obj = kobj_id(o);
    g_hle.kernel.htab[i].linear = lin;
    kobj_ref(o);
    pm_unlock(&g_host.kernel.klock);
    return ((u32)lin << 15) | (u32)i;
}
/* the table's entry for h, or -1. Pseudo-handles have the reserved bits set, so they are never in it */
static int h_index_nl(u32 h) {
    u32 i = h & 0x7FFF, lin = (h >> 15) & 0x7FFF;
    if ((h >> 30) || !lin || i >= (u32)g_hle.kernel.htop || !g_hle.kernel.htab[i].obj ||
        g_hle.kernel.htab[i].linear != lin)
        return -1;
    return (int)i;
}
KObj* h_get_nl(u32 h) {
    int i = h_index_nl(h);
    return i < 0 ? NULL : kobj_at(g_hle.kernel.htab[i].obj);
}
static KObj* h_resolve_nl(u32 h, int t) {
    KObj* o;
    if (h == H_CUR_THREAD) {
        KThread* cur = kthread_cur();
        o = cur ? &cur->o : NULL;
    } else if (h == H_CUR_PROC)
        o = &g_hle.kernel.process;
    else
        o = h_get_nl(h);
    return o && (!t || o->type == t) ? o : NULL;
}
KObj* h_get_type(u32 h, int t) {
    KObj* o;
    pm_lock(&g_host.kernel.klock);
    o = h_resolve_nl(h, t);
    pm_unlock(&g_host.kernel.klock);
    return o;
}
KObj* h_get_ref(u32 h, int t) {
    KObj* o;
    pm_lock(&g_host.kernel.klock);
    o = h_resolve_nl(h, t);
    kobj_ref(o);
    pm_unlock(&g_host.kernel.klock);
    return o;
}
/* the table's reference goes after the lock is dropped: closing the last handle to a session destroys its service */
int h_close(u32 h) {
    KObj* o;
    int i;
    pm_lock(&g_host.kernel.klock);
    i = h_index_nl(h);
    if (i < 0) {
        pm_unlock(&g_host.kernel.klock);
        R_THROW(R_BADHANDLE);
    }
    o = kobj_at(g_hle.kernel.htab[i].obj);
    g_hle.kernel.htab[i].obj = 0;
    g_hle.kernel.htab[i].next = (i16)g_hle.kernel.hfree;
    g_hle.kernel.hfree = i;
    pm_unlock(&g_host.kernel.klock);
    kobj_unref(o);
    R_SUCCEED();
}

/* ============================================================ waiting and waking */
/* an object's waiters: the threads in WaitSynchronization on it (wait_kind 1) and those waiting for a thread to pause
   (5, svc.c), each naming it in its waits. A thread waiting on the same handle twice names it twice, and leaves it
   twice */
void waiter_add(KObj* o, KThread* t) {
    if (t->nwaits == (int)(sizeof t->waits / sizeof *t->waits)) fatal("A thread waits on more than 64 objects");
    t->waits[t->nwaits++] = kobj_id(o);
    o->nwait++;
}
void waiter_del(KObj* o, KThread* t) {
    u32 id = kobj_id(o);
    int i;
    for (i = 0; i < t->nwaits; i++)
        if (t->waits[i] == id) {
            memmove(t->waits + i, t->waits + i + 1, sizeof *t->waits * (size_t)(t->nwaits - i - 1));
            t->nwaits--;
            o->nwait--;
            return;
        }
}
/* the waiters are found among the threads: few objects have any, and those with one have a handful */
static void wake_waiters_nl(KObj* o, int kind) {
    u32 id;
    int i, k;
    if (!o->nwait) return;
    id = kobj_id(o);
    for (i = 0; i < KTHREAD_MAX; i++) {
        KThread* w = &g_hle.thread.threads[i];
        if (w->o.type != KO_THREAD || !w->nwaits ||
            (kind ? w->wait_kind != kind : w->wait_kind != 1 && w->wait_kind != 5))
            continue;
        for (k = 0; k < w->nwaits && w->waits[k] != id; k++) {}
        if (k == w->nwaits) continue;
        if (w->wait_kind == 1 && !w->woken) w->wait_key = id;
        wake(w, 0);
    }
}
void kobj_wake_nl(KObj* o, int kind) { wake_waiters_nl(o, kind); }
/* ends t's wait with result. Only the first wake of a wait counts: a thread signaled by an object and then cancelled
   before it runs again returns the signal, as the console's NotifyAvailable makes the thread no longer cancellable */
void wake(KThread* t, int result) {
    if (t->woken) return;
    t->woken = 1;
    t->wait_result = result;
    pc_broadcast(kt_cv(t));
}
/* wakes the threads waiting on o, if it is signaled. A WaitSynchronization waiter is told which object it was (in
   wait_key, which its wait doesn't use otherwise): by the time it runs, another thread may have cleared the event */
void kobj_signal_nl(KObj* o) {
    if (kobj_waitable(o) && !kobj_signaled(o)) return;
    wake_waiters_nl(o, 0);
}
/* t (the calling thread, with g_klock held) blocks until it is woken or timeout ns pass (negative: none, 0: it only
   checks). It lets go of its core first, as a console thread leaves the CPU when it blocks: the SVC takes it back on
   the way out (svc_leave), never here, where it would wait for a core with g_klock held. 1 when woken, 0 timed out.
   The deadline is in guest time, which stops while the game is paused (clock.c), and the thread counts as stopped for
   a game pause while it waits: a wake or a timeout during one waits for its end */
int sleep_nl(KThread* t, i64 timeout) {
    u64 end = 0;
    int r = 1;
    if (timeout > ((i64)1 << 62)) timeout = -1; /* centuries: no deadline the clock could reach */
    t->wait_timeout = timeout;
    if (timeout > 0) end = guest_ns() + (u64)timeout;
    t->wait_end = end;
    if (t->woken || !timeout) return t->woken;
    core_leave(t);
    t->park = PARK_WAIT;
    __atomic_store_n(&t->stopped, 1, __ATOMIC_SEQ_CST);
    while (!t->woken) {
        i64 left = -1;
        kthread_retire_check(&g_host.kernel.klock);
        if (timeout > 0 && !g_hle.thread.hold) {
            u64 now = guest_ns();
            if (now >= end) {
                r = 0;
                break;
            }
            left = (i64)(end - now);
        }
        pc_wait(kt_cv(t), &g_host.kernel.klock, left);
    }
    while (g_hle.thread.hold) {
        kthread_retire_check(&g_host.kernel.klock);
        pc_wait(kt_cv(t), &g_host.kernel.klock, -1);
    }
    __atomic_store_n(&t->stopped, 0, __ATOMIC_SEQ_CST);
    return r;
}

/* ============================================================ events */
/* the readable end, with no reference: the first handle takes one, or the service that keeps it. From
   g_hle.kernel.events (KEVENT_MAX) */
KEvent* kevent_new(const char* name) {
    KEvent* e;
    pm_lock(&g_host.kernel.pool_lk);
    e = (KEvent*)POOL_TAKE(g_hle.kernel.events, g_hle.kernel.event_next, "events");
    e->o.type = KO_EVENT;
    pm_unlock(&g_host.kernel.pool_lk);
    kobj_set_name(&e->o, name);
    return e;
}
/* An event stays signaled until it is cleared, waking every waiter. The console notifies only when the event was
   clear, when no thread can be waiting on it; the waiters are woken either way here, in case a service set the flag
   by itself */
void kevent_signal(KEvent* e) {
    pm_lock(&g_host.kernel.klock);
    e->signaled = 1;
    kobj_signal_nl(&e->o);
    pm_unlock(&g_host.kernel.klock);
}
void kevent_clear(KEvent* e) {
    pm_lock(&g_host.kernel.klock);
    e->signaled = 0;
    pm_unlock(&g_host.kernel.klock);
}
