/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/kernel/svc.cpp, svc/svc_thread.cpp, svc/svc_processor.cpp, svc/svc_tick.cpp,
   svc/svc_event.cpp, svc/svc_port.cpp, svc/svc_session.cpp, svc/svc_ipc.cpp, svc/svc_resource_limit.cpp,
   svc/svc_exception.cpp, svc/svc_cache.cpp, svc/svc_activity.cpp, svc/svc_debug_string.cpp, svc/svc_info.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* the SVC dispatch: each SVC's registers in the 64-bit ABI (and the 32-bit one's, through snx_svc32), the thread's
   entry and exit (its core, pausing), and the SVCs that aren't memory or synchronization */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/clock.h"

#define R_BADPERM      0xD801 /* InvalidNewMemoryPermission */
#define R_OUTOFHANDLES 0xD201 /* OutOfHandles */
#define R_BADPOINTER   0xE601 /* InvalidPointer */

/* ============================================================ objects only the SVCs make */
/* svcCreateEvent's writable end: it signals and clears the readable end it holds a reference to */
typedef struct {
    KObj o;
    KEvent* r;
} KWEvent;
static void wevent_destroy(KObj* o) {
    kobj_unref(&((KWEvent*)o)->r->o);
    free(o->waiters);
    free(o);
}
/* the process's resource limit, for GetInfo 9: one object, never destroyed */
static KObj g_reslimit = {KO_RESLIMIT, 1, NULL, NULL, NULL, 0, 0, "resource limit"};

/* a handle for o, which has no reference yet: the handle's is its only one. With the table full it is destroyed */
static u32 give_handle(KObj* o, u32* out) {
    u32 h = h_new(o);
    if (!h) {
        if (o->destroy) o->destroy(o);
        return R_OUTOFHANDLES;
    }
    *out = h;
    return 0;
}
static KThread* thread_of(u32 h) { return (KThread*)h_get_type(h, KO_THREAD); }
static int valid_prio(i32 p) { return p >= 0 && p < 64 && ((g_prio_mask >> p) & 1); }

/* ============================================================ entering and leaving an SVC */
/* svcSetThreadActivity's pause: a paused thread stops at its next SVC, before it (where 1) or on its way out of the
   one it is in (2), off its core, until it is resumed. A thread blocked inside an SVC runs no game code, which is
   paused enough for GetThreadContext3 */
static int pause_point(KThread* t, int where) {
    int i;
    if (!__atomic_load_n(&t->suspended, __ATOMIC_SEQ_CST)) return 0;
    core_leave(t);
    pm_lock(&g_klock);
    t->pause_at = where;
    for (i = 0; i < t->o.nwait; i++)
        if (t->o.waiters[i]->wait_kind == 5) wake(t->o.waiters[i], 0);
    while (t->suspended) pc_wait(&t->cv, &g_klock, -1);
    t->pause_at = 0;
    pm_unlock(&g_klock);
    return 1;
}
/* cur (with g_klock held) waits until t, asked to pause, runs no game code: it is inside an SVC, stopped at a pause
   point, has exited, or was resumed meanwhile. pause_point and svc_exit_thread wake it */
static void pause_settle_nl(KThread* cur, KThread* t) {
    while (t->suspended && t->state == 1 && !t->pause_at && !__atomic_load_n(&t->in_svc, __ATOMIC_SEQ_CST)) {
        cur->woken = 0;
        cur->wait_kind = 5;
        waiter_add(&t->o, cur);
        sleep_nl(cur, -1);
        waiter_del(&t->o, cur);
        cur->wait_kind = 0;
    }
}
/* the core a thread whose core is no longer in its affinity mask moves to: its ideal core, or the highest it may use,
   as the console's SetCoreMask picks it */
static int core_allowed(const KThread* t) {
    u64 mask = __atomic_load_n(&t->affinity, __ATOMIC_RELAXED);
    int ideal = __atomic_load_n(&t->ideal_core, __ATOMIC_RELAXED), k;
    if (ideal >= 0 && ideal < NCORES && ((mask >> ideal) & 1)) return ideal;
    for (k = NCORES - 1; k >= 0; k--)
        if ((mask >> k) & 1) return k;
    return t->core;
}
static void svc_enter(KThread* t) {
    __atomic_store_n(&t->in_svc, 1, __ATOMIC_SEQ_CST);
    pause_point(t, 1);
    core_enter(t);
    core_preempt(t); /* a waiter of better priority takes the core at the holder's next SVC */
}
/* The thread takes its core back if it let go of it (it blocked, or served an IPC request), on the core its mask
   allows now, and gives it up to a waiter of better priority, as the console preempts on the way out of an SVC. Only
   then does it leave the SVC and check for a pause: in_svc is cleared before suspended is read, and SetThreadActivity
   sets suspended before it reads in_svc (both sequentially consistent), so either the pauser sees the thread still in
   its SVC or the thread sees the pause. A paused thread holds no core, so nobody waits for it */
static void svc_leave(KThread* t) {
    if (t->core < 0 || t->core >= NCORES || !((__atomic_load_n(&t->affinity, __ATOMIC_RELAXED) >> t->core) & 1)) {
        core_leave(t);
        t->core = core_allowed(t);
    }
    core_enter(t);
    core_preempt(t);
    __atomic_store_n(&t->in_svc, 0, __ATOMIC_SEQ_CST);
    if (pause_point(t, 2)) core_enter(t);
}

