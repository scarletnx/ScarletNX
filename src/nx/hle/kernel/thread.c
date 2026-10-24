/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* guest threads (each on a host thread of its own), the thread table, process exit, and the four cores the threads
   take turns on */
#include "common/utilities/bits.h"
#include "nx/hle/kernel/kernel_int.h"
#include <errno.h>
#include <string.h>
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/hle/hle.h"

/* ============================================================ the cores */
/* The cores' state is g_hle.thread's core_* (kernel_int.h has how they are ordered and locked).

   A console core runs nothing but its own threads, so a thread that wakes another core's thread keeps running at once,
   and the woken one starts beside it. On the host every thread may take any CPU, and the host scheduler may put the
   woken thread on the waker's CPU and run it there in the waker's place until it blocks or a tick passes: a game's job
   worker then stops right after the unlock that wakes the main thread, which reuses a job the worker is not done
   with. So each core the game may use gets a share of the host CPUs (every n-th one, n the game's cores),
   apart from the other cores' shares, and a thread's host thread runs on the share of the core it holds */

/* the core a thread would run on, kept within the cores the runtime has */
static int core_of(const KThread* t) { return t->core >= 0 && t->core < NCORES ? t->core : 0; }
/* the lowest core of t's mask that nobody holds, or -1 */
static int core_idle_nl(const KThread* t) {
    int j;
    for (j = 0; j < NCORES; j++)
        if (((t->affinity >> j) & 1) && !g_hle.thread.core_owner[j]) return j;
    return -1;
}
/* the holder of core k is asked to let go (CPU.poll, read by generated code) when a waiter would preempt it, and while
   the game is paused, so that it reaches a hold point. Also when its core has a waiter and its mask an idle core: it
   moves there, as the console's scheduler does at any priority */
/* stored only when it changes: the holder's code reads poll at each backward branch, and a store of the same value
   still takes its cache line away from the holder's core */
static void core_flag_nl(int k) {
    KThread* o = kt_at(g_hle.thread.core_owner[k]);
    u32 v;
    if (!o) return;
    v = __atomic_load_n(&g_hle.thread.hold, __ATOMIC_RELAXED) || g_hle.thread.core_best[k] < o->prio ||
                (g_hle.thread.core_wait[k] && core_idle_nl(o) >= 0)
            ? 1u
            : 0u;
    if (__atomic_load_n(&o->cpu.poll, __ATOMIC_RELAXED) != v) __atomic_store_n(&o->cpu.poll, v, __ATOMIC_RELAXED);
}
static void core_rebest_nl(int k) {
    KThread* w;
    int best = 64;
    for (w = kt_at(g_hle.thread.core_wait[k]); w; w = kt_at(w->core_next))
        if (w->prio < best) best = w->prio;
    g_hle.thread.core_best[k] = best;
}
static void core_unlink_nl(int k, KThread* t) {
    u32* l = &g_hle.thread.core_wait[k];
    u32 i = kt_ix(t);
    while (*l && *l != i) l = &kt_at(*l)->core_next;
    if (*l) *l = t->core_next;
    t->core_next = 0;
    core_rebest_nl(k);
}
/* whether a waits ahead of b */
static int core_before(const KThread* a, const KThread* b) {
    return a->prio < b->prio || (a->prio == b->prio && a->core_ticket < b->core_ticket);
}
/* t, which held core k since core_give_nl, lets go of it: the time goes to the report at exit (kernel_core_report) */
static void core_held_nl(int k, KThread* t) {
    u64 held = plat_ns() - g_host.thread.core_since[k];
    g_host.thread.core_held_ns[k] += held;
    t->core_held_ns += held;
}
/* t holds core k from now on, and the host thread waiting for it (if any) goes on */
static void core_give_nl(int k, KThread* t) {
    g_hle.thread.core_owner[k] = kt_ix(t);
    g_host.thread.core_since[k] = plat_ns();
    t->core = k;
    t->on_core = k;
    __atomic_store_n(&t->cpu.poll, 0u, __ATOMIC_RELAXED);
    core_flag_nl(k);
    pc_signal(kt_cv(t));
}
/* core k has just fallen free: its best waiter takes it. With none, it takes the best thread waiting for another core
   that its mask lets run here, unless that thread is about to preempt its own core's holder, as the console's
   scheduler moves a ready thread to a core that would otherwise idle */
