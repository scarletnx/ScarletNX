/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's mutexes, condition variables, rwlocks, spinlocks, barriers and semaphores as HLE (bionic_sync.c), on every
   host: in bionic's layout inside the guest's storage, never a host object (FreeBSD's libthr and winpthreads keep a
   pointer to the heap there, macOS's are larger than bionic's, and a guest copies them and sets bionic's own
   initializers). A wait is a futex wait of the GNU ABI runtime's kernel, so a guest signal runs its handler during it
   (hle_signal.c) */
#include "android/bridge/bionic_sync.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_signal.h"
#include "common/utilities/endian.h"
#include "gnuabi/abi/errno/errno.h"
#include <stdbool.h>
#include <string.h>

enum { FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_WAIT_BITSET = 9, FUTEX_PRIVATE = 128, FUTEX_CLOCK_REALTIME = 256 };

/* the kernel's futex for the HLE call that c makes */
static int futex_wait(void* context, uint32_t* word, uint32_t value, const BionicTime* deadline) {
    uint8_t at[16];
    uint64_t a[6] = {(uint64_t)(uintptr_t)word, FUTEX_WAIT | FUTEX_PRIVATE, value, 0, 0, ~0u};
    if (deadline) {
        snx_st_le64(at, (uint64_t)deadline->seconds);
        snx_st_le64(at + 8, (uint64_t)deadline->nanoseconds);
        a[1] = FUTEX_WAIT_BITSET | FUTEX_PRIVATE | (deadline->clock == 0 ? FUTEX_CLOCK_REALTIME : 0);
        a[3] = (uint64_t)(uintptr_t)at;
    }
    return (int)hle_kernel_syscall(98, a, context, NULL);
}
static void futex_wake(void* context, uint32_t* word, int count) {
    const uint64_t a[6] = {(uint64_t)(uintptr_t)word, FUTEX_WAKE | FUTEX_PRIVATE, (uint64_t)count, 0, 0, 0};
    (void)hle_kernel_syscall(98, a, context, NULL);
}
static uint32_t futex_tid(void* context) {
    (void)context;
    return hle_kernel_tid();
}
static BionicFutex futex_of(CPU* c) {
    BionicFutex f = {futex_wait, futex_wake, futex_tid, c};
    return f;
}

/* a guest timespec {s64, s64} at address on clock */
static BionicTime time_at(uint64_t address, int clock) {
    const uint8_t* p = (const uint8_t*)(uintptr_t)address;
    BionicTime t = {(int64_t)snx_ld_le64(p), (int64_t)snx_ld_le64(p + 8), clock};
    return t;
}
static bool clock_valid(uint64_t clock) { return clock == 0 || clock == 1; } /* CLOCK_REALTIME, CLOCK_MONOTONIC */
/* a relative time from now, as an absolute one on CLOCK_MONOTONIC */
static BionicTime monotonic_after(uint64_t address) {
    BionicTime t = time_at(address, 1);
    if (t.seconds < 0 || t.nanoseconds < 0 || t.nanoseconds >= 1000000000) return t; /* refused as it is */
    int64_t now = gnu_monotonic_ns();
    if (t.seconds > (INT64_MAX - now) / 1000000000 - 1) t.seconds = (INT64_MAX - now) / 1000000000 - 1;
    int64_t at = now + t.seconds * 1000000000 + t.nanoseconds;
    BionicTime r = {at / 1000000000, at % 1000000000, 1};
    return r;
}

/* ---- attributes ---- */
#define ATTR_INIT(name, fn)                                                                                            \
    static void name(CPU* c, void* data) {                                                                             \
        (void)data;                                                                                                    \
        X(0) = (uint64_t)fn(PTR(0));                                                                                   \
    }
#define ATTR_SET(name, fn)                                                                                             \
    static void name(CPU* c, void* data) {                                                                             \
        (void)data;                                                                                                    \
        X(0) = (uint64_t)fn(PTR(0), (int)X(1));                                                                        \
    }
