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
#include "nx/hle/hle.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/tracy.h"

#define R_BADPERM      0xD801 /* InvalidNewMemoryPermission */
#define R_OUTOFHANDLES 0xD201 /* OutOfHandles */
#define R_BADPOINTER   0xE601 /* InvalidPointer */

/* ============================================================ objects only the SVCs make */
/* svcCreateEvent's writable end (KWEvent), from g_hle.svc.wevents (KHANDLED_MAX). The process's resource limit, for
   GetInfo 9, is g_hle.svc.reslimit: one object, never destroyed */
void kwevent_destroy(KObj* o) {
    kobj_unref(kobj_at(((KWEvent*)o)->r));
    pm_lock(&g_host.kernel.pool_lk);
    POOL_PUT((KWEvent*)o);
    pm_unlock(&g_host.kernel.pool_lk);
}

/* a handle for o, which has no reference yet: the handle's is its only one. With the table full it is destroyed */
static u32 give_handle(KObj* o, u32* out) {
    u32 h = h_new(o);
    if (!h) {
        kobj_destroy(o);
        R_THROW(R_OUTOFHANDLES);
    }
    *out = h;
    R_SUCCEED();
}
/* the thread h names, with a reference the caller drops (thread_put): threads are freed with their last one */
static KThread* thread_of(u32 h) { return (KThread*)h_get_ref(h, KO_THREAD); }
static void thread_put(KThread* t) { kobj_unref(t ? &t->o : NULL); }
static int valid_prio(i32 p) { return p >= 0 && p < 64 && ((g_hle.svcmem.prio_mask >> p) & 1); }

/* ============================================================ entering and leaving an SVC */
/* svcSetThreadActivity's pause: a paused thread stops at its next SVC, before it (where 1) or on its way out of the
   one it is in (2), off its core, until it is resumed. A thread blocked inside an SVC runs no game code, which is
   paused enough for GetThreadContext3 */
static int pause_point(KThread* t, int where) {
    if (!__atomic_load_n(&t->suspended, __ATOMIC_SEQ_CST)) return 0;
    core_leave(t);
    pm_lock(&g_host.kernel.klock);
    t->pause_at = where;
    kobj_wake_nl(&t->o, 5);
    while (t->suspended) pc_wait(kt_cv(t), &g_host.kernel.klock, -1);
    t->pause_at = 0;
    pm_unlock(&g_host.kernel.klock);
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
    kthread_hold_point(t); /* a game pause, before the SVC does anything */
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
    if (kthread_hold_point(t)) core_enter(t);
}

/* ============================================================ threads */
/* svcCreateThread: a core of the process's (-2 its default), a priority it may use, then the thread, not started */
static u32 create_thread(u64 entry, u64 arg, u64 stack_top, i32 prio, i32 core, u32* out) {
    KThread* t;
    if (core == -2) core = g_hle.thread.default_core;
    R_UNLESS(core >= 0 && core < NCORES && ((g_hle.svcmem.core_mask >> core) & 1), R_BADCORE);
    R_UNLESS(valid_prio(prio), R_BADPRIO);
    t = kthread_create(entry, arg, stack_top, prio, core);
    return give_handle(&t->o, out);
}
static u32 start_thread(u32 h) {
    KThread* t = thread_of(h);
    int st;
    R_UNLESS(t, R_BADHANDLE);
    pm_lock(&g_host.kernel.klock);
    st = t->state;
    pm_unlock(&g_host.kernel.klock);
    if (st == 0) kthread_start(t);
    thread_put(t);
    R_UNLESS(st == 0, R_BADSTATE);
    R_SUCCEED();
}
/* svcSleepThread: a sleep off the core, or one of the three yields (0 on its core, -1 and -2 to any core) */
static void sleep_thread(KThread* t, i64 ns) {
    int i;
    t->sleep_n++;
    if (g_arm_mod) {
        t->sleep_lr = t->arm.r[14];
        memset(t->sleep_x, 0, sizeof t->sleep_x);
        for (i = 0; i < 15; i++) t->sleep_x[i] = t->arm.r[i];
    } else {
        t->sleep_lr = t->cpu.x[30];
        memcpy(t->sleep_x, t->cpu.x, sizeof t->sleep_x);
    }
    if (ns > 0) {
        pm_lock(&g_host.kernel.klock);
        t->woken = 0;
        sleep_nl(t, ns);
        pm_unlock(&g_host.kernel.klock);
        return;
    }
    if (ns != 0 && ns != -1 && ns != -2) return;
    core_yield(t, ns ? 2 : 0);
    /* a yield loop with nobody waiting for the core spins: let the host run something else now and then */
    if ((++t->yields & 63) == 0) plat_yield();
}
static u32 set_thread_priority(u32 h, i32 prio) {
    KThread* t;
    R_UNLESS(valid_prio(prio), R_BADPRIO);
    t = thread_of(h);
    R_UNLESS(t, R_BADHANDLE);
    pm_lock(&g_host.kernel.klock);
    prio_set_base_nl(t, prio);
    pm_unlock(&g_host.kernel.klock);
    thread_put(t);
    R_SUCCEED();
}
/* svcSetThreadCoreMask (Eden's svc_thread.cpp): core -2 is the process's default core alone, -3 keeps the ideal core,
   -1 has none. The thread moves when its core leaves the mask (the calling thread at once, another at its next SVC) */
