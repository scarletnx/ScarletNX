/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The core model of thread.c against the console's scheduler: a ready thread that would wait behind its core's holder
   runs on an idle core its mask allows, a core that falls idle takes a thread waiting for another one, a thread of
   better priority stays and flags the holder, and only svcSleepThread(0) keeps a yielding thread on its core. A thread
   runs on its core's share of the host CPUs, apart from the other cores' threads, as on the console. The threads here
   are bare KThreads; each one that has to wait for a core waits on a host thread of its own */
#include "nx/hle/kernel/thread.c"
#include "nx/hle/hle.c"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

u8* g_mem;
MemLayout g_layout = {0x08000000ull,   0x2000000000ull, 0x100000000ull, 0x200000000ull,  0x300000000ull,
                      0x1000000000ull, 0x1300000000ull, 0x100000000ull, 0x1400000000ull, 0x1800000000ull};
GuestFn* g_disp;
u64 g_code_lo, g_code_hi, g_code_memory, g_main_stack_size;
const RtAPI g_rtapi;
const SnxArmModule* g_arm_mod; /* a 64-bit game */
const SnxArmRt g_arm_rt;
void arm_thread_run(KThread* t) { (void)t; }
/* fatal ends the test, but where the test catches it (fatal_jump set): its message kept, back to the test; the last
   line logged kept too */
static jmp_buf* fatal_jump;
static char fatal_said[256], logged[256];
void fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    if (fatal_jump) {
        vsnprintf(fatal_said, sizeof fatal_said, fmt, a);
        va_end(a);
        longjmp(*fatal_jump, 1);
    }
    vfprintf(stderr, fmt, a);
    va_end(a);
    abort();
}
void logf_(int lvl, const char* fmt, ...) {
    va_list a;
    (void)lvl;
    va_start(a, fmt);
    vsnprintf(logged, sizeof logged, fmt, a);
    va_end(a);
}
u32 h_new(KObj* o) {
    (void)o;
    return 0;
}
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    (void)size;
    (void)state;
    (void)perm;
    return 0;
}
void kobj_signal_nl(KObj* o) { (void)o; }
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) {
    (void)lo;
    (void)hi;
    (void)size;
    (void)align;
    return 0;
}
int mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    (void)addr;
    (void)size;
    (void)state;
    (void)perm;
    return 0;
}
u64 mem_total(void) { return 0; }
u64 used_memory(void) { return 0; }
void plat_exit_now(int code) { exit(code); }
/* a guest thread's host thread, never run here: the stack it was asked with; thread_fails makes the system refuse
   it, as one out of room does */
static size_t started_stack;
static int starts, thread_fails;
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    (void)fn;
    (void)arg;
    started_stack = stack;
    ++starts;
    if (thread_fails) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}
int plat_address_space(u64* mapped, u64* largest_free) {
    *mapped = (u64)1000 << 20;
    *largest_free = (u64)20 << 20;
    return 1;
}
void plat_thread_exit(void) { pthread_exit(NULL); }
/* a game pause's clock, and its report of a thread that didn't stop */
u64 plat_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000u + (u64)t.tv_nsec;
}
void plat_sleep_ns(i64 ns) {
    struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&t, NULL);
}
void kernel_dump_threads(void) {}
void save_hints(void) {}
void snx_save_before_exit(void) {}
void snx_fpcr_apply(CPU* c) { (void)c; }
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pc_init(PCond* c) { pthread_cond_init(&c->c, NULL); }
void pc_signal(PCond* c) { pthread_cond_signal(&c->c); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    int r;
    (void)ns; /* the core model only waits without a timeout */
    /* a failed wait (EINVAL on macOS for a condition variable never initialized) returns at once without
       releasing the lock, and the caller's loop would spin holding it */
    if ((r = pthread_cond_wait(&c->c, &m->l)) != 0) fatal("pthread_cond_wait: %d\n", r);
    return r;
}

static unsigned checks, failures;
#define CHECK(expr, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "kernel_cores.c:%d: %s: ", __LINE__, #expr);                                               \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

enum { SNX_KCT_THREADS = 4 };
/* the host: 12 CPUs, as the machine a game's job worker stopped on (kernel/thread.c). core_pin's last move, the last
   one of each test thread (snx_kct_who, which the host threads below set; the test's own host thread's is slot
   SNX_KCT_THREADS), and how many there were */