static void core_handoff_nl(int k) {
    KThread* best = NULL;
    KThread* w;
    int j, from = k;
    for (w = kt_at(g_hle.thread.core_wait[k]); w; w = kt_at(w->core_next))
        if (!best || core_before(w, best)) best = w;
    if (!best)
        for (j = 0; j < NCORES; j++) {
            if (j == k || !g_hle.thread.core_owner[j]) continue;
            for (w = kt_at(g_hle.thread.core_wait[j]); w; w = kt_at(w->core_next))
                if (((w->affinity >> k) & 1) && w->prio >= kt_at(g_hle.thread.core_owner[j])->prio &&
                    (!best || core_before(w, best))) {
                    best = w;
                    from = j;
                }
        }
    if (!best) { /* a holder with a waiter elsewhere may move here */
        for (j = 0; j < NCORES; j++)
            if (j != k && g_hle.thread.core_wait[j]) core_flag_nl(j);
        return;
    }
    core_unlink_nl(from, best);
    if (from != k) core_flag_nl(from);
    core_give_nl(k, best);
}
/* t waits on core k's list until a holder hands the core over (it may be another core by then) */
static void core_wait_nl(KThread* t, int k) {
    t->core = k;
    t->on_core = -2;
    t->core_ticket = ++g_hle.thread.core_ticket;
    t->core_next = g_hle.thread.core_wait[k];
    g_hle.thread.core_wait[k] = kt_ix(t);
    if (t->prio < g_hle.thread.core_best[k]) g_hle.thread.core_best[k] = t->prio;
    core_flag_nl(k);
    u64 t0 = plat_ns(), waited;
    while (t->on_core == -2) pc_wait(kt_cv(t), &g_host.thread.core_lk, -1);
    waited = plat_ns() - t0;
    t->core_wait_ns += waited;
    t->core_waits++;
    g_host.thread.core_wait_ns[k] += waited;
}
/* t (holding no core) takes one: its own if free; with a holder it would preempt, it waits there and flags the holder;
   otherwise an idle core of its mask, or a place in its own core's line */
static void core_take_nl(KThread* t) {
    int k = core_of(t), j;
    if (!g_hle.thread.core_owner[k]) {
        core_give_nl(k, t);
        return;
    }
    if (t->prio >= kt_at(g_hle.thread.core_owner[k])->prio && (j = core_idle_nl(t)) >= 0) {
        core_give_nl(j, t);
        return;
    }
    core_wait_nl(t, k);
}
/* t lets go of the core it holds, which goes to the next thread */
static void core_release_nl(KThread* t) {
    int k = t->on_core;
    t->on_core = -1;
    __atomic_store_n(&t->cpu.poll, 0u, __ATOMIC_RELAXED);
    if (g_hle.thread.core_owner[k] == kt_ix(t)) {
        g_hle.thread.core_owner[k] = 0;
        core_held_nl(k, t);
    }
    core_handoff_nl(k);
}
/* after t took a core: its host thread moves to the core's share of the host CPUs, if it was on another one. Fewer
   host CPUs than cores leave nothing to share out */
static void core_pin(KThread* t) {
    int k = t->on_core, n = snx_popcount64(g_hle.svcmem.core_mask), cpus[256], m = 0, len = 0;
    char list[256] = "";
    if (k == t->pinned) return;
    t->pinned = k;
    if (g_hle.thread.host_ncpu < n || !((g_hle.svcmem.core_mask >> k) & 1)) return;
    for (int i = snx_popcount64(g_hle.svcmem.core_mask & ((1ull << k) - 1)); i < g_hle.thread.host_ncpu; i += n)
        cpus[m++] = g_hle.thread.host_cpu[i];
    plat_thread_cpus(cpus, m);
    if (__atomic_exchange_n(&g_hle.thread.pin_said[k], 1, __ATOMIC_RELAXED)) return;
    for (int i = 0; i < m && len < (int)sizeof list - 8; i++)
        len += snprintf(list + len, sizeof list - len, " %d", cpus[i]);
    LOG("kernel: core %d runs on host CPUs%s\n", k, list);
}
void core_enter(KThread* t) {
    if (!t || !g_settings.emulated_cores) return;
    pm_lock(&g_host.thread.core_lk);
    if (t->on_core < 0) core_take_nl(t);
    pm_unlock(&g_host.thread.core_lk);
    core_pin(t);
}
void core_leave(KThread* t) {
    if (!t || !g_settings.emulated_cores) return;
    pm_lock(&g_host.thread.core_lk);
    if (t->on_core >= 0) core_release_nl(t);
    pm_unlock(&g_host.thread.core_lk);
}
/* every waiter that would preempt t has flagged it, so an unflagged holder has nothing to do here: that is the path of
   every SVC */
