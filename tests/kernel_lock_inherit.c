/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Priority inheritance for the locks the runtime keeps on the host, nn/sync.c's std::mutex and nn/os_sync.c's SDK
   mutex, as the console's kernel gives it to a mutex's holder in svcArbitrateLock. The case a game's job system froze
   on: a worker of low priority holds the lock and waits for its core, which a thread of better priority keeps while it
   spins in game code (it polls, as generated code does at a backward branch) for work that a thread of the best
   priority, on another core, can only finish once it has the lock. On the console the waiter raises the holder above
   the spinner, which lets go of the core at once; the holder lets go of the lock and drops back to its own priority.
   The real cores (thread.c), the kernel's inheritance (sync.c) and both families run here, host threads standing for
   the game's */
#include "nx/hle/kernel/thread.c"
#include "nx/hle/kernel/sync.c"
#include "nx/nn/sync.c"
#include "nx/nn/os_sync.c"
#include "nx/hle/hle.c"
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* the guest memory the SDK lock word lives in: a guest address is an offset into it */
static u8 snx_kli_mem[0x1000];
u8* g_mem = snx_kli_mem;
u64 g_as_size = AS_MAX;
MemLayout g_layout;
_Atomic GuestFn* g_disp;
u64 g_code_lo, g_code_hi, g_code_memory, g_main_stack_size;
const RtAPI g_rtapi = {.cas128 = snx_runtime_cas128};
const SnxArmModule* g_arm_mod;
int g_ounce;
const SnxArmRt g_arm_rt;
void (*g_gpu_mapping_waits)(void);
[[noreturn]] static void unreached(const char* what) {
    fprintf(stderr, "kernel_lock_inherit: %s was called\n", what);
    abort();
}
void arm_thread_run(KThread* t) { (void)t, unreached("arm_thread_run"); }
int nce_on(void) { return 0; }
void nce_thread_run(KThread* t, u64 pc) { (void)t, (void)pc, unreached("nce_thread_run"); }
void nce_kick(KThread* t) { (void)t; }
void guest_stats_thread_start(u32 ix) { (void)ix; }
void guest_stats_thread_end(u32 ix) { (void)ix; }
void gvk_scratch_free(void) {}
void svc_resume(KThread* t) { (void)t, unreached("svc_resume"); }
void svc_resume_leave(KThread* t) { (void)t, unreached("svc_resume_leave"); }
void svc_service_retry(KThread* t) { (void)t, unreached("svc_service_retry"); }
void kobj_wake_nl(KObj* o, int kind) { (void)o, (void)kind; }
int svc_pause_point(KThread* t) { return (void)t, 0; }
void fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
    abort();
}
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
u32 h_new(KObj* o) { return (void)o, 0; }
/* the threads here have no handle: their lock tags name their slots (SNX_LOCK_TAG_SLOT) */
KObj* h_get_nl(u32 h) { return (void)h, NULL; }
static u8 snx_kli_tls[0x1000];
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    return (void)size, (void)state, (void)perm, (u64)(uintptr_t)snx_kli_tls;
}
void kobj_signal_nl(KObj* o) { (void)o; }
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) { return (void)lo, (void)hi, (void)size, (void)align, 0; }
int mem_map(u64 addr, u64 size, u32 state, u32 perm) { return (void)addr, (void)size, (void)state, (void)perm, 0; }
int mem_user_mapped(u64 addr, u64 size) { return addr + size <= sizeof snx_kli_mem; }
u64 mem_total(void) { return 0; }
u64 used_memory(void) { return 0; }
void plat_exit_now(int code) { exit(code); }
#if defined(__linux__)
uint32_t plat_tid(void) { return (uint32_t)syscall(SYS_gettid); }
#else
uint32_t plat_tid(void) { return (uint32_t)(uintptr_t)pthread_self(); }
#endif
void snx_thread_name(const char* name) { (void)name; }
int plat_thread(PThreadFn fn, void* arg, size_t stack) { return (void)fn, (void)arg, (void)stack, -1; }
int plat_address_space(u64* mapped, u64* largest_free) { return (void)mapped, (void)largest_free, 0; }
void plat_thread_exit(void) { pthread_exit(NULL); }
void plat_thread_where(u32 tid, char* out, size_t n) { (void)tid, snprintf(out, n, "?"); }
u64 plat_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000u + (u64)t.tv_nsec;
}
i64 plat_unix_ns(void) { return (i64)plat_ns(); }
void plat_sleep_ns(i64 ns) {
    struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&t, NULL);
}
void kernel_dump_threads(void) {}
void save_hints(void) {}
void snx_save_before_exit(int frontend) { (void)frontend; }
void snx_fpcr_apply(CPU* c) { (void)c; }
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_destroy(PMutex* m) { pthread_mutex_destroy(&m->l); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
int pm_trylock(PMutex* m) { return pthread_mutex_trylock(&m->l) == 0; }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pc_init(PCond* c) { pthread_cond_init(&c->c, NULL); }
void pc_signal(PCond* c) { pthread_cond_signal(&c->c); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 ns) { return (void)ns, pthread_cond_wait(&c->c, &m->l); }
void plat_yield(void) { sched_yield(); }
void pm_wait_note(const void* m, const void* at, int cond) { (void)m, (void)at, (void)cond; }
/* a word's wait, polled until the word moves, so a wake needs no futex; one the test never ends fails it after 5 s */
void plat_wait_word(_Atomic u32* w, u32 v, i64 ns) {
    struct timespec pause = {0, 50000};
    (void)ns;
    for (int i = 0; i < 100000 && atomic_load_explicit(w, memory_order_acquire) == v; i++) nanosleep(&pause, NULL);
    if (atomic_load_explicit(w, memory_order_acquire) == v) fatal("a wait on a word was never ended");
}
void plat_wake_word(_Atomic u32* w) { (void)w; }
void plat_wake_word_one(_Atomic u32* w) { (void)w; }
int plat_cpus(int* ids, int max) { return (void)ids, (void)max, 0; }
int plat_thread_cpus(const int* ids, int n) { return (void)ids, (void)n, 0; }
/* a handle and a thread's last reference keep its slot: these threads live for the whole test */
void kobj_ref(KObj* o) {
    if (o) atomic_fetch_add_explicit(&o->refs, 1, memory_order_relaxed);
}
void kobj_unref(KObj* o) {
    if (o) atomic_fetch_sub_explicit(&o->refs, 1, memory_order_relaxed);
}
KObj* kobj_at(u32 id) { return (void)id, NULL; }
u32 kobj_id(const KObj* o) { return (u32)(uintptr_t)o; }
int kobj_waitable(const KObj* o) { return (void)o, 0; }
int kobj_signaled(const KObj* o) { return (void)o, 0; }
void waiter_add(KObj* o, KThread* t) { (void)o, (void)t; }
void waiter_del(KObj* o, KThread* t) { (void)o, (void)t; }
/* the kernel's own waits, which no case makes: every waiter here waits on the host */
int sleep_nl(KThread* t, i64 timeout) { (void)t, (void)timeout, unreached("sleep_nl"); }
int sleep_held(KThread* t, PMutex* held, i64 timeout) { (void)t, (void)held, (void)timeout, unreached("sleep_held"); }
void wake(KThread* t, int result) { (void)t, (void)result, unreached("wake"); }
u64 guest_ns(void) { return plat_ns(); }
u64 guest_ticks(void) { unreached("guest_ticks"); }
ClockRate clock_rate(void) { unreached("clock_rate"); }
u64 kernel_find_symbol_exact(const char* name) { return (void)name, 1; }
void hook_run_original(CPU* c, u64 addr) { (void)c, (void)addr, unreached("hook_run_original"); }

static unsigned checks, failures;
#define CHECK(expr, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "kernel_lock_inherit.c:%d: %s: ", __LINE__, #expr);                                        \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* a lock family's lock and unlock, on the lock at the guest address LOCK. cv: the waiter waits on the
   std::condition_variable at CV with the std::mutex first, and needs the mutex again once the spinner notifies it */
typedef struct {
    const char* name;
    void (*lock)(CPU* c, u64 m);
    void (*unlock)(CPU* c, u64 m);
    int cv;
} Family;
enum { LOCK = 0x100, UNIQUE_LOCK = 0x200, CV = 0x300 };
/* how long the spinner spins before the test calls it starved and lets the others finish */
#define STARVED_NS 2000000000ull
enum { HOLDER, SPINNER, WAITER };
static CPU snx_kli_cpu[3];
static struct {
    const Family* f;
    _Atomic int holding, spinning, done, starved, cv_waiting;
    _Atomic unsigned spinner_polls;
    int holder_prio_back, holder_prio_after;
    u64 spun_ns;
} g;
static KThread* thread(int i, int prio, int core) {
    KThread* t = &g_hle.thread.threads[i];
    memset(t, 0, sizeof *t);
    t->o.type = KO_THREAD;
    t->o.refs = 1;
    t->state = 1;
    t->prio = t->base_prio = prio;
    t->core = core;
    t->affinity = 1ull << core;
    t->on_core = -1;
    t->pinned = -1;
    return t;
}
/* the worker: takes the lock on its core, and is preempted holding it at its next poll, by the spinner; once it runs
   again it lets go of the lock, and blocks */
static void* holder_body(void* p) {
    KThread* t = (KThread*)p;
    t_hle.thread.cur = t;
    core_enter(t);
    g.f->lock(&snx_kli_cpu[HOLDER], LOCK);
    atomic_store_explicit(&g.holding, 1, memory_order_seq_cst);
    while (!atomic_load_explicit(&t->cpu.poll, memory_order_relaxed)) sched_yield();
    core_poll(t); /* the spinner takes the core; this thread waits for it, holding the lock */
    g.holder_prio_back = t->prio;
    g.f->unlock(&snx_kli_cpu[HOLDER], LOCK);
    g.holder_prio_after = t->prio;
    core_leave(t);
    return NULL;
}
/* the main thread's worker loop: holds its core and spins, polling, until the waiter's work is done */
static void* spinner_body(void* p) {
    KThread* t = (KThread*)p;
    u64 start;
    t_hle.thread.cur = t;
    core_enter(t);
    atomic_store_explicit(&g.spinning, 1, memory_order_seq_cst);
    start = plat_ns();
    while (!atomic_load_explicit(&g.done, memory_order_seq_cst)) {
        /* until it is woken: the waiter may not wait on it yet at the first notify */
        if (g.f->cv) sync_cond_notify_one(&snx_kli_cpu[SPINNER], CV);
        if (atomic_load_explicit(&t->cpu.poll, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&g.spinner_polls, 1, memory_order_relaxed);
            core_poll(t);
        }
        if (plat_ns() - start > STARVED_NS) {
            atomic_store_explicit(&g.starved, 1, memory_order_seq_cst);
            break;
        }
    }
    g.spun_ns = plat_ns() - start;
    core_leave(t); /* starved, it lets go so that the others end */
    return NULL;
}
/* the job of the best priority, on another core: it needs the lock */
static void* waiter_body(void* p) {
    KThread* t = (KThread*)p;
    t_hle.thread.cur = t;
    core_enter(t);
    g.f->lock(&snx_kli_cpu[WAITER], LOCK);
    if (g.f->cv) { /* it waits holding the mutex, which the wait lets go of and takes again */
        atomic_store_explicit(&g.cv_waiting, 1, memory_order_seq_cst);
        sync_cond_wait(&snx_kli_cpu[WAITER], CV, UNIQUE_LOCK);
    }
    atomic_store_explicit(&g.done, 1, memory_order_seq_cst);
    g.f->unlock(&snx_kli_cpu[WAITER], LOCK);
    core_leave(t);
    return NULL;
}
static void until(_Atomic int* flag) {
    while (!atomic_load_explicit(flag, memory_order_seq_cst)) sched_yield();
}
/* until t waits for its core (on_core -2), for up to 2 s: 1 if it does */
static int waits_for_core(const KThread* t) {
    struct timespec pause = {0, 100000};
    for (int i = 0; i < 20000 && atomic_load_explicit(&t->on_core, memory_order_seq_cst) != -2; i++)
        nanosleep(&pause, NULL);
    return atomic_load_explicit(&t->on_core, memory_order_seq_cst) == -2;
}

static void run(const Family* f) {
    pthread_t h[3];
    KThread *holder = thread(HOLDER, 51, 0), *spinner = thread(SPINNER, 44, 0), *waiter = thread(WAITER, 36, 1);
    memset(&g, 0, sizeof g);
    memset(snx_kli_mem, 0, sizeof snx_kli_mem);
    g.f = f;
    for (int i = 0; i < 3; i++) snx_kli_cpu[i].mem = g_mem; /* hook_host's: a guest address is an offset into it */
    for (int k = 0; k < NCORES; k++) {
        g_hle.thread.core_owner[k] = 0;
        g_hle.thread.core_wait[k] = 0;
        g_hle.thread.core_best[k] = 64;
    }
    snx_st_le64(g_mem + UNIQUE_LOCK, LOCK); /* a unique_lock<mutex>: the mutex's address, then whether it owns it */
    snx_st_le64(g_mem + UNIQUE_LOCK + 8, 1);
    if (f->cv) { /* the waiter waits on the condition variable first, the mutex let go of */
        pthread_create(&h[WAITER], NULL, waiter_body, waiter);
        until(&g.cv_waiting);
        while (atomic_load_explicit(&obj(LOCK)->holder, memory_order_seq_cst)) sched_yield();
    }
    pthread_create(&h[HOLDER], NULL, holder_body, holder);
    until(&g.holding);
    pthread_create(&h[SPINNER], NULL, spinner_body, spinner);
    until(&g.spinning);
    CHECK(waits_for_core(holder), "%s: the holder (prio 51) holds the lock but is on core %d, not waiting for core 0",
          f->name, atomic_load(&holder->on_core));
    if (!f->cv) pthread_create(&h[WAITER], NULL, waiter_body, waiter);
    for (int i = 0; i < 3; i++) pthread_join(h[i], NULL);
    CHECK(!g.starved,
          "%s: the spinner (prio 44) kept core 0 for %llu ms while the lock's holder (prio 51) waited for it: the "
          "waiter's priority 36 never reached the holder, so it never ran to let go of the lock",
          f->name, (unsigned long long)(g.spun_ns / 1000000u));
    CHECK(g.holder_prio_back == 36, "%s: the holder ran at priority %d holding the lock its waiter of 36 wants",
          f->name, g.holder_prio_back);
    CHECK(g.holder_prio_after == 51, "%s: the holder runs at priority %d after letting go of the lock, not its own 51",
          f->name, g.holder_prio_after);
    CHECK(g.spinner_polls >= 1, "%s: the spinner was never asked to let go of its core (%u polls)", f->name,
          g.spinner_polls);
    CHECK(!g_host.thread.hlwait && !g_host.thread.hl_owner[WAITER] && !g_host.thread.hl_key[WAITER],
          "%s: the waiter is still listed as waiting: list head %u, holder %u, lock %llx", f->name,
          g_host.thread.hlwait, g_host.thread.hl_owner[WAITER], (unsigned long long)g_host.thread.hl_key[WAITER]);
    CHECK(holder->o.refs == 1, "%s: the holder has %d references, not 1: a waiter kept one", f->name,
          atomic_load(&holder->o.refs));
}

static const Family k_std_mutex = {"std::mutex (cond_var family)", sync_mutex_lock, sync_mutex_unlock, 0};
static const Family k_std_cv = {"std::condition_variable's wake (cond_var family)", sync_mutex_lock, sync_mutex_unlock,
                                1};
static const Family k_sdk_mutex = {"SdkMutexType (os_sync family)", os_sdk_mutex_lock, os_sdk_mutex_unlock, 0};

int main(void) {
    hle_init();
    g_settings.emulated_cores_mode = SNX_KERNEL_CORES_AUTO;
    g_lock_futex = 1;
    run(&k_std_mutex);
    run(&k_std_cv);
    run(&k_sdk_mutex);
    if (failures) {
        fprintf(stderr, "kernel_lock_inherit: %u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("kernel_lock_inherit: %u checks passed\n", checks);
    return 0;
}