/* ============================================================ threads */
/* svcCreateThread: a core of the process's (-2 its default), a priority it may use, then the thread, not started */
static u32 create_thread(u64 entry, u64 arg, u64 stack_top, i32 prio, i32 core, u32* out) {
    KThread* t;
    if (core == -2) core = g_default_core;
    if (core < 0 || core >= NCORES || !((g_core_mask >> core) & 1)) return R_BADCORE;
    if (!valid_prio(prio)) return R_BADPRIO;
    t = kthread_create(entry, arg, stack_top, prio, core);
    *out = h_new(&t->o);
    return *out ? 0 : R_OUTOFHANDLES;
}
static u32 start_thread(u32 h) {
    KThread* t = thread_of(h);
    int st;
    if (!t) return R_BADHANDLE;
    pm_lock(&g_klock);
    st = t->state;
    pm_unlock(&g_klock);
    if (st != 0) return R_BADSTATE;
    kthread_start(t);
    return 0;
}
/* svcSleepThread: a sleep off the core, or one of the three yields (0 on its core, -1 and -2 to any core) */
static void sleep_thread(KThread* t, CPU* c, i64 ns) {
    t->sleep_n++;
    t->sleep_lr = g_arm_mod ? t->arm.r[14] : c->x[30];
    memcpy(t->sleep_x, c->x, sizeof t->sleep_x);
    if (ns > 0) {
        pm_lock(&g_klock);
        t->woken = 0;
        sleep_nl(t, ns);
        pm_unlock(&g_klock);
        return;
    }
    if (ns != 0 && ns != -1 && ns != -2) return;
    core_yield(t, ns ? 2 : 0);
    /* a yield loop with nobody waiting for the core spins: let the host run something else now and then */
    if ((++t->yields & 63) == 0) plat_yield();
}
static u32 set_thread_priority(u32 h, i32 prio) {
    KThread* t;
    if (!valid_prio(prio)) return R_BADPRIO;
    t = thread_of(h);
    if (!t) return R_BADHANDLE;
    pm_lock(&g_klock);
    prio_set_base_nl(t, prio);
    pm_unlock(&g_klock);
    return 0;
}
/* svcSetThreadCoreMask (Eden's svc_thread.cpp): core -2 is the process's default core alone, -3 keeps the ideal core,
   -1 has none. The thread moves when its core leaves the mask (the calling thread at once, another at its next SVC) */
static u32 set_thread_core_mask(KThread* cur, u32 h, i32 core, u64 mask) {
    KThread* t;
    if (core == -2) {
        core = g_default_core;
        mask = 1ull << core;
    } else {
        if ((mask | g_core_mask) != g_core_mask) return R_BADCORE;
        if (!mask) return R_BADCOMBO;
        if (core >= 0 && core < NCORES) {
            if (!((mask >> core) & 1)) return R_BADCOMBO;
        } else if (core != -3 && core != -1)
            return R_BADCORE;
    }
    t = thread_of(h);
    if (!t) return R_BADHANDLE;
    pm_lock(&g_klock);
    if (core == -3) {
        core = t->ideal_core;
        if (core >= 0 && !((mask >> core) & 1)) {
            pm_unlock(&g_klock);
            return R_BADCOMBO;
        }
    }
    __atomic_store_n(&t->ideal_core, core, __ATOMIC_RELAXED);
    __atomic_store_n(&t->affinity, mask, __ATOMIC_RELAXED);
    if (t->state == 0) t->core = core_allowed(t); /* not started: on no core's list yet */
    pm_unlock(&g_klock);
    if (t == cur) {
        int k = core >= 0 ? core : (mask >> t->core) & 1 ? t->core : core_allowed(t);
        if (k != t->core) { /* the caller holds its core: nothing else changes its core field meanwhile */
            core_leave(t);
            t->core = k;
            core_enter(t);
        }
    }
    return 0;
}
/* svcSetThreadActivity: 0 runs a paused thread again, 1 pauses it, and returns once it runs no game code (InvalidState
   when it already is in that state, or hasn't started, or has exited). Not the calling thread (Busy) */
static u32 set_thread_activity(KThread* cur, u32 h, u32 act) {
    KThread* t;
    u32 r = 0;
    if (act > 1) return R_BADENUM;
    t = thread_of(h);
    if (!t) return R_BADHANDLE;
    if (t == cur) return R_BUSY;
    pm_lock(&g_klock);
    if (t->state != 1 || t->suspended == (int)act)
        r = R_BADSTATE;
    else {
        __atomic_store_n(&t->suspended, (int)act, __ATOMIC_SEQ_CST);
        if (!act) {
            int i;
            pc_broadcast(&t->cv);
            for (i = 0; i < t->o.nwait; i++) /* a GetThreadContext3 waiting for it to pause has its answer */
                if (t->o.waiters[i]->wait_kind == 5) wake(t->o.waiters[i], 0);
        } else
            pause_settle_nl(cur, t);
    }
    pm_unlock(&g_klock);
    return r;
}
static void put64(u8* p, u64 v) { memcpy(p, &v, 8); }
/* svcGetThreadContext3: the console's ThreadContext (x0 to x28 at 0, fp, lr and sp at 0xE8, pc at 0x100, pstate at
   0x108, the 32 vector registers at 0x110, fpcr and fpsr at 0x310, tpidr at 0x318) of a paused thread (InvalidState
   otherwise; a pause still settling is waited for), zeros for one that has exited. A thread paused while blocked
   inside an SVC has only its callee-saved registers (x19 to x28 and v8 to v15, r4 to r11 and d8 to d15 for a 32-bit
   one), as the console keeps no more of them across an SVC; its pc, like that of one paused before its SVC, is the
   svc instruction (the thread's own pc is past it), and one paused after its SVC is past it */