void core_preempt(KThread* t) {
    int k, j;
    if (!t || !__atomic_load_n(&t->cpu.poll, __ATOMIC_RELAXED)) return;
    pm_lock(&g_host.thread.core_lk);
    if (!g_settings.emulated_cores) { /* only a game pause sets poll: kernel_hold, under core_lk */
        if (!__atomic_load_n(&g_hle.thread.hold, __ATOMIC_RELAXED))
            __atomic_store_n(&t->cpu.poll, 0u, __ATOMIC_RELAXED);
        pm_unlock(&g_host.thread.core_lk);
        return;
    }
    k = t->on_core;
    if (k >= 0 && g_hle.thread.core_best[k] < t->prio) {
        core_release_nl(t);
        core_take_nl(t);
    } else if (k >= 0 && g_hle.thread.core_wait[k] && (j = core_idle_nl(t)) >= 0) {
        core_release_nl(t); /* the waiter takes k, t the idle core */
        core_give_nl(j, t);
    } else
        __atomic_store_n(&t->cpu.poll, 0u, __ATOMIC_RELAXED);
    pm_unlock(&g_host.thread.core_lk);
    core_pin(t);
}
void core_poll(KThread* t) {
    core_preempt(t);
    if (kthread_hold_point(t, PARK_RUN)) core_enter(t);
}
/* the thread a yield with core migration moves to core k on the console (Mesosphère's
   KScheduler::YieldWithCoreMigration): one waiting for another core that core k is in the mask of, of the yielding
   thread's priority or better, the best first. Not from a core whose holder is of priority 0 or 1, which nothing is
   moved away from (HighestCoreMigrationAllowedPriority). NULL for none, else its core in *from */
static KThread* core_suggested_nl(const KThread* t, int k, int* from) {
    KThread *best = NULL, *w;
    int j;
    for (j = 0; j < NCORES; j++) {
        if (j == k || (g_hle.thread.core_owner[j] && kt_at(g_hle.thread.core_owner[j])->prio < 2)) continue;
        for (w = kt_at(g_hle.thread.core_wait[j]); w; w = kt_at(w->core_next))
            if (((w->affinity >> k) & 1) && w->prio <= t->prio && (!best || core_before(w, best))) {
                best = w;
                *from = j;
            }
    }
    return best;
}
/* svcSleepThread's yields. With waiters for its core, the core goes to the best of them, whatever its priority, and t
   lines up behind the waiters of its own priority. A waiter of worse priority that took the core gives it back at its
   next poll: a yield doesn't hand a better thread's time to a worse one. migrate (2, for -1 and -2) also lets it run
   on an idle core of its mask meanwhile. With none, a migrating yield takes a thread waiting for another core onto
   this one and lines up behind it, as the console does: a thread that polls with svcSleepThread(-1) lends its core to
   threads its core could run, which otherwise wait for a busy core of their own. Without that either, t goes on at
   once */
void core_yield(KThread* t, int migrate) {
    int k, j;
    KThread* w;
    if (!t) return;
    if (!g_settings.emulated_cores) { /* the host's threads are the cores: let it run another */
        plat_yield();
        return;
    }
    pm_lock(&g_host.thread.core_lk);
    k = t->on_core;
    if (k >= 0 && g_hle.thread.core_wait[k]) {
        core_release_nl(t);
        if (migrate && (j = core_idle_nl(t)) >= 0)
            core_give_nl(j, t);
        else
            core_wait_nl(t, k);
    } else if (k >= 0 && migrate && (w = core_suggested_nl(t, k, &j))) {
        core_unlink_nl(j, w);
        core_flag_nl(j);
        t->on_core = -1;
        __atomic_store_n(&t->cpu.poll, 0u, __ATOMIC_RELAXED);
        core_held_nl(k, t);
        core_give_nl(k, w); /* not core_release_nl: its hand-off could pick another thread */
        core_wait_nl(t, k);
    }
    pm_unlock(&g_host.thread.core_lk);
    core_pin(t);
}
/* a waiter whose priority changes goes behind the waiters of its new priority, as a thread is requeued on the
   console; a holder is flagged if a waiter now beats it */
