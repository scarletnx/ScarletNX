/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit bridge's threads (arm32.h): pthread_create and the others in bionic's 32-bit layouts, the mutexes,
   conditions, keys, onces and semaphores, the guest's signals, setjmp and longjmp, the system calls that guest code
   makes itself, and the C++ guards of static objects.

   A guest thread is a host thread with a CPU state of its own (arm32.c). A pthread_t is the thread's TLS block's
   address. A mutex, a condition or a semaphore is a host object found by the guest's address. Every wait waits in
   slices of 10 ms, and runs the signals sent to the thread between them, as Linux interrupts a wait: a garbage
   collector stops each thread with a signal and waits until each has run its handler.

   A signal's handler runs on the guest's stack in a frame of Linux's for ARM: a siginfo_t and a ucontext_t whose
   uc_mcontext has the registers that the thread had, which the handler may change (a JIT's null checks resume
   somewhere else). It runs between two blocks, or in a wait, never in the middle of a block */
#define _GNU_SOURCE
#include "android/bridge/arm32.h"
#include "common/utilities/endian.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define R(n)        (c->r[n])
#define HANDLER(n)  static void n(SnxArmCPU* c, void* data)
#define UNUSED      (void)data
#define SLICE_NS    10000000

static uint32_t rd32(uint32_t a) { return snx_ld_le32(arm32_mem + a); }
static void wr32(uint32_t a, uint32_t v) { snx_st_le32(arm32_mem + a, v); }

/* Linux's numbers */
enum { L_EINTR = 4, L_EAGAIN = 11, L_ENOMEM = 12, L_EBUSY = 16, L_EINVAL = 22, L_ESRCH = 3, L_ENOSYS = 38,
       L_ETIMEDOUT = 110, L_EDEADLK = 35 };

/* ---- the threads ---- */

typedef struct ThreadInfo {
    uint32_t self, tid;
    uint32_t stack_base, stack_size;
    uint32_t* pending;      /* the thread's arm32_pending_signals: the signals it runs, those not blocked */
    uint64_t sent, blocked; /* the signals sent and not run, and its mask, a bit for each (1 << (sig - 1)) */
    uint32_t keys[128];     /* pthread_getspecific's values */
    uint32_t cleanup;       /* __pthread_cleanup_push's list, the newest first */
    uint32_t altstack, altstack_size;
    bool on_altstack;
    /* pthread_create's */
    bool created, detached, done;
    uint32_t start, arg, result;
    pthread_t host;
    jmp_buf exit;
    struct ThreadInfo* next;
} ThreadInfo;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed; /* a thread started, ended or was detached */
    ThreadInfo* threads;
    uint32_t key_destructors[128];
    uint32_t key_count;
} th = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

static _Thread_local ThreadInfo* t_info;

/* the calling thread's record, made the first time (a thread of the JVM's too) */
static ThreadInfo* me(void) {
    if (t_info) return t_info;
    ThreadInfo* t = calloc(1, sizeof *t);
    if (!t) abort();
    t->self = arm32_thread_self();
    t->tid = arm32_thread_id();
    arm32_thread_stack(&t->stack_base, &t->stack_size);
    t->pending = &arm32_pending_signals;
    pthread_mutex_lock(&th.lock);
    t->next = th.threads;
    th.threads = t;
    pthread_mutex_unlock(&th.lock);
    t_info = t;
    return t;
}
/* a thread by its pthread_t or its id, under th.lock */
static ThreadInfo* by_self(uint32_t self) {
    for (ThreadInfo* t = th.threads; t; t = t->next)
        if (t->self == self) return t;
    return NULL;
}
static ThreadInfo* by_tid(uint32_t tid) {
    for (ThreadInfo* t = th.threads; t; t = t->next)
        if (t->tid == tid) return t;
    return NULL;
}
static void forget(ThreadInfo* t) {
    for (ThreadInfo** at = &th.threads; *at; at = &(*at)->next)
        if (*at == t) {
            *at = t->next;
            break;
        }
    free(t);
}

/* ---- the waits ---- */

static bool deliverable(void) { return __atomic_load_n(&arm32_pending_signals, __ATOMIC_RELAXED) != 0; }

/* the signals due run now, from a wait: true when one ran (the wait then ends with EINTR or a spurious wake) */
static bool run_signals(void) {
    if (!deliverable()) return false;
    arm32_signals_run(arm32_cpu());
    return true;
}

static struct timespec after_ns(int64_t ns, clockid_t clock) {
    struct timespec t;
    clock_gettime(clock, &t);
    ns += t.tv_nsec;
    t.tv_sec += (time_t)(ns / 1000000000);
    t.tv_nsec = (long)(ns % 1000000000);
    return t;
}
static bool before(const struct timespec* a, const struct timespec* b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}
/* a guest timespec (two 32-bit words) */
static struct timespec guest_time(uint32_t a) {
    struct timespec t = {(time_t)(int32_t)rd32(a), (long)(int32_t)rd32(a + 4)};
    return t;
}

/* ---- mutexes, conditions and semaphores: host objects by the guest's address ---- */

typedef struct Sync {
    uint32_t address;
    int kind; /* 0 a mutex, 1 a condition, 2 a semaphore */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t count; /* a semaphore's */
    struct Sync* next;
} Sync;
enum { SYNC_BUCKETS = 4096 };
static Sync* sync_table[SYNC_BUCKETS];
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;

