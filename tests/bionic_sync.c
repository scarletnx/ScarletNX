/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's synchronization objects in the Android bridge (src/android/bridge/bionic_sync.c), on the futex of the GNU
   ABI runtime's kernel: lock contention between threads, recursion, errorcheck, a broadcast and a timeout, rwlock
   readers and writers, semaphores, barriers, spinlocks, bionic's static initializers and a copied object */
#include "android/bridge/bionic_sync.h"
#include "gnuabi/kernel/futex.h"
#include "gnuabi/kernel/kernel.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "bionic_sync.c:%d: check failed: %s\n", __LINE__, #x);                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ---- a kernel for the host's own addresses (base 0), and a kernel thread on each test thread ---- */
static int op_read(void* context, uint64_t address, void* out, size_t size) {
    (void)context;
    memcpy(out, (const void*)(uintptr_t)address, size);
    return 0;
}
static int op_write(void* context, uint64_t address, const void* in, size_t size) {
    (void)context;
    memcpy((void*)(uintptr_t)address, in, size);
    return 0;
}
static void op_end(void* context, GnuKernelThread* t, int status, int sig, const char* reason) {
    (void)context, (void)t, (void)sig, (void)reason;
    exit(status);
}
static void op_leave(void* context, GnuKernelThread* t) { (void)context, (void)t; }
/* every address is the test's own: the futex's word needs no probe, which would race with the atomics on it */
static int op_check(void* context, uint64_t address, size_t size, int write) {
    (void)context, (void)address, (void)size, (void)write;
    return 0;
}
static const GnuKernelOps ops = {op_read, op_write, op_check, NULL, op_end, op_leave, NULL, NULL, NULL, NULL};

static struct {
    GnuKernel kernel;
    uint32_t next_tid;
} g = {.next_tid = 100};

typedef struct {
    GnuKernelThread k;
    CPU cpu;
} TestThread;
static _Thread_local TestThread* t_self;

static void attach(void) {
    TestThread* t = calloc(1, sizeof *t);
    CHECK(t && !gnu_kernel_thread_add(&g.kernel, &t->k, __atomic_add_fetch(&g.next_tid, 1, __ATOMIC_RELAXED),
                                      &t->cpu, t));
    gnu_kernel_set_self(&t->k);
    t_self = t;
}
static void detach(void) {
    gnu_kernel_thread_remove(&t_self->k);
    gnu_kernel_thread_destroy(&t_self->k);
    free(t_self);
    t_self = NULL;
}

static int wait_on(void* context, uint32_t* word, uint32_t value, const BionicTime* deadline) {
    (void)context;
    int64_t at[2];
    uint64_t a[6] = {(uint64_t)(uintptr_t)word, 0 | 128, value, 0, 0, ~0u};
    if (deadline) {
        at[0] = deadline->seconds;
        at[1] = deadline->nanoseconds;
        a[1] = 9 | 128 | (deadline->clock == 0 ? 256 : 0);
        a[3] = (uint64_t)(uintptr_t)at;
    }
    return (int)gnu_futex(&t_self->k, a);
}
static void wake(void* context, uint32_t* word, int count) {
    (void)context;
    const uint64_t a[6] = {(uint64_t)(uintptr_t)word, 1 | 128, (uint64_t)count, 0, 0, 0};
    (void)gnu_futex(&t_self->k, a);
}
static uint32_t tid(void* context) {
    (void)context;
    return t_self->k.tid;
}
static const BionicFutex f = {wait_on, wake, tid, NULL};

/* a deadline ms from now on a clock */
static BionicTime after_ms(int clock, int ms) {
    int64_t now = clock ? gnu_monotonic_ns() : gnu_realtime_ns(), at = now + (int64_t)ms * 1000000;
    BionicTime t = {at / 1000000000, at % 1000000000, clock};
    return t;
}

/* the guest's storage, aligned as bionic's types are */
typedef struct {
    _Alignas(8) uint32_t w[14];
} Storage;

static void run(int n, void* (*fn)(void*), void* arg) {
    pthread_t threads[8];
    for (int i = 0; i < n; i++) CHECK(!pthread_create(&threads[i], NULL, fn, arg));
    for (int i = 0; i < n; i++) pthread_join(threads[i], NULL);
}

