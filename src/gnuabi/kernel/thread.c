/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The kernel's process and threads (kernel.h): the list, the exit of a thread, and the stop of the other threads when
   one ends the process or execs */
#include "gnuabi/kernel/kernel.h"
#include "gnuabi/kernel/futex.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static _Thread_local struct {
    GnuKernelThread* self;
} t_kernel;

GnuKernelThread* gnu_kernel_self(void) { return t_kernel.self; }
void gnu_kernel_set_self(GnuKernelThread* t) { t_kernel.self = t; }

void gnu_kernel_init(GnuKernel* k, const GnuKernelOps* ops, void* context, unsigned char* base, uint32_t pid) {
    *k = (GnuKernel){0};
    k->ops = ops;
    k->context = context;
    k->base = base;
    k->pid = pid;
    atomic_store_explicit(&k->dumpable, 1, memory_order_relaxed);
    gnu_mutex_init(&k->lock, 1); /* a sender of a signal holds it around gnu_signal_send */
    gnu_cond_init(&k->changed);
    gnu_futex_init(&k->futex);
}
void gnu_kernel_destroy(GnuKernel* k) {
    free(k->threads);
    gnu_mutex_destroy(&k->futex.lock);
    gnu_cond_destroy(&k->changed);
    gnu_mutex_destroy(&k->lock);
    k->threads = NULL;
    k->thread_count = k->thread_capacity = 0;
}

int gnu_kernel_read(GnuKernel* k, uint64_t address, void* out, size_t size) {
    return k->ops->read(k->context, address, out, size) ? -GNU_EFAULT : 0;
}
int gnu_kernel_write(GnuKernel* k, uint64_t address, const void* in, size_t size) {
    return k->ops->write(k->context, address, in, size) ? -GNU_EFAULT : 0;
}

int gnu_kernel_thread_add(GnuKernel* k, GnuKernelThread* t, uint32_t tid, CPU* regs, void* runtime) {
    *t = (GnuKernelThread){0};
    t->kernel = k;
    t->tid = tid;
    t->regs = regs;
    t->runtime = runtime;
    t->altstack.flags = SNX_SS_DISABLE;
    t->timer_slack = 50000; /* Linux's default */
    gnu_kernel_host_thread_name(t->name);
    gnu_mutex_init(&t->wait_lock, 0);
    gnu_cond_init(&t->wake);
    gnu_mutex_lock(&k->lock);
    if (k->thread_count == k->thread_capacity) {
        size_t capacity = k->thread_capacity ? k->thread_capacity * 2 : 8;
        GnuKernelThread** threads = realloc(k->threads, capacity * sizeof *threads);
        if (!threads) {
            gnu_mutex_unlock(&k->lock);
            gnu_kernel_thread_destroy(t);
            return -GNU_ENOMEM;
        }
        k->threads = threads;
        k->thread_capacity = capacity;
    }
    k->threads[k->thread_count++] = t;
    gnu_mutex_unlock(&k->lock);
    return 0;
}

int gnu_kernel_thread_remove(GnuKernelThread* t) {
    GnuKernel* k = t->kernel;
    int found = 0;
    gnu_mutex_lock(&k->lock);
    for (size_t i = 0; i < k->thread_count && !found; i++)
        if (k->threads[i] == t) {
            k->threads[i] = k->threads[--k->thread_count];
            gnu_cond_broadcast(&k->changed);
            found = 1;
        }
    gnu_mutex_unlock(&k->lock);
    return found;
}
void gnu_kernel_thread_destroy(GnuKernelThread* t) {
    gnu_mutex_destroy(&t->wait_lock);
    gnu_cond_destroy(&t->wake);
}

/* the robust futexes the thread holds at its exit get FUTEX_OWNER_DIED, and a waiter of each wakes (Linux's
   exit_robust_list): the head is {next, futex_offset, list_op_pending}, and each entry's first word is the next */
static void robust_one(GnuKernelThread* t, uint64_t entry, int64_t offset) {
    GnuKernel* k = t->kernel;
    uint64_t at = (entry & ~1ull) + (uint64_t)offset;
    uint32_t probe;
    if (!(entry & ~1ull) || (at & 3)) return;
    int bad = k->ops->check ? k->ops->check(k->context, at, 4, 1) : gnu_kernel_read(k, at, &probe, 4);
    if (bad) return;
    _Atomic uint32_t* word = (_Atomic uint32_t*)(uintptr_t)((uintptr_t)k->base + at);
    uint32_t raw = atomic_load_explicit(word, memory_order_seq_cst);
    while ((snx_le32(raw) & 0x3fffffffu) == t->tid) {
        uint32_t old = snx_le32(raw), next = (old & 0x80000000u) | 0x40000000u; /* FUTEX_WAITERS kept, OWNER_DIED */
        if (atomic_compare_exchange_strong_explicit(word, &raw, snx_le32(next), memory_order_seq_cst,
                                                    memory_order_seq_cst)) {
            if (old & 0x80000000u) gnu_futex_wake(k, at, 1);
            return;
        }
    }
}
static void robust_exit(GnuKernelThread* t) {
    GnuKernel* k = t->kernel;
    uint64_t head[3];
    if (!t->robust_list || gnu_kernel_read(k, t->robust_list, head, sizeof head)) return;
    uint64_t entry = snx_le64(head[0]), pending = snx_le64(head[2]);
    int64_t offset = (int64_t)snx_le64(head[1]);
    for (int limit = 0; entry != t->robust_list && limit < 2048; limit++) {
        uint64_t next;
        if (gnu_kernel_read(k, entry & ~1ull, &next, 8)) break;
        if (entry != pending) robust_one(t, entry, offset);
        entry = snx_le64(next);
    }
    if (pending) robust_one(t, pending, offset);
}