void core_set_prio(KThread* t, int prio) {
    pm_lock(&g_host.thread.core_lk);
    t->prio = prio;
    if (t->on_core == -2) {
        int k = core_of(t);
        t->core_ticket = ++g_hle.thread.core_ticket;
        core_rebest_nl(k);
        core_flag_nl(k);
    } else if (t->on_core >= 0)
        core_flag_nl(t->on_core);
    pm_unlock(&g_host.thread.core_lk);
}

/* ============================================================ threads */
_Thread_local void* tls_thread;
KThread* kthread_cur(void) { return (KThread*)tls_thread; }

/* the host's CPUs, which the cores pin to (the locks and the cores' wait lists start in hle_init) */
void kernel_init(void) {
    g_hle.thread.host_ncpu = plat_cpus(g_hle.thread.host_cpu, 256);
}

/* Each thread's thread-local storage is 0x200 bytes (the IPC message buffer in the first 0x100), eight to a page, as
   the console hands them out; TPIDRRO_EL0 (TPIDRURO for a 32-bit thread) points at it. Under g_klock. A freed
   thread's TLS isn't handed out again, as it wasn't while threads were never freed */
static u64 tls_alloc_nl(void) {
    HleThread* h = &g_hle.thread;
    if (h->tls_used == 8) {
        h->tls_page = kmem_alloc(0x1000, MS_TLS, PERM_RW);
        h->tls_used = 0;
    }
    return h->tls_page + 0x200 * (u64)h->tls_used++;
}

/* the threads in the pool that haven't exited. Under g_klock, which their states change under, and the pool's lock,
   so no slot is freed or taken meanwhile */
int kthread_live_count(void) {
    int i, n = 0;
    pm_lock(&g_host.kernel.klock);
    pm_lock(&g_host.kernel.pool_lk);
    for (i = 0; i < KTHREAD_MAX; i++)
        if (g_hle.thread.threads[i].o.type == KO_THREAD && g_hle.thread.threads[i].state != 2) n++;
    pm_unlock(&g_host.kernel.pool_lk);
    pm_unlock(&g_host.kernel.klock);
    return n;
}

/* The last reference: the thread's handles are closed and its host thread let go of it (svc_exit_thread), or it never
   started. Nothing else can reach it: every wait on it held a reference (WaitSynchronization's, a mutex waiter's on its
   owner, a pause's through its handle), it holds no core and waits on nothing. It may come with g_klock held (a mutex
   waiter's reference to its owner goes as the mutex changes hands), so it takes only the pool's lock */
void kthread_destroy(KObj* o) {
    pm_lock(&g_host.kernel.pool_lk);
    POOL_PUT((KThread*)o);
    pm_unlock(&g_host.kernel.pool_lk);
}

/* a thread that hasn't started, entering at entry with arg in x0 (r0) and sp at stack_top, its return address 0, from
   g_hle.thread.threads (KTHREAD_MAX). It stays valid while a reference to it is held */
KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core) {
    KThread* t;
    pm_lock(&g_host.kernel.pool_lk);
    t = (KThread*)POOL_TAKE(g_hle.thread.threads, g_hle.thread.thread_next, "threads");
    t->o.type = KO_THREAD;
    pm_unlock(&g_host.kernel.pool_lk);
    kobj_set_name(&t->o, "thread");
    t->prio = t->base_prio = prio;
    t->core = t->ideal_core = core;
    t->affinity = 1ull << core;
    t->on_core = -1;
    t->pinned = -1;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    pc_init(kt_cv(t));
    pm_lock(&g_host.kernel.klock);
    t->id = g_hle.kernel.next_tid++;
    t->tls = tls_alloc_nl();
    pm_unlock(&g_host.kernel.klock);
    if (kthread_live_count() > THREAD_MAX)
        WARN_LIMIT(1, "kernel: more than %d threads, the console's limit\n", THREAD_MAX);

    t->cpu.mem = g_mem;
    t->cpu.disp = g_disp;
    t->cpu.code_lo = g_code_lo;
    t->cpu.code_hi = g_code_hi;
    t->cpu.rt = &g_rtapi;
    t->cpu.thread = t;
    t->cpu.tpidrro = t->tls;
    t->cpu.pc = entry;
    t->cpu.x[0] = arg;
    t->cpu.x[31] = stack_top;
    if (g_arm_mod) {
        SnxArmCPU* a = &t->arm;
        a->r[0] = (u32)arg;
        a->r[13] = (u32)stack_top;
        a->r[15] = (u32)entry & ~1u;
        a->cpsr = 0x10u | (entry & 1 ? SNX_ARM_T : 0u); /* user mode, Thumb for an odd entry address */
        a->mem = g_mem;
        a->mem_size = 1ull << 32;
        a->tpidruro = (u32)t->tls;
        a->rt = &g_arm_rt;
    }
    return t;
}

/* the host thread's stack is thread_stack_mb's: the recompiled code makes each guest call a host call, so it has to
   hold as many frames as the guest's stack does, and more bytes for each */
/* A guest RET sets the pc and returns, and a call whose return address isn't that pc returns too (codegen.c checks
   c->pc against pc + 4 after each BL), so a return past the host frames, like libnx's exit resetting the stack and
   returning to the LR crt0 saved, comes back here with the pc where the guest goes on. Only 0, the entry's own return
   address, means the entry function returned */
static void thread_run(KThread* t, u64 pc) {
    core_enter(t);
    snx_fpcr_apply(&t->cpu);
    if (g_arm_mod)
        arm_thread_run(t);
    else
        do snx_lookup(&t->cpu, pc)(&t->cpu, pc);
        while ((pc = t->cpu.pc) != 0);
    svc_exit_thread(t); /* its entry function returned: there is nothing to return to */
}
static void thread_main(void* p) {
    KThread* t = (KThread*)p;
    tls_thread = t;
    SNX_THREAD_NAME(t->name[0] ? t->name : "guest");
    __atomic_add_fetch(&g_host.thread.nhost, 1, __ATOMIC_ACQ_REL);
    thread_run(t, t->entry);
}
/* the host thread holds a reference from here until svc_exit_thread */
void kthread_start(KThread* t) {
    kobj_ref(&t->o);
    pm_lock(&g_host.kernel.klock);
    t->state = 1;
    pm_unlock(&g_host.kernel.klock);
    LOGV("StartThread %llu entry=%llx prio=%d core=%d\n", (unsigned long long)t->id, (unsigned long long)t->entry,
         t->prio, t->core);
    {
        size_t stack = (size_t)g_settings.thread_stack_mb << 20;
        if (plat_thread(thread_main, t, stack)) {
            int reason = errno; /* plat_thread's */
            u64 mapped = 0, hole = 0;
            if (plat_address_space(&mapped, &hole))
                LOG("thread: a host thread of a %llu MiB stack (thread_stack_mb) could not start, %llu MiB of the "
                    "address space mapped, its largest free run %llu MiB\n",
                    (unsigned long long)(stack >> 20), (unsigned long long)(mapped >> 20),
                    (unsigned long long)(hole >> 20));
            fatal("Could not start a host thread for guest thread %llu: %s", (unsigned long long)t->id,
                  strerror(reason));
        }
    }
}
/* svcExitThread: the thread's waiters (joins) are woken, and its core goes to the next thread. Then its host thread
   lets go of it: its last use of the KThread, which may be freed with that reference */
void svc_exit_thread(KThread* t) {
    pm_lock(&g_host.kernel.klock);
    t->state = 2;
    kobj_signal_nl(&t->o);
    pm_unlock(&g_host.kernel.klock);
    LOGV("ExitThread %llu\n", (unsigned long long)t->id);
    core_leave(t);
    kobj_unref(&t->o);
    __atomic_sub_fetch(&g_host.thread.nhost, 1, __ATOMIC_ACQ_REL);
    plat_thread_exit();
}

