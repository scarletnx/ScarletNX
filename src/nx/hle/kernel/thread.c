/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* guest threads (each on a host thread of its own), the thread table, process exit, and the four cores the threads
   take turns on */
#include "common/utilities/bits.h"
#include "nx/hle/kernel/kernel_int.h"
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/hle/hle.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/gvk/gvk.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/guest_stats.h"

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
        if (((atomic_load_explicit(&t->affinity, memory_order_relaxed) >> j) & 1) &&
            !atomic_load_explicit(&g_hle.thread.core_owner[j], memory_order_relaxed))
            return j;
    return -1;
}
/* the holder of core k is asked to let go (CPU.poll, read by generated code) when a waiter would preempt it, and while
   the game is paused, so that it reaches a hold point. Also when its core has a waiter and its mask an idle core: it
   moves there, as the console's scheduler does at any priority */
/* stored only when it changes: the holder's code reads poll at each backward branch, and a store of the same value
   still takes its cache line away from the holder's core */
static void core_flag_nl(int k) {
    KThread* o = kt_at(atomic_load_explicit(&g_hle.thread.core_owner[k], memory_order_relaxed));
    u32 v;
    if (!o) return;
    v = atomic_load_explicit(&g_hle.thread.hold, memory_order_relaxed) || g_hle.thread.core_best[k] < o->prio ||
                (atomic_load_explicit(&g_hle.thread.core_wait[k], memory_order_relaxed) && core_idle_nl(o) >= 0)
            ? 1u
            : 0u;
    if (g_hle.thread.core_best[k] < o->prio) {
        if (!atomic_load_explicit(&g_host.thread.preempt_since[k], memory_order_relaxed))
            atomic_store_explicit(&g_host.thread.preempt_since[k], plat_ns(), memory_order_relaxed);
    } else
        atomic_store_explicit(&g_host.thread.preempt_since[k], 0, memory_order_relaxed);
    if (atomic_load_explicit(&o->cpu.poll, memory_order_relaxed) == v) return;
    atomic_store_explicit(&o->cpu.poll, v, memory_order_relaxed);
    if (v) nce_kick(o); /* native code reads no poll: it is stopped */
}
static void core_rebest_nl(int k) {
    KThread* w;
    int best = 64;
    for (w = kt_at(atomic_load_explicit(&g_hle.thread.core_wait[k], memory_order_relaxed)); w;
         w = kt_at(atomic_load_explicit(&w->core_next, memory_order_relaxed)))
        if (w->prio < best) best = w->prio;
    g_hle.thread.core_best[k] = best;
}
static void core_unlink_nl(int k, KThread* t) {
    _Atomic u32* l = &g_hle.thread.core_wait[k];
    u32 i = kt_ix(t);
    while (atomic_load_explicit(&(*l), memory_order_relaxed) && atomic_load_explicit(&(*l), memory_order_relaxed) != i)
        l = &kt_at(atomic_load_explicit(&(*l), memory_order_relaxed))->core_next;
    if (atomic_load_explicit(&(*l), memory_order_relaxed))
        atomic_store_explicit(l, atomic_load_explicit(&t->core_next, memory_order_relaxed),
                              memory_order_relaxed); /* core_yield reads the heads without the lock */
    atomic_store_explicit(&t->core_next, 0, memory_order_relaxed);
    core_rebest_nl(k);
}
/* whether a waits ahead of b */
static int core_before(const KThread* a, const KThread* b) {
    return a->prio < b->prio || (a->prio == b->prio && a->core_ticket < b->core_ticket);
}
/* The late-preemption time and the thread that let go, and the core wait's time, computed under core_lk by
   core_held_nl and core_give_nl and emitted by core_unlock once that lock is let go, so no plot or text is formatted
   while the lock is held */
static _Thread_local u64 preempt_late_ns; /* 0: no late preemption to emit */
static _Thread_local u64 preempt_late_id;
static _Thread_local u64 core_wait_plot_ns; /* 0: no core wait plot to emit */
/* t, which held core k since core_give_nl, lets go of it: the time goes to the report at exit (kernel_core_report) */
static void core_held_nl(int k, KThread* t) {
    u64 now = plat_ns(), held = now - g_host.thread.core_since[k],
        flagged = atomic_load_explicit(&g_host.thread.preempt_since[k], memory_order_relaxed);
    g_host.thread.core_held_ns[k] += held;
    t->core_held_ns += held;
    if (!flagged) return;
    /* how late t let go for a waiter of better priority: on the console, at once. Only a late one is plotted */
    if (now - flagged > 10000000u) {
        g_host.thread.preempt_late[k]++;
        preempt_late_ns = now - flagged;
        preempt_late_id = t->id;
    }
    if (now - flagged > g_host.thread.preempt_max_ns[k]) {
        g_host.thread.preempt_max_ns[k] = now - flagged;
        g_host.thread.preempt_max_by[k] = kt_ix(t);
    }
    atomic_store_explicit(&g_host.thread.preempt_since[k], 0, memory_order_relaxed);
}
/* t holds core k from now on. A thread that waited for it in line is woken (its wait is counted for the report), when
   its waker lets go of the lock */
static void core_give_nl(int k, KThread* t) {
    u64 now = plat_ns();
    int waited = atomic_load_explicit(&t->on_core, memory_order_relaxed) == -2;
    atomic_store_explicit(&g_hle.thread.core_owner[k], kt_ix(t), memory_order_relaxed);
    g_host.thread.core_since[k] = now;
    t->core = k;
    atomic_store_explicit(&t->on_core, k, memory_order_release);
    atomic_store_explicit(&t->cpu.poll, 0u, memory_order_relaxed);
    core_flag_nl(k);
    if (!waited) return;
    core_wait_plot_ns = now - t->core_queued;
    t->core_wait_ns += now - t->core_queued;
    t->core_waits++;
    g_host.thread.core_wait_ns[k] += now - t->core_queued;
    kt_notify_later(t);
}
/* The core handoff's decision, computed under core_lk and emitted by core_unlock once that lock is let go, so no
   zone is opened or text formatted while the lock is held. The thread holding the lock is the one that unlocks it.
   The value codes are 0 no taker and other holders flagged, 1 the best waiter took this core, 2 it came from another
   core; -1 means no handoff happened */
static _Thread_local int handoff_why = -1;
#if defined(SNX_TRACY)
/* the handoff's taker, captured under core_lk with the decision, named in the zone's text */
static _Thread_local u64 handoff_tid;
static _Thread_local int handoff_prio, handoff_core, handoff_from;
#endif
/* core k has just fallen free: its best waiter takes it. With none, it takes the best thread waiting for another core
   that its mask lets run here, unless that thread is about to preempt its own core's holder, as the console's
   scheduler moves a ready thread to a core that would otherwise idle */