void gnu_kernel_thread_exit(GnuKernelThread* t, int status) {
    GnuKernel* k = t->kernel;
    robust_exit(t);
    if (t->clear_tid) {
        uint32_t zero = 0;
        if (!gnu_kernel_write(k, t->clear_tid, &zero, 4)) gnu_futex_wake(k, t->clear_tid, 1);
    }
    gnu_mutex_lock(&k->lock);
    /* the decision and the removal at once: two last threads that exit together end the process once */
    int last = k->thread_count == 1 && !atomic_load_explicit(&k->stopping, memory_order_seq_cst);
    if (!last) gnu_kernel_thread_remove(t);
    gnu_mutex_unlock(&k->lock);
    if (last)
        k->ops->end(k->context, t, status, 0, NULL); /* the last thread's exit is the process's */
    else
        k->ops->leave(k->context, t);
}

void gnu_kernel_kick(GnuKernelThread* t) {
    atomic_store_explicit(&t->interrupt, 1, memory_order_seq_cst);
    if (t->regs) atomic_store_explicit(&t->regs->poll, 1, memory_order_relaxed);
    gnu_mutex_lock(&t->wait_lock);
    gnu_cond_broadcast(&t->wake);
    gnu_mutex_unlock(&t->wait_lock);
    GnuKernel* k = t->kernel;
    if (k->ops->kick && atomic_load_explicit(&t->blocking, memory_order_seq_cst)) k->ops->kick(k->context, t);
}

int gnu_kernel_claim_stop(GnuKernelThread* t, uint32_t kind) {
    GnuKernel* k = t->kernel;
    gnu_mutex_lock(&k->lock);
    int other = atomic_load_explicit(&k->stopping, memory_order_seq_cst) && k->stopper != t;
    if (!other) {
        k->stopper = t;
        atomic_store_explicit(&k->stopping, kind, memory_order_seq_cst);
    }
    gnu_mutex_unlock(&k->lock);
    return !other;
}

void gnu_kernel_stop_others(GnuKernelThread* t, int all) {
    GnuKernel* k = t->kernel;
    gnu_mutex_lock(&k->lock);
    for (size_t i = 0; i < k->thread_count; i++)
        if (k->threads[i] != t) gnu_kernel_kick(k->threads[i]);
    for (;;) {
        int waiting = 0;
        for (size_t i = 0; i < k->thread_count && !waiting; i++) {
            GnuKernelThread* o = k->threads[i];
            waiting = o != t && (all || !atomic_load_explicit(&o->blocking, memory_order_seq_cst));
        }
        if (!waiting) break;
        gnu_cond_wait(&k->changed, &k->lock, -1);
    }
    gnu_mutex_unlock(&k->lock);
}

void gnu_kernel_stop_done(GnuKernelThread* t) {
    GnuKernel* k = t->kernel;
    gnu_mutex_lock(&k->lock);
    atomic_store_explicit(&k->stopping, 0, memory_order_seq_cst);
    k->stopper = NULL;
    gnu_mutex_unlock(&k->lock);
}

void gnu_kernel_check_stop(GnuKernelThread* t) {
    GnuKernel* k = t->kernel;
    if (!atomic_load_explicit(&k->stopping, memory_order_seq_cst)) return;
    gnu_mutex_lock(&k->lock);
    int other = k->stopper != t;
    gnu_mutex_unlock(&k->lock);
    if (other) k->ops->leave(k->context, t);
}

void gnu_kernel_enter_call(GnuKernelThread* t) {
    atomic_store_explicit(&t->blocking, 1, memory_order_seq_cst);
    gnu_kernel_check_stop(t); /* seq_cst both ways with gnu_kernel_stop_others: one of them sees the other */
}
void gnu_kernel_leave_call(GnuKernelThread* t) {
    atomic_store_explicit(&t->blocking, 0, memory_order_seq_cst);
    gnu_kernel_check_stop(t);
}

void gnu_kernel_fork_prepare(GnuKernel* k) {
    gnu_mutex_lock(&k->futex.lock);
    gnu_mutex_lock(&k->lock);
}
void gnu_kernel_fork_parent(GnuKernel* k) {
    gnu_mutex_unlock(&k->lock);
    gnu_mutex_unlock(&k->futex.lock);
}
void gnu_kernel_fork_child(GnuKernelThread* t, uint32_t pid) {
    GnuKernel* k = t->kernel;
    /* the other threads' host threads are not in the child: the runtime has freed their records */
    k->threads[0] = t;
    k->thread_count = 1;
    atomic_store_explicit(&k->stopping, 0, memory_order_seq_cst);
    k->stopper = NULL;
    /* a child starts with no pending signals */
    atomic_store_explicit(&k->queue.pending, k->queue.count = 0, memory_order_relaxed);
    atomic_store_explicit(&t->queue.pending, t->queue.count = 0, memory_order_relaxed);
    atomic_store_explicit(&t->interrupt, 0, memory_order_relaxed);
    k->pid = t->tid = pid;
    gnu_futex_forked(&k->futex);
    /* the locks again, not unlocked: a recursive one knows its owner by the thread's id, which the child changed */
    gnu_cond_init(&k->changed);
    gnu_cond_init(&t->wake);
    gnu_mutex_init(&t->wait_lock, 0);
    gnu_mutex_init(&k->lock, 1);
    gnu_mutex_init(&k->futex.lock, 0);
}