/* ============================================================ game pauses */
/* A game pause is not svcSetThreadActivity's: the game's own pause (suspended) stays as the game set it underneath,
   and a thread is stopped for a game pause when it runs nothing until the pause ends. That is a thread at a hold point
   (here, at a poll, at either end of an SVC, after a JIT compile), one blocked in a wait whose wake during the pause
   waits for its end too (sleep_nl, kthread_wait), and one the game paused itself, at its pause point. A thread that
   runs game code has its poll set until it gets to a hold point; one inside an SVC gets to svc_leave's, or blocks.
   stopped is stored before hold is read on the thread's side, and hold before stopped is read on the pauser's (all
   sequentially consistent), so a thread whose wait ends as the pause begins is never counted stopped while it goes on */
int kthread_hold_point(KThread* t, int park) {
    if (!t || !__atomic_load_n(&g_hle.thread.hold, __ATOMIC_SEQ_CST)) return 0;
    core_leave(t);
    pm_lock(&g_host.kernel.klock);
    t->park = park;
    __atomic_store_n(&t->stopped, 2, __ATOMIC_SEQ_CST);
    while (g_hle.thread.hold) {
        kthread_retire_check(&g_host.kernel.klock);
        pc_wait(kt_cv(t), &g_host.kernel.klock, -1);
    }
    __atomic_store_n(&t->stopped, 0, __ATOMIC_SEQ_CST);
    pm_unlock(&g_host.kernel.klock);
    return 1;
}
void kthread_block(KThread* t) {
    if (t) __atomic_store_n(&t->stopped, 1, __ATOMIC_SEQ_CST);
}
void kthread_unblock(KThread* t) {
    if (!t) return;
    __atomic_store_n(&t->stopped, 0, __ATOMIC_SEQ_CST);
    kthread_hold_point(t, PARK_RUN); /* rt_miss set the pc it goes on at */
}
void kthread_commit(void) {
    KThread* t = kthread_cur();
    if (t) g_host.thread.committed[kt_ix(t) - 1] = 1;
}
int kthread_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    KThread* t = kthread_cur();
    u32 slot;
    int r;
    if (!t) return pc_wait(c, m, timeout_ns);
    slot = kt_ix(t) - 1;
    if (g_host.thread.committed[slot]) return pc_wait(c, m, timeout_ns); /* not stopped: a pause waits for it */
    g_host.thread.waiting_on[slot].c = c; /* where kernel_retire wakes it */
    g_host.thread.waiting_on[slot].m = m;
    t->park = PARK_SERVICE;
    __atomic_store_n(&t->stopped, 1, __ATOMIC_SEQ_CST);
    r = pc_wait(c, m, timeout_ns);
    __atomic_store_n(&t->stopped, 0, __ATOMIC_SEQ_CST);
    kthread_retire_check(m);
    if (__atomic_load_n(&g_hle.thread.hold, __ATOMIC_SEQ_CST)) {
        pm_unlock(m);
        kthread_hold_point(t, PARK_SERVICE);
        pm_lock(m);
        r = 0;
    }
    g_host.thread.waiting_on[slot].c = NULL;
    g_host.thread.waiting_on[slot].m = NULL;
    return r;
}

/* ============================================================ a save state's load */
/* The old guest threads end where they stopped, each on its own host thread (they are detached, so nothing joins
   them): the load raises retire, wakes every one, and counts them gone (nhost). What a thread held of g_hle (references,
   a pool slot) goes with the g_hle the load writes over, so a thread only lets go of the host lock it holds */
