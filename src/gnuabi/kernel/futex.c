/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* futex(2) between the guest's threads (futex.h). A waiter reads the word and joins the list under the table's lock,
   so a waker that changed the word before it takes the lock finds it there, as Linux's hash bucket orders them. It
   then sleeps on its thread's condition, which a wake, a signal (gnu_thread_kick) or the timeout ends.
   Behavior from the Linux UAPI (include/uapi/linux/futex.h) and futex(2): the result of each operation, WAKE_OP's
   encoding, the requeue counts, and which operations take FUTEX_CLOCK_REALTIME */
#include "gnuabi/kernel/futex.h"
#include "gnuabi/kernel/signals.h"
#include "common/utilities/endian.h"

enum {
    FUTEX_WAIT = 0,
    FUTEX_WAKE = 1,
    FUTEX_REQUEUE = 3,
    FUTEX_CMP_REQUEUE = 4,
    FUTEX_WAKE_OP = 5,
    FUTEX_WAIT_BITSET = 9,
    FUTEX_WAKE_BITSET = 10,
    FUTEX_PRIVATE_FLAG = 128,
    FUTEX_CLOCK_REALTIME = 256,
};

void gnu_futex_init(GnuFutexTable* f) {
    gnu_mutex_init(&f->lock, 0);
    f->head = f->tail = NULL;
}
void gnu_futex_forked(GnuFutexTable* f) { f->head = f->tail = NULL; }

static void link_waiter(GnuFutexTable* f, GnuFutexWaiter* w) {
    w->next = NULL;
    w->prev = f->tail;
    if (f->tail)
        f->tail->next = w;
    else
        f->head = w;
    f->tail = w;
}
static void unlink_waiter(GnuFutexTable* f, GnuFutexWaiter* w) {
    if (w->prev)
        w->prev->next = w->next;
    else
        f->head = w->next;
    if (w->next)
        w->next->prev = w->prev;
    else
        f->tail = w->prev;
    w->prev = w->next = NULL;
}
/* w out of the list and its thread awake, under the table's lock */
static void wake_waiter(GnuFutexTable* f, GnuFutexWaiter* w) {
    unlink_waiter(f, w);
    GnuKernelThread* t = w->thread;
    gnu_mutex_lock(&t->wait_lock);
    __atomic_store_n(&w->woken, 1, __ATOMIC_SEQ_CST);
    gnu_cond_broadcast(&t->wake);
    gnu_mutex_unlock(&t->wait_lock);
}
/* up to count waiters on address whose bitset meets bitset; a count of 0 or less still wakes one, as Linux's loop
   does */
static int wake_locked(GnuFutexTable* f, uint64_t address, uint32_t bitset, int count) {
    int woken = 0;
    for (GnuFutexWaiter *w = f->head, *next; w; w = next) {
        next = w->next;
        if (w->address != address || !(w->bitset & bitset)) continue;
        wake_waiter(f, w);
        if (++woken >= count) break;
    }
    return woken;
}

int gnu_futex_wake(GnuKernel* p, uint64_t address, int count) {
    gnu_mutex_lock(&p->futex.lock);
    int woken = wake_locked(&p->futex, address, ~0u, count);
    gnu_mutex_unlock(&p->futex.lock);
    return woken;
}

/* the word at a guest address for an atomic access, NULL when it isn't readable (and writable, for write) */
static uint32_t* word_at(GnuKernel* p, uint64_t address, int write) {
    uint32_t probe;
    if (address & 3) return NULL;
    int bad = p->ops->check ? p->ops->check(p->context, address, 4, write) : gnu_kernel_read(p, address, &probe, 4);
    return bad ? NULL : (uint32_t*)(uintptr_t)((uintptr_t)p->base + address);
}

/* a timespec {s64, s64} in guest memory, as nanoseconds */
static int64_t read_timespec(GnuKernel* p, uint64_t address, int64_t* ns) {
    int64_t t[2];
    if (gnu_kernel_read(p, address, t, sizeof t)) return -GNU_EFAULT;
    int64_t seconds = (int64_t)snx_le64((uint64_t)t[0]), nanos = (int64_t)snx_le64((uint64_t)t[1]);
    if (seconds < 0 || nanos < 0 || nanos >= 1000000000) return -GNU_EINVAL;
    *ns = seconds > INT64_MAX / 1000000000 - 1 ? INT64_MAX : seconds * 1000000000 + nanos;
    return 0;
}