static Sync* sync_of(uint32_t address, int kind) {
    unsigned b = (address >> 2) % SYNC_BUCKETS;
    pthread_mutex_lock(&sync_lock);
    Sync* o = sync_table[b];
    while (o && (o->address != address || o->kind != kind)) o = o->next;
    if (!o) {
        o = calloc(1, sizeof *o);
        if (!o) abort();
        o->address = address;
        o->kind = kind;
        /* recursive: bionic's recursive mutex is a value in the guest's word, and a normal one that a thread
           locks twice deadlocks, which no working app does */
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, kind == 0 ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
        pthread_mutex_init(&o->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        pthread_cond_init(&o->cond, NULL);
        o->next = sync_table[b];
        sync_table[b] = o;
    }
    pthread_mutex_unlock(&sync_lock);
    return o;
}
static void sync_forget(uint32_t address, int kind) {
    unsigned b = (address >> 2) % SYNC_BUCKETS;
    pthread_mutex_lock(&sync_lock);
    for (Sync** at = &sync_table[b]; *at; at = &(*at)->next)
        if ((*at)->address == address && (*at)->kind == kind) {
            Sync* o = *at;
            *at = o->next;
            pthread_mutex_destroy(&o->mutex);
            pthread_cond_destroy(&o->cond);
            free(o);
            break;
        }
    pthread_mutex_unlock(&sync_lock);
}

/* a host mutex locked, its wait in slices with the thread's signals run between them */
static void lock_host(pthread_mutex_t* m) {
    while (pthread_mutex_trylock(m)) {
        struct timespec slice = after_ns(SLICE_NS / 10, CLOCK_REALTIME);
#if defined(__APPLE__)
        nanosleep(&(struct timespec){0, SLICE_NS / 10}, NULL);
        (void)slice;
        if (!pthread_mutex_trylock(m)) return;
#else
        if (!pthread_mutex_timedlock(m, &slice)) return;
#endif
        run_signals();
    }
}

HANDLER(p_mutex_init) {
    UNUSED;
    sync_forget(R(0), 0);
    wr32(R(0), 0);
    R(0) = 0;
}
HANDLER(p_mutex_destroy) {
    UNUSED;
    sync_forget(R(0), 0);
    R(0) = 0;
}
HANDLER(p_mutex_lock) {
    UNUSED;
    lock_host(&sync_of(R(0), 0)->mutex);
    R(0) = 0;
}
HANDLER(p_mutex_trylock) {
    UNUSED;
    R(0) = pthread_mutex_trylock(&sync_of(R(0), 0)->mutex) ? L_EBUSY : 0;
}
HANDLER(p_mutex_unlock) {
    UNUSED;
    R(0) = pthread_mutex_unlock(&sync_of(R(0), 0)->mutex) ? L_EINVAL : 0;
}
/* pthread_mutexattr_t is bionic's long: the type. Every guest mutex is a recursive host one */
HANDLER(p_mutexattr_init) {
    UNUSED;
    wr32(R(0), 0);
    R(0) = 0;
}
HANDLER(p_mutexattr_settype) {
    UNUSED;
    if (R(1) > 2) {
        R(0) = L_EINVAL;
        return;
    }
    wr32(R(0), R(1));
    R(0) = 0;
}
HANDLER(p_mutexattr_gettype) {
    UNUSED;
    wr32(R(1), rd32(R(0)));
    R(0) = 0;
}

/* a wait on a guest condition with its guest mutex held: until it is signalled, the deadline (realtime or monotonic)
   passes, or a signal runs (a spurious wake, which POSIX allows). The mutex is held again at the end */
static uint32_t cond_wait(uint32_t cond, uint32_t mutex, const struct timespec* deadline, clockid_t clock) {
    Sync* cv = sync_of(cond, 1);
    Sync* m = sync_of(mutex, 0);
    /* the condition's own mutex is taken before the guest's is let go, so a signal between them is not lost */
    pthread_mutex_lock(&cv->mutex);
    pthread_mutex_unlock(&m->mutex);
    struct timespec slice = after_ns(SLICE_NS, CLOCK_REALTIME);
    uint32_t r = 0;
    if (deadline) {
        /* the deadline on the realtime clock, which the host condition waits by */
        struct timespec now;
        clock_gettime(clock, &now);
        int64_t left = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000 + (deadline->tv_nsec - now.tv_nsec);
        if (left <= 0) {
            r = L_ETIMEDOUT;
        } else {
            struct timespec end = after_ns(left, CLOCK_REALTIME);
            bool last = before(&end, &slice);
            if (pthread_cond_timedwait(&cv->cond, &cv->mutex, last ? &end : &slice) == ETIMEDOUT && last)
                r = L_ETIMEDOUT;
        }
    } else {
        pthread_cond_timedwait(&cv->cond, &cv->mutex, &slice);
    }
    pthread_mutex_unlock(&cv->mutex);
    run_signals();
    lock_host(&m->mutex);
    return r;
}
HANDLER(p_cond_init) {
    UNUSED;
    sync_forget(R(0), 1);
    wr32(R(0), 0);
    R(0) = 0;
}
HANDLER(p_cond_destroy) {
    UNUSED;
    sync_forget(R(0), 1);
    R(0) = 0;
}
HANDLER(p_cond_wait) {
    UNUSED;
    R(0) = cond_wait(R(0), R(1), NULL, CLOCK_REALTIME);
}
/* pthread_cond_timedwait(cond, mutex, abstime) on the realtime clock, the _monotonic_np form on the monotonic one */
HANDLER(p_cond_timedwait) {
    struct timespec t = guest_time(R(2));
    R(0) = cond_wait(R(0), R(1), &t, data ? CLOCK_MONOTONIC : CLOCK_REALTIME);
}
/* pthread_cond_timeout_np(cond, mutex, msecs): a relative wait */
HANDLER(p_cond_timeout_np) {
    UNUSED;
    struct timespec t = after_ns((int64_t)R(2) * 1000000, CLOCK_MONOTONIC);
    R(0) = cond_wait(R(0), R(1), &t, CLOCK_MONOTONIC);
}
static void cond_wake(uint32_t cond, bool all) {
    Sync* cv = sync_of(cond, 1);
    pthread_mutex_lock(&cv->mutex);
    if (all) pthread_cond_broadcast(&cv->cond);
    else pthread_cond_signal(&cv->cond);
    pthread_mutex_unlock(&cv->mutex);
}
HANDLER(p_cond_broadcast) {
    UNUSED;
    cond_wake(R(0), true);
    R(0) = 0;
}
HANDLER(p_cond_signal) {
    UNUSED;
    cond_wake(R(0), false);
    R(0) = 0;
}
HANDLER(p_condattr) {
    UNUSED;
    R(0) = 0;
}

/* sem_t is bionic's 32-bit word: the semaphore is a host count by its address */
HANDLER(s_init) {
    UNUSED;
    sync_forget(R(0), 2);
    Sync* s = sync_of(R(0), 2);
    s->count = R(2);
    R(0) = 0;
}
HANDLER(s_destroy) {
    UNUSED;
    sync_forget(R(0), 2);
    R(0) = 0;
}
HANDLER(s_post) {
    UNUSED;
    Sync* s = sync_of(R(0), 2);
    pthread_mutex_lock(&s->mutex);
    ++s->count;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    R(0) = 0;
}
/* a wait for the count: 0, or -1 with EAGAIN for trywait and ETIMEDOUT at the deadline. A signal's handler runs during
   the wait, and the wait goes on after it: bionic's sem_wait gives EINTR only to apps that target API level 24 or
   newer, and the apps with 32-bit libraries alone are older */
static uint32_t sem_wait_for(uint32_t sem, bool try, const struct timespec* deadline) {
    Sync* s = sync_of(sem, 2);
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        if (s->count) {
            --s->count;
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
        if (try) {
            pthread_mutex_unlock(&s->mutex);
            arm32_set_errno_linux(L_EAGAIN);
            return UINT32_MAX;
        }
        struct timespec slice = after_ns(SLICE_NS, CLOCK_REALTIME);
        if (deadline && before(deadline, &slice)) slice = *deadline;
        pthread_cond_timedwait(&s->cond, &s->mutex, &slice);
        bool got = s->count != 0;
        if (got) --s->count;
        pthread_mutex_unlock(&s->mutex);
        if (got) return 0;
        run_signals();
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (deadline && !before(&now, deadline)) {
            arm32_set_errno_linux(L_ETIMEDOUT);
            return UINT32_MAX;
        }
    }
}
HANDLER(s_wait) {
    UNUSED;
    R(0) = sem_wait_for(R(0), false, NULL);
}
HANDLER(s_trywait) {
    UNUSED;
    R(0) = sem_wait_for(R(0), true, NULL);
}
HANDLER(s_timedwait) {
    UNUSED;
    struct timespec t = guest_time(R(1));
    R(0) = sem_wait_for(R(0), false, &t);
}
HANDLER(s_getvalue) {
    UNUSED;
    Sync* s = sync_of(R(0), 2);
    pthread_mutex_lock(&s->mutex);
    wr32(R(1), s->count);
    pthread_mutex_unlock(&s->mutex);
    R(0) = 0;
}

/* pthread_once(once, init): init run once, other threads waiting until it returned (once: 0 not run, 1 running,
   2 done) */
static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_done = PTHREAD_COND_INITIALIZER;
HANDLER(p_once) {
    UNUSED;
    uint32_t once = R(0), init = R(1);
    pthread_mutex_lock(&once_lock);
    while (rd32(once) == 1) {
        struct timespec slice = after_ns(SLICE_NS, CLOCK_REALTIME);
        pthread_cond_timedwait(&once_done, &once_lock, &slice);
    }
    bool run = rd32(once) == 0;
    if (run) wr32(once, 1);
    pthread_mutex_unlock(&once_lock);
    if (run) {
        arm32_call(init, NULL, 0, NULL);
        pthread_mutex_lock(&once_lock);
        wr32(once, 2);
        pthread_cond_broadcast(&once_done);
        pthread_mutex_unlock(&once_lock);
    }
    R(0) = 0;
}

/* The C++ guards of static objects (ARM's EABI: a 32-bit word whose bit 0 says the object is made). The second byte
   says a thread makes it now: the others wait */
static pthread_mutex_t guard_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t guard_done = PTHREAD_COND_INITIALIZER;
HANDLER(cxa_guard_acquire) {
    UNUSED;
    uint32_t g = R(0);
    if (__atomic_load_n(arm32_mem + g, __ATOMIC_ACQUIRE) & 1) {
        R(0) = 0;
        return;
    }
    pthread_mutex_lock(&guard_lock);
    while (arm32_mem[g + 1]) {
        struct timespec slice = after_ns(SLICE_NS, CLOCK_REALTIME);
        pthread_cond_timedwait(&guard_done, &guard_lock, &slice);
    }
    uint32_t r = arm32_mem[g] & 1 ? 0 : 1;
    if (r) arm32_mem[g + 1] = 1;
    pthread_mutex_unlock(&guard_lock);
    R(0) = r;
}
HANDLER(cxa_guard_release) {
    UNUSED;
    pthread_mutex_lock(&guard_lock);
    arm32_mem[R(0) + 1] = 0;
    __atomic_store_n(arm32_mem + R(0), 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&guard_done);
    pthread_mutex_unlock(&guard_lock);
}
HANDLER(cxa_guard_abort) {
    UNUSED;
    pthread_mutex_lock(&guard_lock);
    arm32_mem[R(0) + 1] = 0;
    pthread_cond_broadcast(&guard_done);
    pthread_mutex_unlock(&guard_lock);
}
HANDLER(cxa_pure_virtual) {
    UNUSED;
    fprintf(stderr, "scarletnx-android: arm32: a pure virtual function called from 0x%08x\n", R(14));
    abort();
}

/* ---- keys ---- */

HANDLER(p_key_create) {
    UNUSED;
    pthread_mutex_lock(&th.lock);
    uint32_t k = th.key_count < 128 ? th.key_count++ : 128;
    if (k < 128) th.key_destructors[k] = R(1);
    pthread_mutex_unlock(&th.lock);
    if (k == 128) {
        R(0) = L_EAGAIN;
        return;
    }
    wr32(R(0), k);
    R(0) = 0;
}
HANDLER(p_key_delete) {
    UNUSED;
    pthread_mutex_lock(&th.lock);
    if (R(0) < 128) th.key_destructors[R(0)] = 0;
    pthread_mutex_unlock(&th.lock);
    R(0) = 0;
}
HANDLER(p_getspecific) {
    UNUSED;
    R(0) = R(0) < 128 ? me()->keys[R(0)] : 0;
}
HANDLER(p_setspecific) {
    UNUSED;
    if (R(0) >= 128) {
        R(0) = L_EINVAL;
        return;
    }
    me()->keys[R(0)] = R(1);
    R(0) = 0;
}

/* ---- pthread_create and the others ---- */

/* bionic's 32-bit pthread_attr_t: flags, stack base, stack size, guard size, policy and priority, 24 bytes */
enum { ATTR_FLAGS = 0, ATTR_STACK = 4, ATTR_STACK_SIZE = 8, ATTR_GUARD = 12, ATTR_POLICY = 16, ATTR_PRIORITY = 20 };
#define DEFAULT_STACK (1u << 20)

HANDLER(a_init) {
    UNUSED;
    memset(arm32_mem + R(0), 0, 24);
    wr32(R(0) + ATTR_STACK_SIZE, DEFAULT_STACK);
    wr32(R(0) + ATTR_GUARD, 4096);
    R(0) = 0;
}
HANDLER(a_zero) {
    UNUSED;
    R(0) = 0;
}
/* the attribute word at data's offset set from r1, or read into *r1 */
HANDLER(a_set) {
    wr32(R(0) + (uint32_t)(uintptr_t)data, R(1));
    R(0) = 0;
}
HANDLER(a_get) {
    wr32(R(1), rd32(R(0) + (uint32_t)(uintptr_t)data));
    R(0) = 0;
}
HANDLER(a_setdetachstate) {
    UNUSED;
    wr32(R(0) + ATTR_FLAGS, R(1) ? 1 : 0); /* PTHREAD_CREATE_DETACHED is 1 */
    R(0) = 0;
}
HANDLER(a_getdetachstate) {
    UNUSED;
    wr32(R(1), rd32(R(0) + ATTR_FLAGS) & 1);
    R(0) = 0;
}
HANDLER(a_setschedparam) {
    UNUSED;
    wr32(R(0) + ATTR_PRIORITY, rd32(R(1)));
    R(0) = 0;
}
HANDLER(a_getschedparam) {
    UNUSED;
    wr32(R(1), rd32(R(0) + ATTR_PRIORITY));
    R(0) = 0;
}
HANDLER(a_setstack) {
    UNUSED;
    wr32(R(0) + ATTR_STACK, R(1));
    wr32(R(0) + ATTR_STACK_SIZE, R(2));
    R(0) = 0;
}
HANDLER(a_getstack) {
    UNUSED;
    wr32(R(1), rd32(R(0) + ATTR_STACK));
    wr32(R(2), rd32(R(0) + ATTR_STACK_SIZE));
    R(0) = 0;
}
/* pthread_getattr_np(thread, attr): its stack, as the guest's allocator gave it */
HANDLER(p_getattr_np) {
    UNUSED;
    ThreadInfo* self = me();
    pthread_mutex_lock(&th.lock);
    ThreadInfo* t = R(0) == self->self ? self : by_self(R(0));
    uint32_t base = t ? t->stack_base : 0, size = t ? t->stack_size : 0;
    bool detached = t && t->detached;
    pthread_mutex_unlock(&th.lock);
    if (!t) {
        R(0) = L_ESRCH;
        return;
    }
    memset(arm32_mem + R(1), 0, 24);
    wr32(R(1) + ATTR_FLAGS, detached ? 1 : 0);
    wr32(R(1) + ATTR_STACK, base);
    wr32(R(1) + ATTR_STACK_SIZE, size);
    wr32(R(1) + ATTR_GUARD, 4096);
    R(0) = 0;
}

static void run_key_destructors(ThreadInfo* t) {
    for (int round = 0; round < 4; ++round) {
        bool any = false;
        for (uint32_t k = 0; k < 128; ++k) {
            pthread_mutex_lock(&th.lock);
            uint32_t d = k < th.key_count ? th.key_destructors[k] : 0;
            pthread_mutex_unlock(&th.lock);
            uint32_t v = t->keys[k];
            if (!d || !v) continue;
            t->keys[k] = 0;
            arm32_call(d, &v, 1, NULL);
            any = true;
        }
        if (!any) break;
    }
}
static void run_cleanups(ThreadInfo* t) {
    while (t->cleanup) {
        uint32_t c = t->cleanup;
        t->cleanup = rd32(c);
        uint32_t arg = rd32(c + 8);
        arm32_call(rd32(c + 4), &arg, 1, NULL);
    }
}

typedef struct {
    uint32_t start, arg, stack_size;
    bool detached;
    ThreadInfo* info; /* the new thread's, which it gives back */
    bool ready;
} Start;

static void* thread_main(void* p) {
    Start* s = p;
    arm32_cpu_with_stack(s->stack_size);
    ThreadInfo* t = me();
    t->created = true;
    t->detached = s->detached;
    t->start = s->start;
    t->arg = s->arg;
    t->host = pthread_self();
    pthread_mutex_lock(&th.lock);
    s->info = t;
    s->ready = true;
    pthread_cond_broadcast(&th.changed);
    pthread_mutex_unlock(&th.lock);
    /* s is the creator's from here on */
    if (!setjmp(t->exit)) t->result = arm32_call(t->start, &t->arg, 1, NULL);
    run_cleanups(t);
    run_key_destructors(t);
    arm32_jni_thread_end();
    pthread_mutex_lock(&th.lock);
    t->done = true;
    bool detached = t->detached;
    t_info = NULL;
    if (detached) forget(t);
    pthread_cond_broadcast(&th.changed);
    pthread_mutex_unlock(&th.lock);
    arm32_thread_end();
    return NULL;
}

/* pthread_create(thread, attr, start, arg) */
HANDLER(p_create) {
    UNUSED;
    Start s = {R(2), R(3), DEFAULT_STACK, false, NULL, false};
    if (R(1)) {
        s.detached = rd32(R(1) + ATTR_FLAGS) & 1;
        if (rd32(R(1) + ATTR_STACK_SIZE)) s.stack_size = rd32(R(1) + ATTR_STACK_SIZE);
    }
    me();
    pthread_t host;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 1u << 20);
    int r = pthread_create(&host, &attr, thread_main, &s);
    pthread_attr_destroy(&attr);
    if (r) {
        R(0) = L_EAGAIN;
        return;
    }
    pthread_mutex_lock(&th.lock);
    while (!s.ready) pthread_cond_wait(&th.changed, &th.lock);
    uint32_t self = s.info->self;
    pthread_mutex_unlock(&th.lock);
    if (R(0)) wr32(R(0), self);
    R(0) = 0;
}
HANDLER(p_join) {
    UNUSED;
    uint32_t target = R(0), out = R(1);
    pthread_mutex_lock(&th.lock);
    ThreadInfo* t = by_self(target);
    if (!t || t->detached || !t->created) {
        pthread_mutex_unlock(&th.lock);
        R(0) = t ? L_EINVAL : L_ESRCH;
        return;
    }
    if (t == t_info) {
        pthread_mutex_unlock(&th.lock);
        R(0) = L_EDEADLK;
        return;
    }
    while (!t->done) {
        struct timespec slice = after_ns(SLICE_NS, CLOCK_REALTIME);
        pthread_cond_timedwait(&th.changed, &th.lock, &slice);
        if (deliverable()) {
            pthread_mutex_unlock(&th.lock);
            run_signals();
            pthread_mutex_lock(&th.lock);
        }
    }
    uint32_t result = t->result;
    forget(t);
    pthread_mutex_unlock(&th.lock);
    if (out) wr32(out, result);
    R(0) = 0;
}
HANDLER(p_detach) {
    UNUSED;
    pthread_mutex_lock(&th.lock);
    ThreadInfo* t = by_self(R(0));
    uint32_t r = t ? 0 : L_ESRCH;
    if (t && t->done) forget(t);
    else if (t) t->detached = true;
    pthread_mutex_unlock(&th.lock);
    R(0) = r;
}
HANDLER(p_exit) {
    UNUSED;
    ThreadInfo* t = me();
    if (!t->created) {
        fprintf(stderr, "scarletnx-android: arm32: pthread_exit on a thread that the guest did not make\n");
        abort();
    }
    t->result = R(0);
    longjmp(t->exit, 1);
}
HANDLER(p_self) {
    UNUSED;
    R(0) = me()->self;
}
HANDLER(p_equal) {
    UNUSED;
    R(0) = R(0) == R(1);
}
HANDLER(p_getschedparam) {
    UNUSED;
    if (R(1)) wr32(R(1), 0); /* SCHED_OTHER */
    if (R(2)) wr32(R(2), 0);
    R(0) = 0;
}
HANDLER(p_cleanup_push) {
    UNUSED;
    ThreadInfo* t = me();
    wr32(R(0), t->cleanup);
    wr32(R(0) + 4, R(1));
    wr32(R(0) + 8, R(2));
    t->cleanup = R(0);
}
HANDLER(p_cleanup_pop) {
    UNUSED;
    ThreadInfo* t = me();
    uint32_t c0 = R(0);
    t->cleanup = rd32(c0);
    if (R(1)) {
        uint32_t arg = rd32(c0 + 8);
        arm32_call(rd32(c0 + 4), &arg, 1, NULL);
    }
}
HANDLER(f_gettid) {
    UNUSED;
    R(0) = me()->tid;
}
HANDLER(f_sched_yield) {
    UNUSED;
    sched_yield();
    run_signals();
    R(0) = 0;
}

