/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's synchronization objects (bionic_sync.h), written from POSIX (pthread_mutex_lock(3p), pthread_cond_wait(3p),
   pthread_rwlock_rdlock(3p), pthread_barrier_wait(3p), sem_wait(3p)) and the sizes and initializers of bionic's
   headers. The words of each object, by 32-bit index in the guest's storage:
   - mutex (10 words): 0 the state (bits 14 and 15 the type, as bionic's initializers set them, bit 13 shared, bits 0
     and 1: 0 free, 1 held, 2 held with waiters, Drepper's three states), 1 the owner's tid, 2 the recursion count
   - cond (12 words): 0 a sequence that each signal and broadcast moves, 1 the flags (bit 0 CLOCK_MONOTONIC, bit 1
     shared), 2 the waiters
   - rwlock (14 words): 0 the state (the readers, or bit 31 a writer), 1 a sequence that each release moves, 2 the
     waiters, 3 the writer's tid, 4 the flags (bit 0 shared, bit 1 writers first), 5 the writers that wait
   - barrier (8 words): 0 the count, 1 the threads that arrived, 2 the round, 3 shared
   - spinlock (2 words): 0 held
   - sem_t (4 words): 0 the value, 1 the waiters, 2 shared */
#include "android/bridge/bionic_sync.h"
#include "gnuabi/abi/errno/errno.h"
#include <stdbool.h>
#include <stddef.h>

#define W(object, i) ((uint32_t*)(object) + (i))
static uint32_t load(uint32_t* w) { return __atomic_load_n(w, __ATOMIC_ACQUIRE); }
static void store(uint32_t* w, uint32_t v) { __atomic_store_n(w, v, __ATOMIC_RELEASE); }
static bool cas(uint32_t* w, uint32_t* expected, uint32_t v) {
    return __atomic_compare_exchange_n(w, expected, v, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static void add(uint32_t* w, uint32_t v) { __atomic_fetch_add(w, v, __ATOMIC_ACQ_REL); }
static void sub(uint32_t* w, uint32_t v) { __atomic_fetch_sub(w, v, __ATOMIC_ACQ_REL); }

static bool valid_time(const BionicTime* t) {
    return !t || (t->nanoseconds >= 0 && t->nanoseconds < 1000000000 && t->seconds >= 0);
}

/* ---- the attributes: a long (an int for the barrier's) ---- */
static uint32_t attr_bits(const void* attr) { return attr ? *(const uint32_t*)attr : 0; }
static void attr_put(void* attr, uint32_t v) { *(uint32_t*)attr = v; }
static int put_field(void* attr, uint32_t mask, unsigned shift, int value, int most) {
    if (value < 0 || value > most) return GNU_EINVAL;
    attr_put(attr, (attr_bits(attr) & ~(mask << shift)) | (uint32_t)value << shift);
    return 0;
}
static int field(const void* attr, uint32_t mask, unsigned shift) { return (int)(attr_bits(attr) >> shift & mask); }

/* mutexattr: bits 0 and 1 the type, bit 2 shared. The long's upper half stays 0 */
int bionic_mutexattr_init(void* attr) {
    *(uint64_t*)attr = 0;
    return 0;
}
int bionic_mutexattr_settype(void* attr, int type) { return put_field(attr, 3, 0, type, BIONIC_MUTEX_ERRORCHECK); }
int bionic_mutexattr_gettype(const void* attr, int* type) {
    *type = field(attr, 3, 0);
    return 0;
}
int bionic_mutexattr_setpshared(void* attr, int pshared) { return put_field(attr, 1, 2, pshared, 1); }
int bionic_mutexattr_getpshared(const void* attr, int* pshared) {
    *pshared = field(attr, 1, 2);
    return 0;
}
/* condattr: bit 0 CLOCK_MONOTONIC, bit 1 shared */
int bionic_condattr_init(void* attr) {
    *(uint64_t*)attr = 0;
    return 0;
}
int bionic_condattr_setclock(void* attr, int clock) { return put_field(attr, 1, 0, clock, 1); }
int bionic_condattr_getclock(const void* attr, int* clock) {
    *clock = field(attr, 1, 0);
    return 0;
}
int bionic_condattr_setpshared(void* attr, int pshared) { return put_field(attr, 1, 1, pshared, 1); }
int bionic_condattr_getpshared(const void* attr, int* pshared) {
    *pshared = field(attr, 1, 1);
    return 0;
}
/* rwlockattr: bit 0 shared, bit 1 the kind (PTHREAD_RWLOCK_PREFER_READER_NP 0, PREFER_WRITER_NONRECURSIVE_NP 1) */
int bionic_rwlockattr_init(void* attr) {
    *(uint64_t*)attr = 0;
    return 0;
}
int bionic_rwlockattr_setpshared(void* attr, int pshared) { return put_field(attr, 1, 0, pshared, 1); }
int bionic_rwlockattr_getpshared(const void* attr, int* pshared) {
    *pshared = field(attr, 1, 0);
    return 0;
}
int bionic_rwlockattr_setkind(void* attr, int kind) { return put_field(attr, 1, 1, kind, 1); }
int bionic_rwlockattr_getkind(const void* attr, int* kind) {
    *kind = field(attr, 1, 1);
    return 0;
}
int bionic_barrierattr_init(void* attr) {
    attr_put(attr, 0);
    return 0;
}
int bionic_barrierattr_setpshared(void* attr, int pshared) { return put_field(attr, 1, 0, pshared, 1); }
int bionic_barrierattr_getpshared(const void* attr, int* pshared) {
    *pshared = field(attr, 1, 0);
    return 0;
}

/* ---- mutexes ---- */
enum { M_STATE = 0, M_OWNER = 1, M_COUNT = 2, M_WORDS = 10, M_HELD = 3u, M_TYPE_SHIFT = 14, M_SHARED = 1u << 13 };

static void zero(void* object, unsigned words) {
    for (unsigned i = 0; i < words; i++) store(W(object, i), 0);
}
int bionic_mutex_init(void* mutex, const void* attr) {
    zero(mutex, M_WORDS);
    uint32_t a = attr_bits(attr);
    store(W(mutex, M_STATE), (a & 3) << M_TYPE_SHIFT | (a & 4 ? M_SHARED : 0));
    return 0;
}
static int type_of(void* mutex) { return (int)(load(W(mutex, M_STATE)) >> M_TYPE_SHIFT & 3); }
int bionic_mutex_destroy(void* mutex) {
    return load(W(mutex, M_STATE)) & M_HELD ? GNU_EBUSY : 0;
}

/* the state word taken (Drepper's mutex, with the type bits kept): 0, or ETIMEDOUT */
static int take(const BionicFutex* f, uint32_t* w, const BionicTime* deadline) {
    uint32_t bits = load(w) & ~M_HELD, c = bits;
    if (cas(w, &c, bits | 1)) return 0;
    if ((c & M_HELD) != 2) c = __atomic_exchange_n(w, bits | 2, __ATOMIC_ACQ_REL);
    while (c & M_HELD) {
        if (f->wait(f->context, w, bits | 2, deadline) == -GNU_ETIMEDOUT) return GNU_ETIMEDOUT;
        c = __atomic_exchange_n(w, bits | 2, __ATOMIC_ACQ_REL);
    }
    return 0;
}
static void give(const BionicFutex* f, uint32_t* w) {
    uint32_t old = __atomic_fetch_and(w, ~M_HELD, __ATOMIC_ACQ_REL);
    if ((old & M_HELD) == 2) f->wake(f->context, w, 1);
}

static int lock(const BionicFutex* f, void* mutex, const BionicTime* deadline, bool once) {
    int type = type_of(mutex);
    if (type > BIONIC_MUTEX_ERRORCHECK) return GNU_EINVAL;
    uint32_t self = f->tid(f->context);
    if (type != BIONIC_MUTEX_NORMAL && load(W(mutex, M_OWNER)) == self) {
        if (type == BIONIC_MUTEX_ERRORCHECK) return once ? GNU_EBUSY : GNU_EDEADLK;
        if (load(W(mutex, M_COUNT)) == UINT32_MAX) return GNU_EAGAIN;
        add(W(mutex, M_COUNT), 1);
        return 0;
    }
    uint32_t* w = W(mutex, M_STATE);
    uint32_t free_state = load(w) & ~M_HELD;
    if (!cas(w, &free_state, free_state | 1)) {
        if (once) return GNU_EBUSY;
        if (!valid_time(deadline)) return GNU_EINVAL;
        int r = take(f, w, deadline);
        if (r) return r;
    }
    store(W(mutex, M_OWNER), self);
    store(W(mutex, M_COUNT), 0);
    return 0;
}
int bionic_mutex_lock(const BionicFutex* f, void* mutex) { return lock(f, mutex, NULL, false); }
int bionic_mutex_trylock(const BionicFutex* f, void* mutex) { return lock(f, mutex, NULL, true); }
int bionic_mutex_timedlock(const BionicFutex* f, void* mutex, const BionicTime* deadline) {
    return lock(f, mutex, deadline, false);
}
int bionic_mutex_unlock(const BionicFutex* f, void* mutex) {
    int type = type_of(mutex);
    if (type != BIONIC_MUTEX_NORMAL) {
        if (load(W(mutex, M_OWNER)) != f->tid(f->context) || !(load(W(mutex, M_STATE)) & M_HELD)) return GNU_EPERM;
        if (type == BIONIC_MUTEX_RECURSIVE && load(W(mutex, M_COUNT))) {
            sub(W(mutex, M_COUNT), 1);
            return 0;
        }
    }
    store(W(mutex, M_OWNER), 0);
    give(f, W(mutex, M_STATE));
    return 0;
}

/* ---- condition variables ---- */
enum { C_SEQ = 0, C_FLAGS = 1, C_WAITERS = 2, C_WORDS = 12 };

int bionic_cond_init(void* cond, const void* attr) {
    zero(cond, C_WORDS);
    store(W(cond, C_FLAGS), attr_bits(attr) & 3);
    return 0;
}
int bionic_cond_destroy(void* cond) { return load(W(cond, C_WAITERS)) ? GNU_EBUSY : 0; }
static void notify(const BionicFutex* f, void* cond, int count) {
    add(W(cond, C_SEQ), 1); /* a waiter between its read of the sequence and its sleep sees it moved */
    if (load(W(cond, C_WAITERS))) f->wake(f->context, W(cond, C_SEQ), count);
}
int bionic_cond_signal(const BionicFutex* f, void* cond) {
    notify(f, cond, 1);
    return 0;
}
int bionic_cond_broadcast(const BionicFutex* f, void* cond) {
    notify(f, cond, 0x7fffffff);
    return 0;
}
int bionic_cond_wait(const BionicFutex* f, void* cond, void* mutex, const BionicTime* deadline) {
    if (!valid_time(deadline)) return GNU_EINVAL;
    int type = type_of(mutex);
    uint32_t self = f->tid(f->context);
    if (type != BIONIC_MUTEX_NORMAL && load(W(mutex, M_OWNER)) != self) return GNU_EPERM;
    BionicTime at;
    if (deadline) {
        at = *deadline;
        if (at.clock < 0) at.clock = (int)(load(W(cond, C_FLAGS)) & 1);
    }
    uint32_t seq = load(W(cond, C_SEQ));
    add(W(cond, C_WAITERS), 1);
    /* the mutex let go whole (a recursive one's count kept), and taken again after, as POSIX has it */
    uint32_t count = load(W(mutex, M_COUNT));
    store(W(mutex, M_OWNER), 0);
    give(f, W(mutex, M_STATE));
    int r = f->wait(f->context, W(cond, C_SEQ), seq, deadline ? &at : NULL);
    sub(W(cond, C_WAITERS), 1);
    (void)take(f, W(mutex, M_STATE), NULL);
    store(W(mutex, M_OWNER), self);
    store(W(mutex, M_COUNT), count);
    /* a signal's EINTR is a spurious wakeup, which POSIX allows */
    return r == -GNU_ETIMEDOUT ? GNU_ETIMEDOUT : 0;
}

/* ---- rwlocks ---- */
enum {
    R_STATE = 0, R_SEQ = 1, R_WAITERS = 2, R_WRITER = 3, R_FLAGS = 4, R_WRITERS_WAITING = 5, R_WORDS = 14,
    R_WRITE = 0x80000000u, R_READERS_MAX = 0x7fffffffu
};

int bionic_rwlock_init(void* rwlock, const void* attr) {
    zero(rwlock, R_WORDS);
    store(W(rwlock, R_FLAGS), attr_bits(attr) & 3);
    return 0;
}
int bionic_rwlock_destroy(void* rwlock) {
    return load(W(rwlock, R_STATE)) || load(W(rwlock, R_WAITERS)) ? GNU_EBUSY : 0;
}
/* a wait for the state to change: the sequence read before the state was */
static int rw_wait(const BionicFutex* f, void* rwlock, uint32_t seq, const BionicTime* deadline) {
    add(W(rwlock, R_WAITERS), 1);
    int r = f->wait(f->context, W(rwlock, R_SEQ), seq, deadline);
    sub(W(rwlock, R_WAITERS), 1);
    return r == -GNU_ETIMEDOUT ? GNU_ETIMEDOUT : 0;
}
static int rdlock(const BionicFutex* f, void* rwlock, const BionicTime* deadline, bool once) {
    uint32_t* w = W(rwlock, R_STATE);
    bool writers_first = load(W(rwlock, R_FLAGS)) & 2;
    for (;;) {
        uint32_t seq = load(W(rwlock, R_SEQ)), s = load(w);
        if (s & R_WRITE) {
            if (load(W(rwlock, R_WRITER)) == f->tid(f->context)) return GNU_EDEADLK;
        } else if (!(writers_first && load(W(rwlock, R_WRITERS_WAITING)))) {
            if (s == R_READERS_MAX) return GNU_EAGAIN;
            if (cas(w, &s, s + 1)) return 0;
            continue;
        }
        if (once) return GNU_EBUSY;
        if (!valid_time(deadline)) return GNU_EINVAL;
        int r = rw_wait(f, rwlock, seq, deadline);
        if (r) return r;
    }
}
static int wrlock(const BionicFutex* f, void* rwlock, const BionicTime* deadline, bool once) {
    uint32_t* w = W(rwlock, R_STATE);
    uint32_t self = f->tid(f->context);
    for (;;) {
        uint32_t seq = load(W(rwlock, R_SEQ)), s = 0;
        if (cas(w, &s, R_WRITE)) {
            store(W(rwlock, R_WRITER), self);
            return 0;
        }
        if ((s & R_WRITE) && load(W(rwlock, R_WRITER)) == self) return GNU_EDEADLK;
        if (once) return GNU_EBUSY;
        if (!valid_time(deadline)) return GNU_EINVAL;
        add(W(rwlock, R_WRITERS_WAITING), 1);
        int r = rw_wait(f, rwlock, seq, deadline);
        sub(W(rwlock, R_WRITERS_WAITING), 1);
        if (r) return r;
    }
}
int bionic_rwlock_rdlock(const BionicFutex* f, void* rwlock, const BionicTime* deadline) {
    return rdlock(f, rwlock, deadline, false);
}
int bionic_rwlock_tryrdlock(const BionicFutex* f, void* rwlock) { return rdlock(f, rwlock, NULL, true); }
int bionic_rwlock_wrlock(const BionicFutex* f, void* rwlock, const BionicTime* deadline) {
    return wrlock(f, rwlock, deadline, false);
}
int bionic_rwlock_trywrlock(const BionicFutex* f, void* rwlock) { return wrlock(f, rwlock, NULL, true); }
int bionic_rwlock_unlock(const BionicFutex* f, void* rwlock) {
    uint32_t* w = W(rwlock, R_STATE);
    uint32_t s = load(w);
    for (;;) {
        if (s & R_WRITE) {
            if (load(W(rwlock, R_WRITER)) != f->tid(f->context)) return GNU_EPERM;
            store(W(rwlock, R_WRITER), 0);
            store(w, 0);
            s = 0;
            break;
        }
        if (!s) return GNU_EPERM;
        if (cas(w, &s, s - 1)) {
            s -= 1;
            break;
        }
    }
    if (!s) {
        add(W(rwlock, R_SEQ), 1);
        if (load(W(rwlock, R_WAITERS))) f->wake(f->context, W(rwlock, R_SEQ), 0x7fffffff);
    }
    return 0;
}

/* ---- spinlocks: bionic's is a mutex's size of a long; this one spins on its first word ---- */
int bionic_spin_init(void* lock, int pshared) {
    if (pshared != BIONIC_PROCESS_PRIVATE && pshared != BIONIC_PROCESS_SHARED) return GNU_EINVAL;
    zero(lock, 2);
    return 0;
}
int bionic_spin_destroy(void* lock) { return load(W(lock, 0)) ? GNU_EBUSY : 0; }
int bionic_spin_lock(void* lock) {
    while (__atomic_exchange_n(W(lock, 0), 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(W(lock, 0), __ATOMIC_RELAXED)) {}
    return 0;
}
int bionic_spin_trylock(void* lock) { return __atomic_exchange_n(W(lock, 0), 1, __ATOMIC_ACQUIRE) ? GNU_EBUSY : 0; }
int bionic_spin_unlock(void* lock) {
    store(W(lock, 0), 0);
    return 0;
}

/* ---- barriers ---- */
enum { B_COUNT = 0, B_ARRIVED = 1, B_ROUND = 2, B_SHARED = 3, B_WORDS = 8 };

int bionic_barrier_init(void* barrier, const void* attr, unsigned count) {
    if (!count) return GNU_EINVAL;
    zero(barrier, B_WORDS);
    store(W(barrier, B_SHARED), attr_bits(attr) & 1);
    store(W(barrier, B_COUNT), count);
    return 0;
}
int bionic_barrier_destroy(void* barrier) { return load(W(barrier, B_ARRIVED)) ? GNU_EBUSY : 0; }
int bionic_barrier_wait(const BionicFutex* f, void* barrier) {
    uint32_t round = load(W(barrier, B_ROUND));
    uint32_t arrived = __atomic_add_fetch(W(barrier, B_ARRIVED), 1, __ATOMIC_ACQ_REL);
    if (arrived == load(W(barrier, B_COUNT))) {
        /* the last of the round: the next round starts, and the others go */
        store(W(barrier, B_ARRIVED), 0);
        add(W(barrier, B_ROUND), 1);
        f->wake(f->context, W(barrier, B_ROUND), 0x7fffffff);
        return BIONIC_BARRIER_SERIAL_THREAD;
    }
    while (load(W(barrier, B_ROUND)) == round) (void)f->wait(f->context, W(barrier, B_ROUND), round, NULL);
    return 0;
}

/* ---- semaphores ---- */
enum { S_VALUE = 0, S_WAITERS = 1, S_SHARED = 2, S_WORDS = 4 };

static int fail(int* error, int number) {
    *error = number;
    return -1;
}
int bionic_sem_init(void* sem, int pshared, unsigned value, int* error) {
    if (value > BIONIC_SEM_VALUE_MAX) return fail(error, GNU_EINVAL);
    zero(sem, S_WORDS);
    store(W(sem, S_SHARED), pshared != 0);
    store(W(sem, S_VALUE), value);
    return 0;
}
int bionic_sem_destroy(void* sem, int* error) {
    (void)error;
    (void)sem;
    return 0; /* POSIX: undefined with waiters; bionic gives 0 */
}
int bionic_sem_post(const BionicFutex* f, void* sem, int* error) {
    uint32_t* w = W(sem, S_VALUE);
    uint32_t v = load(w);
    do {
        if (v >= BIONIC_SEM_VALUE_MAX) return fail(error, GNU_EOVERFLOW);
    } while (!cas(w, &v, v + 1));
    if (load(W(sem, S_WAITERS))) f->wake(f->context, w, 1);
    return 0;
}
int bionic_sem_trywait(void* sem, int* error) {
    uint32_t* w = W(sem, S_VALUE);
    uint32_t v = load(w);
    while (v)
        if (cas(w, &v, v - 1)) return 0;
    return fail(error, GNU_EAGAIN);
}
int bionic_sem_wait(const BionicFutex* f, void* sem, const BionicTime* deadline, int* error) {
    uint32_t* w = W(sem, S_VALUE);
    for (;;) {
        uint32_t v = load(w);
        while (v)
            if (cas(w, &v, v - 1)) return 0;
        if (!valid_time(deadline)) return fail(error, GNU_EINVAL);
        add(W(sem, S_WAITERS), 1);
        int r = f->wait(f->context, w, 0, deadline);
        sub(W(sem, S_WAITERS), 1);
        if (r == -GNU_ETIMEDOUT) return fail(error, GNU_ETIMEDOUT);
        if (r == -GNU_EINTR) return fail(error, GNU_EINTR); /* sem_wait is the one that a handler ends */
    }
}
int bionic_sem_getvalue(void* sem, int* value, int* error) {
    (void)error;
    *value = (int)load(W(sem, S_VALUE));
    return 0;
}