static int64_t futex_wait(GnuKernelThread* t, const uint64_t a[6], unsigned operation, uint32_t bitset) {
    GnuKernel* p = t->kernel;
    int64_t deadline = -1;
    if (!bitset) return -GNU_EINVAL;
    if (a[3]) {
        int64_t ns;
        int64_t e = read_timespec(p, a[3], &ns);
        if (e) return e;
        int64_t now = gnu_monotonic_ns();
        if (operation == FUTEX_WAIT)
            deadline = ns > INT64_MAX - now ? INT64_MAX : now + ns; /* relative */
        else if (a[1] & FUTEX_CLOCK_REALTIME) {
            /* an absolute time on the realtime clock, waited for on the monotonic one: a change of the realtime
               clock during the wait doesn't move it */
            int64_t left = ns - gnu_realtime_ns();
            deadline = left > INT64_MAX - now ? INT64_MAX : now + left;
        } else
            deadline = ns; /* CLOCK_MONOTONIC's absolute time is the host's, gnu_monotonic_ns's */
    }
    uint32_t* word = word_at(p, a[0], 0);
    if (!word) return a[0] & 3 ? -GNU_EINVAL : -GNU_EFAULT;
    GnuFutexWaiter w = {NULL, NULL, a[0], bitset, 0, t};
    gnu_mutex_lock(&p->futex.lock);
    if (snx_le32(__atomic_load_n(word, __ATOMIC_SEQ_CST)) != (uint32_t)a[2]) {
        gnu_mutex_unlock(&p->futex.lock);
        return -GNU_EAGAIN;
    }
    link_waiter(&p->futex, &w);
    gnu_mutex_unlock(&p->futex.lock);
    int64_t result = 0;
    gnu_mutex_lock(&t->wait_lock);
    for (;;) {
        if (__atomic_load_n(&w.woken, __ATOMIC_SEQ_CST)) break;
        if (__atomic_load_n(&t->interrupt, __ATOMIC_SEQ_CST)) {
            gnu_mutex_unlock(&t->wait_lock);
            int wanted = gnu_signal_wanted(t);
            gnu_mutex_lock(&t->wait_lock);
            /* Linux: ERESTARTSYS without a timeout, a restart with the time that is left with one, and EINTR when a
               handler runs */
            if (wanted) {
                result = a[3] ? -GNU_ERESTART_RESTARTBLOCK : -GNU_ERESTARTSYS;
                break;
            }
            continue; /* another thread took the signal: wait on */
        }
        if (gnu_cond_wait(&t->wake, &t->wait_lock, deadline)) {
            result = -GNU_ETIMEDOUT;
            break;
        }
    }
    gnu_mutex_unlock(&t->wait_lock);
    gnu_mutex_lock(&p->futex.lock);
    if (__atomic_load_n(&w.woken, __ATOMIC_SEQ_CST))
        result = 0; /* a wake that came with the timeout or the signal wins, as Linux reports it */
    else
        unlink_waiter(&p->futex, &w);
    gnu_mutex_unlock(&p->futex.lock);
    return result;
}

