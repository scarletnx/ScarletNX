/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The core model of thread.c against the console's scheduler: a ready thread that would wait behind its core's holder
   runs on an idle core its mask allows, a core that falls idle takes a thread waiting for another one, a thread of
   better priority stays and flags the holder, and only svcSleepThread(0) keeps a yielding thread on its core. A yield
   hands the core to a thread of the same priority or a better one, and only svcSleepThread(-2) to a worse one. The
   threads here are bare KThreads; each one that has to wait for a core waits on a host thread of its own */
#include "nx/hle/kernel/thread.c"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

u8* g_mem;
MemLayout g_layout = {0x08000000ull,   0x2000000000ull, 0x100000000ull, 0x200000000ull,  0x300000000ull,
                      0x1000000000ull, 0x1300000000ull, 0x100000000ull, 0x1400000000ull, 0x1800000000ull};
GuestFn* g_disp;
u64 g_code_lo, g_code_hi, g_core_mask = 7, g_prio_mask, g_code_memory, g_main_stack_size, g_next_tid;
int g_verbose;
PMutex g_klock = PMUTEX_INIT;
const RtAPI g_rtapi;
const SnxArmModule* g_arm_mod; /* a 64-bit game */
const SnxArmRt g_arm_rt;
void arm_thread_run(KThread* t) { (void)t; }
__thread void* tls_thread;
void fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    abort();
}
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
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
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    (void)fn;
    (void)arg;
    (void)stack;
    return -1;
}
void plat_thread_exit(void) { pthread_exit(NULL); }
void snx_save_before_exit(void) {}
void snx_fpcr_apply(CPU* c) { (void)c; }
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pc_init(PCond* c) { pthread_cond_init(&c->c, NULL); }
void pc_signal(PCond* c) { pthread_cond_signal(&c->c); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    (void)ns; /* the core model only waits without a timeout */
    return pthread_cond_wait(&c->c, &m->l);
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
static KThread snx_kct_thread[SNX_KCT_THREADS];
/* thread i at this priority, on this core, with this core mask; nobody holds or waits for a core */
static KThread* thread(int i, int prio, int core, u64 mask) {
    KThread* t = &snx_kct_thread[i];
    memset(t, 0, sizeof *t);
    t->prio = t->base_prio = prio;
    t->core = core;
    t->affinity = mask;
    t->on_core = -1;
    return t;
}
static void reset(void) {
    for (int k = 0; k < NCORES; k++) {
        g_core_owner[k] = NULL;
        g_core_wait[k] = NULL;
        g_core_best[k] = 64;
    }
}
static int on_core(KThread* t) {
    pm_lock(&g_core_lk);
    int k = t->on_core;
    pm_unlock(&g_core_lk);
    return k;
}
/* until t holds a core, or waits for one (-2), for up to 2 s; the core it ends on */
static int settle(KThread* t, int want) {
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 2000 && on_core(t) != want; i++) nanosleep(&pause, NULL);
    return on_core(t);
}
static void* enter_body(void* p) {
    core_enter((KThread*)p);
    return NULL;
}
typedef struct {
    KThread* t;
    int migrate, any;
} SnxKctYield;
static void* yield_body(void* p) {
    SnxKctYield* y = (SnxKctYield*)p;
    core_yield(y->t, y->migrate, y->any);
    return NULL;
}

int main(void) {
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
    pm_lock(&g_core_lk);
    CHECK(!g_core_wait[0] && g_core_best[0] == 64 && !a->cpu.poll, "core 0 still has a waiter, best %d",
          g_core_best[0]);
    pm_unlock(&g_core_lk);
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
    CHECK(on_core(b) == -2 && !g_core_owner[1], "45 left its core for core %d", on_core(b));
    core_leave(a);
    pthread_join(h, NULL);
    CHECK(b->on_core == 0, "45 got core %d", b->on_core);
    core_leave(b);

    /* svcSleepThread(0) hands the core to a thread of its priority and waits for it again, other cores idle or not */
    reset();
    a = thread(0, 44, 0, 7), w = thread(3, 44, 0, 1);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "44 with mask 1 waits: on core %d", w->on_core);
    SnxKctYield y = {a, 0, 0};
    pthread_create(&h2, NULL, yield_body, &y);
    pthread_join(h, NULL);
    CHECK(w->on_core == 0 && settle(a, -2) == -2 && a->core == 0, "yield 0: 44 on %d, 44 on %d for %d", w->on_core,
          on_core(a), a->core);
    core_leave(w);
    pthread_join(h2, NULL);
    CHECK(a->on_core == 0, "yield 0: back on core %d", a->on_core);
    core_leave(a);

    /* svcSleepThread(-1) hands it over and runs on an idle core */
    reset();
    a = thread(0, 44, 0, 7), w = thread(3, 44, 0, 1);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "44 with mask 1 waits: on core %d", w->on_core);
    y.t = a, y.migrate = 2;
    pthread_create(&h2, NULL, yield_body, &y);
    CHECK(settle(a, 1) == 1 && a->core == 1 && settle(w, 0) == 0, "yield -1: 44 on %d, 44 on %d", on_core(w),
          on_core(a));
    pthread_join(h, NULL);
    core_leave(w);
    pthread_join(h2, NULL);
    core_leave(a);

    /* neither svcSleepThread(0) nor (-1) lets a worse priority run: the yielding thread keeps its core */
    reset();
    a = thread(0, 44, 0, 7), w = thread(3, 50, 0, 1);
    core_enter(a);
    pthread_create(&h, NULL, enter_body, w);
    CHECK(settle(w, -2) == -2, "50 with mask 1 waits: on core %d", w->on_core);
    core_yield(a, 0, 0);
    CHECK(on_core(a) == 0 && on_core(w) == -2, "yield 0 to 50: 44 on %d, 50 on %d", on_core(a), on_core(w));
    core_yield(a, 2, 0);
    CHECK(on_core(a) == 0 && on_core(w) == -2, "yield -1 to 50: 44 on %d, 50 on %d", on_core(a), on_core(w));

    /* svcSleepThread(-2) does, and runs on an idle core */
    y.t = a, y.migrate = 2, y.any = 1;
    pthread_create(&h2, NULL, yield_body, &y);
    CHECK(settle(a, 1) == 1 && settle(w, 0) == 0, "yield -2: 50 on %d, 44 on %d", on_core(w), on_core(a));
    pthread_join(h, NULL);
    core_leave(w);
    pthread_join(h2, NULL);
    core_leave(a);

    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