/* ---- mutexes ---- */
static Storage mutex; /* zero: PTHREAD_MUTEX_INITIALIZER */
static long counter;
static void* contend(void* arg) {
    (void)arg;
    attach();
    for (int i = 0; i < 20000; i++) {
        CHECK(!bionic_mutex_lock(&f, &mutex));
        counter++;
        CHECK(!bionic_mutex_unlock(&f, &mutex));
    }
    detach();
    return NULL;
}
static Storage held;
static void* try_from_other(void* expected) {
    attach();
    CHECK(bionic_mutex_trylock(&f, &held) == *(int*)expected);
    CHECK(bionic_mutex_unlock(&f, &held) == (*(int*)expected ? 1 /* EPERM */ : 0));
    detach();
    return NULL;
}

static void mutexes(void) {
    run(4, contend, NULL);
    CHECK(counter == 80000);
    /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP: 0x4000 in the first int */
    held = (Storage){{0x4000}};
    CHECK(!bionic_mutex_lock(&f, &held) && !bionic_mutex_lock(&f, &held) && !bionic_mutex_trylock(&f, &held));
    int busy = 16; /* EBUSY */
    run(1, try_from_other, &busy);
    CHECK(!bionic_mutex_unlock(&f, &held) && !bionic_mutex_unlock(&f, &held) && !bionic_mutex_unlock(&f, &held));
    CHECK(bionic_mutex_unlock(&f, &held) == 1); /* EPERM: not held */
    /* PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP: 0x8000 */
    held = (Storage){{0x8000}};
    CHECK(!bionic_mutex_lock(&f, &held));
    CHECK(bionic_mutex_lock(&f, &held) == 35); /* EDEADLK */
    CHECK(bionic_mutex_trylock(&f, &held) == 16);
    CHECK(!bionic_mutex_unlock(&f, &held) && bionic_mutex_unlock(&f, &held) == 1);
    /* the attributes, a timed lock that times out, and a copy of a free mutex */
    uint64_t attr;
    int type;
    CHECK(!bionic_mutexattr_init(&attr) && !bionic_mutexattr_settype(&attr, BIONIC_MUTEX_RECURSIVE));
    CHECK(!bionic_mutexattr_gettype(&attr, &type) && type == BIONIC_MUTEX_RECURSIVE);
    CHECK(bionic_mutexattr_settype(&attr, 3) == 22);
    CHECK(!bionic_mutexattr_settype(&attr, BIONIC_MUTEX_NORMAL));
    CHECK(!bionic_mutex_init(&held, &attr) && !bionic_mutex_lock(&f, &held));
    Storage copy = held; /* a copy of a held mutex is held too */
    CHECK(bionic_mutex_trylock(&f, &copy) == 16);
    CHECK(!bionic_mutex_unlock(&f, &held));
    Storage free_copy = held;
    CHECK(!bionic_mutex_lock(&f, &free_copy) && !bionic_mutex_unlock(&f, &free_copy));
    CHECK(!bionic_mutex_destroy(&held));
}

static Storage locked;
static void* timed_out(void* arg) {
    (void)arg;
    attach();
    BionicTime t = after_ms(1, 30);
    CHECK(bionic_mutex_timedlock(&f, &locked, &t) == 110); /* ETIMEDOUT */
    BionicTime bad = {0, 1000000000, 0};
    CHECK(bionic_mutex_timedlock(&f, &locked, &bad) == 22); /* EINVAL */
    detach();
    return NULL;
}
static void timed_mutex(void) {
    CHECK(!bionic_mutex_lock(&f, &locked));
    run(1, timed_out, NULL);
    CHECK(!bionic_mutex_unlock(&f, &locked));
}