/* nanosleep(req, rem): in slices, -1 with EINTR (and the time left) when a signal ran */
static uint32_t sleep_ns(int64_t ns, uint32_t rem) {
    struct timespec end = after_ns(ns, CLOCK_MONOTONIC);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!before(&now, &end)) return 0;
        int64_t left = (int64_t)(end.tv_sec - now.tv_sec) * 1000000000 + (end.tv_nsec - now.tv_nsec);
        struct timespec step = {0, (long)(left < SLICE_NS ? left : SLICE_NS)};
        nanosleep(&step, NULL);
        if (run_signals()) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            left = (int64_t)(end.tv_sec - now.tv_sec) * 1000000000 + (end.tv_nsec - now.tv_nsec);
            if (rem) {
                wr32(rem, left > 0 ? (uint32_t)(left / 1000000000) : 0);
                wr32(rem + 4, left > 0 ? (uint32_t)(left % 1000000000) : 0);
            }
            arm32_set_errno_linux(L_EINTR);
            return UINT32_MAX;
        }
    }
}
HANDLER(f_nanosleep) {
    UNUSED;
    struct timespec t = guest_time(R(0));
    R(0) = sleep_ns((int64_t)t.tv_sec * 1000000000 + t.tv_nsec, R(1));
}
HANDLER(f_usleep) {
    UNUSED;
    R(0) = sleep_ns((int64_t)R(0) * 1000, 0);
}
HANDLER(f_sleep) {
    UNUSED;
    R(0) = sleep_ns((int64_t)R(0) * 1000000000, 0) ? 1 : 0;
}