static u32 get_thread_context(KThread* cur, u64 out, u32 h) {
    KThread* t = thread_of(h);
    u8 ctx[0x320];
    u32 w[2];
    int i, inside, at_svc;
    if (!t) return R_BADHANDLE;
    if (t == cur) return R_BUSY;
    if (out >= AS_END || sizeof ctx > AS_END - out) return R_BADPOINTER;
    pm_lock(&g_klock);
    pause_settle_nl(cur, t);
    if (t->state != 2 && (t->state != 1 || !t->suspended)) {
        pm_unlock(&g_klock);
        return R_BADSTATE;
    }
    memset(ctx, 0, sizeof ctx);
    inside = !t->pause_at;
    at_svc = t->pause_at != 2;
    if (t->state == 2) { /* nothing left of it */
    } else if (g_arm_mod) {
        const SnxArmCPU* a = &t->arm;
        u32 pc = a->r[15];
        if (at_svc) pc -= a->cpsr & SNX_ARM_T ? 2u : 4u;
        for (i = inside ? 4 : 0; i < (inside ? 12 : 13); i++) put64(ctx + 8 * i, a->r[i]);
        put64(ctx + 8 * 13, a->r[13]);
        put64(ctx + 8 * 14, a->r[14]);
        put64(ctx + 0xE8, a->r[11]);
        put64(ctx + 0xF0, a->r[14]);
        put64(ctx + 0xF8, a->r[13]);
        put64(ctx + 0x100, pc);
        w[0] = a->cpsr;
        memcpy(ctx + 0x108, w, 4);
        if (inside)
            memcpy(ctx + 0x110 + 8 * 8, a->d + 8, 8 * 8);
        else
            memcpy(ctx + 0x110, a->d, sizeof a->d);
        w[0] = a->fpscr & 0x07FF9F00u; /* FPSCR's FPCR and FPSR fields */
        w[1] = a->fpscr & 0xF800009Fu;
        memcpy(ctx + 0x310, w, 8);
        put64(ctx + 0x318, a->tpidrurw);
    } else {
        const CPU* c = &t->cpu;
        if (inside) {
            memcpy(ctx + 8 * 19, c->x + 19, 10 * 8);
            memcpy(ctx + 8 * 29, c->x + 29, 3 * 8); /* fp, lr, sp */
            memcpy(ctx + 0x110 + 16 * 8, c->v + 8, 8 * sizeof c->v[0]);
        } else {
            memcpy(ctx, c->x, 32 * 8); /* x0 to x28, fp, lr, sp */
            memcpy(ctx + 0x110, c->v, sizeof c->v);
        }
        put64(ctx + 0x100, at_svc ? c->pc - 4 : c->pc);
        w[0] = (c->n ? 1u << 31 : 0) | (c->z ? 1u << 30 : 0) | (c->cf ? 1u << 29 : 0) | (c->vf ? 1u << 28 : 0);
        memcpy(ctx + 0x108, w, 4);
        w[0] = c->fpcr;
        w[1] = c->fpsr;
        memcpy(ctx + 0x310, w, 8);
        memcpy(ctx + 0x318, &c->tpidr, 8);
    }
    pm_unlock(&g_klock);
    memcpy(GP(out), ctx, sizeof ctx);
    return 0;
}
/* svcCancelSynchronization: ends the thread's WaitSynchronization with Cancelled, or the next one it makes */
static u32 cancel_sync(u32 h) {
    KThread* t = thread_of(h);
    if (!t) return R_BADHANDLE;
    pm_lock(&g_klock);
    if (t->wait_kind == 1 && !t->woken)
        wake(t, R_CANCELLED);
    else
        t->cancel = 1;
    pm_unlock(&g_klock);
    return 0;
}

/* ============================================================ events */
static u32 create_event(u32* wh, u32* rh) {
    KEvent* r = kevent_new("event");
    KWEvent* w = (KWEvent*)calloc(1, sizeof *w);
    u32 res;
    if (!w) fatal("Out of memory for an event");
    w->o.type = KO_WEVENT;
    w->o.destroy = wevent_destroy;
    w->o.name = "writable event";
    w->r = r;
    kobj_ref(&r->o);
    res = give_handle(&w->o, wh);
    if (res) return res;
    *rh = h_new(&r->o);
    if (!*rh) {
        h_close(*wh);
        return R_OUTOFHANDLES;
    }
    return 0;
}
static u32 signal_event(u32 h) {
    KWEvent* w = (KWEvent*)h_get_ref(h, KO_WEVENT);
    if (!w) return R_BADHANDLE;
    kevent_signal(w->r);
    kobj_unref(&w->o);
    return 0;
}
/* ClearEvent takes either end; ResetSignal the readable end (or the process), and fails on one that isn't signaled */
static u32 clear_event(u32 h, int reset) {
    KObj* o = h_get_ref(h, 0);
    KEvent* e = NULL;
    u32 r = 0;
    if (!o) return R_BADHANDLE;
    if (o->type == KO_WEVENT && !reset)
        e = ((KWEvent*)o)->r;
    else if (o->type == KO_EVENT)
        e = (KEvent*)o;
    else if (o->type != KO_PROCESS || !reset)
        r = R_BADHANDLE;
    if (!r) {
        pm_lock(&g_klock);
        if (reset && (!e || !e->signaled))
            r = R_BADSTATE;
        else
            e->signaled = 0;
        pm_unlock(&g_klock);
    }
    kobj_unref(o);
    return r;
}