#define ATTR_GET(name, fn)                                                                                             \
    static void name(CPU* c, void* data) {                                                                             \
        (void)data;                                                                                                    \
        int v = 0;                                                                                                     \
        int r = fn(PTR(0), &v);                                                                                        \
        if (!r) snx_st_le32(PTR(1), (uint32_t)v);                                                                      \
        X(0) = (uint64_t)r;                                                                                            \
    }
ATTR_INIT(g_mutexattr_init, bionic_mutexattr_init)
ATTR_SET(g_mutexattr_settype, bionic_mutexattr_settype)
ATTR_GET(g_mutexattr_gettype, bionic_mutexattr_gettype)
ATTR_SET(g_mutexattr_setpshared, bionic_mutexattr_setpshared)
ATTR_GET(g_mutexattr_getpshared, bionic_mutexattr_getpshared)
ATTR_INIT(g_condattr_init, bionic_condattr_init)
ATTR_SET(g_condattr_setclock, bionic_condattr_setclock)
ATTR_GET(g_condattr_getclock, bionic_condattr_getclock)
ATTR_SET(g_condattr_setpshared, bionic_condattr_setpshared)
ATTR_GET(g_condattr_getpshared, bionic_condattr_getpshared)
ATTR_INIT(g_rwlockattr_init, bionic_rwlockattr_init)
ATTR_SET(g_rwlockattr_setpshared, bionic_rwlockattr_setpshared)
ATTR_GET(g_rwlockattr_getpshared, bionic_rwlockattr_getpshared)
ATTR_SET(g_rwlockattr_setkind, bionic_rwlockattr_setkind)
ATTR_GET(g_rwlockattr_getkind, bionic_rwlockattr_getkind)
ATTR_INIT(g_barrierattr_init, bionic_barrierattr_init)
ATTR_SET(g_barrierattr_setpshared, bionic_barrierattr_setpshared)
ATTR_GET(g_barrierattr_getpshared, bionic_barrierattr_getpshared)
static void g_attr_destroy(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}

/* ---- mutexes ---- */
static void g_mutex_init(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_mutex_init(PTR(0), X(1) ? PTR(1) : NULL);
}
static void g_mutex_destroy(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_mutex_destroy(PTR(0));
}
static void g_mutex_lock(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)bionic_mutex_lock(&f, PTR(0));
}
static void g_mutex_trylock(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)bionic_mutex_trylock(&f, PTR(0));
}
static void g_mutex_unlock(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)bionic_mutex_unlock(&f, PTR(0));
}
/* pthread_mutex_timedlock (CLOCK_REALTIME), _timedlock_monotonic_np, pthread_mutex_clocklock(m, clock, t) */
static void mutex_timed(CPU* c, int clock, uint64_t at) {
    BionicFutex f = futex_of(c);
    BionicTime t = time_at(at, clock);
    X(0) = (uint64_t)bionic_mutex_timedlock(&f, PTR(0), &t);
}
static void g_mutex_timedlock(CPU* c, void* data) {
    (void)data;
    mutex_timed(c, 0, X(1));
}
static void g_mutex_timedlock_monotonic(CPU* c, void* data) {
    (void)data;
    mutex_timed(c, 1, X(1));
}
static void g_mutex_clocklock(CPU* c, void* data) {
    (void)data;
    if (!clock_valid(X(1))) {
        X(0) = GNU_EINVAL;
        return;
    }
    mutex_timed(c, (int)X(1), X(2));
}