/* ---- signals ---- */

enum { NSIG_ = 65, SA_SIGINFO_ = 4, SA_ONSTACK_ = 0x08000000, SA_RESETHAND_ = 0x80000000, SA_NODEFER_ = 0x40000000 };
typedef struct {
    uint32_t handler, flags, mask;
} Action;
static Action actions[NSIG_];
static pthread_mutex_t action_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t bit(unsigned sig) { return UINT64_C(1) << (sig - 1); }
/* the thread's signals that it runs, after a change of what it was sent or blocks (under th.lock) */
static void update_deliverable(ThreadInfo* t) {
    uint64_t due = t->sent & ~t->blocked;
    /* SIGKILL and SIGSTOP are never blocked */
    due |= t->sent & (bit(9) | bit(19));
    __atomic_store_n(t->pending, (uint32_t)(due != 0), __ATOMIC_RELEASE);
}
static void send_to(ThreadInfo* t, unsigned sig) {
    t->sent |= bit(sig);
    update_deliverable(t);
}
/* a signal to the process: the calling thread when it does not block it, else the first one that does not */
static uint32_t send_process(unsigned sig) {
    ThreadInfo* self = me();
    pthread_mutex_lock(&th.lock);
    ThreadInfo* target = !(self->blocked & bit(sig)) ? self : NULL;
    for (ThreadInfo* t = th.threads; t && !target; t = t->next)
        if (!(t->blocked & bit(sig))) target = t;
    send_to(target ? target : self, sig);
    pthread_mutex_unlock(&th.lock);
    return 0;
}