/* ============================================================ IPC, process, debug */
/* svcConnectToNamedPort: a name of at most 11 characters (OutOfRange without its NUL in 12 bytes), a service the
   runtime has (NotFound) */
static u32 connect_port(u64 name_addr, u32* out) {
    char name[12];
    SvcObj* s;
    if (name_addr >= AS_END || sizeof name > AS_END - name_addr) return R_BADPOINTER;
    memcpy(name, GP(name_addr), sizeof name);
    if (memchr(name, 0, sizeof name) == NULL) return R_OUTOFRANGE;
    s = svc_open(name);
    if (!s) return R_NOTFOUND;
    *out = session_new(s);
    return *out ? 0 : R_OUTOFHANDLES;
}
/* the request is served on the calling thread, which lets go of its core meanwhile, as it would wait for the server */
static void send_request(KThread* t, u32 h, u64 msg, u64 size) {
    core_leave(t);
    ipc_handle(t, h, msg, size);
}
static u32 get_process_id(u32 h, u64* out) {
    KObj* o = h_get_ref(h, 0);
    int ok = o && (o->type == KO_PROCESS || o->type == KO_THREAD);
    kobj_unref(o);
    if (!ok) return R_BADHANDLE;
    *out = K_PROCESS_ID;
    return 0;
}
/* how many of the n bytes at a the kernel can read for a dump: up to the first page that is free or inaccessible */
static u64 readable_len(u64 a, u64 n) {
    u64 p = a, end;
    if (a < AS_BASE || a >= AS_END) return 0;
    if (n > AS_END - a) n = AS_END - a;
    end = a + n;
    while (p < end) {
        MemBlock b;
        mem_query(p, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !b.size) break;
        p = b.addr + b.size;
    }
    return p < end ? p - a : n;
}
static void output_debug_string(u64 s, u64 n) {
    if (n > 4096) n = 4096;
    n = readable_len(s, n);
    while (n && (GP(s)[n - 1] == '\n' || GP(s)[n - 1] == '\r')) n--;
    LOG("[game] %.*s\n", (int)n, (const char*)GP(s));
}
/* svcBreak: a notification (bit 31: ro telling a debugger about a module) returns; anything else ends the game */
static void svc_break(CPU* c, u32 reason, u64 info, u64 size) {
    if (reason & 0x80000000u) return;
    WARN("\n*** The game called svcBreak (reason %x, info %llx, %llu bytes) ***\n", reason, (unsigned long long)info,
         (unsigned long long)size);
    if (size && size <= 0x400) { /* what the info points at, as far as it is mapped: a message, or words */
        u64 n = readable_len(info, size), i, len = 0;
        const u8* p = GP(info);
        while (len < n && p[len] >= 32 && p[len] < 127) len++;
        if (len >= 4 && (len == n || !p[len]))
            WARN("  info: \"%.*s\"\n", (int)len, (const char*)p);
        else {
            for (i = 0; i + 8 <= n && i < 0x100; i += 8)
                WARN("  info+%02llx: %016llx\n", (unsigned long long)i, (unsigned long long)gr64(info + i));
            if (i < n && i < 0x100 && n - i >= 4) WARN("  info+%02llx: %08x\n", (unsigned long long)i, gr32(info + i));
        }
        if (n < size) WARN("  (the rest, from info+%llx, isn't mapped)\n", (unsigned long long)n);
    }
    kernel_dump_regs(c);
    kernel_dump_threads();
    save_hints();
    fatal("The game stopped (svcBreak, reason %x). See the .log for details.", reason);
}
/* svcGetResourceLimit{Limit,Current,Peak}Value: physical memory, threads, events, transfer memories, sessions. The
   peak reads as the limit; the counts the runtime doesn't keep are 0 */
static u32 resource_limit(u32 h, u32 which, int kind, u64* out) {
    static const u64 limit[5] = {0, THREAD_MAX, 800, 256, 64};
    if (which > 4) return R_BADENUM;
    if (!h_get_type(h, KO_RESLIMIT)) return R_BADHANDLE;
    if (kind == 0)
        *out = which ? limit[which] : mem_total();
    else if (which == 0)
        *out = used_memory();
    else
        *out = which == 1 ? (u64)kthread_live_count() : 0;
    return 0;
}
static u32 get_info(u32 id0, u32 h, u64 id1, u64* out) {
    if (id0 != 9) return svc_get_info(id0, h, id1, out);
    if (h) return R_BADHANDLE; /* ResourceLimit: always succeeds after its checks */
    if (id1) return R_BADCOMBO;
    *out = h_new(&g_reslimit);
    return 0;
}