/* ---- condition variables ---- */
static Storage cond, cond_mutex; /* PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER */
static int ready, woken, waiting;
static void* waiter(void* arg) {
    (void)arg;
    attach();
    CHECK(!bionic_mutex_lock(&f, &cond_mutex));
    waiting++;
    while (!ready) CHECK(!bionic_cond_wait(&f, &cond, &cond_mutex, NULL));
    woken++;
    CHECK(!bionic_mutex_unlock(&f, &cond_mutex));
    detach();
    return NULL;
}
static void* broadcaster(void* arg) {
    (void)arg;
    attach();
    for (;;) {
        CHECK(!bionic_mutex_lock(&f, &cond_mutex));
        int all = waiting == 3;
        if (all) {
            ready = 1;
            CHECK(!bionic_cond_broadcast(&f, &cond));
        }
        CHECK(!bionic_mutex_unlock(&f, &cond_mutex));
        if (all) break;
    }
    detach();
    return NULL;
}
static void* waiter_or_broadcaster(void* arg) {
    int index = __atomic_fetch_add((int*)arg, 1, __ATOMIC_RELAXED);
    return index < 3 ? waiter(NULL) : broadcaster(NULL);
}
static void conds(void) {
    int index = 0;
    run(4, waiter_or_broadcaster, &index);
    CHECK(woken == 3);
    /* a timeout on each clock, with a recursive mutex held twice: still held twice after */
    uint64_t attr;
    int clock;
    CHECK(!bionic_condattr_init(&attr) && !bionic_condattr_setclock(&attr, 1));
    CHECK(!bionic_condattr_getclock(&attr, &clock) && clock == 1 && bionic_condattr_setclock(&attr, 2) == 22);
    Storage mono, m = {{0x4000}};
    CHECK(!bionic_cond_init(&mono, &attr));
    CHECK(!bionic_mutex_lock(&f, &m) && !bionic_mutex_lock(&f, &m));
    BionicTime t = after_ms(1, 30);
    t.clock = -1; /* the cond's own: CLOCK_MONOTONIC */
    CHECK(bionic_cond_wait(&f, &mono, &m, &t) == 110);
    BionicTime r = after_ms(0, 30);
    CHECK(bionic_cond_wait(&f, &cond, &m, &r) == 110);
    CHECK(!bionic_mutex_unlock(&f, &m) && !bionic_mutex_unlock(&f, &m) && bionic_mutex_unlock(&f, &m) == 1);
    CHECK(!bionic_cond_destroy(&mono));
}

/* ---- rwlocks ---- */
static Storage rwlock; /* PTHREAD_RWLOCK_INITIALIZER */
static int readers, most_readers, writing;
static void* reader(void* arg) {
    (void)arg;
    attach();
    for (int i = 0; i < 2000; i++) {
        CHECK(!bionic_rwlock_rdlock(&f, &rwlock, NULL));
        int now = __atomic_add_fetch(&readers, 1, __ATOMIC_ACQ_REL);
        CHECK(!__atomic_load_n(&writing, __ATOMIC_ACQUIRE));
        int seen = __atomic_load_n(&most_readers, __ATOMIC_RELAXED);
        while (now > seen && !__atomic_compare_exchange_n(&most_readers, &seen, now, 0, __ATOMIC_RELAXED,
                                                          __ATOMIC_RELAXED)) {}
        __atomic_sub_fetch(&readers, 1, __ATOMIC_ACQ_REL);
        CHECK(!bionic_rwlock_unlock(&f, &rwlock));
    }
    detach();
    return NULL;
}
static long written;
static void* writer(void* arg) {
    (void)arg;
    attach();
    for (int i = 0; i < 2000; i++) {
        CHECK(!bionic_rwlock_wrlock(&f, &rwlock, NULL));
        CHECK(!__atomic_exchange_n(&writing, 1, __ATOMIC_ACQ_REL) && !__atomic_load_n(&readers, __ATOMIC_ACQUIRE));
        written++;
        __atomic_store_n(&writing, 0, __ATOMIC_RELEASE);
        CHECK(!bionic_rwlock_unlock(&f, &rwlock));
    }
    detach();
    return NULL;
}
static void* reader_or_writer(void* arg) {
    int index = __atomic_fetch_add((int*)arg, 1, __ATOMIC_RELAXED);
    return index < 4 ? reader(NULL) : writer(NULL);
}
static Storage two_readers;
static void* try_write(void* arg) {
    (void)arg;
    attach();
    CHECK(bionic_rwlock_trywrlock(&f, &two_readers) == 16 && !bionic_rwlock_tryrdlock(&f, &two_readers));
    BionicTime t = after_ms(0, 30);
    CHECK(bionic_rwlock_wrlock(&f, &two_readers, &t) == 110);
    CHECK(!bionic_rwlock_unlock(&f, &two_readers));
    detach();
    return NULL;
}
static void rwlocks(void) {
    int index = 0;
    run(6, reader_or_writer, &index);
    CHECK(written == 4000 && !readers);
    CHECK(!bionic_rwlock_rdlock(&f, &two_readers, NULL) && !bionic_rwlock_rdlock(&f, &two_readers, NULL));
    run(1, try_write, NULL);
    CHECK(!bionic_rwlock_unlock(&f, &two_readers) && !bionic_rwlock_unlock(&f, &two_readers));
    CHECK(bionic_rwlock_unlock(&f, &two_readers) == 1);
    CHECK(!bionic_rwlock_wrlock(&f, &two_readers, NULL));
    CHECK(bionic_rwlock_rdlock(&f, &two_readers, NULL) == 35 && bionic_rwlock_wrlock(&f, &two_readers, NULL) == 35);
    CHECK(bionic_rwlock_destroy(&two_readers) == 16 && !bionic_rwlock_unlock(&f, &two_readers));
    CHECK(!bionic_rwlock_destroy(&two_readers));
}