/* Linux's ARM frame: siginfo_t (128 bytes), then ucontext_t: uc_flags, uc_link, uc_stack (12 bytes), uc_mcontext
   (trap_no, error_code, oldmask, r0 to r10, fp, ip, sp, lr, pc, cpsr, fault_address: 84 bytes), uc_sigmask, and room
   for the rest of bionic's (the padding and the VFP registers) */
enum {
    SIGINFO_SIZE = 128,
    UC_MCONTEXT = 20,
    MC_R0 = 12,
    MC_CPSR = 12 + 16 * 4,
    UC_SIGMASK = 20 + 84,
    UCONTEXT_SIZE = 744
};

static void run_one(SnxArmCPU* c, ThreadInfo* t, unsigned sig) {
    pthread_mutex_lock(&action_lock);
    Action a = actions[sig];
    if (a.flags & SA_RESETHAND_) actions[sig].handler = 0;
    pthread_mutex_unlock(&action_lock);
    if (a.handler == 1) return; /* SIG_IGN */
    if (a.handler == 0) {       /* SIG_DFL: the ones that do nothing, else the end of the app */
        if (sig == 17 || sig == 23 || sig == 28 || sig == 18 || sig >= 32) return; /* CHLD, URG, WINCH, CONT, RT */
        fprintf(stderr, "scarletnx-android: arm32: signal %u with no handler ends the app (pc 0x%08x)\n", sig,
                c->r[15]);
        exit(128 + (int)sig);
    }
    /* the frame on the guest's stack, or its alternate one */
    uint32_t sp = c->r[13];
    bool alt = (a.flags & SA_ONSTACK_) && t->altstack && !t->on_altstack;
    if (alt) sp = t->altstack + t->altstack_size;
    sp = (sp - SIGINFO_SIZE - UCONTEXT_SIZE - 64) & ~7u;
    uint32_t info = sp + 64, uc = info + SIGINFO_SIZE;
    memset(arm32_mem + info, 0, SIGINFO_SIZE + UCONTEXT_SIZE);
    wr32(info, sig);
    wr32(info + 8, (uint32_t)-6); /* SI_TKILL */
    wr32(info + 12, (uint32_t)getpid());
    wr32(uc + 8, t->altstack);
    wr32(uc + 12, t->altstack ? (t->on_altstack ? 1 : 0) : 2); /* SS_ONSTACK, SS_DISABLE */
    wr32(uc + 16, t->altstack_size);
    uint32_t mc = uc + UC_MCONTEXT;
    wr32(mc + 8, (uint32_t)t->blocked);
    for (unsigned i = 0; i < 16; ++i) wr32(mc + MC_R0 + 4 * i, c->r[i]);
    wr32(mc + MC_CPSR, c->cpsr);
    wr32(uc + UC_SIGMASK, (uint32_t)t->blocked);
    /* the handler's mask: the action's, and the signal itself */
    pthread_mutex_lock(&th.lock);
    uint64_t old = t->blocked;
    t->blocked |= a.mask | ((a.flags & SA_NODEFER_) ? 0 : bit(sig));
    update_deliverable(t);
    pthread_mutex_unlock(&th.lock);
    uint64_t d[32];
    uint32_t fpscr = c->fpscr;
    memcpy(d, c->d, sizeof d);
    bool was_alt = t->on_altstack;
    if (alt) t->on_altstack = true;
    uint32_t saved_sp = c->r[13];
    c->r[13] = sp;
    uint32_t args[3] = {sig, info, uc};
    arm32_call(a.handler, args, (a.flags & SA_SIGINFO_) ? 3 : 1, NULL);
    c->r[13] = saved_sp;
    t->on_altstack = was_alt;
    /* the registers that the frame has now, which the handler may have changed */
    for (unsigned i = 0; i < 16; ++i) c->r[i] = rd32(mc + MC_R0 + 4 * i);
    c->cpsr = (c->cpsr & ~(0xf8000000u | SNX_ARM_T)) | (rd32(mc + MC_CPSR) & (0xf8000000u | SNX_ARM_T));
    memcpy(c->d, d, sizeof d);
    c->fpscr = fpscr;
    c->exclusive_valid = 0;
    pthread_mutex_lock(&th.lock);
    t->blocked = rd32(uc + UC_SIGMASK) | (old & ~UINT64_C(0xffffffff));
    update_deliverable(t);
    pthread_mutex_unlock(&th.lock);
}

