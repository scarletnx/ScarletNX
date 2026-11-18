/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* Guest threads without a libc: clone(CLONE_THREAD) with the TLS and tid words, a futex lock that four threads
   contend for, CLONE_CHILD_CLEARTID's wake at a thread's exit, the futex operations (WAIT, WAKE, WAIT_BITSET,
   CMP_REQUEUE, WAKE_OP, timeouts), and exit_group from a thread while the main thread sleeps. Exits 0 */
#include "threading.h"
#include <stdatomic.h>

alignas(16) static u8 stacks[6][65536];
static volatile u32 lock_word, counter, ctid[6], ptid[6], tls_seen[6], requeue_word, requeue_target;
static volatile _Atomic u32 woken;
static volatile u32 tids[6];

static void lock(void) {
    /* 0 free, 1 taken, 2 taken with waiters (Drepper's mutex) */
    volatile _Atomic u32* w = (volatile _Atomic u32*)&lock_word; /* the futex's word: plain, as threading.h's load */
    u32 c = 0;
    if (atomic_compare_exchange_strong_explicit(w, &c, 1, memory_order_acquire, memory_order_relaxed)) return;
    if (c != 2) c = atomic_exchange_explicit(w, 2, memory_order_acquire);
    while (c) {
        futex(&lock_word, 128 /* WAIT | PRIVATE */, 2, 0, 0, 0);
        c = atomic_exchange_explicit(w, 2, memory_order_acquire);
    }
}
static void unlock(void) {
    if (atomic_fetch_sub_explicit((volatile _Atomic u32*)&lock_word, 1, memory_order_release) != 1) {
        store(&lock_word, 0);
        futex(&lock_word, 129 /* WAKE | PRIVATE */, 1, 0, 0, 0);
    }
}

static int worker(u64 index) {
    u64 tls;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
    tls_seen[index] = (u32)tls;
    tids[index] = (u32)gettid();
    for (int i = 0; i < 2000; i++) {
        lock();
        counter = counter + 1;
        unlock();
    }
    return 0;
}

static int requeued(u64 index) {
    (void)index;
    while (load(&requeue_word) == 0) futex(&requeue_word, 0, 0, 0, 0, 0);
    atomic_fetch_add_explicit(&woken, 1, memory_order_seq_cst);
    return 0;
}

static int ender(u64 index) {
    (void)index;
    sleep_ns(20000000);
    call(94, 0, 0, 0); /* exit_group(0): the main thread's sleep ends with the process */
    return 9;
}

static long start(int i, int (*fn)(u64)) {
    ctid[i] = 0xffffffff;
    return spawn_thread(THREAD_FLAGS, stacks[i] + sizeof stacks[i], (u32*)&ptid[i], 0x7000 + (u64)i, (u32*)&ctid[i],
                        fn, (u64)i);
}

int guest_main(u64* stack) {
    (void)stack;
    s64 pid = getpid();
    CHECK(gettid() == pid);
    /* four threads on one lock */
    for (int i = 0; i < 4; i++) {
        long tid = start(i, worker);
        CHECK(tid > 0 && tid != pid && (u32)tid == ptid[i]);
    }
    for (int i = 0; i < 4; i++) {
        join(&ctid[i]);
        CHECK(tls_seen[i] == 0x7000u + (u32)i && tids[i] == ptid[i]);
    }
    CHECK(counter == 8000);
    put("threads: lock ok\n");

    /* the futex operations' results */
    volatile u32 word = 5, word2 = 5;
    CHECK(futex(&word, 0, 4, 0, 0, 0) == -EAGAIN);
    s64 short_wait[2] = {0, 10000000};
    CHECK(futex(&word, 0, 5, (u64)short_wait, 0, 0) == -ETIMEDOUT);
    s64 past[2] = {0, 0}; /* CLOCK_MONOTONIC's zero: long gone */
    CHECK(futex(&word, 9, 5, (u64)past, 0, 0xffffffff) == -ETIMEDOUT);
    CHECK(futex(&word, 9, 5, 0, 0, 0) == -22); /* a bitset of 0 */
    CHECK(futex(&word, 1 | 256, 1, 0, 0, 0) == -38); /* CLOCK_REALTIME with a wake */
    CHECK(futex(&word, 1, 1, 0, 0, 0) == 0);
    /* WAKE_OP: word2 += 1, and a wake on word2 when it was 5 */
    u32 op = 1u << 28 | 0u << 24 | 1u << 12 | 5u;
    CHECK(futex(&word, 5, 1, 1, &word2, op) == 0 && word2 == 6);
    put("threads: futex results ok\n");

    /* two waiters moved from one word to another, and woken there */
    start(4, requeued);
    start(5, requeued);
    s64 moved = 0;
    while (moved < 2) {
        s64 r = futex(&requeue_word, 4 /* CMP_REQUEUE */, 0, 0x7fffffff, &requeue_target, 0);
        CHECK(r >= 0);
        moved += r;
    }
    store(&requeue_word, 1);
    s64 wakes = 0;
    while (wakes < 2) wakes += futex(&requeue_target, 1, 2, 0, 0, 0);
    join(&ctid[4]);
    join(&ctid[5]);
    CHECK(woken == 2);
    put("threads: requeue ok\n");

    /* exit_group from another thread ends the main one's sleep */
    start(0, ender);
    sleep_ns(5000000000);
    return 3;
}