void kthread_retire_check(PMutex* held) {
    if (!__atomic_load_n(&g_host.thread.retire, __ATOMIC_ACQUIRE)) return;
    if (held) pm_unlock(held);
    __atomic_sub_fetch(&g_host.thread.nhost, 1, __ATOMIC_ACQ_REL);
    plat_thread_exit();
}
int kernel_retire(int timeout_ms) {
    u64 t0 = plat_ns();
    u32 i;
    pm_lock(&g_host.kernel.klock);
    __atomic_store_n(&g_host.thread.retire, 1, __ATOMIC_RELEASE);
    for (i = 0; i < KTHREAD_MAX; i++) pc_broadcast(&g_host.thread.cv[i]);
    pm_unlock(&g_host.kernel.klock);
    for (i = 0; i < KTHREAD_MAX; i++) {
        PCond* c = g_host.thread.waiting_on[i].c;
        PMutex* m = g_host.thread.waiting_on[i].m;
        if (!c || !m) continue;
        pm_lock(m); /* so the wake can't come before the thread waits */
        pc_broadcast(c);
        pm_unlock(m);
    }
    while (__atomic_load_n(&g_host.thread.nhost, __ATOMIC_ACQUIRE) > 0) {
        if (plat_ns() - t0 >= (u64)timeout_ms * 1000000u) {
            WARN("state: %d guest threads didn't end within %d ms\n",
                 __atomic_load_n(&g_host.thread.nhost, __ATOMIC_ACQUIRE), timeout_ms);
            return -1;
        }
        plat_sleep_ns(1000000);
    }
    __atomic_store_n(&g_host.thread.retire, 0, __ATOMIC_RELEASE);
    memset(g_host.thread.waiting_on, 0, sizeof g_host.thread.waiting_on);
    return 0;
}
/* A restored thread, on a host thread of its own: it waits for the load to let the game go, then goes on as it
   stopped. An SVC it had yet to begin, or whose request a service never answered, runs again from its svc
   instruction (the argument registers are as they were: the SVC writes its outputs only as it returns); a wait it
   was blocked in goes on (svc_resume), and code at its pc runs on */
static void thread_resume_main(void* p) {
    KThread* t = (KThread*)p;
    int park = t->park;
    tls_thread = t;
    __atomic_add_fetch(&g_host.thread.nhost, 1, __ATOMIC_ACQ_REL);
    kthread_hold_point(t, park);
    if (park == PARK_ENTER || park == PARK_SERVICE) {
        if (g_arm_mod)
            t->arm.r[15] -= t->arm.cpsr & SNX_ARM_T ? 2u : 4u;
        else
            t->cpu.pc -= 4;
        if (park == PARK_SERVICE) svc_service_retry(t);
        __atomic_store_n(&t->in_svc, 0, __ATOMIC_SEQ_CST);
    } else if (park == PARK_WAIT)
        svc_resume(t);
    else if (__atomic_load_n(&t->in_svc, __ATOMIC_SEQ_CST))
        svc_resume_leave(t); /* stopped on its way out of an SVC, its outputs written */
    thread_run(t, g_arm_mod ? t->arm.r[15] : t->cpu.pc);
}
void kthread_resume(KThread* t) {
    t->cpu.mem = g_mem;
    t->cpu.disp = g_disp;
    t->cpu.code_lo = g_code_lo;
    t->cpu.code_hi = g_code_hi;
    t->cpu.rt = &g_rtapi;
    t->cpu.thread = t;
    t->cpu.poll = 0;
    t->cpu.excl_on = 0; /* the monitor the old thread armed: its store-exclusive fails, and the game tries again */
    if (g_arm_mod) {
        t->arm.mem = g_mem;
        t->arm.mem_size = 1ull << 32;
        t->arm.rt = &g_arm_rt;
    }
    t->on_core = -1;
    t->pinned = -1;
    t->core_next = 0;
    t->stopped = 0;
    t->pause_at = 0;
    pc_init(kt_cv(t));
    if (plat_thread(thread_resume_main, t, (size_t)g_settings.thread_stack_mb << 20))
        fatal("Could not start a host thread for guest thread %llu", (unsigned long long)t->id);
}

/* each thread that has started and not exited, with g_klock and the pool's lock held */
#define FOR_LIVE_THREADS(t)                                                                                            \
    for (KThread* t = g_hle.thread.threads; t < g_hle.thread.threads + KTHREAD_MAX; t++)                               \
        if (t->o.type == KO_THREAD && t->state == 1)
