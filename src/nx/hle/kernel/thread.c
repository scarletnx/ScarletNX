/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* threads: creating, starting, the thread a host thread runs, exiting */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/runtime/main.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/arm.h"
/* ============================================================ threads */
KThread* g_threads[256];
int g_nthreads;
KThread* kthread_cur(void) { return (KThread*)tls_thread; }
static void kthread_set(void* t) { tls_thread = t; }
static int th_sig(KObj* o) { return ((KThread*)o)->state == 2; }
static void th_destroy(KObj* o) { (void)o; /* thread objects are never freed */ }
static u64 alloc_tls(void) {
    u64 a = kmem_alloc(0x1000, MS_TLS, PERM_RW);
    memset(GP(a), 0, 0x1000);
    return a;
}

KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core) {
    KThread* t = calloc(1, sizeof *t);
    t->o.type = KO_THREAD;
    t->o.is_signaled = th_sig;
    t->o.destroy = th_destroy;
    t->o.name = "thread";
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->prio = t->base_prio = prio;
    t->core = core < 0 ? 0 : core;
    t->ideal_core = t->core;
    t->on_core = -1;
    t->affinity = 1ull << t->core;
    t->tls = alloc_tls();
    pc_init(&t->cv);
    pm_lock(&g_klock);
    t->id = g_next_tid++;
    /* the table is for the dumps and the thread counts: once full, an exited thread's slot is reused */
    if (g_nthreads < 256)
        g_threads[g_nthreads++] = t;
    else {
        int i = 0;
        while (i < 256 && g_threads[i]->state != 2) i++;
        if (i < 256)
            g_threads[i] = t;
        else
            WARN_LIMIT(1, "kernel: more than 256 live threads, the dumps leave the newest out\n");
    }
    pm_unlock(&g_klock);
    CPU* c = &t->cpu;
    memset(c, 0, sizeof *c);
    c->mem = g_mem;
    c->disp = g_disp;
    c->code_lo = g_code_lo;
    c->code_hi = g_code_hi;
    c->rt = &g_rtapi;
    c->thread = t;
    c->tpidrro = t->tls;
    c->x[0] = arg;
    c->x[31] = stack_top;
    c->pc = entry;
    if (g_arm_mod) { /* a 32-bit thread: the kernel starts it in Thumb state when bit 0 of its entry is set */
        SnxArmCPU* a = &t->arm;
        memset(a, 0, sizeof *a);
        a->mem = g_mem;
        a->mem_size = 0x100000000ull;
        a->rt = &g_arm_rt;
        a->tpidruro = (u32)t->tls;
        a->r[0] = (u32)arg;
        a->r[13] = (u32)stack_top;
        a->r[15] = (u32)entry & ~1u;
        a->cpsr = 0x10 | (entry & 1 ? SNX_ARM_T : 0); /* user mode */
    }
    return t;
}

/* A console core runs one thread at a time, and games build on it: Tears of the Kingdom keeps per-core state (command
   memory cursors, heap caches) without locks, indexed by svcGetCurrentProcessorNumber, and every thread created on the
   process's default core shares it. Guest threads run on host threads in parallel, so a thread holds its core while it
   runs and lets go only where a console thread is off the CPU too: when it blocks in the kernel, sleeps or yields,
   makes an IPC call, or waits in WFE. A thread letting go hands the core to its waiting thread of highest priority, the
   earliest among equals. The console also preempts a running thread as soon as one of higher priority becomes ready:
   here the holder is stopped at its next SVC, or at its next backward branch (a taken one in the interpreter: CPU.poll,
   core_poll), so a thread spinning in a loop lets go of its core within one round of it. A ready thread that would wait
   behind its core's holder runs on an idle core its mask allows instead, and a core that falls idle takes a thread
   waiting for another one, as the console's scheduler moves threads to idle cores (Ender Magnolia's threads all take
   cores 0 to 2, and boot only when one runs beside a spinning one). A yield hands the core to a waiting thread of the
   same priority or a better one, and only svcSleepThread(-2) to one of lower priority; only svcSleepThread(0) keeps
   the yielding thread on its core */
static PMutex g_core_lk;
static PCond g_core_cv[NCORES];
static KThread* g_core_owner[NCORES];
static KThread* g_core_wait[NCORES]; /* the threads waiting for each core, linked by core_next */
static int g_core_best[NCORES]; /* the best (lowest) priority waiting, 64 when none: core_preempt and core_yield read it
                                   unlocked */