void arm32_signals_run(SnxArmCPU* c) {
    ThreadInfo* t = me();
    for (;;) {
        pthread_mutex_lock(&th.lock);
        uint64_t due = t->sent & ~t->blocked;
        unsigned sig = 0;
        for (unsigned s = 1; s < NSIG_ && !sig; ++s)
            if (due & bit(s)) sig = s;
        if (sig) t->sent &= ~bit(sig);
        update_deliverable(t);
        pthread_mutex_unlock(&th.lock);
        if (!sig) return;
        run_one(c, t, sig);
    }
}

/* bionic's 32-bit struct sigaction: handler, sa_mask (32 bits), sa_flags, sa_restorer */
HANDLER(s_action) {
    UNUSED;
    uint32_t sig = R(0), act = R(1), old = R(2);
    if (sig == 0 || sig >= NSIG_ || ((sig == 9 || sig == 19) && act)) {
        arm32_set_errno_linux(L_EINVAL);
        R(0) = UINT32_MAX;
        return;
    }
    pthread_mutex_lock(&action_lock);
    Action previous = actions[sig];
    if (act) actions[sig] = (Action){rd32(act), rd32(act + 8), rd32(act + 4)};
    pthread_mutex_unlock(&action_lock);
    if (old) {
        wr32(old, previous.handler);
        wr32(old + 4, previous.mask);
        wr32(old + 8, previous.flags);
        wr32(old + 12, 0);
    }
    R(0) = 0;
}
/* signal and bsd_signal(sig, handler): the old handler, SIG_ERR (-1) for a bad signal */
HANDLER(s_signal) {
    UNUSED;
    uint32_t sig = R(0);
    if (sig == 0 || sig >= NSIG_ || sig == 9 || sig == 19) {
        R(0) = UINT32_MAX;
        return;
    }
    pthread_mutex_lock(&action_lock);
    uint32_t previous = actions[sig].handler;
    actions[sig] = (Action){R(1), 0x10000000u /* SA_RESTART */, 0};
    pthread_mutex_unlock(&action_lock);
    R(0) = previous;
}
/* sigprocmask and pthread_sigmask(how, set, old): SIG_BLOCK 0, SIG_UNBLOCK 1, SIG_SETMASK 2 */
static uint32_t set_mask(uint32_t how, uint32_t set, uint32_t old) {
    ThreadInfo* t = me();
    pthread_mutex_lock(&th.lock);
    if (old) wr32(old, (uint32_t)t->blocked);
    uint32_t r = 0;
    if (set) {
        uint64_t m = rd32(set);
        if (how == 0) t->blocked |= m;
        else if (how == 1) t->blocked &= ~m;
        else if (how == 2) t->blocked = (t->blocked & ~UINT64_C(0xffffffff)) | m;
        else r = L_EINVAL;
        t->blocked &= ~(bit(9) | bit(19));
        update_deliverable(t);
    }
    pthread_mutex_unlock(&th.lock);
    return r;
}
HANDLER(s_procmask) {
    UNUSED;
    uint32_t r = set_mask(R(0), R(1), R(2));
    if (r) arm32_set_errno_linux((int)r);
    R(0) = r ? UINT32_MAX : 0;
}
HANDLER(s_pthread_sigmask) {
    UNUSED;
    R(0) = set_mask(R(0), R(1), R(2));
}
/* sigsuspend(mask): the mask until a signal runs, then -1 with EINTR */
HANDLER(s_suspend) {
    UNUSED;
    ThreadInfo* t = me();
    pthread_mutex_lock(&th.lock);
    uint64_t old = t->blocked;
    t->blocked = (t->blocked & ~UINT64_C(0xffffffff)) | rd32(R(0));
    update_deliverable(t);
    pthread_mutex_unlock(&th.lock);
    while (!run_signals()) {
        struct timespec step = {0, SLICE_NS / 10};
        nanosleep(&step, NULL);
    }
    pthread_mutex_lock(&th.lock);
    t->blocked = old;
    update_deliverable(t);
    pthread_mutex_unlock(&th.lock);
    arm32_set_errno_linux(L_EINTR);
    R(0) = UINT32_MAX;
}
/* sigaltstack(ss, old): stack_t is ss_sp, ss_flags, ss_size */
HANDLER(s_altstack) {
    UNUSED;
    ThreadInfo* t = me();
    if (R(1)) {
        wr32(R(1), t->altstack);
        wr32(R(1) + 4, t->altstack ? (t->on_altstack ? 1 : 0) : 2);
        wr32(R(1) + 8, t->altstack_size);
    }
    if (R(0)) {
        bool disable = rd32(R(0) + 4) & 2;
        t->altstack = disable ? 0 : rd32(R(0));
        t->altstack_size = disable ? 0 : rd32(R(0) + 8);
    }
    R(0) = 0;
}
HANDLER(s_kill) {
    UNUSED;
    uint32_t pid = R(0), sig = R(1);
    if (pid != 0 && (int32_t)pid != getpid() && (int32_t)pid != -1) {
        arm32_set_errno_linux(L_ESRCH);
        R(0) = UINT32_MAX;
        return;
    }
    if (sig >= NSIG_) {
        arm32_set_errno_linux(L_EINVAL);
        R(0) = UINT32_MAX;
        return;
    }
    R(0) = sig ? send_process(sig) : 0;
}
HANDLER(s_raise) {
    UNUSED;
    uint32_t sig = R(0);
    if (!sig || sig >= NSIG_) {
        R(0) = UINT32_MAX;
        return;
    }
    ThreadInfo* t = me();
    pthread_mutex_lock(&th.lock);
    send_to(t, sig);
    pthread_mutex_unlock(&th.lock);
    run_signals();
    R(0) = 0;
}
/* a signal to a thread by its id: 0, or -errno */
static int32_t send_tid(uint32_t tid, uint32_t sig) {
    if (sig >= NSIG_) return -L_EINVAL;
    pthread_mutex_lock(&th.lock);
    ThreadInfo* t = by_tid(tid);
    if (t && sig) send_to(t, sig);
    pthread_mutex_unlock(&th.lock);
    return t ? 0 : -L_ESRCH;
}
HANDLER(s_pthread_kill) {
    UNUSED;
    pthread_mutex_lock(&th.lock);
    ThreadInfo* t = by_self(R(0));
    uint32_t tid = t ? t->tid : 0;
    pthread_mutex_unlock(&th.lock);
    int32_t r = t ? send_tid(tid, R(1)) : -L_ESRCH;
    R(0) = (uint32_t)-r;
}
HANDLER(s_tkill) {
    UNUSED;
    int32_t r = send_tid(R(0), R(1));
    if (r) arm32_set_errno_linux(-r);
    R(0) = r ? UINT32_MAX : 0;
}