static void core_handoff_nl(int k) {
    KThread* best = NULL;
    KThread* w;
    int j, from = k;
    for (w = kt_at(atomic_load_explicit(&g_hle.thread.core_wait[k], memory_order_relaxed)); w;
         w = kt_at(atomic_load_explicit(&w->core_next, memory_order_relaxed)))
        if (!best || core_before(w, best)) best = w;
    if (!best)
        for (j = 0; j < NCORES; j++) {
            if (j == k || !atomic_load_explicit(&g_hle.thread.core_owner[j], memory_order_relaxed)) continue;
            for (w = kt_at(atomic_load_explicit(&g_hle.thread.core_wait[j], memory_order_relaxed)); w;
                 w = kt_at(atomic_load_explicit(&w->core_next, memory_order_relaxed)))
                if (((atomic_load_explicit(&w->affinity, memory_order_relaxed) >> k) & 1) &&
                    w->prio >= kt_at(atomic_load_explicit(&g_hle.thread.core_owner[j], memory_order_relaxed))->prio &&
                    (!best || core_before(w, best))) {
                    best = w;
                    from = j;
                }
        }
    if (!best) { /* a holder with a waiter elsewhere may move here */
        handoff_why = 0;
        for (j = 0; j < NCORES; j++)
            if (j != k && atomic_load_explicit(&g_hle.thread.core_wait[j], memory_order_relaxed)) core_flag_nl(j);
        return;
    }
#if defined(SNX_TRACY)
    /* Capture unconditionally: the emit below is gated only by handoff_why, so fields set only while attached would
       be stale when a client attaches between the decision and the emit */
    handoff_tid = best->id;
    handoff_prio = best->prio;
    handoff_core = k;
    handoff_from = from;
#endif
    handoff_why = from == k ? 1 : 2;
    core_unlink_nl(from, best);
    if (from != k) core_flag_nl(from);
    core_give_nl(k, best);
}
/* t lines up on core k's list, where a holder hands it the core (it may be another core by then) */
static void core_queue_nl(KThread* t, int k) {
    t->core = k;
    atomic_store_explicit(&t->on_core, -2, memory_order_relaxed);
    t->core_queued = plat_ns();
    t->core_ticket = ++g_hle.thread.core_ticket;
    atomic_store_explicit(&t->core_next, atomic_load_explicit(&g_hle.thread.core_wait[k], memory_order_relaxed),
                          memory_order_relaxed);
    atomic_store_explicit(&g_hle.thread.core_wait[k], kt_ix(t), memory_order_relaxed);
    if (t->prio < g_hle.thread.core_best[k]) g_hle.thread.core_best[k] = t->prio;
    core_flag_nl(k);
}
/* a hint to the host CPU that this thread spins: it gives the other hardware thread of the core its share */
static void spin_hint(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __builtin_arm_yield();
#endif
}
/* the calling thread t, lined up for a core, waits without the lock until it is handed one. It watches its own word
   for CORE_SPIN_NS first, so a handoff within that costs no sleep and no wake: one game's handoffs took 150 us on
   average with its cores held half the time while threads waited for them, and 125 us with the watch. Watching
   changes nothing of who gets the core next, only how soon the one handed it runs. Where the host has fewer than two
   CPUs a core it sleeps at once, as the time spent watching would be another thread's */
#define CORE_SPIN_NS 20000
static void core_await(KThread* t) {
    if (g_hle.thread.host_ncpu >= 2 * NCORES) {
        u64 until = plat_ns() + CORE_SPIN_NS;
        for (u32 i = 1;; i++) {
            if (atomic_load_explicit(&t->on_core, memory_order_acquire) != -2) return;
            if (!(i & 31) && plat_ns() >= until) break;
            spin_hint();
        }
    }
    for (;;) {
        u32 seq = kt_seq(t);
        if (atomic_load_explicit(&t->on_core, memory_order_acquire) != -2) break;
        kt_wait(t, seq, NULL, -1);
    }
}
/* t (holding no core) takes one: its own if free; with a holder it would preempt, it lines up there and flags the
   holder; otherwise an idle core of its mask, or a place in its own core's line. 1 when it holds one now */
static int core_take_nl(KThread* t) {
    int k = core_of(t), j;
    if (!atomic_load_explicit(&g_hle.thread.core_owner[k], memory_order_relaxed)) {
        core_give_nl(k, t);
        return 1;
    }
    if (t->prio >= kt_at(atomic_load_explicit(&g_hle.thread.core_owner[k], memory_order_relaxed))->prio &&
        (j = core_idle_nl(t)) >= 0) {
        core_give_nl(j, t);
        return 1;
    }
    core_queue_nl(t, k);
    return 0;
}
/* t lets go of the core it holds, which goes to the next thread */
static void core_release_nl(KThread* t) {
    int k = atomic_load_explicit(&t->on_core, memory_order_relaxed);
    atomic_store_explicit(&t->on_core, -1, memory_order_relaxed);
    atomic_store_explicit(&t->cpu.poll, 0u, memory_order_relaxed);
    if (atomic_load_explicit(&g_hle.thread.core_owner[k], memory_order_relaxed) == kt_ix(t)) {
        atomic_store_explicit(&g_hle.thread.core_owner[k], 0, memory_order_relaxed);
        core_held_nl(k, t);
    }
    core_handoff_nl(k);
}
/* after t took a core: its host thread moves to the core's share of the host CPUs, if it was on another one. Fewer
   host CPUs than cores leave nothing to share out */