/* ============================================================ the dispatch */
/* the SVCs whose x0 out isn't a result (or that have no out), which the invalid-handle warning skips. The first three
   are left out of the crash report's ring too: a polling loop would fill it */
static int svc_quiet(u32 imm) { return imm == 0x0B || imm == 0x10 || imm == 0x1E; }
static int svc_no_result(u32 imm) {
    return svc_quiet(imm) || imm == 0x07 || imm == 0x0A || imm == 0x16 || imm == 0x1D || imm == 0x26 || imm == 0x2A ||
           imm == 0x36 || imm == 0x3C || imm == 0x3D;
}

#define W(i) ((u32)c->x[i])
#define I(i) ((i32)(u32)c->x[i])
void snx_svc(CPU* c, u32 imm) {
    KThread* t = (KThread*)c->thread;
    SvcRec in;
    u32 n = 0, h = 0;
    u64 v = 0;
    in.imm = imm;
    in.res = 0;
    in.o1 = 0;
    in.lr = g_arm_mod ? t->arm.r[14] : c->x[30];
    memcpy(in.a, c->x, sizeof in.a);
    if (!svc_quiet(imm)) {
        n = __atomic_fetch_add(&g_svc_ring_n, 1, __ATOMIC_RELAXED);
        g_svc_ring[n & 63] = in;
    }
    svc_enter(t);
    switch (imm) {
        case 0x01:
            c->x[0] = svc_set_heap_size(c->x[1]);
            if (!c->x[0]) c->x[1] = HEAP_BASE;
            break;
        case 0x02: c->x[0] = svc_set_mem_perm(c->x[0], c->x[1], W(2)); break;
        case 0x03: c->x[0] = svc_set_mem_attr(c->x[0], c->x[1], W(2), W(3)); break;
        case 0x04: c->x[0] = svc_map_memory(c->x[0], c->x[1], c->x[2]); break;
        case 0x05: c->x[0] = svc_unmap_memory(c->x[0], c->x[1], c->x[2]); break;
        case 0x06:
            c->x[0] = svc_query(c->x[0], c->x[2]);
            c->x[1] = 0; /* PageInfo: no flags */
            break;
        case 0x07: process_exit(0); break;
        case 0x08:
            c->x[0] = create_thread(c->x[1], c->x[2], c->x[3], I(4), I(5), &h);
            c->x[1] = h;
            break;
        case 0x09: c->x[0] = start_thread(W(0)); break;
        case 0x0A: svc_exit_thread(t); break;
        case 0x0B: sleep_thread(t, c, (i64)c->x[0]); break;
        case 0x0C: {
            KThread* o = thread_of(W(1));
            c->x[0] = o ? 0 : R_BADHANDLE;
            c->x[1] = o ? (u64)o->prio : 0;
            break;
        }
        case 0x0D: c->x[0] = set_thread_priority(W(0), I(1)); break;
        case 0x0E: {
            KThread* o = thread_of(W(2));
            c->x[0] = o ? 0 : R_BADHANDLE;
            c->x[1] = o ? (u32)o->ideal_core : 0;
            c->x[2] = o ? o->affinity : 0;
            break;
        }
        case 0x0F: c->x[0] = set_thread_core_mask(t, W(0), I(1), c->x[2]); break;
        case 0x10: c->x[0] = (u64)t->core; break;
        case 0x11: c->x[0] = signal_event(W(0)); break;
        case 0x12: c->x[0] = clear_event(W(0), 0); break;
        case 0x13: c->x[0] = svc_map_shm(W(0), c->x[1], c->x[2], W(3)); break;
        case 0x14: c->x[0] = svc_unmap_shm(W(0), c->x[1], c->x[2]); break;
        case 0x15: {
            KTmem* m = NULL;
            u32 r = svc_tmem_new(c->x[1], c->x[2], W(3), &m);
            if (!r) r = give_handle(&m->o, &h);
            c->x[0] = r;
            c->x[1] = h;
            break;
        }
        case 0x16: c->x[0] = (u32)h_close(W(0)); break;
        case 0x17: c->x[0] = clear_event(W(0), 1); break;
        case 0x18: {
            u32 idx = ~0u;
            c->x[0] = svc_wait_sync(t, c->x[1], W(2), (i64)c->x[3], &idx);
            c->x[1] = idx;
            break;
        }
        case 0x19: c->x[0] = cancel_sync(W(0)); break;
        case 0x1A: c->x[0] = svc_arbitrate_lock(t, W(0), c->x[1], W(2)); break;
        case 0x1B: c->x[0] = svc_arbitrate_unlock(c->x[0]); break;
        case 0x1C: c->x[0] = svc_wait_cv(t, c->x[0], c->x[1], W(2), (i64)c->x[3]); break;
        case 0x1D: svc_signal_cv(c->x[0], I(1)); break; /* no result */
        case 0x1E: c->x[0] = guest_ticks(); break;
        case 0x1F:
            c->x[0] = connect_port(c->x[1], &h);
            c->x[1] = h;
            break;
        case 0x21: send_request(t, W(0), t->tls, 0x100); break;
        case 0x22: {
            u32 r = ipc_user_buffer(c->x[0], c->x[1]);
            if (r)
                c->x[0] = r;
            else
                send_request(t, W(2), c->x[0], c->x[1]);
            break;
        }
        case 0x24:
            c->x[0] = get_process_id(W(1), &v);
            c->x[1] = v;
            break;
        case 0x25: {
            KThread* o = thread_of(W(1));
            c->x[0] = o ? 0 : R_BADHANDLE;
            c->x[1] = o ? o->id : 0;
            break;
        }
        case 0x26: svc_break(c, W(0), c->x[1], c->x[2]); break;
        case 0x27:
            output_debug_string(c->x[0], c->x[1]);
            c->x[0] = 0;
            break;
        case 0x28: /* ReturnFromException with no exception handler: the console ends the process */
            WARN("\n*** The game returned from an exception it had no handler for (%x) ***\n", W(0));
            kernel_dump_regs(c);
            save_hints();
            fatal("The game stopped (ReturnFromException %x). See the .log for details.", W(0));
            break;
        case 0x29:
            c->x[0] = get_info(W(1), W(2), c->x[3], &v);
            c->x[1] = v;
            break;
        case 0x2A: break;                    /* FlushEntireDataCache */
        case 0x2B: c->x[0] = 0; break;       /* FlushDataCache: guest memory is the host's, coherent */
        case 0x2C: c->x[0] = svc_map_physical(c->x[0], c->x[1]); break;
        case 0x2D: c->x[0] = svc_unmap_physical(c->x[0], c->x[1]); break;
        case 0x30:
        case 0x31:
        case 0x37:
            c->x[0] = resource_limit(W(1), W(2), imm == 0x30 ? 0 : imm == 0x31 ? 1 : 2, &v);
            c->x[1] = v;
            break;
        case 0x32: c->x[0] = set_thread_activity(t, W(0), W(1)); break;
        case 0x33: c->x[0] = get_thread_context(t, c->x[0], W(1)); break;
        case 0x34: c->x[0] = svc_wait_addr(t, c->x[0], W(1), I(2), (i64)c->x[3]); break;
        case 0x35: c->x[0] = svc_signal_addr(c->x[0], W(1), I(2), I(3)); break;
        case 0x36: break; /* SynchronizePreemptionState */
        case 0x3C: break; /* KernelDebug */
        case 0x3D: break; /* ChangeKernelTraceState */
        case 0x45: {
            u32 rh = 0;
            c->x[0] = create_event(&h, &rh);
            c->x[1] = h;
            c->x[2] = rh;
            break;
        }
        case 0x4B: {
            KObj* o = NULL;
            u32 r = svc_codemem_new(c->x[1], c->x[2], &o);
            if (!r) r = give_handle(o, &h);
            c->x[0] = r;
            c->x[1] = h;
            break;
        }
        case 0x4C: {
            KObj* o = h_get_ref(W(0), KO_CODEMEM);
            c->x[0] = o ? svc_codemem_control(o, W(1), c->x[2], c->x[3], W(4)) : R_BADHANDLE;
            kobj_unref(o);
            break;
        }
        case 0x5D:
        case 0x5E:
        case 0x5F: c->x[0] = 0; break; /* the process data cache operations */
        default:
            WARN_LIMIT(8, "svc %02x: not implemented (x0=%llx x1=%llx x2=%llx), NotImplemented\n", imm,
                       (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
            c->x[0] = R_NOTIMPL;
            break;
    }
    svc_leave(t);
    if (!svc_no_result(imm) && (u32)c->x[0] == R_BADHANDLE)
        WARN_LIMIT(20, "svc %02x with an invalid handle (x0=%llx x1=%llx x2=%llx x3=%llx), lr %llx\n", imm,
                   (unsigned long long)in.a[0], (unsigned long long)in.a[1], (unsigned long long)in.a[2],
                   (unsigned long long)in.a[3], (unsigned long long)in.lr);
    if (!svc_quiet(imm) && __atomic_load_n(&g_svc_ring_n, __ATOMIC_RELAXED) - n < 64) { /* not another SVC's yet */
        g_svc_ring[n & 63].res = (u32)c->x[0];
        g_svc_ring[n & 63].o1 = c->x[1];
    }
}

/* ============================================================ 32-bit threads */
/* Each SVC's registers in the 32-bit ABI, from Eden's svc.cpp wrappers (SvcWrap_*64From32 against SvcWrap_*64), as
   "inputs>outputs": x:r is the 64-bit ABI's register x and the 32-bit one's r, x:lo+hi a 64-bit value in two. NULL:
   r0 to r7 are x0 to x7 */
static const char* const g_svc32[0x80] = {
    NULL,
    "1:1>0:0 1:1", /* 01 SetHeapSize */
    "0:0 1:1 2:2>0:0", /* 02 SetMemoryPermission */
    "0:0 1:1 2:2 3:3>0:0", /* 03 SetMemoryAttribute */
    "0:0 1:1 2:2>0:0", /* 04 MapMemory */
    "0:0 1:1 2:2>0:0", /* 05 UnmapMemory */
    "0:0 2:2>0:0 1:1", /* 06 QueryMemory */
    ">", /* 07 ExitProcess */
    "1:1 2:2 3:3 4:0 5:4>0:0 1:1", /* 08 CreateThread */
    "0:0>0:0", /* 09 StartThread */
    ">", /* 0a ExitThread */
    "0:0+1>", /* 0b SleepThread */
    "1:1>0:0 1:1", /* 0c GetThreadPriority */
    "0:0 1:1>0:0", /* 0d SetThreadPriority */
    "2:2>0:0 1:1 2:2+3", /* 0e GetThreadCoreMask */
    "0:0 1:1 2:2+3>0:0", /* 0f SetThreadCoreMask */
    ">0:0", /* 10 GetCurrentProcessorNumber */
    "0:0>0:0", /* 11 SignalEvent */
    "0:0>0:0", /* 12 ClearEvent */
    "0:0 1:1 2:2 3:3>0:0", /* 13 MapSharedMemory */
    "0:0 1:1 2:2>0:0", /* 14 UnmapSharedMemory */
    "1:1 2:2 3:3>0:0 1:1", /* 15 CreateTransferMemory */
    "0:0>0:0", /* 16 CloseHandle */
    "0:0>0:0", /* 17 ResetSignal */
    "1:1 2:2 3:0+3>0:0 1:1", /* 18 WaitSynchronization */
    "0:0>0:0", /* 19 CancelSynchronization */
    "0:0 1:1 2:2>0:0", /* 1a ArbitrateLock */
    "0:0>0:0", /* 1b ArbitrateUnlock */
    "0:0 1:1 2:2 3:3+4>0:0", /* 1c WaitProcessWideKeyAtomic */
    "0:0 1:1>", /* 1d SignalProcessWideKey */
    ">0:0+1", /* 1e GetSystemTick */
    "1:1>0:0 1:1", /* 1f ConnectToNamedPort */
    NULL,
    "0:0>0:0", /* 21 SendSyncRequest */
    "0:0 1:1 2:2>0:0", /* 22 SendSyncRequestWithUserBuffer */
    "1:1 2:2 3:3>0:0 1:1", /* 23 SendAsyncRequestWithUserBuffer */
    "1:1>0:0 1:1+2", /* 24 GetProcessId */
    "1:1>0:0 1:1+2", /* 25 GetThreadId */
    "0:0 1:1 2:2>", /* 26 Break */
    "0:0 1:1>0:0", /* 27 OutputDebugString */
    "0:0>", /* 28 ReturnFromException */
    "1:1 2:2 3:0+3>0:0 1:1+2", /* 29 GetInfo */
    ">", /* 2a FlushEntireDataCache */
    "0:0 1:1>0:0", /* 2b FlushDataCache */
    "0:0 1:1>0:0", /* 2c MapPhysicalMemory */
    "0:0 1:1>0:0", /* 2d UnmapPhysicalMemory */
    NULL,
    NULL,
    "1:1 2:2>0:0 1:1+2", /* 30 GetResourceLimitLimitValue */
    "1:1 2:2>0:0 1:1+2", /* 31 GetResourceLimitCurrentValue */
    "0:0 1:1>0:0", /* 32 SetThreadActivity */
    "0:0 1:1>0:0", /* 33 GetThreadContext3 */
    "0:0 1:1 2:2 3:3+4>0:0", /* 34 WaitForAddress */
    "0:0 1:1 2:2 3:3>0:0", /* 35 SignalToAddress */
    ">", /* 36 SynchronizePreemptionState */
    "1:1 2:2>0:0 1:1+2", /* 37 GetResourceLimitPeakValue */
    NULL,
    "1:1>0:0 1:1", /* 39 CreateIoPool */
    "1:1 2:2+3 3:0 4:4 5:5>0:0 1:1", /* 3a CreateIoRegion */
    NULL,
    "0:0 1:2+3 2:1+4 3:5+6>", /* 3c KernelDebug */
    "0:0>", /* 3d ChangeKernelTraceState */
    NULL,
    NULL,
    "2:2 3:3>0:0 1:1 2:2", /* 40 CreateSession */
    "1:1>0:0 1:1", /* 41 AcceptSession */
    NULL,
    "1:1 2:2 3:3 4:0+4>0:0 1:1", /* 43 ReplyAndReceive */
    "1:1 2:2 3:3 4:0 5:4 6:5+6>0:0 1:1", /* 44 ReplyAndReceiveWithUserBuffer */
    ">0:0 1:1 2:2", /* 45 CreateEvent */
    "0:0 1:1 2:2 3:3>0:0", /* 46 MapIoRegion */
    "0:0 1:1 2:2>0:0", /* 47 UnmapIoRegion */
    "0:0 1:1>0:0", /* 48 MapPhysicalMemoryUnsafe */
    "0:0 1:1>0:0", /* 49 UnmapPhysicalMemoryUnsafe */
    "0:0>0:0", /* 4a SetUnsafeLimit */
    "1:1 2:2>0:0 1:1", /* 4b CreateCodeMemory */
    "0:0 1:1 2:2+3 3:4+5 4:6>0:0", /* 4c ControlCodeMemory */
    ">", /* 4d SleepSystem */
    "1:2+3 2:0 3:1>0:0 1:1", /* 4e ReadWriteRegister */
    "0:0 1:1>0:0", /* 4f SetProcessActivity */
    "1:1 2:2 3:3>0:0 1:1", /* 50 CreateSharedMemory */
    "0:0 1:1 2:2 3:3>0:0", /* 51 MapTransferMemory */
    "0:0 1:1 2:2>0:0", /* 52 UnmapTransferMemory */
    "1:1 2:2>0:0 1:1", /* 53 CreateInterruptEvent */
    NULL,
    "2:2+3 3:0>0:0 1:1 2:2", /* 55 QueryIoMapping */
    "1:2+3 2:0+1>0:0 1:1", /* 56 CreateDeviceAddressSpace */
    "0:0 1:1>0:0", /* 57 AttachDeviceAddressSpace */
    "0:0 1:1>0:0", /* 58 DetachDeviceAddressSpace */
    "0:0 1:1 2:2+3 3:4 4:5+6 5:7>0:0", /* 59 MapDeviceAddressSpaceByForce */
    "0:0 1:1 2:2+3 3:4 4:5+6 5:7>0:0", /* 5a MapDeviceAddressSpaceAligned */
    NULL,
    "0:0 1:1 2:2+3 3:4 4:5+6>0:0", /* 5c UnmapDeviceAddressSpace */
    "0:0 1:2+3 2:1+4>0:0", /* 5d InvalidateProcessDataCache */
    "0:0 1:2+3 2:1+4>0:0", /* 5e StoreProcessDataCache */
    "0:0 1:2+3 2:1+4>0:0", /* 5f FlushProcessDataCache */
    "1:2+3>0:0 1:1", /* 60 DebugActiveProcess */
    "0:0>0:0", /* 61 BreakDebugProcess */
    "0:0>0:0", /* 62 TerminateDebugProcess */
    "0:0 1:1>0:0", /* 63 GetDebugEvent */
    "0:0 1:1 2:2 3:3>0:0", /* 64 ContinueDebugEvent */
    "1:1 2:2>0:0 1:1", /* 65 GetProcessList */
    "1:1 2:2 3:3>0:0 1:1", /* 66 GetThreadList */
    "0:0 1:1 2:2+3 3:4>0:0", /* 67 GetDebugThreadContext */
    "0:0 1:2+3 2:1 3:4>0:0", /* 68 SetDebugThreadContext */
    "0:0 2:2 3:3>0:0 1:1", /* 69 QueryDebugProcessMemory */
    "0:0 1:1 2:2 3:3>0:0", /* 6a ReadDebugProcessMemory */
    "0:0 1:1 2:2 3:3>0:0", /* 6b WriteDebugProcessMemory */
    "0:0 1:2+3 2:1+4>0:0", /* 6c SetHardwareBreakPoint */
    "2:2 3:0+1 4:3>0:0 1:1+2 2:3", /* 6d GetDebugThreadParam */
    NULL,
    "1:1 2:2 3:0+3>0:0 1:1+2", /* 6f GetSystemInfo */
    "2:2 3:3 4:0>0:0 1:1 2:2", /* 70 CreatePort */
    "1:1 2:2>0:0 1:1", /* 71 ManageNamedPort */
    "1:1>0:0 1:1", /* 72 ConnectToPort */
    "0:0 1:2+3 2:1+4 3:5>0:0", /* 73 SetProcessMemoryPermission */
    "0:0 1:1 2:2+3 3:4>0:0", /* 74 MapProcessMemory */
    "0:0 1:1 2:2+3 3:4>0:0", /* 75 UnmapProcessMemory */
    "0:0 2:2 3:1+3>0:0 1:1", /* 76 QueryProcessMemory */
    "0:0 1:2+3 2:1+4 3:5+6>0:0", /* 77 MapProcessCodeMemory */
    "0:0 1:2+3 2:1+4 3:5+6>0:0", /* 78 UnmapProcessCodeMemory */
    "1:1 2:2 3:3>0:0 1:1", /* 79 CreateProcess */
    "0:0 1:1 2:2 3:3+4>0:0", /* 7a StartProcess */
    "0:0>0:0", /* 7b TerminateProcess */
    "1:1 2:2>0:0 1:1+2", /* 7c GetProcessInfo */
    ">0:0 1:1", /* 7d CreateResourceLimit */
    "0:0 1:1 2:2+3>0:0", /* 7e SetResourceLimitLimitValue */
    NULL,
};
static int digit(const char** p) {
    int d = **p - '0';
    (*p)++;
    return d;
}
void snx_svc32(SnxArmCPU* a, u32 imm) {
    KThread* t = kthread_cur();
    CPU* c = &t->cpu;
    const char* m = imm < 0x80 ? g_svc32[imm] : NULL;
    const char* p;
    int i;
    c->x[30] = a->r[14]; /* for the SVC log and GetThreadContext3: the return address, and pc past the svc */
    c->pc = a->r[15];
    if (!m) {
        for (i = 0; i < 8; i++) c->x[i] = a->r[i];
        snx_svc(c, imm);
        for (i = 0; i < 8; i++) a->r[i] = (u32)c->x[i];
        return;
    }
    for (p = m; *p && *p != '>';) {
        int x = digit(&p), lo, hi = -1;
        p++; /* ':' */
        lo = digit(&p);
        if (*p == '+') {
            p++;
            hi = digit(&p);
        }
        c->x[x] = a->r[lo] | (hi >= 0 ? (u64)a->r[hi] << 32 : 0);
        if (*p == ' ') p++;
    }
    snx_svc(c, imm);
    if (*p == '>') p++;
    while (*p) {
        int x = digit(&p), lo, hi = -1;
        p++;
        lo = digit(&p);
        if (*p == '+') {
            p++;
            hi = digit(&p);
        }
        a->r[lo] = (u32)c->x[x];
        if (hi >= 0) a->r[hi] = (u32)(c->x[x] >> 32);
        if (*p == ' ') p++;
    }
}
