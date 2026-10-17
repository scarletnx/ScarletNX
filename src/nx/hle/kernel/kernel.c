/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/kernel/k_handle_table.h, k_handle_table.cpp, k_synchronization_object.cpp,
   k_readable_event.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project */
/* kernel objects and their references, the handle table, waiting on objects and waking, events */
#include "nx/hle/kernel/kernel_int.h"

PMutex g_klock;
u64 g_title_id = 0x0100000000001000ull;
u64 g_sysres_size;
u64 g_next_tid = 0x50;
SvcRec g_svc_ring[64];
u32 g_svc_ring_n;

/* ============================================================ references */
void kobj_ref(KObj* o) {
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}
/* the last reference destroys the object. Never called with g_klock held where a destroy may take it (a session's
   service objects do) */
void kobj_unref(KObj* o) {
    if (o && __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0 && o->destroy) o->destroy(o);
}

/* ============================================================ the handle table */
/* A handle is (linear id << 15) | index, as the console's: the linear id (1 to 0x7FFF, counting up and wrapping) tells
   a stale handle from the one now in its slot, and bits 30 and 31 stay clear, so a handle fits the owner field of a
   mutex word beside its waiters bit. 1024 entries is the most the console's table holds. Free entries are a list
   through next, with -1 at its end; entries above h_top were never used */
#define H_MAX 1024
typedef struct {
    KObj* obj;
    u16 linear;
    i16 next;
} HEntry;
static HEntry g_htab[H_MAX];
static int g_hfree = -1, g_htop;
static u16 g_hlinear = 1;

/* the one process, for the pseudo-handle: never destroyed, and never signaled, as the game never ends it */
static int proc_signaled(KObj* o) {
    (void)o;
    return 0;
}
static KObj g_process = {KO_PROCESS, 1, NULL, proc_signaled, NULL, 0, 0, "process"};

u32 h_new(KObj* o) {
    int i;
    u16 lin;
    if (!o) return 0;
    pm_lock(&g_klock);
    if (g_hfree >= 0) {
        i = g_hfree;
        g_hfree = g_htab[i].next;
    } else if (g_htop < H_MAX)
        i = g_htop++;
    else {
        pm_unlock(&g_klock);
        WARN_LIMIT(4, "kernel: the handle table is full (%d handles), %s gets none\n", H_MAX, o->name);
        return 0;
    }
    lin = g_hlinear;
    g_hlinear = lin == 0x7FFF ? 1 : (u16)(lin + 1);
    g_htab[i].obj = o;
    g_htab[i].linear = lin;
    kobj_ref(o);
    pm_unlock(&g_klock);
    return ((u32)lin << 15) | (u32)i;
}
/* the table's entry for h, or -1. Pseudo-handles have the reserved bits set, so they are never in it */
static int h_index_nl(u32 h) {
    u32 i = h & 0x7FFF, lin = (h >> 15) & 0x7FFF;
    if ((h >> 30) || !lin || i >= (u32)g_htop || !g_htab[i].obj || g_htab[i].linear != lin) return -1;
    return (int)i;
}
KObj* h_get_nl(u32 h) {
    int i = h_index_nl(h);
    return i < 0 ? NULL : g_htab[i].obj;
}
static KObj* h_resolve_nl(u32 h, int t) {
    KObj* o;
    if (h == H_CUR_THREAD) {
        KThread* cur = kthread_cur();
        o = cur ? &cur->o : NULL;
    } else if (h == H_CUR_PROC)
        o = &g_process;
    else
        o = h_get_nl(h);
    return o && (!t || o->type == t) ? o : NULL;
}
KObj* h_get(u32 h) {
    KObj* o;
    pm_lock(&g_klock);
    o = h_resolve_nl(h, 0);
    pm_unlock(&g_klock);
    return o;
}
KObj* h_get_type(u32 h, int t) {
    KObj* o;
    pm_lock(&g_klock);
    o = h_resolve_nl(h, t);
    pm_unlock(&g_klock);
    return o;
}
KObj* h_get_ref(u32 h, int t) {
    KObj* o;
    pm_lock(&g_klock);
    o = h_resolve_nl(h, t);
    kobj_ref(o);
    pm_unlock(&g_klock);
    return o;
}
/* the table's reference goes after the lock is dropped: closing the last handle to a session destroys its service */
int h_close(u32 h) {
    KObj* o;
    int i;
    pm_lock(&g_klock);
    i = h_index_nl(h);
    if (i < 0) {
        pm_unlock(&g_klock);
        return R_BADHANDLE;
    }
    o = g_htab[i].obj;
    g_htab[i].obj = NULL;
    g_htab[i].next = (i16)g_hfree;
    g_hfree = i;
    pm_unlock(&g_klock);
    kobj_unref(o);
    return 0;
}