static void core_pin(KThread* t) {
    int k = atomic_load_explicit(&t->on_core, memory_order_relaxed), n = snx_popcount64(g_hle.svcmem.core_mask),
        cpus[256], m = 0, len = 0;
    char list[256] = "";
    if (k == t->pinned) return;
    t->pinned = k;
    if (g_hle.thread.host_ncpu < n || !((g_hle.svcmem.core_mask >> k) & 1)) return;
    for (int i = snx_popcount64(g_hle.svcmem.core_mask & ((1ull << k) - 1)); i < g_hle.thread.host_ncpu; i += n)
        cpus[m++] = g_hle.thread.host_cpu[i];
    if (plat_thread_cpus(cpus, m)) WARN_LIMIT(1, "Could not move a game thread to its CPUs: %s\n", strerror(errno));
    if (atomic_exchange_explicit(&g_hle.thread.pin_said[k], 1, memory_order_relaxed)) return;
    for (int i = 0; i < m && len < (int)sizeof list - 8; i++)
        len += snprintf(list + len, sizeof list - len, " %d", cpus[i]);
    LOG("kernel: core %d runs on host CPUs%s\n", k, list);
}
/* the lock let go of, and the threads its holder handed a core to woken */
static void core_unlock(void) {
    pm_unlock(&g_host.thread.core_lk);
    if (handoff_why >= 0) {
        SNX_ZONE_CS(z_handoff, "kern core handoff", 4);
        SNX_ZONE_VALUE(z_handoff, (u64)handoff_why);
#if defined(SNX_TRACY)
        if (handoff_why > 0)
            SNX_ZONE_TEXT(z_handoff, "thread %llu prio %d takes core %d from %d", (unsigned long long)handoff_tid,
                          handoff_prio, handoff_core, handoff_from);
#endif
        handoff_why = -1;
    }
    if (core_wait_plot_ns) {
        SNX_PLOT("kern core wait ms", (double)core_wait_plot_ns / 1e6);
        core_wait_plot_ns = 0;
    }
    if (preempt_late_ns) {
        SNX_PLOT("kern preempt late ms", (double)preempt_late_ns / 1e6);
#if defined(SNX_TRACY)
        if (snx_tracy_attached()) {
            char msg[96];
            int n = snprintf(msg, sizeof msg, "kern preempt late %llu ms, thread %llu",
                             (unsigned long long)(preempt_late_ns / 1000000u), (unsigned long long)preempt_late_id);
            if (n > 0) SNX_MSG(msg, (size_t)(n < (int)sizeof msg ? (size_t)n : sizeof msg - 1));
        }
#endif
        preempt_late_ns = 0;
    }
    kt_flush();
}
/* Only t's own thread lets go of a core t holds (another thread hands one only to a thread lined up for it), so a
   thread that holds one sees it here without the lock: the path of nearly every SVC's entry and exit. The one
   exception is a fault on memory the GPU holds (kthread_fault_lend), which only makes on_core -1: a thread that read
   its core here a moment before takes one again at its next poll. One lined up waits on its word, without the lock,
   until a holder hands it a core. A thread that just started is lined up already, or holds its core (kthread_start) */
void core_enter(KThread* t) {
    int got = 1;
    if (!t || !kernel_emulated_cores()) return;
    if (atomic_load_explicit(&t->on_core, memory_order_acquire) < 0) {
        pm_lock(&g_host.thread.core_lk);
        got = atomic_load_explicit(&t->on_core, memory_order_relaxed);
        got = got == -1 ? core_take_nl(t) : got >= 0;
        core_unlock();
        if (!got) core_await(t);
    }
    core_pin(t);
}
void core_leave(KThread* t) {
    if (!t || !kernel_emulated_cores()) return;
    pm_lock(&g_host.thread.core_lk);
    if (atomic_load_explicit(&t->on_core, memory_order_relaxed) >= 0) core_release_nl(t);
    core_unlock();
}
/* A change of guest memory is about to wait for the renderer (g_gpu_mapping_waits). On the console an SVC that changes
   memory holds its core to its end, so a thread of the same priority lined up on that core runs only once the caller
   blocks. The renderer's lock and its landings are waits the console's SVC doesn't have, seconds long at times, with
   every thread of the core behind them: there, and nowhere else in the SVC, the calling thread lets go of its core.
   svc_leave takes it back; outside an SVC nothing would, so there the thread keeps it */
void core_leave_waiting(void) {
    KThread* t = kthread_cur();
    int k = -1;
    if (!t || !kernel_emulated_cores() || !atomic_load_explicit(&t->in_svc, memory_order_seq_cst)) return;
    pm_lock(&g_host.thread.core_lk);
    if (atomic_load_explicit(&t->on_core, memory_order_relaxed) >= 0) {
        k = atomic_load_explicit(&t->on_core, memory_order_relaxed);
        core_release_nl(t);
        g_host.thread.waits_off_core++;
    }
    core_unlock();
    if (k >= 0)
        LOG_LIMIT(1, "kernel: thread %llu let go of core %d in a memory SVC to wait for the renderer, the first time\n",
                  (unsigned long long)t->id, k);
}
/* The two words after the IPC buffer in a thread's TLS, which the console's kernel shares with the SDK: the disable
   count, which the SDK raises around its spinning locks (a busy mutex's, a light message queue's, a light semaphore's),
   and the interrupt flag, which the kernel sets when it would have taken the thread's core meanwhile */
#define SNX_TLS_DISABLE_COUNT  0x100
#define SNX_TLS_INTERRUPT_FLAG 0x102
/* every waiter that would preempt t has flagged it, so an unflagged holder has nothing to do here: that is the path of
   every SVC. stopped: t's game code was stopped where it ran (a poll at a backward branch, NCE's kick), not at an SVC.
   A thread stopped with its disable count raised keeps its core and runs on, as on the console: it may hold a
   spinning lock, and the thread that took its core would spin on that lock for good while the holder waits for the
   core. Its interrupt flag is set instead and its poll stays set: the SDK, its count back at zero, sees the flag and
   calls svcSynchronizePreemptionState, where the core changes hands (svc_enter) */
#if defined(SNX_TRACY)
/* the waiter of best priority on core k, named in a preemption's or a yield's text */
static KThread* core_best_waiter_nl(int k) {
    KThread *b = NULL, *w;
    for (w = kt_at(atomic_load_explicit(&g_hle.thread.core_wait[k], memory_order_relaxed)); w;
         w = kt_at(atomic_load_explicit(&w->core_next, memory_order_relaxed)))
        if (!b || core_before(w, b)) b = w;
    return b;
}
#endif
/* The kern core switch zone's value is the decision: 0 an SVC entry or exit, 1 a game poll, 2 a game pause with no
   core emulation, 3 kept past a waiter because its disable count is raised, 4 it took back a core lent in a fault, 5 a
   better waiter preempted it, 6 it moved to an idle core, 7 nothing moved. The decision is stored while core_lk is
   held and emitted once it is let go. A depth of zero keeps the callstack opt-in through the runtime's setting */