static u64 g_core_ticket;
/* the holder of core k is asked to let go (CPU.poll, which generated code checks on every backward branch and the
   interpreter on every taken one) while a thread of higher priority waits for the core: the console would have
   preempted it already */
static void core_flag_nl(int k) {
    KThread* o = g_core_owner[k];
    if (o)
        __atomic_store_n(&o->cpu.poll, __atomic_load_n(&g_core_best[k], __ATOMIC_RELAXED) < o->prio, __ATOMIC_RELAXED);
}
static KThread* core_first_nl(int k) {
    KThread* b = NULL;
    for (KThread* w = g_core_wait[k]; w; w = w->core_next)
        if (!b || w->prio < b->prio || (w->prio == b->prio && w->core_ticket < b->core_ticket)) b = w;
    __atomic_store_n(&g_core_best[k], b ? b->prio : 64, __ATOMIC_RELAXED);
    core_flag_nl(k);
    return b;
}
/* core k to w, a thread waiting for it */
static void core_grant_nl(KThread* w, int k) {
    KThread** pp = &g_core_wait[k];
    while (*pp != w) pp = &(*pp)->core_next;
    *pp = w->core_next;
    core_first_nl(k);
    g_core_owner[k] = w;
    w->on_core = k;
    core_flag_nl(k);
    pc_broadcast(&g_core_cv[k]);
}
/* the best thread waiting for another core that k may take: k is idle, and the console runs a thread on an idle core of
   its mask rather than behind a busy one. A waiter of better priority than its core's holder stays, it takes that core
   at the holder's next poll */
static KThread* core_steal_nl(int k) {
    KThread* b = NULL;
    int from = -1;
    for (int i = 0; i < NCORES; i++)
        for (KThread* w = i == k ? NULL : g_core_wait[i]; w; w = w->core_next)
            if (((w->affinity >> k) & 1) && !(g_core_owner[i] && w->prio < g_core_owner[i]->prio) &&
                (!b || w->prio < b->prio || (w->prio == b->prio && w->core_ticket < b->core_ticket))) {
                b = w;
                from = i;
            }
    if (!b) return NULL;
    KThread** pp = &g_core_wait[from];
    while (*pp != b) pp = &(*pp)->core_next;
    *pp = b->core_next;
    core_first_nl(from);
    b->core = k;
    b->core_next = g_core_wait[k];
    g_core_wait[k] = b;
    pc_broadcast(&g_core_cv[from]); /* it waits there: core_grant_nl's on_core lets it go */
    return b;
}
static void core_release_nl(KThread* t) {
    int k = t->on_core;
    g_core_owner[k] = NULL;
    t->on_core = -1;
    __atomic_store_n(&t->cpu.poll, 0, __ATOMIC_RELAXED);
    KThread* w = core_first_nl(k);
    if (!w) w = core_steal_nl(k);
    if (w) core_grant_nl(w, k);
}
/* migrate 1: when its core is held by a thread it wouldn't preempt, t runs on an idle core of its mask instead; 2: when
   its core is held at all (a yield, which just handed it over) */