/* ============================================================ waiting and waking */
/* an object's waiters: the threads in WaitSynchronization on it (wait_kind 1) and those waiting for a thread to pause
   (5, svc.c). A thread waiting on the same handle twice is in the list twice, and leaves it twice */
void waiter_add(KObj* o, KThread* t) {
    if (o->nwait == o->capwait) {
        int cap = o->capwait ? o->capwait * 2 : 4;
        KThread** w = realloc(o->waiters, sizeof *w * (size_t)cap);
        if (!w) fatal("Out of memory for the waiters of a %s", o->name);
        o->waiters = w;
        o->capwait = cap;
    }
    o->waiters[o->nwait++] = t;
}
void waiter_del(KObj* o, KThread* t) {
    int i;
    for (i = 0; i < o->nwait; i++)
        if (o->waiters[i] == t) {
            memmove(o->waiters + i, o->waiters + i + 1, sizeof *o->waiters * (size_t)(o->nwait - i - 1));
            o->nwait--;
            return;
        }
}
/* ends t's wait with result. Only the first wake of a wait counts: a thread signaled by an object and then cancelled
   before it runs again returns the signal, as the console's NotifyAvailable makes the thread no longer cancellable */
void wake(KThread* t, int result) {
    if (t->woken) return;
    t->woken = 1;
    t->wait_result = result;
    pc_broadcast(&t->cv);
}
/* wakes the threads waiting on o, if it is signaled. A WaitSynchronization waiter is told which object it was (in
   wait_key, which its wait doesn't use otherwise): by the time it runs, another thread may have cleared the event */
void kobj_signal_nl(KObj* o) {
    int i;
    if (o->is_signaled && !o->is_signaled(o)) return;
    for (i = 0; i < o->nwait; i++) {
        KThread* w = o->waiters[i];
        if (w->wait_kind == 1 && !w->woken) w->wait_key = (u64)(size_t)o;
        wake(w, 0);
    }
}
/* t (the calling thread, with g_klock held) blocks until it is woken or timeout ns pass (negative: none, 0: it only
   checks). It lets go of its core first, as a console thread leaves the CPU when it blocks: the SVC takes it back on
   the way out (svc_leave), never here, where it would wait for a core with g_klock held. 1 when woken, 0 timed out */
int sleep_nl(KThread* t, i64 timeout) {
    u64 end = 0;
    if (timeout > ((i64)1 << 62)) timeout = -1; /* centuries: no deadline the clock could reach */
    t->wait_timeout = timeout;
    if (timeout > 0) end = plat_ns() + (u64)timeout;
    if (!t->woken && timeout) core_leave(t);
    while (!t->woken) {
        i64 left = -1;
        if (timeout >= 0) {
            u64 now = plat_ns();
            if (!timeout || now >= end) return 0;
            left = (i64)(end - now);
        }
        pc_wait(&t->cv, &g_klock, left);
    }
    return 1;
}

/* ============================================================ events */
static int ev_signaled(KObj* o) { return ((KEvent*)o)->signaled; }
static void ev_destroy(KObj* o) {
    free(o->waiters);
    free(o);
}
/* the readable end, with no reference: the first handle takes one, or the service that keeps it */
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    if (!e) fatal("Out of memory for the event %s", name);
    e->o.type = KO_EVENT;
    e->o.is_signaled = ev_signaled;
    e->o.destroy = ev_destroy;
    e->o.name = name;
    return e;
}
/* An event stays signaled until it is cleared, waking every waiter. The console notifies only when the event was
   clear, when no thread can be waiting on it; the waiters are woken either way here, in case a service set the flag
   by itself */
void kevent_signal(KEvent* e) {
    pm_lock(&g_klock);
    e->signaled = 1;
    kobj_signal_nl(&e->o);
    pm_unlock(&g_klock);
}
void kevent_clear(KEvent* e) {
    pm_lock(&g_klock);
    e->signaled = 0;
    pm_unlock(&g_klock);
}
