/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* A signal that ends a wait of the kernel (rt_sigsuspend) leaves nothing behind for the network stack's waits: a
   ppoll of an empty eventfd afterwards waits out its 100 ms and returns 0. A watchdog thread ends the process with 1
   when the ppoll has not returned 10 s later. Exits 0 */
#include "threading.h"

static u8 stack[65536] __attribute__((aligned(16)));
static u8 watchdog_stack[65536] __attribute__((aligned(16)));
static volatile u32 handled, main_tid, ready, ctid, watchdog_ctid, done;
struct pollfd {
    s32 fd;
    short events, revents;
};

static void handler(int sig) {
    (void)sig;
    handled = handled + 1;
}
static int sender(u64 sig) {
    await(&ready);
    sleep_ns(50000000);
    tgkill(main_tid, (int)sig);
    return 0;
}
static int watchdog(u64 unused) {
    (void)unused;
    sleep_ns(10000000000);
    if (!load(&done)) {
        put("wake: ppoll did not return\n");
        call(94, 1, 0, 0);
    }
    return 0;
}

int guest_main(u64* sp) {
    (void)sp;
    main_tid = (u32)gettid();
    s64 eventfd = call(19, 0, 0, 0); /* eventfd2(0, 0) */
    CHECK(eventfd >= 0);
    CHECK(sigaction(SIGUSR1, (void*)handler, 0, 0, 0) == 0);
    CHECK(sigprocmask(0, BIT(SIGUSR1), 0) == 0);
    ctid = 1;
    CHECK(spawn_thread(THREAD_FLAGS, stack + sizeof stack, 0, 0, (u32*)&ctid, sender, SIGUSR1) > 0);
    raise_flag(&ready);
    u64 none = 0;
    CHECK(syscall6(133, (u64)&none, 8, 0, 0, 0, 0) == -EINTR && handled == 1);
    join(&ctid);

    watchdog_ctid = 1;
    CHECK(spawn_thread(THREAD_FLAGS, watchdog_stack + sizeof watchdog_stack, 0, 0, (u32*)&watchdog_ctid, watchdog, 0) >
          0);
    struct pollfd p = {(s32)eventfd, 1 /* POLLIN */, 0};
    s64 brief[2] = {0, 100000000};
    CHECK(syscall6(73, (u64)&p, 1, (u64)brief, 0, 8, 0) == 0 && p.revents == 0);
    store(&done, 1);
    put("wake: ppoll waits after a signal\n");
    call(94, 0, 0, 0); /* exit_group: start.S's exit would wait for the watchdog's thread */
    return 0;
}