/* ---- semaphores, barriers, spinlocks ---- */
static Storage sem;
static int taken;
static void* sem_taker(void* arg) {
    (void)arg;
    attach();
    int error = 0;
    CHECK(!bionic_sem_wait(&f, &sem, NULL, &error));
    __atomic_add_fetch(&taken, 1, __ATOMIC_RELAXED);
    detach();
    return NULL;
}
static void* sem_poster(void* arg) {
    (void)arg;
    attach();
    int error = 0;
    for (int i = 0; i < 2; i++) CHECK(!bionic_sem_post(&f, &sem, &error));
    detach();
    return NULL;
}
static void* taker_or_poster(void* arg) {
    int index = __atomic_fetch_add((int*)arg, 1, __ATOMIC_RELAXED);
    return index < 2 ? sem_taker(NULL) : sem_poster(NULL);
}
static Storage barrier;
static int serial;
static void* rounds(void* arg) {
    (void)arg;
    attach();
    for (int i = 0; i < 200; i++)
        if (bionic_barrier_wait(&f, &barrier) == BIONIC_BARRIER_SERIAL_THREAD)
            __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED);
    detach();
    return NULL;
}
static Storage spin;
static long spun;
static void* spinner(void* arg) {
    (void)arg;
    for (int i = 0; i < 20000; i++) {
        CHECK(!bionic_spin_lock(&spin));
        spun++;
        CHECK(!bionic_spin_unlock(&spin));
    }
    return NULL;
}
static void others(void) {
    int error = 0, value = -1;
    CHECK(!bionic_sem_init(&sem, 0, 0, &error) && bionic_sem_trywait(&sem, &error) == -1 && error == 11);
    int index = 0;
    run(3, taker_or_poster, &index);
    CHECK(taken == 2 && !bionic_sem_getvalue(&sem, &value, &error) && value == 0);
    BionicTime t = after_ms(0, 30);
    CHECK(bionic_sem_wait(&f, &sem, &t, &error) == -1 && error == 110);
    CHECK(bionic_sem_init(&sem, 0, 0x40000000u, &error) == -1 && error == 22);
    CHECK(!bionic_barrier_init(&barrier, NULL, 4) && bionic_barrier_init(&barrier, NULL, 0) == 22);
    run(4, rounds, NULL);
    CHECK(serial == 200 && !bionic_barrier_destroy(&barrier));
    CHECK(!bionic_spin_init(&spin, 0));
    run(4, spinner, NULL);
    CHECK(spun == 80000 && !bionic_spin_trylock(&spin) && bionic_spin_trylock(&spin) == 16);
    CHECK(bionic_spin_destroy(&spin) == 16 && !bionic_spin_unlock(&spin));
}

int main(void) {
    gnu_kernel_init(&g.kernel, &ops, NULL, NULL, 1);
    attach();
    mutexes();
    timed_mutex();
    conds();
    rwlocks();
    others();
    detach();
    puts("bionic_sync: ok");
    return 0;
}