static void core_wait_nl(KThread* t, int migrate) {
    int k = t->core;
    KThread* o = g_core_owner[k];
    if (o && (migrate == 2 || (migrate == 1 && o->prio <= t->prio)))
        for (int j = 0; j < NCORES; j++)
            if (j != k && ((t->affinity >> j) & 1) && !g_core_owner[j] && !g_core_wait[j]) {
                t->core = k = j;
                break;
            }
    t->core_ticket = g_core_ticket++;
    t->on_core = -2;
    t->core_next = g_core_wait[k];
    g_core_wait[k] = t;
    KThread* w = core_first_nl(k); /* core_preempt sees the new waiter while the core is held */
    if (!g_core_owner[k]) core_grant_nl(w, k);
    while (t->on_core < 0) pc_wait(&g_core_cv[k], &g_core_lk, -1); /* k, or a core that took it (core_steal_nl) */
}
void core_enter(KThread* t) {
    if (!t || t->on_core >= 0) return;
    pm_lock(&g_core_lk);
    core_wait_nl(t, 1);
    pm_unlock(&g_core_lk);
}
void core_leave(KThread* t) {
    if (!t || t->on_core < 0) return;
    pm_lock(&g_core_lk);
    core_release_nl(t);
    pm_unlock(&g_core_lk);
}
void core_preempt(KThread* t) {
    int k = t->on_core;
    if (k < 0 || __atomic_load_n(&g_core_best[k], __ATOMIC_RELAXED) >= t->prio) return;
    pm_lock(&g_core_lk);
    core_release_nl(t);
    core_wait_nl(t, 1);
    pm_unlock(&g_core_lk);
}
void core_poll(KThread* t) {
    if (t) core_preempt(t);
}
void core_yield(KThread* t, int migrate, int any) {
    int k = t->on_core, best;
    if (k < 0) return;
    /* svcSetThreadCoreMask didn't move it meanwhile, and nobody it yields to waits: only a thread of its own priority
       (or a better one) unless the yield is to any thread. On the console a yield without or with core migration moves
       the thread behind its own priority's queue (Atmosphère's KScheduler::YieldWithoutCoreMigration), so a thread of
       lower priority never runs while this one could */
    best = __atomic_load_n(&g_core_best[k], __ATOMIC_RELAXED);
    if (t->core == k && (best == 64 || (!any && best > t->prio))) return;
    pm_lock(&g_core_lk);
    core_release_nl(t);
    core_wait_nl(t, migrate);
    pm_unlock(&g_core_lk);
}
void core_set_prio(KThread* t, int prio) {
    pm_lock(&g_core_lk);
    t->prio = prio;
    for (int k = 0; k < NCORES; k++) core_first_nl(k);
    pm_unlock(&g_core_lk);
}
static void thread_body(void* p) {
    KThread* t = p;
    kthread_set(t);
    core_enter(t);
    if (g_arm_mod) arm_thread_run(t);
    CPU* c = &t->cpu;
    snx_fpcr_apply(c);
    LOGV("[thread %llu] starts at %llx\n", (unsigned long long)t->id, (unsigned long long)c->pc);
    for (;;) {
        if (UNLIKELY(c->pc == 0)) { /* entry function returned */
            svc_exit_thread(t);
        }
        GuestFn f = snx_lookup(c, c->pc);
        f(c, c->pc);
    }
}
int kthread_live_count(void) {
    int live = 0;
    pm_lock(&g_klock);
    for (int i = 0; i < g_nthreads; i++)
        if (g_threads[i]->state != 2) live++;
    pm_unlock(&g_klock);
    return live;
}
void kthread_start(KThread* t) {
    t->state = 1;
    if (plat_thread(thread_body, t, (size_t)64 << 20)) fatal("could not create a system thread");
}
void svc_exit_thread(KThread* t) {
    core_leave(t);
    pm_lock(&g_klock);
    t->state = 2;
    kobj_signal_nl(&t->o);
    pm_unlock(&g_klock);
    LOGV("[thread %llu] exits\n", (unsigned long long)t->id);
    plat_thread_exit();
}

void kernel_init(void) {
    pm_init(&g_klock);
    pm_init(&g_core_lk);
    for (int k = 0; k < NCORES; k++) {
        pc_init(&g_core_cv[k]);
        g_core_best[k] = 64;
    }
}

/* main thread: stack size from npdm, x0=0, x1=own handle */
KThread* g_main;
int g_default_core;
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core) {
    if (core < 0 || core >= NCORES || !((g_core_mask >> core) & 1))
        fatal("The game's main thread asks for CPU core %d, which its own settings don't allow", core);
    g_default_core = core;
    stack_size = (stack_size + 0xFFF) & ~0xFFFull;
    u64 used = used_memory();
    if (used > mem_total() || stack_size > mem_total() - used)
        fatal("The game's image, system resource and main stack exceed its memory limit");
    u64 st = mem_find_free(STACK_BASE, STACK_BASE + STACK_SIZE, stack_size + 0x2000, 0x1000);
    if (!st || mem_map(st + 0x1000, stack_size, MS_STACK, PERM_RW))
        fatal("Could not map the game's main thread stack (%llu KB)", (unsigned long long)(stack_size >> 10));
    g_main_stack_size = stack_size; /* the main stack is charged separately from the heap */
    st += 0x1000;                   /* a free page on each side */
    KThread* t = kthread_create(entry, 0, st + stack_size, prio, core);
    g_main = t;
    strcpy(t->name, "main");
    u32 h = h_new(&t->o);
    t->cpu.x[1] = h;
    t->arm.r[1] = h;
    kthread_start(t);
}

/* svcExitProcess. exit() would run the atexit handlers and tear the runtime down while the other guest threads still
   run in it, so the process ends at once, the way main.c ends it when the window closes (after snx_save_before_exit,
   as there) */
void process_exit(int code) {
    snx_save_before_exit();
    LOG("The game exited (code %d)\n", code);
    plat_exit_now(code);
}