static u32 set_thread_core_mask(KThread* cur, u32 h, i32 core, u64 mask) {
    KThread* t;
    if (core == -2) {
        core = g_hle.thread.default_core;
        mask = 1ull << core;
    } else {
        R_UNLESS((mask | g_hle.svcmem.core_mask) == g_hle.svcmem.core_mask, R_BADCORE);
        R_UNLESS(mask, R_BADCOMBO);
        if (core >= 0 && core < NCORES) {
            if (!((mask >> core) & 1)) R_THROW(R_BADCOMBO);
        } else if (core != -3 && core != -1)
            R_THROW(R_BADCORE);
    }
    t = thread_of(h);
    R_UNLESS(t, R_BADHANDLE);
    pm_lock(&g_host.kernel.klock);
    if (core == -3) {
        core = t->ideal_core;
        if (core >= 0 && !((mask >> core) & 1)) {
            pm_unlock(&g_host.kernel.klock);
            thread_put(t);
            R_THROW(R_BADCOMBO);
        }
    }
    __atomic_store_n(&t->ideal_core, core, __ATOMIC_RELAXED);
    __atomic_store_n(&t->affinity, mask, __ATOMIC_RELAXED);
    if (t->state == 0) t->core = core_allowed(t); /* not started: on no core's list yet */
    pm_unlock(&g_host.kernel.klock);
    if (t == cur) {
        int k = core >= 0 ? core : (mask >> t->core) & 1 ? t->core : core_allowed(t);
        if (k != t->core) { /* the caller holds its core: nothing else changes its core field meanwhile */
            core_leave(t);
            t->core = k;
            core_enter(t);
        }
    }
    thread_put(t);
    R_SUCCEED();
}
/* svcSetThreadActivity: 0 runs a paused thread again, 1 pauses it, and returns once it runs no game code (InvalidState
   when it already is in that state, or hasn't started, or has exited). Not the calling thread (Busy) */
static u32 set_thread_activity(KThread* cur, u32 h, u32 act) {
    KThread* t;
    u32 r = 0;
    R_UNLESS(act <= 1, R_BADENUM);
    t = thread_of(h);
    R_UNLESS(t, R_BADHANDLE);
    if (t == cur) {
        thread_put(t);
        R_THROW(R_BUSY);
    }
    pm_lock(&g_host.kernel.klock);
    if (t->state != 1 || t->suspended == (int)act)
        r = R_BADSTATE;
    else {
        __atomic_store_n(&t->suspended, (int)act, __ATOMIC_SEQ_CST);
        if (!act) {
            pc_broadcast(kt_cv(t));
            kobj_wake_nl(&t->o, 5); /* a GetThreadContext3 waiting for it to pause has its answer */
        } else
            pause_settle_nl(cur, t);
    }
    pm_unlock(&g_host.kernel.klock);
    thread_put(t);
    return r;
}
/* the context is the guest's structure, little-endian: the registers go in as numbers, but for the vector registers,
   which hold the guest's bytes (cpu.h's V128) */
