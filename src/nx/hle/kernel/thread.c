/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* guest threads (each on a host thread of its own), the thread table, process exit, and the four cores the threads
   take turns on */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/gpu/renderer.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"

KThread* g_threads[256];
int g_nthreads;
KThread* g_main;
int g_default_core;

/* ============================================================ the cores */
/* A core is held by at most one thread at a time, which runs game code; the others wait for it. Host threads are all
   runnable at once and the host scheduler knows nothing of guest priorities, so the order among the threads of one
   core comes from here: a core that falls free goes to its best waiter (lowest priority value, then the earliest),
   never to whichever host thread wakes first. Every field below and each thread's on_core, core, core_next and
   core_ticket change under g_core_lk, which is taken after g_klock when both are held (sleep_nl lets go of its core
   with g_klock held), and never the other way round */
static PMutex g_core_lk = PMUTEX_INIT;
static KThread* g_core_owner[NCORES]; /* the thread holding each core, or NULL */
static KThread* g_core_wait[NCORES];  /* the threads waiting for it, through core_next, in no order */
static int g_core_best[NCORES];       /* the best priority among them, 64 with none */
static u64 g_core_ticket;             /* arrival order, for waiters of one priority */

/* the core a thread would run on, kept within the cores the runtime has */
static int core_of(const KThread* t) { return t->core >= 0 && t->core < NCORES ? t->core : 0; }
/* the holder of core k is asked to let go (CPU.poll, read by generated code) when a waiter would preempt it */
static void core_flag_nl(int k) {
    KThread* o = g_core_owner[k];
    if (o) __atomic_store_n(&o->cpu.poll, g_core_best[k] < o->prio ? 1u : 0u, __ATOMIC_RELAXED);
}
static void core_rebest_nl(int k) {
    KThread* w;
    int best = 64;
    for (w = g_core_wait[k]; w; w = w->core_next)
        if (w->prio < best) best = w->prio;
    g_core_best[k] = best;
}
static void core_unlink_nl(int k, KThread* t) {
    KThread** l = &g_core_wait[k];
    while (*l && *l != t) l = &(*l)->core_next;
    if (*l) *l = t->core_next;
    t->core_next = NULL;
    core_rebest_nl(k);
}
/* whether a waits ahead of b */
static int core_before(const KThread* a, const KThread* b) {
    return a->prio < b->prio || (a->prio == b->prio && a->core_ticket < b->core_ticket);
}
/* t holds core k from now on, and the host thread waiting for it (if any) goes on */
static void core_give_nl(int k, KThread* t) {
    g_core_owner[k] = t;
    t->core = k;
    t->on_core = k;
    __atomic_store_n(&t->cpu.poll, 0u, __ATOMIC_RELAXED);
    core_flag_nl(k);
    pc_signal(&t->cv);
}
/* core k has just fallen free: its best waiter takes it. With none, it takes the best thread waiting for another core
   that its mask lets run here, unless that thread is about to preempt its own core's holder, as the console's
   scheduler moves a ready thread to a core that would otherwise idle */
static void core_handoff_nl(int k) {
    KThread* best = NULL;
    KThread* w;
    int j, from = k;
    for (w = g_core_wait[k]; w; w = w->core_next)
        if (!best || core_before(w, best)) best = w;
    if (!best)
        for (j = 0; j < NCORES; j++) {
            if (j == k || !g_core_owner[j]) continue;
            for (w = g_core_wait[j]; w; w = w->core_next)
                if (((w->affinity >> k) & 1) && w->prio >= g_core_owner[j]->prio && (!best || core_before(w, best))) {
                    best = w;
                    from = j;
                }
        }
    if (!best) return;
    core_unlink_nl(from, best);
    if (from != k) core_flag_nl(from);
    core_give_nl(k, best);
}
/* the lowest core of t's mask that nobody holds, or -1 */
static int core_idle_nl(const KThread* t) {
    int j;
    for (j = 0; j < NCORES; j++)
        if (((t->affinity >> j) & 1) && !g_core_owner[j]) return j;
    return -1;
}
/* t waits on core k's list until a holder hands the core over (it may be another core by then) */
static void core_wait_nl(KThread* t, int k) {
    t->core = k;
    t->on_core = -2;
    t->core_ticket = ++g_core_ticket;
    t->core_next = g_core_wait[k];
    g_core_wait[k] = t;
    if (t->prio < g_core_best[k]) g_core_best[k] = t->prio;
    core_flag_nl(k);
    while (t->on_core == -2) pc_wait(&t->cv, &g_core_lk, -1);
}
/* t (holding no core) takes one: its own if free; with a holder it would preempt, it waits there and flags the holder;
   otherwise an idle core of its mask, or a place in its own core's line */