/* ---- setjmp and longjmp: guest code, as they move the guest's own registers and stack ---- */

/* The jmp_buf: r4 to r12 (r12 the stack pointer), lr, d8 to d15, then sigsetjmp's mask and whether it saved it */
static const uint32_t setjmp_code[] = {
    0xe1a0c00d, /* mov r12, sp */
    0xe8a05ff0, /* stmia r0!, {r4-r12, lr} */
    0xeca08b10, /* vstmia r0!, {d8-d15} */
    0xe3a00000, /* mov r0, #0 */
    0xe12fff1e, /* bx lr */
};
static const uint32_t longjmp_code[] = {
    0xe8b05ff0, /* ldmia r0!, {r4-r12, lr} */
    0xecb08b10, /* vldmia r0!, {d8-d15} */
    0xe1a0d00c, /* mov sp, r12 */
    0xe1b00001, /* movs r0, r1 */
    0x03a00001, /* moveq r0, #1 */
    0xe12fff1e, /* bx lr */
};
enum { JB_MASK = 104, JB_SAVED = 108 };
static uint32_t setjmp_at, longjmp_at;

/* sigsetjmp(env, savemask): the mask kept, then the guest's setjmp from the same lr */
HANDLER(j_sigsetjmp) {
    UNUSED;
    wr32(R(0) + JB_SAVED, R(1) ? 1 : 0);
    if (R(1)) wr32(R(0) + JB_MASK, (uint32_t)me()->blocked);
    R(15) = setjmp_at;
    c->cpsr &= ~SNX_ARM_T;
}
HANDLER(j_setjmp) {
    UNUSED;
    wr32(R(0) + JB_SAVED, 0);
    R(15) = setjmp_at;
    c->cpsr &= ~SNX_ARM_T;
}
/* siglongjmp(env, value): the mask that sigsetjmp kept, then the guest's longjmp */
HANDLER(j_siglongjmp) {
    UNUSED;
    if (rd32(R(0) + JB_SAVED)) {
        uint32_t m = rd32(R(0) + JB_MASK);
        ThreadInfo* t = me();
        pthread_mutex_lock(&th.lock);
        t->blocked = (t->blocked & ~UINT64_C(0xffffffff)) | m;
        update_deliverable(t);
        pthread_mutex_unlock(&th.lock);
    }
    R(15) = longjmp_at;
    c->cpsr &= ~SNX_ARM_T;
}

/* ---- the system calls that guest code makes ---- */

/* futex(address, op, value, timeout): FUTEX_WAIT and FUTEX_WAKE (private or not), by the guest's address */
static int32_t futex(uint32_t address, uint32_t op, uint32_t value, uint32_t timeout) {
    Sync* f = sync_of(address, 3);
    switch (op & 0x7f) {
    case 0: { /* FUTEX_WAIT */
        struct timespec end = timeout ? after_ns((int64_t)(int32_t)rd32(timeout) * 1000000000 + (int32_t)rd32(timeout + 4),
                                                 CLOCK_REALTIME)
                                      : (struct timespec){0, 0};
        pthread_mutex_lock(&f->mutex);
        if (__atomic_load_n((uint32_t*)(void*)(arm32_mem + address), __ATOMIC_SEQ_CST) != value) {
            pthread_mutex_unlock(&f->mutex);
            return -L_EAGAIN;
        }
        struct timespec slice = after_ns(SLICE_NS, CLOCK_REALTIME);
        if (timeout && before(&end, &slice)) slice = end;
        int r = pthread_cond_timedwait(&f->cond, &f->mutex, &slice);
        pthread_mutex_unlock(&f->mutex);
        if (run_signals()) return -L_EINTR;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (r == ETIMEDOUT && timeout && !before(&now, &end)) return -L_ETIMEDOUT;
        return 0; /* a wake, or a slice's end: a spurious one, which the caller checks for */
    }
    case 1: /* FUTEX_WAKE */
        pthread_mutex_lock(&f->mutex);
        if (value == 1) pthread_cond_signal(&f->cond);
        else pthread_cond_broadcast(&f->cond);
        pthread_mutex_unlock(&f->mutex);
        return (int32_t)(value > 1 ? 1 : value);
    default: return -L_ENOSYS;
    }
}

uint32_t arm32_syscall(uint32_t number, const uint32_t a[6]) {
    switch (number) {
    case 0xf0002: /* __ARM_NR_cacheflush(start, end, flags): the code there changed */
        arm32_code_changed(a[0], a[1]);
        return 0;
    case 0xf0005: /* __ARM_NR_set_tls */
        arm32_cpu()->tpidruro = a[0];
        return 0;
    case 224: return me()->tid;           /* gettid */
    case 20: return (uint32_t)getpid();   /* getpid */
    case 158: sched_yield(); return 0;    /* sched_yield */
    case 238: return (uint32_t)send_tid(a[0], a[1]); /* tkill */
    case 268: return (uint32_t)send_tid(a[1], a[2]); /* tgkill */
    case 240: return (uint32_t)futex(a[0], a[1], a[2], a[3]);
    case 263: { /* clock_gettime(clock, ts) */
        struct timespec ts;
        clockid_t id = a[0] == 1 || a[0] == 4 ? CLOCK_MONOTONIC : CLOCK_REALTIME;
        clock_gettime(id, &ts);
        wr32(a[1], (uint32_t)ts.tv_sec);
        wr32(a[1] + 4, (uint32_t)ts.tv_nsec);
        return 0;
    }
    default: {
        static uint32_t said[64];
        static unsigned count;
        bool first = true;
        for (unsigned i = 0; i < count && first; ++i) first = said[i] != number;
        if (first && count < 64) {
            said[count++] = number;
            fprintf(stderr, "scarletnx-android: arm32: system call %u is not there yet: ENOSYS\n", number);
        }
        return (uint32_t)-L_ENOSYS;
    }
    }
}
/* syscall(number, ...): the guest's own calls, -1 and errno for a failure */
HANDLER(f_syscall) {
    UNUSED;
    uint32_t args[6];
    for (unsigned i = 0; i < 6; ++i) args[i] = arm32_word(c, i + 1);
    uint32_t r = arm32_syscall(R(0), args);
    if (r >= (uint32_t)-4095) {
        arm32_set_errno_linux(-(int32_t)r);
        r = UINT32_MAX;
    }
    R(0) = r;
}
HANDLER(f_cacheflush) {
    UNUSED;
    arm32_code_changed(R(0), R(1));
    R(0) = 0;
}