static void core_switch(KThread* t, int stopped) {
    int k, j, got = 1, kept = 0, best = 64, decision = stopped ? 1 : 0;
    u16 count = 0;
#if defined(SNX_TRACY)
    u64 switch_tid = 0;
    int switch_prio = 0;
#endif
    if (!t || !atomic_load_explicit(&t->cpu.poll, memory_order_relaxed)) return;
    SNX_ZONE_CS(z_switch, "kern core switch", 0);
    if (stopped && t->tls) count = gr16(t->tls + SNX_TLS_DISABLE_COUNT); /* its own TLS: only t's thread is here */
    pm_lock(&g_host.thread.core_lk);
    if (!kernel_emulated_cores()) { /* only a game pause sets poll: kernel_hold, under core_lk */
        decision = 2;
        if (!atomic_load_explicit(&g_hle.thread.hold, memory_order_relaxed))
            atomic_store_explicit(&t->cpu.poll, 0u, memory_order_relaxed);
        pm_unlock(&g_host.thread.core_lk);
        SNX_ZONE_VALUE(z_switch, (u64)decision);
        return;
    }
    k = atomic_load_explicit(&t->on_core, memory_order_relaxed);
    if (count && (k == -1 || (k >= 0 && g_hle.thread.core_best[k] < t->prio))) {
        decision = 3;
        /* either way it would line up for a core here, behind the thread that may want its lock */
        gw16(t->tls + SNX_TLS_INTERRUPT_FLAG, 1);
        kept = 1;
        if (k >= 0) best = g_hle.thread.core_best[k];
        g_host.thread.kept++;
    } else if (k == -1) { /* its core was let go while it waited in a fault (kthread_fault_lend): it takes one again */
        decision = 4;
        atomic_store_explicit(&t->cpu.poll, 0u, memory_order_relaxed);
        got = core_take_nl(t);
    } else if (k >= 0 && g_hle.thread.core_best[k] < t->prio) {
        decision = 5;
#if defined(SNX_TRACY)
        if (snx_tracy_attached()) {
            KThread* bw = core_best_waiter_nl(k);
            if (bw) {
                switch_tid = bw->id;
                switch_prio = bw->prio;
            }
        }
#endif
        core_release_nl(t);
        got = core_take_nl(t);
    } else if (k >= 0 && atomic_load_explicit(&g_hle.thread.core_wait[k], memory_order_relaxed) &&
               (j = core_idle_nl(t)) >= 0) {
        decision = 6;
        core_release_nl(t); /* the waiter takes k, t the idle core */
        core_give_nl(j, t);
    } else {
        decision = 7;
        atomic_store_explicit(&t->cpu.poll, 0u, memory_order_relaxed);
    }
    core_unlock();
    SNX_ZONE_VALUE(z_switch, (u64)decision);
#if defined(SNX_TRACY)
    if (decision == 5 && switch_tid)
        SNX_ZONE_TEXT(z_switch, "thread %llu prio %d preempted by thread %llu prio %d", (unsigned long long)t->id,
                      t->prio, (unsigned long long)switch_tid, switch_prio);
#endif
    if (kept) { /* it holds the core it held, or none still: nothing moved */
        if (k >= 0)
            LOG_LIMIT(1,
                      "kernel: thread %llu (prio %d) keeps core %d past a waiter of priority %d: its disable count "
                      "is %u\n",
                      (unsigned long long)t->id, t->prio, k, best, (unsigned)count);
        else
            LOG_LIMIT(1,
                      "kernel: thread %llu (prio %d), its core lent in a fault, runs on with none: its disable "
                      "count is %u\n",
                      (unsigned long long)t->id, t->prio, (unsigned)count);
        return;
    }
    SNX_ZONE_END(z_switch); /* the wait for a core is not a zone */
    if (!got) core_await(t);
    core_pin(t);
}
void core_preempt(KThread* t) { core_switch(t, 0); }
void core_poll(KThread* t) {
    if (t) { /* for the thread dump: only t's own thread writes its count */
        _Atomic u32* polls = &g_host.thread.polls[kt_ix(t) - 1];
        atomic_store_explicit(polls, atomic_load_explicit(polls, memory_order_relaxed) + 1, memory_order_relaxed);
    }
    core_switch(t, 1);
    if (kthread_hold_point(t, PARK_RUN)) core_enter(t);
}
/* svcSynchronizePreemptionState: the SDK found the interrupt flag that core_switch set. The flag goes; the core went
   to its waiter on the way into the SVC */
void core_synchronize(KThread* t) {
    if (t && t->tls) gw16(t->tls + SNX_TLS_INTERRUPT_FLAG, 0);
}
/* The traps, as they learn that host thread tid waits in a fault on memory the GPU holds (plat_trap_waiting), before
   its page lands, which can wait for the GPU for seconds: the guest thread blocked in that fault lets go of its core,
   which goes to the core's next thread, and its poll is set. On the console the access takes no time, so a waiter of
   better priority runs at once. The fault may be a late one, whose thread runs again already: it then runs without a
   core only until its next poll, where it takes one again (core_preempt), or its next SVC (core_enter). core_owner
   and on_core change together, under core_lk, as when a thread leaves its core */
void kthread_fault_lend(u32 tid) {
    KThread* t = NULL;
    if (!kernel_emulated_cores() || !tid) return;
    /* a slot keeps the host thread of a thread that exited, whose number the system may give another thread: only a
       live one holds a core */
    for (int i = 0; i < KTHREAD_MAX && !t; i++)
        if (atomic_load_explicit(&g_host.thread.host_tid[i], memory_order_relaxed) == tid &&
            g_hle.thread.threads[i].o.type == KO_THREAD && g_hle.thread.threads[i].state == 1)
            t = &g_hle.thread.threads[i];
    if (!t) return; /* a thread of the runtime's own */
    pm_lock(&g_host.thread.core_lk);
    int k = atomic_load_explicit(&t->on_core, memory_order_relaxed);
    if (k >= 0 && atomic_load_explicit(&g_hle.thread.core_owner[k], memory_order_relaxed) == kt_ix(t)) {
        core_release_nl(t);
        atomic_store_explicit(&t->cpu.poll, 1u, memory_order_relaxed);
    }
    core_unlock();
}
/* Read without core_lk, as a thread dump is: a moment stale is all a report needs. The holder's state says where it
   is: in game code that reaches no poll, or blocked off the CPU (a fault on memory the GPU holds) */