/* ---- condition variables ---- */
static void g_cond_init(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_cond_init(PTR(0), X(1) ? PTR(1) : NULL);
}
static void g_cond_destroy(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_cond_destroy(PTR(0));
}
static void g_cond_signal(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)bionic_cond_signal(&f, PTR(0));
}
static void g_cond_broadcast(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)bionic_cond_broadcast(&f, PTR(0));
}
static void g_cond_wait(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)bionic_cond_wait(&f, PTR(0), PTR(1), NULL);
}
static void cond_timed(CPU* c, const BionicTime* t) {
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)bionic_cond_wait(&f, PTR(0), PTR(1), t);
}
/* pthread_cond_timedwait: the cond's clock */
static void g_cond_timedwait(CPU* c, void* data) {
    (void)data;
    BionicTime t = time_at(X(2), -1);
    cond_timed(c, &t);
}
static void g_cond_timedwait_monotonic(CPU* c, void* data) {
    (void)data;
    BionicTime t = time_at(X(2), 1);
    cond_timed(c, &t);
}
static void g_cond_timedwait_relative(CPU* c, void* data) {
    (void)data;
    BionicTime t = monotonic_after(X(2));
    cond_timed(c, &t);
}
static void g_cond_clockwait(CPU* c, void* data) {
    (void)data;
    if (!clock_valid(X(2))) {
        X(0) = GNU_EINVAL;
        return;
    }
    BionicTime t = time_at(X(3), (int)X(2));
    cond_timed(c, &t);
}

/* ---- rwlocks ---- */
static void g_rwlock_init(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_rwlock_init(PTR(0), X(1) ? PTR(1) : NULL);
}
static void g_rwlock_destroy(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_rwlock_destroy(PTR(0));
}
static void rw(CPU* c, bool write, bool once, const BionicTime* t) {
    BionicFutex f = futex_of(c);
    if (once)
        X(0) = (uint64_t)(write ? bionic_rwlock_trywrlock(&f, PTR(0)) : bionic_rwlock_tryrdlock(&f, PTR(0)));
    else
        X(0) = (uint64_t)(write ? bionic_rwlock_wrlock(&f, PTR(0), t) : bionic_rwlock_rdlock(&f, PTR(0), t));
}
static void g_rwlock_rdlock(CPU* c, void* data) {
    (void)data;
    rw(c, false, false, NULL);
}
static void g_rwlock_tryrdlock(CPU* c, void* data) {
    (void)data;
    rw(c, false, true, NULL);
}
static void g_rwlock_wrlock(CPU* c, void* data) {
    (void)data;
    rw(c, true, false, NULL);
}
static void g_rwlock_trywrlock(CPU* c, void* data) {
    (void)data;
    rw(c, true, true, NULL);
}
/* data: 0 a reader, 1 a writer; with CLOCK_REALTIME, CLOCK_MONOTONIC, or the clock argument (clockrdlock) */
static void rw_timed(CPU* c, bool write, int clock) {
    uint64_t at = X(1);
    if (clock < 0) {
        if (!clock_valid(X(1))) {
            X(0) = GNU_EINVAL;
            return;
        }
        clock = (int)X(1);
        at = X(2);
    }
    BionicTime t = time_at(at, clock);
    rw(c, write, false, &t);
}
static void g_rwlock_timedrdlock(CPU* c, void* data) {
    (void)data;
    rw_timed(c, false, 0);
}
static void g_rwlock_timedrdlock_monotonic(CPU* c, void* data) {
    (void)data;
    rw_timed(c, false, 1);
}
static void g_rwlock_clockrdlock(CPU* c, void* data) {
    (void)data;
    rw_timed(c, false, -1);
}
static void g_rwlock_timedwrlock(CPU* c, void* data) {
    (void)data;
    rw_timed(c, true, 0);
}
static void g_rwlock_timedwrlock_monotonic(CPU* c, void* data) {
    (void)data;
    rw_timed(c, true, 1);
}
static void g_rwlock_clockwrlock(CPU* c, void* data) {
    (void)data;
    rw_timed(c, true, -1);
}
static void g_rwlock_unlock(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)bionic_rwlock_unlock(&f, PTR(0));
}

