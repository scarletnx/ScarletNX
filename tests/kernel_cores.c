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
#include <sched.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <time.h>

u8* g_mem;
MemLayout g_layout = {0x08000000ull,   0x2000000000ull, 0x100000000ull, 0x200000000ull,  0x300000000ull,
                      0x1000000000ull, 0x1300000000ull, 0x100000000ull, 0x1400000000ull, 0x1800000000ull};
GuestFn* g_disp;
u64 g_code_lo, g_code_hi, g_code_memory, g_main_stack_size;
const RtAPI g_rtapi = {.cas128 = snx_runtime_cas128};
const SnxArmModule* g_arm_mod; /* a 64-bit game */
int g_ounce;                   /* main.c's: a Switch program unless a case says otherwise */
const SnxArmRt g_arm_rt;
void arm_thread_run(KThread* t) { (void)t; }
/* gpu.c's, which kernel_init sets: the renderer calls it as a change of guest memory starts to wait for it */
void (*g_gpu_mapping_waits)(void);
/* nce.c, whose NCE no game here is of: thread_run asks it, and a flagged core kicks its thread */
int nce_on(void) { return 0; }
void nce_thread_run(KThread* t, u64 pc) { (void)t, (void)pc; }
void nce_kick(KThread* t) { (void)t; }
/* guest_stats.c, which no core here counts for: a thread's start and end */
void guest_stats_thread_start(u32 ix) { (void)ix; }
void guest_stats_thread_end(u32 ix) { (void)ix; }
/* svc.c's resume of a thread a save state restored (thread_resume, which no case here calls): an unoptimized build keeps
   those calls, and the test links at every level with these, which abort if a case ever reaches them */
void svc_resume(KThread* t) { (void)t, abort(); }
void svc_resume_leave(KThread* t) { (void)t, abort(); }
void svc_service_retry(KThread* t) { (void)t, abort(); }
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
/* a thread's TLS page (thread.c's tls_alloc_nl): with no guest memory here a guest address is the host's, so this is a
   host page, the same for every caller, where a poll reads the thread's disable count as zero */
static u8 snx_kct_tls[0x1000];
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    (void)size;
    (void)state;
    (void)perm;
    return (u64)(uintptr_t)snx_kct_tls;
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
/* the thread's id, as common/platform/thread.c gives it: here, so the test doesn't link that file's mutexes over
   its own */
#if defined(__linux__)
uint32_t plat_tid(void) { return (uint32_t)syscall(SYS_gettid); }
#else
uint32_t plat_tid(void) { return (uint32_t)(uintptr_t)pthread_self(); }
#endif
/* the system's name for a thread (common.c), which this test does not link */
void snx_thread_name(const char* name) { (void)name; }
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
/* kernel_core_watch's question: unknown here */
void plat_thread_where(u32 tid, char* out, size_t n) {
    (void)tid;
    snprintf(out, n, "?");
}
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
void plat_yield(void) { sched_yield(); }
void pc_signal(PCond* c) { pthread_cond_signal(&c->c); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
/* a service's wait (kthread_wait) */
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    int r;
    (void)ns; /* the test's service waits have no timeout */
    /* a failed wait (EINVAL on macOS for a condition variable never initialized) returns at once without
       releasing the lock, and the caller's loop would spin holding it */
    if ((r = pthread_cond_wait(&c->c, &m->l)) != 0) fatal("pthread_cond_wait: %d\n", r);
    return r;
}
/* a thread waits on its word, polled here until the word moves: a wait the word never ends fails the test after 2 s
   instead of hanging it, and each wake of a word is counted (snx_kct_woken, by the word's thread) */