#define CORE_WATCH_NS 250000000u
#define CORE_WATCH_LINES 50u
void kernel_core_watch(void) {
    if (!kernel_emulated_cores() || g_host.thread.preempt_lines >= CORE_WATCH_LINES) return;
    u64 now = plat_ns();
    for (int k = 0; k < NCORES; k++) {
        u64 since = atomic_load_explicit(&g_host.thread.preempt_since[k], memory_order_relaxed);
        const KThread* o = kt_at(atomic_load_explicit(&g_hle.thread.core_owner[k], memory_order_relaxed));
        if (!since || !o || since > now || now - since < CORE_WATCH_NS || g_host.thread.preempt_said[k] == since)
            continue;
        g_host.thread.preempt_said[k] = since;
        g_host.thread.preempt_lines++;
        u32 ix = kt_ix(o) - 1, tid = atomic_load_explicit(&g_host.thread.host_tid[ix], memory_order_relaxed);
        u64 left = atomic_load_explicit(&g_host.thread.svc_ns[ix], memory_order_relaxed);
        char where[96] = "?";
        if (tid) plat_thread_where(tid, where, sizeof where);
        LOG("kernel: thread %llu (prio %d) keeps core %d %llu ms after a waiter of priority %d came: %s, host thread %s, "
            "poll %u, %u polls, out of an SVC for %llu ms, pc %llx lr %llx\n",
            (unsigned long long)o->id, o->prio, k, (unsigned long long)((now - since) / 1000000u),
            g_hle.thread.core_best[k],
            atomic_load_explicit(&o->in_svc, memory_order_relaxed) ? "in an SVC" : "in game code", where,
            atomic_load_explicit(&o->cpu.poll, memory_order_relaxed),
            atomic_load_explicit(&g_host.thread.polls[ix], memory_order_relaxed),
            (unsigned long long)(left && now > left ? (now - left) / 1000000u : 0), (unsigned long long)o->cpu.pc,
            (unsigned long long)o->cpu.x[30]);
    }
}
/* the thread a yield with core migration moves to core k on the console (Mesosphère's
   KScheduler::YieldWithCoreMigration): one waiting for another core that core k is in the mask of, of the yielding
   thread's priority or better, the best first. Not from a core whose holder is of priority 0 or 1, which nothing is
   moved away from (HighestCoreMigrationAllowedPriority). NULL for none, else its core in *from */
static KThread* core_suggested_nl(const KThread* t, int k, int* from) {
    KThread *best = NULL, *w;
    int j;
    for (j = 0; j < NCORES; j++) {
        if (j == k || (atomic_load_explicit(&g_hle.thread.core_owner[j], memory_order_relaxed) &&
                       kt_at(atomic_load_explicit(&g_hle.thread.core_owner[j], memory_order_relaxed))->prio < 2))
            continue;
        for (w = kt_at(atomic_load_explicit(&g_hle.thread.core_wait[j], memory_order_relaxed)); w;
             w = kt_at(atomic_load_explicit(&w->core_next, memory_order_relaxed)))
            if (((atomic_load_explicit(&w->affinity, memory_order_relaxed) >> k) & 1) && w->prio <= t->prio &&
                (!best || core_before(w, best))) {
                best = w;
                *from = j;
            }
    }
    return best;
}
/* svcSleepThread's yields. A yield without migration (0) hands the core only to a waiter of t's priority or better,
   and t lines up behind the waiters of its own priority. The console's (Mesosphère's
   KScheduler::YieldWithoutCoreMigration) only moves t to the back of its own priority on its core, so a waiter of
   worse priority does not run. When the core went to such a waiter and came back, that waiter lined up again, and
   with an idle core in t's mask t was flagged and moved there on its way out of the SVC: svcSleepThread(0) changed
   t's core. A migrating yield (2, for -1 and -2) gives the core to the best waiter, whatever its priority, and lets t
   run on an idle core of its mask meanwhile. A waiter of worse priority that took the core gives it back at its next
   poll. With no waiter, a migrating yield takes a thread waiting for another core onto this one and lines up behind
   it, as the console does: a thread that polls with svcSleepThread(-1) lends its core to threads its core could run,
   which otherwise wait for a busy core of their own. Without that either, t goes on at once */
void core_yield(KThread* t, int migrate) {
    int k, j, got = 1, why = 4;
    KThread* w;
#if defined(SNX_TRACY)
    u64 yield_tid = 0;
    int yield_prio = 0;
#endif
    if (!t) return;
    if (!kernel_emulated_cores()) { /* the host's threads are the cores: let it run another */
        plat_yield();
        return;
    }
    /* nobody waits for any core: nothing to hand over or take in, so no lock. One that lines up meanwhile has
       flagged t if it would preempt it, and otherwise comes after this yield, as if it had arrived a moment later */
    for (k = 0; k < NCORES && !atomic_load_explicit(&g_hle.thread.core_wait[k], memory_order_relaxed); k++) {}
    if (k == NCORES) return;
    /* the zone's value is the decision: 1 a waiter took the core and t took an idle one, 2 it took the core and t
       lined up, 3 t took a waiter from another core, 4 no waiter moved. It is stored under core_lk and emitted after */
    SNX_ZONE(z_yield, "kern core yield");
    pm_lock(&g_host.thread.core_lk);
    k = atomic_load_explicit(&t->on_core, memory_order_relaxed);
    if (k >= 0 && atomic_load_explicit(&g_hle.thread.core_wait[k], memory_order_relaxed) &&
        (migrate || g_hle.thread.core_best[k] <= t->prio)) {
#if defined(SNX_TRACY)
        if (snx_tracy_attached()) {
            KThread* bw = core_best_waiter_nl(k);
            if (bw) {
                yield_tid = bw->id;
                yield_prio = bw->prio;
            }
        }
#endif
        core_release_nl(t);
        if (migrate && (j = core_idle_nl(t)) >= 0) {
            core_give_nl(j, t);
            why = 1;
        } else {
            core_queue_nl(t, k);
            got = 0;
            why = 2;
        }
    } else if (k >= 0 && migrate && (w = core_suggested_nl(t, k, &j))) {
#if defined(SNX_TRACY)
        if (snx_tracy_attached()) {
            yield_tid = w->id;
            yield_prio = w->prio;
        }
#endif
        core_unlink_nl(j, w);
        core_flag_nl(j);
        atomic_store_explicit(&t->on_core, -1, memory_order_relaxed);
        atomic_store_explicit(&t->cpu.poll, 0u, memory_order_relaxed);
        core_held_nl(k, t);
        core_give_nl(k, w); /* not core_release_nl: its hand-off could pick another thread */
        core_queue_nl(t, k);
        got = 0;
        why = 3;
    }
    core_unlock();
    SNX_ZONE_VALUE(z_yield, (u64)why);
#if defined(SNX_TRACY)
    if (yield_tid)
        SNX_ZONE_TEXT(z_yield, "thread %llu prio %d took core %d", (unsigned long long)yield_tid, yield_prio, k);
#endif
    SNX_ZONE_END(z_yield); /* the wait for a core is not a zone */
    if (!got) core_await(t);
    core_pin(t);
}
/* a waiter whose priority changes goes behind the waiters of its new priority, as a thread is requeued on the
   console; a holder is flagged if a waiter now beats it */