/* ---- spinlocks and barriers ---- */
static void g_spin_init(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_spin_init(PTR(0), (int)X(1));
}
static void g_spin_destroy(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_spin_destroy(PTR(0));
}
static void g_spin_lock(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_spin_lock(PTR(0));
}
static void g_spin_trylock(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_spin_trylock(PTR(0));
}
static void g_spin_unlock(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_spin_unlock(PTR(0));
}
static void g_barrier_init(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_barrier_init(PTR(0), X(1) ? PTR(1) : NULL, (unsigned)X(2));
}
static void g_barrier_destroy(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)bionic_barrier_destroy(PTR(0));
}
static void g_barrier_wait(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    X(0) = (uint64_t)(int64_t)bionic_barrier_wait(&f, PTR(0));
}

/* ---- semaphores: 0, or -1 and the guest's errno ---- */
static void sem_result(CPU* c, int r, int error) {
    if (r) hle_set_errno(error);
    X(0) = (uint64_t)(int64_t)r;
}
static void g_sem_init(CPU* c, void* data) {
    (void)data;
    int error = 0, r = bionic_sem_init(PTR(0), (int)X(1), (unsigned)X(2), &error);
    sem_result(c, r, error);
}
static void g_sem_destroy(CPU* c, void* data) {
    (void)data;
    int error = 0, r = bionic_sem_destroy(PTR(0), &error);
    sem_result(c, r, error);
}
static void g_sem_post(CPU* c, void* data) {
    (void)data;
    BionicFutex f = futex_of(c);
    int error = 0, r = bionic_sem_post(&f, PTR(0), &error);
    sem_result(c, r, error);
}
static void sem_timed(CPU* c, const BionicTime* t) {
    BionicFutex f = futex_of(c);
    int error = 0, r = bionic_sem_wait(&f, PTR(0), t, &error);
    sem_result(c, r, error);
}
static void g_sem_wait(CPU* c, void* data) {
    (void)data;
    sem_timed(c, NULL);
}
static void g_sem_timedwait(CPU* c, void* data) {
    (void)data;
    BionicTime t = time_at(X(1), 0);
    sem_timed(c, &t);
}
static void g_sem_timedwait_monotonic(CPU* c, void* data) {
    (void)data;
    BionicTime t = time_at(X(1), 1);
    sem_timed(c, &t);
}
static void g_sem_clockwait(CPU* c, void* data) {
    (void)data;
    if (!clock_valid(X(1))) {
        sem_result(c, -1, GNU_EINVAL);
        return;
    }
    BionicTime t = time_at(X(2), (int)X(1));
    sem_timed(c, &t);
}
static void g_sem_trywait(CPU* c, void* data) {
    (void)data;
    int error = 0, r = bionic_sem_trywait(PTR(0), &error);
    sem_result(c, r, error);
}
static void g_sem_getvalue(CPU* c, void* data) {
    (void)data;
    int error = 0, value = 0, r = bionic_sem_getvalue(PTR(0), &value, &error);
    if (!r) snx_st_le32(PTR(1), (uint32_t)value);
    sem_result(c, r, error);
}