static int snx_kct_host[256] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, snx_kct_nhost = 12;
static char snx_kct_moved[256], snx_kct_moved_by[SNX_KCT_THREADS + 1][256];
static int snx_kct_moves;
static _Thread_local int snx_kct_who = SNX_KCT_THREADS;
static pthread_mutex_t snx_kct_lk = PTHREAD_MUTEX_INITIALIZER;
int plat_cpus(int* ids, int max) {
    int n = snx_kct_nhost < max ? snx_kct_nhost : max;
    memcpy(ids, snx_kct_host, sizeof *ids * (size_t)n);
    return n;
}
void plat_thread_cpus(const int* ids, int n) {
    int len = 0;
    pthread_mutex_lock(&snx_kct_lk);
    snx_kct_moved[0] = 0;
    for (int i = 0; i < n; i++)
        len += snprintf(snx_kct_moved + len, sizeof snx_kct_moved - len, i ? " %d" : "%d", ids[i]);
    strcpy(snx_kct_moved_by[snx_kct_who], snx_kct_moved);
    snx_kct_moves++;
    pthread_mutex_unlock(&snx_kct_lk);
}
/* the threads are the pool's first slots, where the cores' lists find them (kt_at) */
#define snx_kct_thread (g_hle.thread.threads)
/* thread i at this priority, on this core, with this core mask; nobody holds or waits for a core */
static KThread* thread(int i, int prio, int core, u64 mask) {
    KThread* t = &snx_kct_thread[i];
    memset(t, 0, sizeof *t);
    /* a thread waits for its core on its own cv, as kthread_create sets it up: all zeros is a valid condition
       variable on glibc but not on macOS */
    pc_init(kt_cv(t));
    t->prio = t->base_prio = prio;
    t->core = core;
    t->affinity = mask;
    t->on_core = -1;
    t->pinned = -1;
    return t;
}
static void reset(void) {
    for (int k = 0; k < NCORES; k++) {
        g_hle.thread.core_owner[k] = 0;
        g_hle.thread.core_wait[k] = 0;
        g_hle.thread.core_best[k] = 64;
    }
}
static int on_core(KThread* t) {
    pm_lock(&g_host.thread.core_lk);
    int k = t->on_core;
    pm_unlock(&g_host.thread.core_lk);
    return k;
}
/* until t holds a core, or waits for one (-2), for up to 2 s; the core it ends on */
static int settle(KThread* t, int want) {
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000 && on_core(t) != want; i++) nanosleep(&pause, NULL);
    return on_core(t);
}
/* the last move (by >= 0: the last one of test thread by) went to the CPUs in want, such as "1 4 7 10", and there
   were moves moves so far */
static int moved(int by, const char* want, int moves) {
    const char* got = by < 0 ? snx_kct_moved : snx_kct_moved_by[by];
    int ok;
    pthread_mutex_lock(&snx_kct_lk);
    ok = !strcmp(got, want) && snx_kct_moves == moves;
    if (!ok) fprintf(stderr, "  moved to \"%s\" (%d moves), not \"%s\" (%d)\n", got, snx_kct_moves, want, moves);
    pthread_mutex_unlock(&snx_kct_lk);
    return ok;
}
static int moved_to(const char* want, int moves) { return moved(-1, want, moves); }
/* a host with these CPUs, nothing moved yet */
static void host(const int* ids, int n) {
    memcpy(snx_kct_host, ids, sizeof *ids * (size_t)n);
    snx_kct_nhost = n;
    g_hle.thread.host_ncpu = plat_cpus(g_hle.thread.host_cpu, 256);
    memset(snx_kct_moved_by, 0, sizeof snx_kct_moved_by);
    snx_kct_moved[0] = 0;
    snx_kct_moves = 0;
}
static void* enter_body(void* p) {
    snx_kct_who = (int)((KThread*)p - snx_kct_thread);
    core_enter((KThread*)p);
    return NULL;
}
/* a thread for a game pause: mode 0 runs game code that polls (counting steps), 1 game code that never polls, 2 is
   blocked in a service's wait until signaled, then takes a step */