void core_set_prio(KThread* t, int prio) {
    pm_lock(&g_host.thread.core_lk);
    t->prio = prio;
    atomic_store_explicit(&t->arb_prio, prio, memory_order_relaxed);
    if (atomic_load_explicit(&t->on_core, memory_order_relaxed) == -2) {
        int k = core_of(t);
        t->core_ticket = ++g_hle.thread.core_ticket;
        core_rebest_nl(k);
        core_flag_nl(k);
    } else if (atomic_load_explicit(&t->on_core, memory_order_relaxed) >= 0)
        core_flag_nl(atomic_load_explicit(&t->on_core, memory_order_relaxed));
    pm_unlock(&g_host.thread.core_lk);
}

/* ============================================================ waiting and waking a thread */
u32 kt_seq(const KThread* t) { return atomic_load_explicit(kt_word(t), memory_order_acquire); }
void kt_notify(KThread* t) {
    atomic_fetch_add_explicit(kt_word(t), 1u, memory_order_release);
    plat_wake_word(kt_word(t));
}
void kt_notify_later(KThread* t) {
    if (t_hle.thread.later.n == KT_LATER) {
        kt_notify(t);
        return;
    }
    atomic_fetch_add_explicit(kt_word(t), 1u, memory_order_release);
    t_hle.thread.later.ix[t_hle.thread.later.n++] = kt_ix(t);
}
void kt_flush(void) {
    u32 n = t_hle.thread.later.n;
    t_hle.thread.later.n = 0;
    for (u32 i = 0; i < n; i++) plat_wake_word(kt_word(kt_at(t_hle.thread.later.ix[i])));
}
void kt_wait(KThread* t, u32 seq, PMutex* held, i64 timeout_ns) {
    if (held) pm_unlock(held);
    kt_flush();
    pm_wait_note(held, __builtin_return_address(0), 1);
    plat_wait_word(kt_word(t), seq, timeout_ns);
    pm_wait_note(NULL, NULL, 0);
    if (held) pm_lock(held);
}
void klock_unlock(void) {
    pm_unlock(&g_host.kernel.klock);
#if defined(SNX_TRACY)
    kernel_wait_plot_flush();
#endif
    kt_flush();
}

/* ============================================================ threads */
KThread* kthread_cur(void) { return t_hle.thread.cur; }

/* the host's CPUs, which the cores pin to (the locks and the cores' wait lists start in hle_init) */
int kernel_emulated_cores(void) {
    int mode = g_settings.emulated_cores_mode;
    return mode == SNX_KERNEL_CORES_AUTO ? !g_ounce : mode != 0;
}
void kernel_init(void) {
    g_hle.thread.host_ncpu = plat_cpus(g_hle.thread.host_cpu, 256);
    g_gpu_mapping_waits = core_leave_waiting;
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
    klock_unlock();
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
   g_hle.thread.threads (KTHREAD_MAX). It stays valid while a reference to it is held. A process that has THREAD_MAX
   threads that haven't exited gets LimitReached, counted and taken under g_klock so that two creations can't both
   take the last one; with every slot in use (exited threads keep theirs while a handle to them is open) it gets
   OutOfResource. The console counts an exited thread until its last handle closes, and has a creation wait 100 ms
   for that: here it stops counting as it exits, so no creation the console lets through is refused. An Ounce
   program's limit isn't known, and none is refused for it */
u32 kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core, KThread** out) {
    KThread* t = NULL;
    int i, live = 0;
    *out = NULL;
    pm_lock(&g_host.kernel.klock);
    pm_lock(&g_host.kernel.pool_lk);
    for (i = 0; i < KTHREAD_MAX; i++)
        if (g_hle.thread.threads[i].o.type == KO_THREAD && g_hle.thread.threads[i].state != 2) live++;
    if (live < THREAD_MAX || g_ounce) t = (KThread*)POOL_TRY(g_hle.thread.threads, g_hle.thread.thread_next);
    if (t) t->o.type = KO_THREAD;
    pm_unlock(&g_host.kernel.pool_lk);
    klock_unlock();
    if (!t) return live < THREAD_MAX || g_ounce ? SNX_RC_SVC_OUT_OF_RESOURCE : SNX_RC_SVC_LIMIT_REACHED;
    if (live >= THREAD_MAX)
        WARN_LIMIT(1,
                   "kernel: thread %d of an Ounce program, past the %d a console application may have: its own "
                   "limit isn't known here, so it is made\n",
                   live + 1, THREAD_MAX);
    /* a slot's earlier thread may have ended inside an SVC, as a save state's load ends them where they stopped */
    atomic_store_explicit(&g_host.thread.handed[kt_ix(t) - 1], 0, memory_order_relaxed);
    /* nor is its host thread's number this one's: this thread holds a core before it has a host thread */
    atomic_store_explicit(&g_host.thread.host_tid[kt_ix(t) - 1], 0, memory_order_relaxed);
    kobj_set_name(&t->o, "thread");
    t->prio = t->base_prio = prio;
    atomic_store_explicit(&t->arb_prio, prio, memory_order_relaxed);
    t->core = core;
    atomic_store_explicit(&t->ideal_core, core, memory_order_relaxed);
    atomic_store_explicit(&t->affinity, 1ull << core, memory_order_relaxed);
    atomic_store_explicit(&t->on_core, -1, memory_order_relaxed);
    t->pinned = -1;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    pm_lock(&g_host.kernel.klock);
    t->id = g_hle.kernel.next_tid++;
    t->tls = tls_alloc_nl();
    klock_unlock();

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
    *out = t;
    R_SUCCEED();
}

/* A thread finds its own handle at 0x110 of its TLS, where the console's kernel puts it from 22.0.0 on, for every
   program (as Eden's KProcess::Run and CreateThread do): no code of the program writes it there, and the SDK from 22 on,
   the Switch's as the Ounce's, takes a lock's owner from it (InternalCriticalSection::IsLockedByCurrentThread), from
   the C library's first lock on. Left 0, it reads as the owner of every free lock, and the first pthread_mutex_lock of
   a normal mutex aborts as a relock. Older SDKs leave the word alone: the kernel's own words end at 0x10f. The thread
   keeps it too, for the lock words the hooks write for it (nn/os_sync.c), whatever its SDK reads */