/* ---- the table ---- */

#define A(n, f, d) {n, f, (void*)(uintptr_t)(d)}
const Arm32Symbol arm32_thread_symbols[] = {
    A("pthread_create", p_create, 0),
    A("pthread_join", p_join, 0),
    A("pthread_detach", p_detach, 0),
    A("pthread_exit", p_exit, 0),
    A("pthread_self", p_self, 0),
    A("pthread_equal", p_equal, 0),
    A("pthread_kill", s_pthread_kill, 0),
    A("pthread_sigmask", s_pthread_sigmask, 0),
    A("pthread_getattr_np", p_getattr_np, 0),
    A("pthread_getschedparam", p_getschedparam, 0),
    A("pthread_setschedparam", a_zero, 0),
    A("pthread_setname_np", a_zero, 0),
    A("pthread_attr_init", a_init, 0),
    A("pthread_attr_destroy", a_zero, 0),
    A("pthread_attr_setdetachstate", a_setdetachstate, 0),
    A("pthread_attr_getdetachstate", a_getdetachstate, 0),
    A("pthread_attr_setstacksize", a_set, ATTR_STACK_SIZE),
    A("pthread_attr_getstacksize", a_get, ATTR_STACK_SIZE),
    A("pthread_attr_setguardsize", a_set, ATTR_GUARD),
    A("pthread_attr_getguardsize", a_get, ATTR_GUARD),
    A("pthread_attr_setschedpolicy", a_set, ATTR_POLICY),
    A("pthread_attr_getschedpolicy", a_get, ATTR_POLICY),
    A("pthread_attr_setschedparam", a_setschedparam, 0),
    A("pthread_attr_getschedparam", a_getschedparam, 0),
    A("pthread_attr_setstack", a_setstack, 0),
    A("pthread_attr_getstack", a_getstack, 0),
    A("pthread_attr_setscope", a_zero, 0),
    A("pthread_mutex_init", p_mutex_init, 0),
    A("pthread_mutex_destroy", p_mutex_destroy, 0),
    A("pthread_mutex_lock", p_mutex_lock, 0),
    A("pthread_mutex_trylock", p_mutex_trylock, 0),
    A("pthread_mutex_unlock", p_mutex_unlock, 0),
    A("pthread_mutexattr_init", p_mutexattr_init, 0),
    A("pthread_mutexattr_destroy", a_zero, 0),
    A("pthread_mutexattr_settype", p_mutexattr_settype, 0),
    A("pthread_mutexattr_gettype", p_mutexattr_gettype, 0),
    A("pthread_mutexattr_setpshared", a_zero, 0),
    A("pthread_cond_init", p_cond_init, 0),
    A("pthread_cond_destroy", p_cond_destroy, 0),
    A("pthread_cond_wait", p_cond_wait, 0),
    A("pthread_cond_timedwait", p_cond_timedwait, 0),
    A("pthread_cond_timedwait_monotonic_np", p_cond_timedwait, 1),
    A("pthread_cond_timedwait_monotonic", p_cond_timedwait, 1),
    A("pthread_cond_timeout_np", p_cond_timeout_np, 0),
    A("pthread_cond_broadcast", p_cond_broadcast, 0),
    A("pthread_cond_signal", p_cond_signal, 0),
    A("pthread_condattr_init", p_condattr, 0),
    A("pthread_condattr_destroy", p_condattr, 0),
    A("pthread_condattr_setpshared", p_condattr, 0),
    A("pthread_key_create", p_key_create, 0),
    A("pthread_key_delete", p_key_delete, 0),
    A("pthread_getspecific", p_getspecific, 0),
    A("pthread_setspecific", p_setspecific, 0),
    A("pthread_once", p_once, 0),
    A("__pthread_cleanup_push", p_cleanup_push, 0),
    A("__pthread_cleanup_pop", p_cleanup_pop, 0),
    A("sem_init", s_init, 0),
    A("sem_destroy", s_destroy, 0),
    A("sem_post", s_post, 0),
    A("sem_wait", s_wait, 0),
    A("sem_trywait", s_trywait, 0),
    A("sem_timedwait", s_timedwait, 0),
    A("sem_getvalue", s_getvalue, 0),
    A("__cxa_guard_acquire", cxa_guard_acquire, 0),
    A("__cxa_guard_release", cxa_guard_release, 0),
    A("__cxa_guard_abort", cxa_guard_abort, 0),
    A("__cxa_pure_virtual", cxa_pure_virtual, 0),
    A("gettid", f_gettid, 0),
    A("sched_yield", f_sched_yield, 0),
    A("nanosleep", f_nanosleep, 0),
    A("usleep", f_usleep, 0),
    A("sleep", f_sleep, 0),
    A("sigaction", s_action, 0),
    A("signal", s_signal, 0),
    A("bsd_signal", s_signal, 0),
    A("sigprocmask", s_procmask, 0),
    A("sigsuspend", s_suspend, 0),
    A("sigaltstack", s_altstack, 0),
    A("kill", s_kill, 0),
    A("raise", s_raise, 0),
    A("tkill", s_tkill, 0),
    A("setjmp", j_setjmp, 0),
    A("_setjmp", j_setjmp, 0),
    A("sigsetjmp", j_sigsetjmp, 0),
    A("longjmp", j_siglongjmp, 0),
    A("_longjmp", j_siglongjmp, 0),
    A("siglongjmp", j_siglongjmp, 0),
    A("syscall", f_syscall, 0),
    A("cacheflush", f_cacheflush, 0),
    {"", NULL, NULL},
};

void arm32_thread_init(void) {
    setjmp_at = arm32_guest_code(setjmp_code, sizeof setjmp_code / sizeof *setjmp_code);
    longjmp_at = arm32_guest_code(longjmp_code, sizeof longjmp_code / sizeof *longjmp_code);
    me();
}