static void put64(u8* p, u64 v) { snx_st_le64(p, v); }
/* svcGetThreadContext3: the console's ThreadContext (x0 to x28 at 0, fp, lr and sp at 0xE8, pc at 0x100, pstate at
   0x108, the 32 vector registers at 0x110, fpcr and fpsr at 0x310, tpidr at 0x318) of a paused thread (InvalidState
   otherwise; a pause still settling is waited for), zeros for one that has exited. A thread paused while blocked
   inside an SVC has only its callee-saved registers (x19 to x28 and v8 to v15, r4 to r11 and d8 to d15 for a 32-bit
   one), as the console keeps no more of them across an SVC; its pc, like that of one paused before its SVC, is the
   svc instruction (the thread's own pc is past it), and one paused after its SVC is past it */
static u32 get_thread_context(KThread* cur, u64 out, u32 h) {
    KThread* t = thread_of(h);
    u8 ctx[0x320];
    int i, inside, at_svc;
    u32 r = 0;
    R_UNLESS(t, R_BADHANDLE);
    if (t == cur)
        r = R_BUSY;
    else if (!(out < AS_END && sizeof ctx <= AS_END - out))
        r = R_BADPOINTER;
    if (r) {
        thread_put(t);
        return r;
    }
    pm_lock(&g_host.kernel.klock);
    pause_settle_nl(cur, t);
    if (t->state != 2 && (t->state != 1 || !t->suspended)) {
        pm_unlock(&g_host.kernel.klock);
        thread_put(t);
        R_THROW(R_BADSTATE);
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
        snx_st_le32(ctx + 0x108, a->cpsr);
        if (inside) {
            memcpy(ctx + 0x110 + 8 * 8, a->d + 8, 8 * 8);
            snx_le64_array(ctx + 0x110 + 8 * 8, 8);
        } else {
            memcpy(ctx + 0x110, a->d, sizeof a->d);
            snx_le64_array(ctx + 0x110, 32);
        }
        snx_st_le32(ctx + 0x310, a->fpscr & 0x07FF9F00u); /* FPSCR's FPCR and FPSR fields */
        snx_st_le32(ctx + 0x314, a->fpscr & 0xF800009Fu);
        put64(ctx + 0x318, a->tpidrurw);
    } else {
        const CPU* c = &t->cpu;
        if (inside) {
            memcpy(ctx + 8 * 19, c->x + 19, 10 * 8);
            memcpy(ctx + 8 * 29, c->x + 29, 3 * 8); /* fp, lr, sp */
            snx_le64_array(ctx + 8 * 19, 13);
            memcpy(ctx + 0x110 + 16 * 8, c->v + 8, 8 * sizeof c->v[0]);
        } else {
            memcpy(ctx, c->x, 32 * 8); /* x0 to x28, fp, lr, sp */
            snx_le64_array(ctx, 32);
            memcpy(ctx + 0x110, c->v, sizeof c->v);
        }
        put64(ctx + 0x100, at_svc ? c->pc - 4 : c->pc);
        snx_st_le32(ctx + 0x108,
                    (c->n ? 1u << 31 : 0) | (c->z ? 1u << 30 : 0) | (c->cf ? 1u << 29 : 0) | (c->vf ? 1u << 28 : 0));
        snx_st_le32(ctx + 0x310, c->fpcr);
        snx_st_le32(ctx + 0x314, c->fpsr);
        put64(ctx + 0x318, c->tpidr);
    }
    pm_unlock(&g_host.kernel.klock);
    thread_put(t);
    memcpy(GP(out), ctx, sizeof ctx);
    R_SUCCEED();
}
/* svcCancelSynchronization: ends the thread's WaitSynchronization with Cancelled, or the next one it makes */
static u32 cancel_sync(u32 h) {
    KThread* t = thread_of(h);
    R_UNLESS(t, R_BADHANDLE);
    pm_lock(&g_host.kernel.klock);
    if (t->wait_kind == 1 && !t->woken)
        wake(t, R_CANCELLED);
    else
        t->cancel = 1;
    pm_unlock(&g_host.kernel.klock);
    thread_put(t);
    R_SUCCEED();
}