typedef struct {
    KThread* t;
    int mode, stop, signaled;
    u64 steps;
    PMutex m;
    PCond cv;
} SnxKctGame;
static void* game_body(void* p) {
    SnxKctGame* g = (SnxKctGame*)p;
    tls_thread = g->t;
    if (g->mode == 2) {
        pm_lock(&g->m);
        while (!g->signaled) kthread_wait(&g->cv, &g->m, -1);
        pm_unlock(&g->m);
        __atomic_add_fetch(&g->steps, 1, __ATOMIC_SEQ_CST);
        return NULL;
    }
    core_enter(g->t);
    while (!__atomic_load_n(&g->stop, __ATOMIC_SEQ_CST)) {
        __atomic_add_fetch(&g->steps, 1, __ATOMIC_SEQ_CST);
        if (g->mode == 0 && __atomic_load_n(&g->t->cpu.poll, __ATOMIC_RELAXED)) core_poll(g->t);
    }
    core_leave(g->t);
    return NULL;
}
typedef struct {
    KThread* t;
    int migrate;
} SnxKctYield;
static void* yield_body(void* p) {
    SnxKctYield* y = (SnxKctYield*)p;
    snx_kct_who = (int)(y->t - snx_kct_thread);
    core_yield(y->t, y->migrate);
    return NULL;
}

void kobj_ref(KObj* o) {
    if (o) o->refs++;
}
void kobj_unref(KObj* o) {
    if (o && --o->refs == 0 && o->type == KO_THREAD) kthread_destroy(o);
}