static void core_take_nl(KThread* t) {
    int k = core_of(t), j;
    if (!g_core_owner[k]) {
        core_give_nl(k, t);
        return;
    }
    if (t->prio >= g_core_owner[k]->prio && (j = core_idle_nl(t)) >= 0) {
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
    if (g_core_owner[k] == t) g_core_owner[k] = NULL;
    core_handoff_nl(k);
}

void core_enter(KThread* t) {
    if (!t) return;
    pm_lock(&g_core_lk);
    if (t->on_core < 0) core_take_nl(t);
    pm_unlock(&g_core_lk);
}
void core_leave(KThread* t) {
    if (!t) return;
    pm_lock(&g_core_lk);
    if (t->on_core >= 0) core_release_nl(t);
    pm_unlock(&g_core_lk);
}
/* every waiter that would preempt t has flagged it, so an unflagged holder has nothing to do here: that is the path of
   every SVC */
void core_preempt(KThread* t) {
    int k;
    if (!t || !__atomic_load_n(&t->cpu.poll, __ATOMIC_RELAXED)) return;
    pm_lock(&g_core_lk);
    k = t->on_core;
    if (k >= 0 && g_core_best[k] < t->prio) {
        core_release_nl(t);
        core_take_nl(t);
    } else
        __atomic_store_n(&t->cpu.poll, 0u, __ATOMIC_RELAXED);
    pm_unlock(&g_core_lk);
}
void core_poll(KThread* t) { core_preempt(t); }
/* svcSleepThread's yields. With nobody waiting for its core the thread goes on at once, as on the console. Otherwise
   the core goes to the best waiter, whatever its priority, and t lines up behind the waiters of its own priority. A
   waiter of worse priority that took the core gives it back at its next poll: a yield doesn't hand a better thread's
   time to a worse one. migrate (2, for -1 and -2) also lets it run on an idle core of its mask meanwhile */
void core_yield(KThread* t, int migrate) {
    int k, j;
    if (!t) return;
    pm_lock(&g_core_lk);
    k = t->on_core;
    if (k >= 0 && g_core_wait[k]) {
        core_release_nl(t);
        if (migrate && (j = core_idle_nl(t)) >= 0)
            core_give_nl(j, t);
        else
            core_wait_nl(t, k);
    }
    pm_unlock(&g_core_lk);
}
/* a waiter whose priority changes goes behind the waiters of its new priority, as a thread is requeued on the
   console; a holder is flagged if a waiter now beats it */
void core_set_prio(KThread* t, int prio) {
    pm_lock(&g_core_lk);
    t->prio = prio;
    if (t->on_core == -2) {
        int k = core_of(t);
        t->core_ticket = ++g_core_ticket;
        core_rebest_nl(k);
        core_flag_nl(k);
    } else if (t->on_core >= 0)
        core_flag_nl(t->on_core);
    pm_unlock(&g_core_lk);
}

/* ============================================================ threads */
KThread* kthread_cur(void) { return (KThread*)tls_thread; }

void kernel_init(void) {
    int k;
    pm_init(&g_klock);
    pm_init(&g_core_lk);
    for (k = 0; k < NCORES; k++) {
        g_core_owner[k] = NULL;
        g_core_wait[k] = NULL;
        g_core_best[k] = 64;
    }
}

/* the thread can be waited on: it is signaled once it has exited (a join) */
static int thread_signaled(KObj* o) { return ((KThread*)o)->state == 2; }

/* Each thread's thread-local storage is 0x200 bytes (the IPC message buffer in the first 0x100), eight to a page, as
   the console hands them out; TPIDRRO_EL0 (TPIDRURO for a 32-bit thread) points at it. Under g_klock */
static u64 g_tls_page;
static int g_tls_used = 8;
static u64 tls_alloc_nl(void) {
    if (g_tls_used == 8) {
        g_tls_page = kmem_alloc(0x1000, MS_TLS, PERM_RW);
        g_tls_used = 0;
    }
    return g_tls_page + 0x200 * (u64)g_tls_used++;
}

int kthread_live_count(void) {
    int i, n = 0;
    pm_lock(&g_klock);
    for (i = 0; i < g_nthreads; i++)
        if (g_threads[i]->state != 2) n++;
    pm_unlock(&g_klock);
    return n;
}

/* the table the dumps and the live count read: a new thread takes the next slot, or once all 256 are taken the slot
   of a thread that has exited. With 256 live threads the new one is left out (the console allows 96). Under g_klock */
static void table_add_nl(KThread* t) {
    int i;
    if (g_nthreads < (int)(sizeof g_threads / sizeof *g_threads)) {
        g_threads[g_nthreads++] = t;
        return;
    }
    for (i = 0; i < g_nthreads; i++)
        if (g_threads[i]->state == 2) {
            g_threads[i] = t;
            return;
        }
    WARN_LIMIT(1, "kernel: %d live threads, thread %llu is left out of the thread dumps\n", g_nthreads,
               (unsigned long long)t->id);
}

/* a thread that hasn't started, entering at entry with arg in x0 (r0) and sp at stack_top, its return address 0.
   Threads are never freed, so a KThread* stays valid */
KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core) {
    KThread* t = calloc(1, sizeof *t);
    if (!t) fatal("Out of memory for a thread");
    t->o.type = KO_THREAD;
    t->o.is_signaled = thread_signaled;
    t->o.name = "thread";
    t->prio = t->base_prio = prio;
    t->core = t->ideal_core = core;
    t->affinity = 1ull << core;
    t->on_core = -1;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    pc_init(&t->cv);
    pm_lock(&g_klock);
    t->id = g_next_tid++;
    t->tls = tls_alloc_nl();
    table_add_nl(t);
    pm_unlock(&g_klock);
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

/* the host thread's stack: the recompiled code makes each guest call a host call, so it has to hold as many frames as
   the guest's stack does, and more bytes for each */
#define HOST_STACK ((size_t)32 << 20)
static void thread_main(void* p) {
    KThread* t = (KThread*)p;
    tls_thread = t;
    core_enter(t);
    snx_fpcr_apply(&t->cpu);
    if (g_arm_mod)
        arm_thread_run(t);
    else
        snx_lookup(&t->cpu, t->entry)(&t->cpu, t->entry);
    svc_exit_thread(t); /* its entry function returned: there is nothing to return to */
}
void kthread_start(KThread* t) {
    pm_lock(&g_klock);
    t->state = 1;
    pm_unlock(&g_klock);
    LOGV("StartThread %llu entry=%llx prio=%d core=%d\n", (unsigned long long)t->id, (unsigned long long)t->entry,
         t->prio, t->core);
    if (plat_thread(thread_main, t, HOST_STACK))
        fatal("Could not start a host thread for guest thread %llu", (unsigned long long)t->id);
}
/* svcExitThread: the thread's waiters (joins) are woken, and its core goes to the next thread */
void svc_exit_thread(KThread* t) {
    pm_lock(&g_klock);
    t->state = 2;
    kobj_signal_nl(&t->o);
    pm_unlock(&g_klock);
    LOGV("ExitThread %llu\n", (unsigned long long)t->id);
    core_leave(t);
    plat_thread_exit();
}

/* the main thread, as the console starts an application: its stack mapped in the stack region between two free guard
   pages, x0 0 and x1 its own handle, on the core the npdm names, which becomes the process's default core. The
   stack is process memory, charged with the images before any heap */
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core) {
    u64 stack;
    u32 h;
    KThread* t;
    stack_size = (stack_size + 0xFFF) & ~0xFFFull;
    if (core < 0 || core >= NCORES || !((g_core_mask >> core) & 1))
        fatal("The game asks for CPU core %d, which its own settings don't allow (cores %llx)", core,
              (unsigned long long)g_core_mask);
    if (g_code_memory + g_sysres_size + stack_size > mem_total())
        fatal("The game's code (%llu KiB), system resource (%llu KiB) and main stack (%llu KiB) don't fit its %llu MiB "
              "of memory",
              (unsigned long long)(g_code_memory >> 10), (unsigned long long)(g_sysres_size >> 10),
              (unsigned long long)(stack_size >> 10), (unsigned long long)(mem_total() >> 20));
    stack = mem_find_free(STACK_BASE, STACK_BASE + STACK_SIZE, stack_size + 0x2000, 0x1000);
    if (stack) stack += 0x1000;
    if (!stack || mem_map(stack, stack_size, MS_STACK, PERM_RW))
        fatal("No room for the main thread's stack (%llu KiB)", (unsigned long long)(stack_size >> 10));
    g_main_stack_size = stack_size;
    g_default_core = core;
    t = kthread_create(entry, 0, stack + stack_size, prio, core);
    memcpy(t->name, "main", 5);
    g_main = t;
    h = h_new(&t->o);
    t->cpu.x[1] = h;
    t->arm.r[1] = h;
    LOG("main thread: entry %llx, stack %llx (%llu KiB), priority %d, core %d\n", (unsigned long long)entry,
        (unsigned long long)stack, (unsigned long long)(stack_size >> 10), prio, core);
    kthread_start(t);
}

/* svcExitProcess, and the game's end however it comes: what the runtime keeps across runs is saved first, as the
   process ends without joining anything */
void process_exit(int code) {
    LOG("The game exited (code %d)\n", code);
    save_hints();
    if (g_renderer && g_renderer->save) g_renderer->save();
    plat_exit_now(code);
}