void kthread_set_own_handle(KThread* t, u32 h) {
    if (t) t->own_handle = h;
    if (t && t->tls) gw32(t->tls + 0x110, h);
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
    else if (nce_on())
        nce_thread_run(t, pc); /* the code runs natively until the entry function returns */
    else
        do snx_lookup(&t->cpu, pc)(&t->cpu, pc);
        while ((pc = t->cpu.pc) != 0);
    svc_exit_thread(t); /* its entry function returned: there is nothing to return to */
}
/* every kt_ix has a slot of counters */
typedef char snx_check_guest_stats_slots[KTHREAD_MAX < GSTAT_SLOTS ? 1 : -1];
static void thread_main(void* p) {
    KThread* t = (KThread*)p;
    t_hle.thread.cur = t;
    atomic_store_explicit(&g_host.thread.host_tid[kt_ix(t) - 1], plat_tid(), memory_order_relaxed);
#if defined(SNX_TRACY)
    if (snx_tracy_attached()) {
        char msg[64];
        int n = snprintf(msg, sizeof msg, "thread map guest %lu host %lu", (unsigned long)t->id,
                         (unsigned long)plat_tid());
        if (n > 0) SNX_MSG(msg, (size_t)(n < (int)sizeof msg ? (size_t)n : sizeof msg - 1));
    }
#endif
    guest_stats_thread_start(kt_ix(t));
    {
        char hint[16];
        pm_lock(&g_host.thread.core_lk); /* in line since kthread_start, it may be handed a core meanwhile */
        int k = core_of(t);
        pm_unlock(&g_host.thread.core_lk);
        snprintf(hint, sizeof hint, "core %u", (unsigned)k);
        SNX_THREAD_NAME_HINT(t->name[0] ? t->name : "guest", hint);
    }
    atomic_fetch_add_explicit(&g_host.thread.nhost, 1, memory_order_acq_rel);
    thread_run(t, t->entry);
}
/* the host thread holds a reference from here until svc_exit_thread. The state is read and set in one hold of the
   lock: of two StartThread calls on one handle, the second finds the thread started and gets InvalidState. The thread
   takes its core, or its place in line, here, before its host thread runs: on the console it is ready as svcStartThread
   returns, so one that beats the caller on the caller's core has run by then (svc_leave gives it the core), and a
   holder it waits behind has moved to an idle core */
u32 kthread_start(KThread* t) {
    int state;
    pm_lock(&g_host.kernel.klock);
    state = t->state;
    if (state == 0) t->state = 1;
    klock_unlock();
    R_UNLESS(state == 0, SNX_RC_SVC_INVALID_STATE);
    kobj_ref(&t->o);
    LOGV("StartThread %llu entry=%llx prio=%d core=%d\n", (unsigned long long)t->id, (unsigned long long)t->entry,
         t->prio, t->core);
    if (kernel_emulated_cores()) {
        pm_lock(&g_host.thread.core_lk);
        core_take_nl(t);
        core_unlock();
    }
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
    R_SUCCEED();
}
/* svcExitThread: the thread's waiters (joins) are woken, and its core goes to the next thread. Then its host thread
   lets go of it: its last use of the KThread, which may be freed with that reference */