/* FUTEX_WAKE_OP's operation on oldval, and its comparison (FUTEX_OP_* in the UAPI) */
static int wake_op(GnuKernel* p, uint64_t address, uint32_t encoded, int* compared) {
    uint32_t* word = word_at(p, address, 1);
    if (!word) return address & 3 ? -GNU_EINVAL : -GNU_EFAULT;
    unsigned op = (encoded >> 28) & 7, cmp = (encoded >> 24) & 15;
    int32_t oparg = (int32_t)((encoded >> 12) & 0xfff), cmparg = (int32_t)(encoded & 0xfff);
    if (oparg & 0x800) oparg -= 0x1000; /* 12-bit signed fields */
    if (cmparg & 0x800) cmparg -= 0x1000;
    if (encoded & 0x80000000u) oparg = (int32_t)(1u << (oparg & 31)); /* FUTEX_OP_OPARG_SHIFT */
    if (op > 4 || cmp > 5) return -GNU_ENOSYS;
    uint32_t raw = __atomic_load_n(word, __ATOMIC_SEQ_CST), old, next;
    do {
        old = snx_le32(raw);
        switch (op) {
            case 0: next = (uint32_t)oparg; break;
            case 1: next = old + (uint32_t)oparg; break;
            case 2: next = old | (uint32_t)oparg; break;
            case 3: next = old & ~(uint32_t)oparg; break;
            default: next = old ^ (uint32_t)oparg; break;
        }
    } while (!__atomic_compare_exchange_n(word, &raw, snx_le32(next), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    int32_t value = (int32_t)old;
    switch (cmp) {
        case 0: *compared = value == cmparg; break;
        case 1: *compared = value != cmparg; break;
        case 2: *compared = value < cmparg; break;
        case 3: *compared = value <= cmparg; break;
        case 4: *compared = value > cmparg; break;
        default: *compared = value >= cmparg; break;
    }
    return 0;
}

int64_t gnu_futex(GnuKernelThread* t, const uint64_t a[6]) {
    GnuKernel* p = t->kernel;
    unsigned operation = (unsigned)a[1] & ~(unsigned)(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    /* Linux takes FUTEX_CLOCK_REALTIME with the waits whose time is absolute (and FUTEX_WAIT, where it does nothing) */
    if ((a[1] & FUTEX_CLOCK_REALTIME) && operation != FUTEX_WAIT && operation != FUTEX_WAIT_BITSET)
        return -GNU_ENOSYS;
    int count = (int)(uint32_t)a[2];
    switch (operation) {
        case FUTEX_WAIT: return futex_wait(t, a, operation, ~0u);
        case FUTEX_WAIT_BITSET: return futex_wait(t, a, operation, (uint32_t)a[5]);
        case FUTEX_WAKE:
        case FUTEX_WAKE_BITSET: {
            uint32_t bitset = operation == FUTEX_WAKE ? ~0u : (uint32_t)a[5];
            if (!bitset) return -GNU_EINVAL;
            if (a[0] & 3) return -GNU_EINVAL;
            gnu_mutex_lock(&p->futex.lock);
            int woken = wake_locked(&p->futex, a[0], bitset, count);
            gnu_mutex_unlock(&p->futex.lock);
            return woken;
        }
        case FUTEX_REQUEUE:
        case FUTEX_CMP_REQUEUE: {
            int requeue = (int)(uint32_t)a[3]; /* the timeout argument carries val2 */
            if (count < 0 || requeue < 0) return -GNU_EINVAL;
            if ((a[0] | a[4]) & 3) return -GNU_EINVAL;
            uint32_t* word = word_at(p, a[0], 0);
            if (!word) return -GNU_EFAULT;
            gnu_mutex_lock(&p->futex.lock);
            if (operation == FUTEX_CMP_REQUEUE && snx_le32(__atomic_load_n(word, __ATOMIC_SEQ_CST)) != (uint32_t)a[5]) {
                gnu_mutex_unlock(&p->futex.lock);
                return -GNU_EAGAIN;
            }
            /* Linux's futex_requeue counts the waiters it woke and those it moved, for both operations */
            int done = 0, moved = 0;
            for (GnuFutexWaiter *w = p->futex.head, *next; w; w = next) {
                next = w->next;
                if (w->address != a[0]) continue;
                if (done < count) {
                    wake_waiter(&p->futex, w);
                    done++;
                } else if (moved < requeue) {
                    w->address = a[4];
                    moved++;
                } else
                    break;
            }
            gnu_mutex_unlock(&p->futex.lock);
            return done + moved;
        }
        case FUTEX_WAKE_OP: {
            int compared = 0, count2 = (int)(uint32_t)a[3];
            if (a[0] & 3) return -GNU_EINVAL;
            gnu_mutex_lock(&p->futex.lock);
            int e = wake_op(p, a[4], (uint32_t)a[5], &compared);
            if (e) {
                gnu_mutex_unlock(&p->futex.lock);
                return e;
            }
            int woken = wake_locked(&p->futex, a[0], ~0u, count);
            if (compared) woken += wake_locked(&p->futex, a[4], ~0u, count2);
            gnu_mutex_unlock(&p->futex.lock);
            return woken;
        }
    }
    return -GNU_ENOSYS;
}