/* ============================================================ events */
static u32 create_event(u32* wh, u32* rh) {
    KEvent* r = kevent_new("event");
    KWEvent* w;
    pm_lock(&g_host.kernel.pool_lk);
    w = (KWEvent*)POOL_TAKE(g_hle.svc.wevents, g_hle.svc.wevent_next, "writable events");
    w->o.type = KO_WEVENT;
    pm_unlock(&g_host.kernel.pool_lk);
    kobj_set_name(&w->o, "writable event");
    w->r = kobj_id(&r->o);
    kobj_ref(&r->o);
    R_TRY(give_handle(&w->o, wh));
    *rh = h_new(&r->o);
    if (!*rh) {
        h_close(*wh);
        R_THROW(R_OUTOFHANDLES);
    }
    R_SUCCEED();
}
static u32 signal_event(u32 h) {
    KWEvent* w = (KWEvent*)h_get_ref(h, KO_WEVENT);
    R_UNLESS(w, R_BADHANDLE);
    kevent_signal((KEvent*)kobj_at(w->r));
    kobj_unref(&w->o);
    R_SUCCEED();
}
/* ClearEvent takes either end; ResetSignal the readable end (or the process), and fails on one that isn't signaled */
static u32 clear_event(u32 h, int reset) {
    KObj* o = h_get_ref(h, 0);
    KEvent* e = NULL;
    u32 r = 0;
    R_UNLESS(o, R_BADHANDLE);
    if (o->type == KO_WEVENT && !reset)
        e = (KEvent*)kobj_at(((KWEvent*)o)->r);
    else if (o->type == KO_EVENT)
        e = (KEvent*)o;
    else if (o->type != KO_PROCESS || !reset)
        r = R_BADHANDLE;
    if (R_SUCCEEDED(r)) {
        pm_lock(&g_host.kernel.klock);
        if (reset && (!e || !e->signaled))
            r = R_BADSTATE;
        else
            e->signaled = 0;
        pm_unlock(&g_host.kernel.klock);
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
    R_UNLESS(name_addr < AS_END && sizeof name <= AS_END - name_addr, R_BADPOINTER);
    memcpy(name, GP(name_addr), sizeof name);
    R_UNLESS(memchr(name, 0, sizeof name) != NULL, R_OUTOFRANGE);
    s = svc_open(name);
    R_UNLESS(s, R_NOTFOUND);
    *out = session_new(s);
    return *out ? 0 : R_OUTOFHANDLES;
}
/* the request is served on the calling thread, which lets go of its core meanwhile, as it would wait for the server */
static u32 send_request(KThread* t, u32 h, u64 msg, u64 size) {
    core_leave(t);
    return ipc_handle(h, msg, size);
}
static u32 get_process_id(u32 h, u64* out) {
    KObj* o = h_get_ref(h, 0);
    int ok = o && (o->type == KO_PROCESS || o->type == KO_THREAD);
    kobj_unref(o);
    R_UNLESS(ok, R_BADHANDLE);
    *out = K_PROCESS_ID;
    R_SUCCEED();
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
    R_UNLESS(which <= 4, R_BADENUM);
    R_UNLESS(h_get_type(h, KO_RESLIMIT), R_BADHANDLE);
    if (kind == 0)
        *out = which ? limit[which] : mem_total();
    else if (which == 0)
        *out = used_memory();
    else
        *out = which == 1 ? (u64)kthread_live_count() : 0;
    R_SUCCEED();
}
/* Mesosphère's InfoType_MesosphereCurrentProcess (Atmosphère's kernel, read for its behavior): a new handle to the
   calling process, which mods for Atmosphère take before patching their own code (exlaunch's cur_proc_handle) */
#define INFO_CURRENT_PROCESS 65001u
static u32 get_info(u32 id0, u32 h, u64 id1, u64* out) {
    if (id0 == INFO_CURRENT_PROCESS) {
        R_UNLESS(!h, R_BADHANDLE);
        R_UNLESS(!id1, R_BADCOMBO);
        u32 ph = h_new(&g_hle.kernel.process); /* the handle's own reference */
        R_UNLESS(ph, R_OUTOFHANDLES);
        *out = ph;
        R_SUCCEED();
    }
    if (id0 != 9) return svc_get_info(id0, h, id1, out);
    R_UNLESS(!h, R_BADHANDLE); /* ResourceLimit: always succeeds after its checks */
    R_UNLESS(!id1, R_BADCOMBO);
    *out = h_new(&g_hle.svc.reslimit);
    R_SUCCEED();
}

/* ============================================================ the dispatch */
/* Each SVC's registers in both ABIs come from svcgen (hos.svc): g_svc64[imm] takes them from a 64-bit thread's x
   registers, which the NX modules call by number through RtAPI.svcs, and g_svc32[imm] from a 32-bit one's r registers.
   Both call the SVC's handler below, svc_<Name>, with the declaration's arguments, between svc_begin and svc_end */
#define SVC_QUIET  1 /* left out of the crash report's ring: a polling loop would fill it */
#define SVC_NOWARN 2 /* x0 out isn't a result, or InvalidHandle is the SVC's usual answer (CloseHandle) */

/* the SVC's record in the ring and its entry: the record's number */
static u32 svc_begin(KThread* t, SvcRec* in, u32 imm, int flags) {
    u32 n = 0;
    in->imm = imm;
    in->res = 0;
    in->o1 = 0;
    if (!(flags & SVC_QUIET)) {
        n = __atomic_fetch_add(&g_hle.kernel.svc_ring_n, 1, __ATOMIC_RELAXED);
        g_hle.kernel.svc_ring[n & 63] = *in;
    }
    svc_enter(t);
    return n;
}
static u32 svc_begin64(KThread* t, SvcRec* in, u32 imm, const CPU* c, int flags) {
    in->lr = c->x[30];
    memcpy(in->a, c->x, sizeof in->a);
    return svc_begin(t, in, imm, flags);
}
static u32 svc_begin32(KThread* t, SvcRec* in, u32 imm, const SnxArmCPU* a, int flags) {
    int i;
    in->lr = a->r[14];
    for (i = 0; i < 4; i++) in->a[i] = a->r[i];
    return svc_begin(t, in, imm, flags);
}
/* the SVC's exit, with its first two output registers: the invalid-handle warning, and the result in its record */
static void svc_end(KThread* t, const SvcRec* in, u32 n, int flags, u64 res, u64 o1) {
    svc_leave(t);
    if (!(flags & SVC_NOWARN) && (u32)res == R_BADHANDLE)
        WARN_LIMIT(20, "svc %02x with an invalid handle (x0=%llx x1=%llx x2=%llx x3=%llx), lr %llx\n", in->imm,
                   (unsigned long long)in->a[0], (unsigned long long)in->a[1], (unsigned long long)in->a[2],
                   (unsigned long long)in->a[3], (unsigned long long)in->lr);
    if (!(flags & SVC_QUIET) && __atomic_load_n(&g_hle.kernel.svc_ring_n, __ATOMIC_RELAXED) - n < 64) { /* not another SVC's yet */
        g_hle.kernel.svc_ring[n & 63].res = (u32)res;
        g_hle.kernel.svc_ring[n & 63].o1 = o1;
    }
}

#include "hle/svc/hos.svc.inc"

/* every SVC svcgen has no handler for, a stub's or an unknown number's: NotImplemented, logged */
static void not_implemented(u32 imm, u64 a0, u64 a1, u64 a2) {
    const char* name = imm < SNX_SVC_COUNT && g_svc_name[imm] ? g_svc_name[imm] : "unknown";
    WARN_LIMIT(8, "svc %02x (%s): not implemented (x0=%llx x1=%llx x2=%llx), NotImplemented\n", imm, name,
               (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2);
}
static void svc64_unknown(CPU* c, u32 imm) {
    KThread* t = (KThread*)c->thread;
    SvcRec in;
    u32 n = svc_begin64(t, &in, imm, c, 0);
    SNX_ZONE(svc_zone, "unknown");
    not_implemented(imm, c->x[0], c->x[1], c->x[2]);
    c->x[0] = R_NOTIMPL;
    SNX_ZONE_END(svc_zone);
    svc_end(t, &in, n, 0, c->x[0], c->x[1]);
}
static void svc32_unknown(SnxArmCPU* a, u32 imm) {
    KThread* t = kthread_cur();
    SvcRec in;
    u32 n = svc_begin32(t, &in, imm, a, 0);
    SNX_ZONE(svc_zone, "unknown");
    not_implemented(imm, a->r[0], a->r[1], a->r[2]);
    a->r[0] = R_NOTIMPL;
    SNX_ZONE_END(svc_zone);
    svc_end(t, &in, n, 0, a->r[0], a->r[1]);
}

void snx_svc(CPU* c, u32 imm) { (imm < SNX_SVC_COUNT ? g_svc64[imm] : svc64_unknown)(c, imm); }
void snx_svc32(SnxArmCPU* a, u32 imm) { (imm < SNX_SVC_COUNT ? g_svc32[imm] : svc32_unknown)(a, imm); }

/* ============================================================ the handlers, in hos.svc's order */
static u32 svc_SetHeapSize(u64* out_address, u64 size) {
    u32 r = svc_set_heap_size(size);
    if (!r) *out_address = HEAP_BASE;
    return r;
}
static u32 svc_SetMemoryPermission(u64 address, u64 size, u32 perm) { return svc_set_mem_perm(address, size, perm); }
static u32 svc_SetMemoryAttribute(u64 address, u64 size, u32 mask, u32 attr) {
    return svc_set_mem_attr(address, size, mask, attr);
}
static u32 svc_MapMemory(u64 dst_address, u64 src_address, u64 size) {
    return svc_map_memory(dst_address, src_address, size);
}
static u32 svc_UnmapMemory(u64 dst_address, u64 src_address, u64 size) {
    return svc_unmap_memory(dst_address, src_address, size);
}
static u32 svc_MapProcessMemory(u64 dst_address, u32 process_handle, u64 src_address, u64 size) {
    return svc_map_process_memory(dst_address, process_handle, src_address, size);
}
static u32 svc_UnmapProcessMemory(u64 dst_address, u32 process_handle, u64 src_address, u64 size) {
    return svc_unmap_process_memory(dst_address, process_handle, src_address, size);
}
static u32 svc_QueryMemory(u64 out_memory_info, u32* out_page_info, u64 address) {
    *out_page_info = 0; /* no flags */
    return svc_query(out_memory_info, address);
}
static void svc_ExitProcess(void) { process_exit(0); }
static u32 svc_CreateThread(u32* out_handle, u64 func, u64 arg, u64 stack_bottom, i32 priority, i32 core_id) {
    return create_thread(func, arg, stack_bottom, priority, core_id, out_handle);
}
static u32 svc_StartThread(u32 thread_handle) { return start_thread(thread_handle); }
static void svc_ExitThread(void) { svc_exit_thread(kthread_cur()); }
static void svc_SleepThread(i64 ns) { sleep_thread(kthread_cur(), ns); }
static u32 svc_GetThreadPriority(i32* out_priority, u32 thread_handle) {
    KThread* o = thread_of(thread_handle);
    R_UNLESS(o, R_BADHANDLE);
    *out_priority = o->prio;
    thread_put(o);
    R_SUCCEED();
}
static u32 svc_SetThreadPriority(u32 thread_handle, i32 priority) {
    return set_thread_priority(thread_handle, priority);
}
static u32 svc_GetThreadCoreMask(i32* out_core_id, u64* out_affinity_mask, u32 thread_handle) {
    KThread* o = thread_of(thread_handle);
    R_UNLESS(o, R_BADHANDLE);
    *out_core_id = o->ideal_core;
    *out_affinity_mask = o->affinity;
    thread_put(o);
    R_SUCCEED();
}
static u32 svc_SetThreadCoreMask(u32 thread_handle, i32 core_id, u64 affinity_mask) {
    return set_thread_core_mask(kthread_cur(), thread_handle, core_id, affinity_mask);
}
static i32 svc_GetCurrentProcessorNumber(void) { return kthread_cur()->core; }
static u32 svc_SignalEvent(u32 event_handle) { return signal_event(event_handle); }
static u32 svc_ClearEvent(u32 event_handle) { return clear_event(event_handle, 0); }
static u32 svc_MapSharedMemory(u32 shmem_handle, u64 address, u64 size, u32 map_perm) {
    return svc_map_shm(shmem_handle, address, size, map_perm);
}
static u32 svc_UnmapSharedMemory(u32 shmem_handle, u64 address, u64 size) {
    return svc_unmap_shm(shmem_handle, address, size);
}
static u32 svc_CreateTransferMemory(u32* out_handle, u64 address, u64 size, u32 map_perm) {
    KTmem* m = NULL;
    R_TRY(svc_tmem_new(address, size, map_perm, &m));
    return give_handle(&m->o, out_handle);
}
static u32 svc_CloseHandle(u32 handle) { return (u32)h_close(handle); }
static u32 svc_ResetSignal(u32 handle) { return clear_event(handle, 1); }
static u32 svc_WaitSynchronization(i32* out_index, u64 handles, i32 num_handles, i64 timeout_ns) {
    u32 idx = ~0u, r = svc_wait_sync(kthread_cur(), handles, (u32)num_handles, timeout_ns, &idx);
    *out_index = (i32)idx;
    return r;
}
static u32 svc_CancelSynchronization(u32 handle) { return cancel_sync(handle); }
static u32 svc_ArbitrateLock(u32 thread_handle, u64 address, u32 tag) {
    return svc_arbitrate_lock(kthread_cur(), thread_handle, address, tag);
}
static u32 svc_ArbitrateUnlock(u64 address) { return svc_arbitrate_unlock(address); }
static u32 svc_WaitProcessWideKeyAtomic(u64 address, u64 cv_key, u32 tag, i64 timeout_ns) {
    return svc_wait_cv(kthread_cur(), address, cv_key, tag, timeout_ns);
}
static void svc_SignalProcessWideKey(u64 cv_key, i32 count) { svc_signal_cv(cv_key, count); }
static i64 svc_GetSystemTick(void) { return (i64)guest_ticks(); }
static u32 svc_ConnectToNamedPort(u32* out_handle, u64 name) { return connect_port(name, out_handle); }
/* the request in the thread's TLS, its 0x100 bytes */
static u32 svc_SendSyncRequest(u32 session_handle) {
    KThread* t = kthread_cur();
    return send_request(t, session_handle, t->tls, 0x100);
}
static u32 svc_SendSyncRequestWithUserBuffer(u64 message_buffer, u64 message_buffer_size, u32 session_handle) {
    R_TRY(ipc_user_buffer(message_buffer, message_buffer_size));
    return send_request(kthread_cur(), session_handle, message_buffer, message_buffer_size);
}
static u32 svc_GetProcessId(u64* out_process_id, u32 process_handle) {
    return get_process_id(process_handle, out_process_id);
}
static u32 svc_GetThreadId(u64* out_thread_id, u32 thread_handle) {
    KThread* o = thread_of(thread_handle);
    R_UNLESS(o, R_BADHANDLE);
    *out_thread_id = o->id;
    thread_put(o);
    R_SUCCEED();
}
static void svc_Break(u32 break_reason, u64 arg, u64 size) { svc_break(&kthread_cur()->cpu, break_reason, arg, size); }
static u32 svc_OutputDebugString(u64 debug_str, u64 len) {
    output_debug_string(debug_str, len);
    R_SUCCEED();
}
/* with no exception handler, the console ends the process */
static void svc_ReturnFromException(u32 result) {
    WARN("\n*** The game returned from an exception it had no handler for (%x) ***\n", result);
    kernel_dump_regs(&kthread_cur()->cpu);
    save_hints();
    fatal("The game stopped (ReturnFromException %x). See the .log for details.", result);
}
static u32 svc_GetInfo(u64* out, u32 info_type, u32 handle, u64 info_subtype) {
    return get_info(info_type, handle, info_subtype, out);
}
static void svc_FlushEntireDataCache(void) {}
/* guest memory is the host's, coherent */
static u32 svc_FlushDataCache(u64 address, u64 size) {
    (void)address;
    (void)size;
    R_SUCCEED();
}
static u32 svc_MapPhysicalMemory(u64 address, u64 size) { return svc_map_physical(address, size); }
static u32 svc_UnmapPhysicalMemory(u64 address, u64 size) { return svc_unmap_physical(address, size); }
static u32 svc_GetResourceLimitLimitValue(i64* out_limit_value, u32 resource_limit_handle, u32 which) {
    return resource_limit(resource_limit_handle, which, 0, (u64*)out_limit_value);
}
static u32 svc_GetResourceLimitCurrentValue(i64* out_current_value, u32 resource_limit_handle, u32 which) {
    return resource_limit(resource_limit_handle, which, 1, (u64*)out_current_value);
}
static u32 svc_SetThreadActivity(u32 thread_handle, u32 thread_activity) {
    return set_thread_activity(kthread_cur(), thread_handle, thread_activity);
}
static u32 svc_GetThreadContext3(u64 out_context, u32 thread_handle) {
    return get_thread_context(kthread_cur(), out_context, thread_handle);
}
static u32 svc_WaitForAddress(u64 address, u32 arb_type, i32 value, i64 timeout_ns) {
    return svc_wait_addr(kthread_cur(), address, arb_type, value, timeout_ns);
}
static u32 svc_SignalToAddress(u64 address, u32 signal_type, i32 value, i32 count) {
    return svc_signal_addr(address, signal_type, value, count);
}
static void svc_SynchronizePreemptionState(void) {}
static u32 svc_GetResourceLimitPeakValue(i64* out_peak_value, u32 resource_limit_handle, u32 which) {
    return resource_limit(resource_limit_handle, which, 2, (u64*)out_peak_value);
}
static void svc_KernelDebug(u32 kern_debug_type, u64 arg0, u64 arg1, u64 arg2) {
    (void)kern_debug_type;
    (void)arg0;
    (void)arg1;
    (void)arg2;
}
static void svc_ChangeKernelTraceState(u32 kern_trace_state) { (void)kern_trace_state; }
static u32 svc_CreateEvent(u32* out_write_handle, u32* out_read_handle) {
    return create_event(out_write_handle, out_read_handle);
}
static u32 svc_CreateCodeMemory(u32* out_handle, u64 address, u64 size) {
    KObj* o = NULL;
    R_TRY(svc_codemem_new(address, size, &o));
    return give_handle(o, out_handle);
}
static u32 svc_ControlCodeMemory(u32 code_memory_handle, u32 operation, u64 address, u64 size, u32 perm) {
    KObj* o = h_get_ref(code_memory_handle, KO_CODEMEM);
    u32 r = o ? svc_codemem_control(o, operation, address, size, perm) : R_BADHANDLE;
    kobj_unref(o);
    return r;
}
/* the process data cache operations: guest memory is the host's, coherent */
static u32 svc_InvalidateProcessDataCache(u32 process_handle, u64 address, u64 size) {
    (void)process_handle;
    (void)address;
    (void)size;
    R_SUCCEED();
}
static u32 svc_StoreProcessDataCache(u32 process_handle, u64 address, u64 size) {
    return svc_InvalidateProcessDataCache(process_handle, address, size);
}
static u32 svc_FlushProcessDataCache(u32 process_handle, u64 address, u64 size) {
    return svc_InvalidateProcessDataCache(process_handle, address, size);
}