void svc_exit_thread(KThread* t) {
    pm_lock(&g_host.kernel.klock);
    t->state = 2;
    kobj_signal_nl(&t->o);
    klock_unlock();
    LOGV("ExitThread %llu\n", (unsigned long long)t->id);
    core_leave(t);
    guest_stats_thread_end(kt_ix(t));
    kobj_unref(&t->o);
    gvk_scratch_free();
    atomic_fetch_sub_explicit(&g_host.thread.nhost, 1, memory_order_acq_rel);
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
    if (!t || !atomic_load_explicit(&g_hle.thread.hold, memory_order_seq_cst)) return 0;
    core_leave(t);
    pm_lock(&g_host.kernel.klock);
    t->park = park;
    atomic_store_explicit(&t->stopped, 2, memory_order_seq_cst);
#if defined(SNX_TRACY)
    SNX_FIBER_ENTER(kt_ix(t) - 1, 7, "the game is paused");
#endif
    for (;;) {
        u32 seq = kt_seq(t);
        kthread_retire_check(&g_host.kernel.klock);
        if (!atomic_load_explicit(&g_hle.thread.hold, memory_order_relaxed)) break;
        kt_wait(t, seq, &g_host.kernel.klock, -1);
    }
    atomic_store_explicit(&t->stopped, 0, memory_order_seq_cst);
    SNX_FIBER_LEAVE();
    klock_unlock();
    return 1;
}
void kthread_block(KThread* t) {
    if (!t) return;
    t->park = PARK_RUN; /* at the pc the caller set: a thread a save state restarts goes on there */
    atomic_store_explicit(&t->stopped, 1, memory_order_seq_cst);
    /* runs no game code now: a svcSetThreadActivity waiting for it to pause has its answer (stopped is set before
       suspended is read, and the pause sets suspended before it reads stopped) */
    if (atomic_load_explicit(&t->suspended, memory_order_seq_cst)) {
        pm_lock(&g_host.kernel.klock);
        kobj_wake_nl(&t->o, 5);
        klock_unlock();
    }
}
void kthread_unblock(KThread* t) {
    if (!t) return;
    atomic_store_explicit(&t->stopped, 0, memory_order_seq_cst);
    if (atomic_load_explicit(&t->suspended, memory_order_seq_cst)) svc_pause_point(t); /* paused during the wait */
    kthread_hold_point(t, PARK_RUN); /* rt_miss set the pc it goes on at */
}
void kthread_host_wait_begin(void) {
    KThread* t = kthread_cur();
    if (t) core_leave(t);
}
void kthread_host_wait_end(void) {
    KThread* t = kthread_cur();
    if (t) core_enter(t);
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
    /* not stopped, and a pause waits for it: a request that changed a service, or a wait outside an SVC (a function of
       the game's that the runtime runs natively, NVN's syncpoint waits), which no svc instruction makes again */
    if (g_host.thread.committed[slot] || !atomic_load_explicit(&t->in_svc, memory_order_seq_cst))
        return pc_wait(c, m, timeout_ns);
    g_host.thread.waiting_on[slot].c = c; /* where kernel_retire wakes it */
    g_host.thread.waiting_on[slot].m = m;
    t->park = PARK_SERVICE;
    atomic_store_explicit(&t->stopped, 1, memory_order_seq_cst);
#if defined(SNX_TRACY)
    SNX_FIBER_ENTER(kt_ix(t) - 1, 6, "in a service wait");
#endif
    r = pc_wait(c, m, timeout_ns);
    SNX_FIBER_LEAVE();
    atomic_store_explicit(&t->stopped, 0, memory_order_seq_cst);
    kthread_retire_check(m);
    if (atomic_load_explicit(&g_hle.thread.hold, memory_order_seq_cst)) {
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
    if (!atomic_load_explicit(&g_host.thread.retire, memory_order_acquire)) return;
    if (held) pm_unlock(held);
    guest_stats_thread_end(kt_ix(kthread_cur()));
    gvk_scratch_free();
    atomic_fetch_sub_explicit(&g_host.thread.nhost, 1, memory_order_acq_rel);
    SNX_FIBER_LEAVE_ALL(); /* this host thread ends inside its guest's wait */
    plat_thread_exit();
}
int kernel_retire(int timeout_ms) {
    u64 t0 = plat_ns();
    u32 i;
    pm_lock(&g_host.kernel.klock);
    atomic_store_explicit(&g_host.thread.retire, 1, memory_order_release);
    for (i = 0; i < KTHREAD_MAX; i++) kt_notify(&g_hle.thread.threads[i]);
    klock_unlock();
    for (i = 0; i < KTHREAD_MAX; i++) {
        PCond* c = g_host.thread.waiting_on[i].c;
        PMutex* m = g_host.thread.waiting_on[i].m;
        if (!c || !m) continue;
        pm_lock(m); /* so the wake can't come before the thread waits */
        pc_broadcast(c);
        pm_unlock(m);
    }
    while (atomic_load_explicit(&g_host.thread.nhost, memory_order_acquire) > 0) {
        if (plat_ns() - t0 >= (u64)timeout_ms * 1000000u) {
            WARN("state: %d guest threads didn't end within %d ms\n",
                 atomic_load_explicit(&g_host.thread.nhost, memory_order_acquire), timeout_ms);
            return -1;
        }
        plat_sleep_ns(1000000);
    }
    atomic_store_explicit(&g_host.thread.retire, 0, memory_order_release);
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
    {
        char hint[16];
        snprintf(hint, sizeof hint, "core %u", (unsigned)core_of(t));
        SNX_THREAD_NAME_HINT(t->name[0] ? t->name : "guest", hint);
    }
    t_hle.thread.cur = t;
    atomic_store_explicit(&g_host.thread.host_tid[kt_ix(t) - 1], plat_tid(), memory_order_relaxed);
    atomic_fetch_add_explicit(&g_host.thread.nhost, 1, memory_order_acq_rel);
    kthread_hold_point(t, park);
    if (park == PARK_ENTER || park == PARK_SERVICE) {
        if (g_arm_mod)
            t->arm.r[15] -= t->arm.cpsr & SNX_ARM_T ? 2u : 4u;
        else
            t->cpu.pc -= 4;
        if (park == PARK_SERVICE) svc_service_retry(t);
        atomic_store_explicit(&t->in_svc, 0, memory_order_seq_cst);
    } else if (park == PARK_WAIT)
        svc_resume(t);
    else if (atomic_load_explicit(&t->in_svc, memory_order_seq_cst))
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
    atomic_store_explicit(&t->cpu.poll, 0, memory_order_relaxed);
    t->cpu.excl_on = 0; /* the monitor the old thread armed: its store-exclusive fails, and the game tries again */
    if (g_arm_mod) {
        t->arm.mem = g_mem;
        t->arm.mem_size = 1ull << 32;
        t->arm.rt = &g_arm_rt;
    }
    atomic_store_explicit(&t->on_core, -1, memory_order_relaxed);
    t->pinned = -1;
    atomic_store_explicit(&t->core_next, 0, memory_order_relaxed);
    atomic_store_explicit(&t->stopped, 0, memory_order_relaxed);
    t->pause_at = 0;
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
    atomic_store_explicit(&g_hle.thread.hold, 1, memory_order_seq_cst);
    /* under the cores' lock, so that no core_flag_nl that read hold before it was set clears a flag after this */
    pm_lock(&g_host.thread.core_lk);
    for (k = 0; k < NCORES; k++) core_flag_nl(k);
    if (!kernel_emulated_cores()) { /* no thread holds a core: each one running game code is asked itself */
        pm_lock(&g_host.kernel.pool_lk);
        FOR_LIVE_THREADS(t) {
            atomic_store_explicit(&t->cpu.poll, 1u, memory_order_relaxed);
            nce_kick(t);
        }
        pm_unlock(&g_host.kernel.pool_lk);
    }
    pm_unlock(&g_host.thread.core_lk);
    klock_unlock();
    for (;;) {
        char name[sizeof g_hle.thread.threads[0].name] = "";
        u64 id = 0;
        int late = 0;
        pm_lock(&g_host.kernel.klock);
        pm_lock(&g_host.kernel.pool_lk);
        FOR_LIVE_THREADS(t) {
            if (atomic_load_explicit(&t->stopped, memory_order_seq_cst) ||
                (atomic_load_explicit(&t->suspended, memory_order_relaxed) && t->pause_at))
                continue;
            late = 1;
            id = t->id;
            memcpy(name, t->name, sizeof name);
            break;
        }
        pm_unlock(&g_host.kernel.pool_lk);
        klock_unlock();
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
    atomic_store_explicit(&g_hle.thread.hold, 0, memory_order_seq_cst);
    pm_lock(&g_host.kernel.pool_lk);
    FOR_LIVE_THREADS(t) kt_notify(t);
    pm_unlock(&g_host.kernel.pool_lk);
    klock_unlock();
}
#undef FOR_LIVE_THREADS

/* the main thread, as the console starts an application: its stack mapped in the stack region between two free guard
   pages, x0 0 and x1 its own handle, on the core the npdm names, which becomes the process's default core. The
   stack is process memory, charged with the images before any heap. With args, the thread enters a function of the
   SDK in the place of the game's linker's entry: x0 its handle, x1 to x4 the four of args */
void kernel_run_main(u64 entry, const u64* args, u64 stack_size, int prio, int core) {
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
    if (kthread_create(entry, 0, stack + stack_size, prio, core, &t))
        fatal("No thread slot for the main thread: all %d are in use", KTHREAD_MAX);
    memcpy(t->name, "main", 5);
    g_hle.thread.main_tid = t->id;
    h = h_new(&t->o);
    kthread_set_own_handle(t, h);
    t->cpu.x[1] = h;
    t->arm.r[1] = h;
    if (args) {
        t->cpu.x[0] = h;
        memcpy(&t->cpu.x[1], args, 4 * sizeof(u64));
    }
    LOG("main thread: entry %llx, stack %llx (%llu KiB), priority %d, core %d\n", (unsigned long long)entry,
        (unsigned long long)stack, (unsigned long long)(stack_size >> 10), prio, core);
    (void)kthread_start(t); /* made above: it has not started */
}

/* svcExitProcess, and the game's end however it comes: what the runtime keeps across runs is saved first, as the
   process ends without joining anything. Through snx_save_before_exit, so an unattended limit reached meanwhile doesn't
   write the same files at once */
void process_exit(int code) {
    LOG("The game exited (code %d)\n", code);
    snx_save_before_exit(0);
    snx_tracy_shutdown();
    plat_exit_now(code);
}