static unsigned snx_kct_woken[KTHREAD_MAX];
void plat_wait_word(u32* w, u32 v, i64 ns) {
    struct timespec pause = {0, 100000};
    (void)ns; /* the waits here have no timeout */
    for (int i = 0; i < 20000 && __atomic_load_n(w, __ATOMIC_ACQUIRE) == v; i++) nanosleep(&pause, NULL);
    if (__atomic_load_n(w, __ATOMIC_ACQUIRE) == v) fatal("a wait on a thread's word was never ended\n");
}
void plat_wake_word(u32* w) { __atomic_add_fetch(&snx_kct_woken[w - g_host.thread.wword], 1u, __ATOMIC_SEQ_CST); }
void pm_wait_note(const void* m, const void* at, int cond) {
    (void)m;
    (void)at;
    (void)cond;
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
int plat_thread_cpus(const int* ids, int n) {
    int len = 0;
    pthread_mutex_lock(&snx_kct_lk);
    snx_kct_moved[0] = 0;
    for (int i = 0; i < n; i++)
        len += snprintf(snx_kct_moved + len, sizeof snx_kct_moved - len, i ? " %d" : "%d", ids[i]);
    strcpy(snx_kct_moved_by[snx_kct_who], snx_kct_moved);
    snx_kct_moves++;
    pthread_mutex_unlock(&snx_kct_lk);
    return 0;
}
/* the threads are the pool's first slots, where the cores' lists find them (kt_at) */
#define snx_kct_thread (g_hle.thread.threads)
/* thread i at this priority, on this core, with this core mask; nobody holds or waits for a core */
static KThread* thread(int i, int prio, int core, u64 mask) {
    KThread* t = &snx_kct_thread[i];
    memset(t, 0, sizeof *t);
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
/* until the 16-bit word at guest address at holds want, for up to 2 s; what it holds then */
static u16 settle16(u64 at, u16 want) {
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000 && gr16(at) != want; i++) nanosleep(&pause, NULL);
    return gr16(at);
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
    t_hle.thread.cur = g->t;
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
/* a thread's next poll in game code (a backward branch with its poll set) */
static void* poll_body(void* p) {
    snx_kct_who = (int)((KThread*)p - snx_kct_thread);
    core_poll((KThread*)p);
    return NULL;
}
/* an SVC of a thread, as svc.c's svc_enter runs it: a waiter of better priority takes the core on the way in */
static void* svc_body(void* p) {
    snx_kct_who = (int)((KThread*)p - snx_kct_thread);
    core_enter((KThread*)p);
    core_preempt((KThread*)p);
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

    /* A change of guest memory that waits for the renderer (g_gpu_mapping_waits, which the renderer calls only then):
       the thread in the SVC lets go of its core, which the thread of its priority lined up behind it takes, and the
       wait is counted and said once. Until then it holds the core through its SVC, as on the console, and outside
       an SVC it keeps it, as nothing would take it back */
    reset();
    a = thread(0, 44, 0, 1), b = thread(1, 44, 0, 1);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, -2) == -2 && b->core == 0, "the same priority and core: on core %d", b->on_core);
    CHECK(g_gpu_mapping_waits == core_leave_waiting, "the kernel did not take the renderer's waits");
    t_hle.thread.cur = a;
    a->id = 82;
    a->in_svc = 1; /* svc_enter: the SVC itself moves no core */
    CHECK(on_core(a) == 0 && on_core(b) == -2, "in an SVC that waits for nothing: holder on %d, the other on %d",
          on_core(a), on_core(b));
    a->in_svc = 0;
    logged[0] = 0;
    g_gpu_mapping_waits();
    CHECK(on_core(a) == 0 && on_core(b) == -2 && !g_host.thread.waits_off_core && !logged[0],
          "outside an SVC: holder on %d, the other on %d, %llu waits counted, said \"%s\"", on_core(a), on_core(b),
          (unsigned long long)g_host.thread.waits_off_core, logged);
    a->in_svc = 1;
    g_gpu_mapping_waits();
    pthread_join(h, NULL);
    CHECK(on_core(a) == -1 && b->on_core == 0 && g_host.thread.waits_off_core == 1,
          "a wait in an SVC: holder on %d, the other on %d, %llu waits counted", on_core(a), b->on_core,
          (unsigned long long)g_host.thread.waits_off_core);
    CHECK(!strcmp(logged,
                  "kernel: thread 82 let go of core 0 in a memory SVC to wait for the renderer, the first time\n"),
          "said \"%s\"", logged);
    g_gpu_mapping_waits(); /* it holds none now: nothing more to let go of, or to count */
    CHECK(b->on_core == 0 && g_host.thread.waits_off_core == 1, "a second wait: the other on %d, %llu waits counted",
          b->on_core, (unsigned long long)g_host.thread.waits_off_core);
    a->in_svc = 0;
    t_hle.thread.cur = NULL;
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

    /* with nobody waiting for its core, svcSleepThread(-1) takes a thread of its priority waiting for a busy core its
       core is in the mask of, and waits behind it (Mesosphère's YieldWithCoreMigration) */
    reset();
    a = thread(0, 44, 0, 7), c = thread(2, 44, 2, 4), w = thread(3, 44, 2, 5);
    core_enter(a);
    core_enter(c);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2 && w->core == 2, "44 behind 44 on core 2: on core %d", on_core(w));
    y.t = a, y.migrate = 2;
    pthread_create(&h2, NULL, yield_body, &y);
    CHECK(settle(w, 0) == 0 && settle(a, -2) == -2 && a->core == 0, "pull: 44 of core 2 on %d, the yielder on %d",
          on_core(w), on_core(a));
    pthread_join(h, NULL);
    core_leave(w);
    pthread_join(h2, NULL);
    CHECK(a->on_core == 0, "pull: the yielder back on core %d", a->on_core);
    core_leave(a);
    core_leave(c);

    /* but not one of worse priority, and svcSleepThread(0) takes none */
    reset();
    a = thread(0, 44, 0, 7), c = thread(2, 44, 2, 4), w = thread(3, 45, 2, 5);
    core_enter(a);
    core_enter(c);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "45 behind 44 on core 2: on core %d", on_core(w));
    y.t = a, y.migrate = 2;
    pthread_create(&h2, NULL, yield_body, &y);
    pthread_join(h2, NULL); /* at once: nothing to take */
    CHECK(a->on_core == 0 && on_core(w) == -2, "yield -1 took 45: 44 on %d, 45 on %d", a->on_core, on_core(w));
    core_set_prio(w, 44);
    y.migrate = 0;
    pthread_create(&h2, NULL, yield_body, &y);
    pthread_join(h2, NULL);
    CHECK(a->on_core == 0 && on_core(w) == -2, "yield 0 took 44: 44 on %d, 44 on %d", a->on_core, on_core(w));
    core_leave(c);
    pthread_join(h, NULL);
    CHECK(w->on_core == 2, "the waiter got core %d", w->on_core);
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
        KThread* t;
        jmp_buf here;
        CHECK(kthread_create(0x1000, 0, 0x2000, 44, 0, &t) == 0 && t, "a thread");
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

    /* The process's threads. THREAD_MAX (96) that haven't exited, started or not: one more is LimitReached, as the
       console answers an application at its ThreadCountMax, where it used to be made with a warning. An exited thread
       stops counting but keeps its slot of the pool's KTHREAD_MAX (256) until its last reference goes: with every slot
       in use a creation below the limit is OutOfResource, and a freed slot holds the next thread. A refused creation
       takes no thread id. An Ounce program's limit isn't known: its 97th thread is made, and the log says so. The
       tests above used the first slots */
    reset();
    memset(g_hle.thread.threads, 0, sizeof g_hle.thread.threads);
    {
        KThread* first[KTHREAD_MAX];
        KThread* t = NULL;
        int taken = 0;
        u32 rc = 0;
        for (int i = 0; i < THREAD_MAX; i++) rc |= kthread_create(0x1000, 0, 0x2000, 44, 0, &first[i]);
        CHECK(rc == 0 && kthread_live_count() == THREAD_MAX, "%d threads made (%x)", kthread_live_count(), rc);
        t = first[0];
        rc = kthread_create(0x1000, 0, 0x2000, 44, 0, &t);
        CHECK(rc == SNX_RC_SVC_LIMIT_REACHED && !t && kthread_live_count() == THREAD_MAX,
              "one more than THREAD_MAX: %x, %d live", rc, kthread_live_count());
        first[3]->state = 1; /* started: it counts as before */
        CHECK(kthread_create(0x1000, 0, 0x2000, 44, 0, &t) == SNX_RC_SVC_LIMIT_REACHED, "a started thread counts");
        g_ounce = 1;
        logged[0] = 0;
        rc = kthread_create(0x1000, 0, 0x2000, 44, 0, &t);
        CHECK(rc == 0 && t && strstr(logged, "thread 97 of an Ounce program, past the 96"), "an Ounce program: %x, %s",
              rc, logged);
        g_ounce = 0;
        if (t) kthread_destroy(&t->o);
        /* each exit makes room for a creation, and the exited thread's slot stays taken */
        rc = 0;
        for (int i = THREAD_MAX; i < KTHREAD_MAX; i++) {
            first[i - THREAD_MAX]->state = 2;
            rc |= kthread_create(0x1000, 0, 0x2000, 44, 0, &first[i]);
        }
        for (int i = 0; i < KTHREAD_MAX; i++) taken += g_hle.thread.threads[i].o.type == KO_THREAD;
        CHECK(rc == 0 && taken == KTHREAD_MAX && kthread_live_count() == THREAD_MAX, "%d threads, %d live (%x)", taken,
              kthread_live_count(), rc);
        CHECK(kthread_create(0x1000, 0, 0x2000, 44, 0, &t) == SNX_RC_SVC_LIMIT_REACHED, "the limit, before the pool");
        first[200]->state = 2;
        rc = kthread_create(0x1000, 0, 0x2000, 44, 0, &t);
        CHECK(rc == SNX_RC_SVC_OUT_OF_RESOURCE && !t && kthread_live_count() == THREAD_MAX - 1,
              "a full pool below the limit: %x, %d live", rc, kthread_live_count());
        kthread_destroy(&first[7]->o); /* an exited thread's last reference */
        CHECK(first[7]->o.type == 0 && kthread_live_count() == THREAD_MAX - 1, "the exited thread's slot is free");
        rc = kthread_create(0x1000, 0, 0x2000, 44, 0, &t);
        CHECK(rc == 0 && t == first[7] && t->id == first[KTHREAD_MAX - 1]->id + 1 && kthread_live_count() == THREAD_MAX,
              "the freed slot holds the new thread, %llu (%x)", (unsigned long long)(t ? t->id : 0), rc);
        /* its host thread starts on thread_stack_mb's stack */
        int before = starts;
        g_settings.thread_stack_mb = 8;
        kthread_start(t);
        CHECK(starts == before + 1 && started_stack == (size_t)8 << 20, "%d started, a stack of %zu bytes",
              starts - before, started_stack);
        /* a second StartThread on the same thread starts no other host thread and takes no reference */
        u32 again = kthread_start(t);
        CHECK(again == SNX_RC_SVC_INVALID_STATE && starts == before + 1 && t->state == 1 && t->o.refs == 1,
              "started twice: %x, %d host threads, %d references", again, starts - before, t->o.refs);
        /* its own handle at 0x110 of its TLS, where the kernel puts it from 22.0.0 on: a Switch program's thread
           finds it there as an Ounce program's does, or its SDK takes every free lock for one it holds */
        for (int ounce = 0; ounce < 2; ounce++) {
            g_ounce = ounce;
            gw32(t->tls + 0x110, 0);
            kthread_set_own_handle(t, 0x8800du + (u32)ounce);
            CHECK(gr32(t->tls + 0x110) == 0x8800du + (u32)ounce, "%s program's thread finds %#x at 0x110 of its TLS",
                  ounce ? "an Ounce" : "a Switch", gr32(t->tls + 0x110));
        }
        g_ounce = 0;
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
        /* its host thread gets to the hold point once the host runs it, which a loaded host may put off past any fixed
           wait: it has 2 s. Then 30 ms more for a thread that ran on to show its steps */
        for (int i = 0; i < 2000 && __atomic_load_n(&g[2].t->stopped, __ATOMIC_SEQ_CST) != 2; i++) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
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
        /* a loaded host may not run it within any fixed wait: it has 2 s to take a step */
        for (int i = 0; i < 2000 && __atomic_load_n(&g[3].steps, __ATOMIC_SEQ_CST) == seen[0]; i++) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        CHECK(__atomic_load_n(&g[3].steps, __ATOMIC_SEQ_CST) > seen[0], "it goes on");
        __atomic_store_n(&g[3].stop, 1, __ATOMIC_SEQ_CST);
        pthread_join(gh[3], NULL);
        g[3].t->state = 2;
    }

    /* without emulated cores (emulated_cores off) the threads run at once, holding no core, so a pause asks each one
       that runs itself; the poll that leaves set is cleared at its first poll after the pause */
    {
        static SnxKctGame g[2];
        pthread_t gh[2];
        u64 seen[2];
        reset();
        g_settings.emulated_cores = 0;
        for (int i = 0; i < 2; i++) {
            g[i].t = &g_hle.thread.threads[i];
            g[i].t->state = 1;
            g[i].t->stopped = 0;
            g[i].t->cpu.poll = 0;
            pthread_create(&gh[i], NULL, game_body, &g[i]);
        }
        for (int i = 0; i < 2000 && !(__atomic_load_n(&g[0].steps, __ATOMIC_SEQ_CST) &&
                                      __atomic_load_n(&g[1].steps, __ATOMIC_SEQ_CST));
             i++) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        CHECK(g[0].steps && g[1].steps && on_core(g[0].t) == -1 && on_core(g[1].t) == -1,
              "both of core 0 run, holding no core: %d %d", on_core(g[0].t), on_core(g[1].t));
        CHECK(kernel_hold(2000) == 0, "the pause holds without cores");
        CHECK(g[0].t->stopped == 2 && g[1].t->stopped == 2, "both stopped at a poll: %d %d", g[0].t->stopped,
              g[1].t->stopped);
        for (int i = 0; i < 2; i++) seen[i] = __atomic_load_n(&g[i].steps, __ATOMIC_SEQ_CST);
        {
            struct timespec pause = {0, 10000000};
            nanosleep(&pause, NULL);
        }
        CHECK(seen[0] == g[0].steps && seen[1] == g[1].steps, "nothing ran while paused without cores");
        kernel_release();
        for (int i = 0; i < 2000 && (__atomic_load_n(&g[0].t->cpu.poll, __ATOMIC_RELAXED) ||
                                     __atomic_load_n(&g[1].t->cpu.poll, __ATOMIC_RELAXED));
             i++) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        CHECK(!g[0].t->cpu.poll && !g[1].t->cpu.poll, "their polls are cleared after the pause: %u %u",
              g[0].t->cpu.poll, g[1].t->cpu.poll);
        for (int i = 0; i < 2; i++) __atomic_store_n(&g[i].stop, 1, __ATOMIC_SEQ_CST);
        for (int i = 0; i < 2; i++) pthread_join(gh[i], NULL);
        CHECK(g[0].steps > seen[0] && g[1].steps > seen[1], "both went on after the pause");
        for (int i = 0; i < 2; i++) g[i].t->state = 2;
        g_settings.emulated_cores = 1;
    }

    /* a wake under g_klock: the woken thread's word moves at once, so a wait that read the word before the wake
       returns, but its host thread is woken only once its waker lets go of the lock (klock_unlock), where it would
       otherwise wake to wait for that lock. Past the list's room the wakes go out at once */
    {
        KThread* s = thread(0, 44, 0, 1);
        u32 seq = kt_seq(s);
        unsigned before = snx_kct_woken[0];
        pm_lock(&g_host.kernel.klock);
        kt_notify_later(s);
        CHECK(kt_seq(s) != seq && snx_kct_woken[0] == before, "moved, not yet woken: %u %u", kt_seq(s) - seq,
              snx_kct_woken[0] - before);
        kt_wait(s, seq, &g_host.kernel.klock, -1); /* returns: the word moved after it was read */
        CHECK(snx_kct_woken[0] == before + 1, "the wait woke the thread its caller owed a wake: %u",
              snx_kct_woken[0] - before);
        kt_notify_later(s);
        klock_unlock();
        CHECK(snx_kct_woken[0] == before + 2, "the unlock woke it: %u", snx_kct_woken[0] - before);
        unsigned was[18];
        for (int i = 1; i <= 17; i++) was[i] = snx_kct_woken[i];
        pm_lock(&g_host.kernel.klock);
        for (int i = 1; i <= 17; i++) kt_notify_later(thread(i, 44, 0, 1));
        CHECK(snx_kct_woken[17] == was[17] + 1 && snx_kct_woken[1] == was[1], "the seventeenth at once, the first "
              "later: %u %u", snx_kct_woken[17] - was[17], snx_kct_woken[1] - was[1]);
        klock_unlock();
        CHECK(snx_kct_woken[1] == was[1] + 1 && snx_kct_woken[16] == was[16] + 1 && snx_kct_woken[17] == was[17] + 1,
              "the rest at the unlock");
    }
    /* A holder blocked in a fault on memory the GPU holds lets go of its core while the page lands, which can take
       seconds: the core's waiter of better priority runs at once, as on the console, where the access takes no time.
       Back in game code, the faulting thread takes a core again at its next poll, here behind that waiter */
    reset();
    a = thread(2, 44, 0, 1), w = thread(3, 31, 0, 1);
    a->o.type = KO_THREAD; /* a live thread: a slot whose thread exited keeps its host thread's number */
    a->state = 1;
    g_host.thread.host_tid[2] = 4242;
    g_host.thread.host_tid[1] = 4242; /* an exited thread's slot, whose number the system gave again */
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "31 waits for core 0 behind 44: on core %d", on_core(w));
    kthread_fault_lend(4242);
    pthread_join(h, NULL);
    CHECK(on_core(w) == 0 && on_core(a) == -1 && __atomic_load_n(&a->cpu.poll, __ATOMIC_RELAXED) == 1,
          "the fault let core 0 go: 31 on %d, 44 on %d with poll %u", on_core(w), on_core(a), a->cpu.poll);
    pthread_create(&h, NULL, poll_body, a);
    CHECK(settle(a, -2) == -2 && a->core == 0, "44's poll waits behind 31: on core %d", on_core(a));
    core_leave(w);
    pthread_join(h, NULL);
    CHECK(on_core(a) == 0 && !a->cpu.poll, "44 back on core %d, poll %u", on_core(a), a->cpu.poll);
    core_leave(a);
    /* with an idle core in its mask, its poll takes that one; a thread of the runtime's own, or one that holds no
       core, changes nothing */
    reset();
    a->o.type = 0;
    g_host.thread.host_tid[1] = g_host.thread.host_tid[2] = 0;
    a = thread(0, 44, 0, 3), w = thread(3, 31, 0, 1);
    a->o.type = KO_THREAD;
    a->state = 1;
    g_host.thread.host_tid[0] = 4242;
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "31 waits for core 0: on core %d", on_core(w));
    kthread_fault_lend(4343);
    CHECK(on_core(a) == 0 && on_core(w) == -2, "another thread's fault moved core 0: 44 on %d, 31 on %d", on_core(a),
          on_core(w));
    kthread_fault_lend(4242);
    pthread_join(h, NULL);
    core_poll(a);
    CHECK(on_core(a) == 1 && on_core(w) == 0, "44 took idle core %d, 31 is on %d", on_core(a), on_core(w));
    core_leave(w);
    core_leave(a);
    kthread_fault_lend(4242);
    CHECK(on_core(a) == -1 && !a->cpu.poll, "a fault off its core: on %d, poll %u", on_core(a), a->cpu.poll);
    g_host.thread.host_tid[0] = 0;
    a->o.type = 0;

    /* A thread stopped in game code with its disable count raised (the SDK raises it around its spinning locks)
       keeps its core past a waiter of better priority, as on the console: the waiter would spin on the lock for good,
       on the core its holder then waits for. The interrupt flag is set in its TLS and its poll stays set. With no
       guest memory here a guest address is the host's, so the TLS is a host array */
    u8 tls[0x200] = {0};
    reset();
    a = thread(0, 45, 0, 1), b = thread(1, 43, 0, 1);
    a->id = 7;
    a->tls = (u64)(uintptr_t)tls;
    gw16(a->tls + SNX_TLS_DISABLE_COUNT, 3);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, -2) == -2 && __atomic_load_n(&a->cpu.poll, __ATOMIC_RELAXED) == 1,
          "43 waits for core 0 behind 45: on core %d, the holder's poll %u", on_core(b), a->cpu.poll);
    logged[0] = 0;
    pthread_create(&h2, NULL, poll_body, a);
    CHECK(settle16(a->tls + SNX_TLS_INTERRUPT_FLAG, 1) == 1 && on_core(a) == 0 && on_core(b) == -2 &&
              __atomic_load_n(&a->cpu.poll, __ATOMIC_RELAXED) == 1,
          "stopped with a disable count of 3: interrupt flag %u, 45 on %d, 43 on %d, poll %u",
          gr16(a->tls + SNX_TLS_INTERRUPT_FLAG), on_core(a), on_core(b), a->cpu.poll);
    if (on_core(a) != 0) { /* a scheduler that took the core: its waiter lets the poll through */
        pthread_join(h, NULL);
        core_leave(b);
        pthread_join(h2, NULL);
    } else {
        pthread_join(h2, NULL);
        CHECK(strstr(logged, "thread 7 (prio 45) keeps core 0 past a waiter of priority 43: its disable count is 3"),
              "the kept core is said with its values: \"%s\"", logged);
        /* an SVC gives the core up whatever the count: only game code stopped where it runs is spared */
        pthread_create(&h2, NULL, svc_body, a);
        pthread_join(h, NULL);
        CHECK(on_core(b) == 0 && settle(a, -2) == -2, "an SVC with the count at 3: 43 on %d, 45 on %d", on_core(b),
              on_core(a));
        core_leave(b);
        pthread_join(h2, NULL);
        CHECK(on_core(a) == 0 && gr16(a->tls + SNX_TLS_INTERRUPT_FLAG) == 1, "45 back on core %d, interrupt flag %u",
              on_core(a), gr16(a->tls + SNX_TLS_INTERRUPT_FLAG));
        /* svcSynchronizePreemptionState clears the flag, and nothing else of the TLS */
        core_synchronize(a);
        CHECK(!gr16(a->tls + SNX_TLS_INTERRUPT_FLAG) && gr16(a->tls + SNX_TLS_DISABLE_COUNT) == 3,
              "after svcSynchronizePreemptionState: interrupt flag %u, disable count %u",
              gr16(a->tls + SNX_TLS_INTERRUPT_FLAG), gr16(a->tls + SNX_TLS_DISABLE_COUNT));
    }
    /* its count back at zero, its next poll gives the core up: the SDK's call may come a moment later */
    b = thread(1, 43, 0, 1);
    pthread_create(&h, NULL, enter_body, b);
    CHECK(settle(b, -2) == -2, "43 waits for core 0 again: on core %d", on_core(b));
    gw16(a->tls + SNX_TLS_DISABLE_COUNT, 0);
    pthread_create(&h2, NULL, poll_body, a);
    pthread_join(h, NULL);
    CHECK(on_core(b) == 0 && settle(a, -2) == -2 && !gr16(a->tls + SNX_TLS_INTERRUPT_FLAG),
          "stopped with a disable count of 0: 43 on %d, 45 on %d, interrupt flag %u", on_core(b), on_core(a),
          gr16(a->tls + SNX_TLS_INTERRUPT_FLAG));
    core_leave(b);
    pthread_join(h2, NULL);
    core_leave(a);
    /* a thread whose core a fault lent away runs on with no core while its count is raised, where it would line up
       behind the core's new holder; at zero its poll lines it up */
    reset();
    a = thread(2, 44, 0, 1), w = thread(3, 31, 0, 1);
    a->o.type = KO_THREAD;
    a->state = 1;
    a->id = 9;
    a->tls = (u64)(uintptr_t)tls;
    g_host.thread.host_tid[2] = 4242;
    gw16(a->tls + SNX_TLS_DISABLE_COUNT, 2);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "31 waits for core 0 behind 44: on core %d", on_core(w));
    kthread_fault_lend(4242);
    pthread_join(h, NULL);
    pthread_create(&h2, NULL, poll_body, a);
    CHECK(settle16(a->tls + SNX_TLS_INTERRUPT_FLAG, 1) == 1 && on_core(a) == -1 && on_core(w) == 0 &&
              __atomic_load_n(&a->cpu.poll, __ATOMIC_RELAXED) == 1,
          "off its core with a disable count of 2: interrupt flag %u, 44 on %d, 31 on %d, poll %u",
          gr16(a->tls + SNX_TLS_INTERRUPT_FLAG), on_core(a), on_core(w), a->cpu.poll);
    if (on_core(a) == -1) {
        pthread_join(h2, NULL);
        CHECK(strstr(logged, "thread 9 (prio 44), its core lent in a fault, runs on with none: its disable count is 2"),
              "the thread with no core is said with its values: \"%s\"", logged);
        gw16(a->tls + SNX_TLS_DISABLE_COUNT, 0);
        pthread_create(&h2, NULL, poll_body, a);
        CHECK(settle(a, -2) == -2, "at a count of 0 its poll waits behind 31: on core %d", on_core(a));
    }
    core_leave(w);
    pthread_join(h2, NULL);
    CHECK(on_core(a) == 0 && !a->cpu.poll, "44 back on core %d, poll %u", on_core(a), a->cpu.poll);
    core_leave(a);
    g_host.thread.host_tid[2] = 0;
    a->o.type = 0;
    CHECK(g_host.thread.kept == 2, "%llu stops kept a core or none, not 2", (unsigned long long)g_host.thread.kept);

    /* The Ounce's NPDM can permit six cores. Its last guest core gets its own share of host CPUs. */
    host(twelve, 12);
    g_hle.svcmem.core_mask = 0x3f;
    reset();
    a = thread(0, 44, 5, 1ull << 5);
    core_enter(a);
    CHECK(on_core(a) == 5 && moved_to("5 11", 1), "sixth guest core: on %d, host CPUs %s", on_core(a),
          snx_kct_moved);
    core_leave(a);

    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