int main(void) {
    hle_init();
    g_hle.svcmem.core_mask = 7; /* cores 0 to 2, as an application's npdm gives */
    static const int twelve[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    pthread_t h, h2;
    kernel_init();

    /* a ready thread behind a holder it wouldn't preempt runs on an idle core of its mask, at once. Whatever is
       expected to return at once runs on a host thread too, so a scheduler that blocks it fails here instead of
       hanging the test: the cleanup lets it through */
    reset();
    KThread *a = thread(0, 44, 0, 7), *b = thread(1, 45, 0, 7), *c, *w;
    core_enter(a);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, 1) == 1 && b->core == 1, "behind 44 on core 0: on core %d", on_core(b));
    core_leave(a);
    pthread_join(h, NULL);
    core_leave(b);

    /* a thread of better priority stays on its core, waits, and flags the holder */
    reset();
    a = thread(0, 45, 0, 7), b = thread(1, 43, 0, 7);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, -2) == -2 && b->core == 0, "43 behind 45: on core %d, waiting for %d", b->on_core, b->core);
    CHECK(__atomic_load_n(&a->cpu.poll, __ATOMIC_RELAXED) == 1, "the holder is not asked to let go");
    core_leave(a);
    pthread_join(h, NULL);
    CHECK(b->on_core == 0, "43 got core %d after the holder let go", b->on_core);
    core_leave(b);

    /* a thread whose mask is its own core never moves, with every other core idle */
    reset();
    a = thread(0, 44, 0, 1), b = thread(1, 45, 0, 1);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, -2) == -2 && b->core == 0, "mask 1: on core %d", b->on_core);
    core_leave(a);
    pthread_join(h, NULL);
    CHECK(b->on_core == 0, "mask 1: got core %d", b->on_core);
    core_leave(b);

    /* a core that falls idle takes a thread waiting for another one, if its mask allows it */
    reset();
    a = thread(0, 44, 0, 1), c = thread(2, 44, 1, 2), b = thread(1, 46, 0, 3);
    core_enter(a);
    core_enter(c);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, -2) == -2 && b->core == 0, "cores 0 and 1 busy: on core %d", b->on_core);
    core_leave(c);
    CHECK(settle(b, 1) == 1 && b->core == 1, "core 1 fell idle: 46 on core %d", on_core(b));
    pm_lock(&g_host.thread.core_lk);
    CHECK(!g_hle.thread.core_wait[0] && g_hle.thread.core_best[0] == 64 && !a->cpu.poll, "core 0 still has a waiter, best %d",
          g_hle.thread.core_best[0]);
    pm_unlock(&g_host.thread.core_lk);
    core_leave(a);
    pthread_join(h, NULL);
    core_leave(b);

    /* but not one that will take its own core from a worse holder */
    reset();
    a = thread(0, 46, 0, 1), c = thread(2, 44, 1, 2), b = thread(1, 45, 0, 3);
    core_enter(a);
    core_enter(c);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, -2) == -2, "45 behind 46: on core %d", b->on_core);
    core_leave(c);
    CHECK(on_core(b) == -2 && !g_hle.thread.core_owner[1], "45 left its core for core %d", on_core(b));
    core_leave(a);
    pthread_join(h, NULL);
    CHECK(b->on_core == 0, "45 got core %d", b->on_core);
    core_leave(b);

    /* svcSleepThread(0) hands the core over and waits for it again, other cores idle or not */
    reset();
    a = thread(0, 44, 0, 7), w = thread(3, 50, 0, 1);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "50 with mask 1 waits: on core %d", w->on_core);
    SnxKctYield y = {a, 0};
    pthread_create(&h2, NULL, yield_body, &y);
    pthread_join(h, NULL);
    CHECK(w->on_core == 0 && settle(a, -2) == -2 && a->core == 0, "yield 0: 50 on %d, 44 on %d for %d", w->on_core,
          on_core(a), a->core);
    core_leave(w);
    pthread_join(h2, NULL);
    CHECK(a->on_core == 0, "yield 0: back on core %d", a->on_core);
    core_leave(a);

    /* svcSleepThread(-1) hands it over and runs on an idle core, on that core's host CPUs */
    host(twelve, 12);
    reset();
    a = thread(0, 44, 0, 7), w = thread(3, 50, 0, 1);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "50 with mask 1 waits: on core %d", w->on_core);
    y.t = a, y.migrate = 2;
    pthread_create(&h2, NULL, yield_body, &y);
    CHECK(settle(a, 1) == 1 && a->core == 1 && settle(w, 0) == 0, "yield -1: 50 on %d, 44 on %d", on_core(w),
          on_core(a));
    pthread_join(h2, NULL);
    pthread_join(h, NULL); /* 50's own move, into core 0's CPUs, is the third: done once its enter returns */
    CHECK(moved(0, "1 4 7 10", 3), "yield -1: 44 on core 1's CPUs");
    core_leave(w);
    core_leave(a);

    /* each core the game may use runs its threads on every third host CPU, its own */
    host(twelve, 12);
    reset();
    a = thread(0, 44, 0, 1), b = thread(1, 44, 1, 2), c = thread(2, 44, 2, 4);
    core_enter(a);
    CHECK(moved_to("0 3 6 9", 1), "core 0's thread");
    core_enter(b);
    CHECK(moved_to("1 4 7 10", 2), "core 1's thread");
    core_enter(c);
    CHECK(moved_to("2 5 8 11", 3), "core 2's thread");
    /* taking the same core again moves nothing */
    core_leave(a);
    core_enter(a);
    CHECK(moved_to("2 5 8 11", 3), "core 0's thread again");
    core_leave(a);
    core_leave(b);
    core_leave(c);

    /* a thread that ran on core 0 and that an idle core takes later moves to that core's CPUs */
    host(twelve, 12);
    reset();
    a = thread(0, 44, 0, 1), b = thread(1, 45, 0, 7);
    core_enter(b);
    core_leave(b);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, 1) == 1, "behind 44 on core 0: on core %d", on_core(b));
    pthread_join(h, NULL);
    CHECK(moved_to("1 4 7 10", 3), "the thread core 1 took");
    core_leave(a);
    core_leave(b);

    /* and so does a holder that lets a better thread have its core and goes on on an idle one */
    host(twelve, 12);
    reset();
    a = thread(0, 45, 0, 3), w = thread(3, 43, 0, 1);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "43 waits for core 0: on core %d", w->on_core);
    core_preempt(a);
    pthread_join(h, NULL);
    CHECK(a->on_core == 1 && moved(SNX_KCT_THREADS, "1 4 7 10", 3), "the preempted 45 on core %d", a->on_core);
    core_leave(a);
    core_leave(w);

    /* the shares follow the game's cores (1 and 2 here) and the CPUs the process was given (taskset 2-9) */
    static const int eight[] = {2, 3, 4, 5, 6, 7, 8, 9};
    host(eight, 8);
    g_hle.svcmem.core_mask = 6;
    reset();
    a = thread(0, 44, 1, 2), b = thread(1, 44, 2, 4);
    core_enter(a);
    CHECK(moved_to("2 4 6 8", 1), "core 1 of cores 1 and 2");
    core_enter(b);
    CHECK(moved_to("3 5 7 9", 2), "core 2 of cores 1 and 2");
    core_leave(a);
    core_leave(b);
    g_hle.svcmem.core_mask = 7;
    host(eight, 8);
    reset();
    a = thread(0, 44, 2, 4);
    core_enter(a);
    CHECK(moved_to("4 7", 1), "core 2 of cores 0 to 2");
    core_leave(a);

    /* fewer host CPUs than cores: nothing to share out, nothing moves */
    host(twelve, 2);
    reset();
    a = thread(0, 44, 0, 1), b = thread(1, 44, 2, 4);
    core_enter(a);
    core_enter(b);
    CHECK(moved_to("", 0), "2 host CPUs for 3 cores");
    core_leave(a);
    core_leave(b);

    /* a host thread the system refuses: the fatal says why, and the log the stack it asked and what the address space
       holds */
    {
        KThread* t = kthread_create(0x1000, 0, 0x2000, 44, 0);
        jmp_buf here;
        g_settings.thread_stack_mb = 16;
        thread_fails = 1;
        fatal_jump = &here;
        if (!setjmp(here)) kthread_start(t);
        fatal_jump = NULL;
        thread_fails = 0;
        CHECK(strstr(fatal_said, strerror(EAGAIN)) != NULL, "the fatal says the reason: %s", fatal_said);
        CHECK(strstr(logged, "a 16 MiB stack (thread_stack_mb) could not start, 1000 MiB of the address space mapped, "
                             "its largest free run 20 MiB") != NULL,
              "the log says the address space: %s", logged);
        t->state = 2; /* exited, as one that started would be */
        kthread_destroy(&t->o);
    }

    /* the thread pool: KTHREAD_MAX (256) slots, filled by as many threads; an exited thread keeps its slot (the live
       count leaves it out) until its last reference goes, and then a new thread takes that slot. A full pool is a bound
       below what a game uses, asserted, so no thread is left out. The tests above used the first slots */
    reset();
    memset(g_hle.thread.threads, 0, sizeof g_hle.thread.threads);
    {
        KThread* first[KTHREAD_MAX];
        KThread* t;
        int taken = 0;
        for (int i = 0; i < KTHREAD_MAX; i++) first[i] = kthread_create(0x1000, 0, 0x2000, 44, 0);
        for (int i = 0; i < KTHREAD_MAX; i++) taken += g_hle.thread.threads[i].o.type == KO_THREAD;
        CHECK(taken == KTHREAD_MAX && kthread_live_count() == KTHREAD_MAX, "%d threads, %d live", taken,
              kthread_live_count());
        first[7]->state = 2;
        CHECK(kthread_live_count() == KTHREAD_MAX - 1, "one exited: %d live", kthread_live_count());
        kthread_destroy(&first[7]->o); /* its last reference */
        CHECK(first[7]->o.type == 0 && kthread_live_count() == KTHREAD_MAX - 1, "the exited thread's slot is free");
        t = kthread_create(0x1000, 0, 0x2000, 44, 0);
        CHECK(t == first[7] && t->id == first[KTHREAD_MAX - 1]->id + 1 && kthread_live_count() == KTHREAD_MAX,
              "the freed slot holds the new thread, %llu", (unsigned long long)t->id);
        /* its host thread starts on thread_stack_mb's stack */
        int before = starts;
        g_settings.thread_stack_mb = 8;
        kthread_start(t);
        CHECK(starts == before + 1 && started_stack == (size_t)8 << 20, "%d started, a stack of %zu bytes",
              starts - before, started_stack);
    }

    /* a game pause: two threads running game code on one core both stop at a poll, the second once the first lets go
       of the core it waits for; a thread blocked in a service's wait counts as stopped, and when its wait ends during
       the pause it runs nothing until the pause ends. Nothing moves while the pause holds, and everything goes on
       after it */
    {
        static SnxKctGame g[4];
        pthread_t gh[4];
        u64 seen[3];
        reset();
        /* the pool test above started a thread that has no host thread: only these four count for the pause */
        for (int i = 4; i < KTHREAD_MAX; i++) g_hle.thread.threads[i].state = 0;
        for (int i = 0; i < 4; i++) {
            g[i].t = &g_hle.thread.threads[i];
            g[i].t->state = 1; /* started: the pause counts it */
            pm_init(&g[i].m);
            pc_init(&g[i].cv);
        }
        g[2].mode = 2;
        pthread_create(&gh[0], NULL, game_body, &g[0]);
        CHECK(settle(g[0].t, 0) == 0, "the first runs on core 0: %d", on_core(g[0].t));
        pthread_create(&gh[1], NULL, game_body, &g[1]);
        CHECK(settle(g[1].t, -2) == -2, "the second waits for core 0: %d", on_core(g[1].t));
        pthread_create(&gh[2], NULL, game_body, &g[2]);
        while (!__atomic_load_n(&g[2].t->stopped, __ATOMIC_SEQ_CST)) sched_yield();
        g[3].t->state = 0; /* the fourth isn't started yet */
        CHECK(kernel_hold(2000) == 0, "the pause holds");
        CHECK(g[0].t->stopped == 2 && g[1].t->stopped == 2 && g[2].t->stopped == 1,
              "stopped at a poll, at a poll, blocked: %d %d %d", g[0].t->stopped, g[1].t->stopped, g[2].t->stopped);
        for (int i = 0; i < 3; i++) seen[i] = __atomic_load_n(&g[i].steps, __ATOMIC_SEQ_CST);
        pm_lock(&g[2].m); /* the blocked one's wait ends */
        g[2].signaled = 1;
        pc_broadcast(&g[2].cv);
        pm_unlock(&g[2].m);
        {
            struct timespec pause = {0, 30000000};
            nanosleep(&pause, NULL);
        }
        CHECK(seen[0] && seen[1] && seen[0] == g[0].steps && seen[1] == g[1].steps && !g[2].steps,
              "nothing ran while paused: %llu %llu %llu", (unsigned long long)g[0].steps,
              (unsigned long long)g[1].steps, (unsigned long long)g[2].steps);
        CHECK(g[2].t->stopped == 2, "the woken one waits at a hold point: %d", g[2].t->stopped);
        kernel_release();
        for (int i = 0; i < 2000 && !(__atomic_load_n(&g[2].steps, __ATOMIC_SEQ_CST) &&
                                      __atomic_load_n(&g[0].steps, __ATOMIC_SEQ_CST) +
                                              __atomic_load_n(&g[1].steps, __ATOMIC_SEQ_CST) >
                                          seen[0] + seen[1]);
             i++) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        CHECK(g[2].steps == 1 && g[0].steps + g[1].steps > seen[0] + seen[1], "everything goes on after the pause");
        for (int i = 0; i < 3; i++) __atomic_store_n(&g[i].stop, 1, __ATOMIC_SEQ_CST);
        for (int i = 0; i < 3; i++) pthread_join(gh[i], NULL);
        for (int i = 0; i < 3; i++) g[i].t->state = 2;

        /* a thread that never polls: the pause gives up, and the game goes on */
        g[3].t->state = 1;
        g[3].mode = 1;
        pthread_create(&gh[3], NULL, game_body, &g[3]);
        CHECK(settle(g[3].t, 0) == 0, "it runs on core 0: %d", on_core(g[3].t));
        CHECK(kernel_hold(50) == -1 && !g_hle.thread.hold, "the pause gives up and lets go");
        seen[0] = __atomic_load_n(&g[3].steps, __ATOMIC_SEQ_CST);
        {
            struct timespec pause = {0, 5000000};
            nanosleep(&pause, NULL);
        }
        CHECK(__atomic_load_n(&g[3].steps, __ATOMIC_SEQ_CST) > seen[0], "it goes on");
        __atomic_store_n(&g[3].stop, 1, __ATOMIC_SEQ_CST);
        pthread_join(gh[3], NULL);
        g[3].t->state = 2;
    }

    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