#define F(name, handler) {#name, handler, NULL}
const HleSymbol hle_sync[] = {
    F(pthread_mutexattr_init, g_mutexattr_init),
    F(pthread_mutexattr_destroy, g_attr_destroy),
    F(pthread_mutexattr_settype, g_mutexattr_settype),
    F(pthread_mutexattr_gettype, g_mutexattr_gettype),
    F(pthread_mutexattr_setpshared, g_mutexattr_setpshared),
    F(pthread_mutexattr_getpshared, g_mutexattr_getpshared),
    F(pthread_mutex_init, g_mutex_init),
    F(pthread_mutex_destroy, g_mutex_destroy),
    F(pthread_mutex_lock, g_mutex_lock),
    F(pthread_mutex_trylock, g_mutex_trylock),
    F(pthread_mutex_unlock, g_mutex_unlock),
    F(pthread_mutex_timedlock, g_mutex_timedlock),
    F(pthread_mutex_timedlock_monotonic_np, g_mutex_timedlock_monotonic),
    F(pthread_mutex_clocklock, g_mutex_clocklock),
    F(pthread_condattr_init, g_condattr_init),
    F(pthread_condattr_destroy, g_attr_destroy),
    F(pthread_condattr_setclock, g_condattr_setclock),
    F(pthread_condattr_getclock, g_condattr_getclock),
    F(pthread_condattr_setpshared, g_condattr_setpshared),
    F(pthread_condattr_getpshared, g_condattr_getpshared),
    F(pthread_cond_init, g_cond_init),
    F(pthread_cond_destroy, g_cond_destroy),
    F(pthread_cond_signal, g_cond_signal),
    F(pthread_cond_broadcast, g_cond_broadcast),
    F(pthread_cond_wait, g_cond_wait),
    F(pthread_cond_timedwait, g_cond_timedwait),
    F(pthread_cond_timedwait_monotonic_np, g_cond_timedwait_monotonic),
    F(pthread_cond_timedwait_monotonic, g_cond_timedwait_monotonic),
    F(pthread_cond_timedwait_relative_np, g_cond_timedwait_relative),
    F(pthread_cond_clockwait, g_cond_clockwait),
    F(pthread_rwlockattr_init, g_rwlockattr_init),
    F(pthread_rwlockattr_destroy, g_attr_destroy),
    F(pthread_rwlockattr_setpshared, g_rwlockattr_setpshared),
    F(pthread_rwlockattr_getpshared, g_rwlockattr_getpshared),
    F(pthread_rwlockattr_setkind_np, g_rwlockattr_setkind),
    F(pthread_rwlockattr_getkind_np, g_rwlockattr_getkind),
    F(pthread_rwlock_init, g_rwlock_init),
    F(pthread_rwlock_destroy, g_rwlock_destroy),
    F(pthread_rwlock_rdlock, g_rwlock_rdlock),
    F(pthread_rwlock_tryrdlock, g_rwlock_tryrdlock),
    F(pthread_rwlock_timedrdlock, g_rwlock_timedrdlock),
    F(pthread_rwlock_timedrdlock_monotonic_np, g_rwlock_timedrdlock_monotonic),
    F(pthread_rwlock_clockrdlock, g_rwlock_clockrdlock),
    F(pthread_rwlock_wrlock, g_rwlock_wrlock),
    F(pthread_rwlock_trywrlock, g_rwlock_trywrlock),
    F(pthread_rwlock_timedwrlock, g_rwlock_timedwrlock),
    F(pthread_rwlock_timedwrlock_monotonic_np, g_rwlock_timedwrlock_monotonic),
    F(pthread_rwlock_clockwrlock, g_rwlock_clockwrlock),
    F(pthread_rwlock_unlock, g_rwlock_unlock),
    F(pthread_spin_init, g_spin_init),
    F(pthread_spin_destroy, g_spin_destroy),
    F(pthread_spin_lock, g_spin_lock),
    F(pthread_spin_trylock, g_spin_trylock),
    F(pthread_spin_unlock, g_spin_unlock),
    F(pthread_barrierattr_init, g_barrierattr_init),
    F(pthread_barrierattr_destroy, g_attr_destroy),
    F(pthread_barrierattr_setpshared, g_barrierattr_setpshared),
    F(pthread_barrierattr_getpshared, g_barrierattr_getpshared),
    F(pthread_barrier_init, g_barrier_init),
    F(pthread_barrier_destroy, g_barrier_destroy),
    F(pthread_barrier_wait, g_barrier_wait),
    F(sem_init, g_sem_init),
    F(sem_destroy, g_sem_destroy),
    F(sem_post, g_sem_post),
    F(sem_wait, g_sem_wait),
    F(sem_timedwait, g_sem_timedwait),
    F(sem_timedwait_monotonic_np, g_sem_timedwait_monotonic),
    F(sem_clockwait, g_sem_clockwait),
    F(sem_trywait, g_sem_trywait),
    F(sem_getvalue, g_sem_getvalue),
    {"", NULL, NULL},
};