int kernel_hold(int timeout_ms) {
    u64 t0 = plat_ns();
    int k;
    pm_lock(&g_host.kernel.klock);
    __atomic_store_n(&g_hle.thread.hold, 1, __ATOMIC_SEQ_CST);
    /* under the cores' lock, so that no core_flag_nl that read hold before it was set clears a flag after this */
    pm_lock(&g_host.thread.core_lk);
    for (k = 0; k < NCORES; k++) core_flag_nl(k);
    if (!g_settings.emulated_cores) { /* no thread holds a core: each one running game code is asked itself */
        pm_lock(&g_host.kernel.pool_lk);
        FOR_LIVE_THREADS(t) __atomic_store_n(&t->cpu.poll, 1u, __ATOMIC_RELAXED);
        pm_unlock(&g_host.kernel.pool_lk);
    }
    pm_unlock(&g_host.thread.core_lk);
    pm_unlock(&g_host.kernel.klock);
    for (;;) {
        char name[sizeof g_hle.thread.threads[0].name] = "";
        u64 id = 0;
        int late = 0;
        pm_lock(&g_host.kernel.klock);
        pm_lock(&g_host.kernel.pool_lk);
        FOR_LIVE_THREADS(t) {
            if (__atomic_load_n(&t->stopped, __ATOMIC_SEQ_CST) || (t->suspended && t->pause_at)) continue;
            late = 1;
            id = t->id;
            memcpy(name, t->name, sizeof name);
            break;
        }
        pm_unlock(&g_host.kernel.pool_lk);
        pm_unlock(&g_host.kernel.klock);
        if (!late) return 0;
        if (plat_ns() - t0 >= (u64)timeout_ms * 1000000u) {
            name[sizeof name - 1] = 0;
            WARN("pause: thread %llu (%s) still runs after %d ms, so the game goes on unpaused. The threads:\n",
                 (unsigned long long)id, name[0] ? name : "unnamed", timeout_ms);
            kernel_dump_threads();
            kernel_release();
            return -1;
        }
        plat_sleep_ns(1000000);
    }
}
void kernel_release(void) {
    pm_lock(&g_host.kernel.klock);
    __atomic_store_n(&g_hle.thread.hold, 0, __ATOMIC_SEQ_CST);
    pm_lock(&g_host.kernel.pool_lk);
    FOR_LIVE_THREADS(t) pc_broadcast(kt_cv(t));
    pm_unlock(&g_host.kernel.pool_lk);
    pm_unlock(&g_host.kernel.klock);
}
#undef FOR_LIVE_THREADS

/* the main thread, as the console starts an application: its stack mapped in the stack region between two free guard
   pages, x0 0 and x1 its own handle, on the core the npdm names, which becomes the process's default core. The
   stack is process memory, charged with the images before any heap */
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core) {
    u64 stack;
    u32 h;
    KThread* t;
    stack_size = (stack_size + 0xFFF) & ~0xFFFull;
    if (core < 0 || core >= NCORES || !((g_hle.svcmem.core_mask >> core) & 1))
        fatal("The game asks for CPU core %d, which its own settings don't allow (cores %llx)", core,
              (unsigned long long)g_hle.svcmem.core_mask);
    if (g_code_memory + g_hle.kernel.sysres_size + stack_size > mem_total())
        fatal("The game's code (%llu KiB), system resource (%llu KiB) and main stack (%llu KiB) don't fit its %llu MiB "
              "of memory",
              (unsigned long long)(g_code_memory >> 10), (unsigned long long)(g_hle.kernel.sysres_size >> 10),
              (unsigned long long)(stack_size >> 10), (unsigned long long)(mem_total() >> 20));
    stack = mem_find_free(STACK_BASE, STACK_BASE + STACK_SIZE, stack_size + 0x2000, 0x1000);
    if (stack) stack += 0x1000;
    if (!stack || mem_map(stack, stack_size, MS_STACK, PERM_RW))
        fatal("No room for the main thread's stack (%llu KiB)", (unsigned long long)(stack_size >> 10));
    g_main_stack_size = stack_size;
    g_hle.thread.default_core = core;
    t = kthread_create(entry, 0, stack + stack_size, prio, core);
    memcpy(t->name, "main", 5);
    g_hle.thread.main_tid = t->id;
    h = h_new(&t->o);
    t->cpu.x[1] = h;
    t->arm.r[1] = h;
    LOG("main thread: entry %llx, stack %llx (%llu KiB), priority %d, core %d\n", (unsigned long long)entry,
        (unsigned long long)stack, (unsigned long long)(stack_size >> 10), prio, core);
    kthread_start(t);
}

/* svcExitProcess, and the game's end however it comes: what the runtime keeps across runs is saved first, as the
   process ends without joining anything. Through snx_save_before_exit, so an unattended limit reached meanwhile doesn't
   write the same files at once */
void process_exit(int code) {
    LOG("The game exited (code %d)\n", code);
    snx_save_before_exit();
    snx_tracy_shutdown();
    plat_exit_now(code);
}
