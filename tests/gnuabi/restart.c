/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* System calls that a signal stops: a read of a pipe restarts after a handler with SA_RESTART and fails with EINTR
   after one without it, and nanosleep fails with EINTR and the time left even with SA_RESTART, as Linux's restart
   rules have them. Exits 0 */
#include "threading.h"

static u8 stack[65536] __attribute__((aligned(16)));
static volatile u32 handled, reader_tid, phase, ctid;
static volatile s64 results[2];
static s32 fds[2];

static void handler(int sig) {
    (void)sig;
    handled = handled + 1;
}
/* until the handler has run n times in all, 30 s at most: it runs once the signal reached the thread and its code is
   compiled, which a loaded machine delays by more than any fixed wait */
static void await_handled(u32 n) {
    for (int ms = 0; handled < n && ms < 30000; ms++) sleep_ns(1000000);
}
static volatile u32 sleeper_tid;
static int alarm(u64 unused) {
    (void)unused;
    sleep_ns(200000000);
    tgkill(sleeper_tid, SIGUSR1);
    return 0;
}
static int reader(u64 unused) {
    (void)unused;
    char byte;
    reader_tid = (u32)gettid();
    for (int i = 0; i < 2; i++) {
        raise_flag(&phase); /* about to read */
        results[i] = call(63, (u64)fds[0], (u64)&byte, 1);
        while (load(&phase)) sleep_ns(1000000);
    }
    return 0;
}

int guest_main(u64* sp) {
    (void)sp;
    CHECK(call(59, (u64)fds, 0, 0) == 0);
    CHECK(sigaction(SIGUSR1, (void*)handler, SA_RESTART, 0, 0) == 0);
    CHECK(sigaction(SIGUSR2, (void*)handler, 0, 0, 0) == 0);
    ctid = 1;
    CHECK(spawn_thread(THREAD_FLAGS, stack + sizeof stack, 0, 0, (u32*)&ctid, reader, 0) > 0);

    /* SA_RESTART: the read goes on after the handler, and returns the byte written after it */
    await(&phase);
    sleep_ns(100000000);
    CHECK(tgkill(reader_tid, SIGUSR1) == 0);
    await_handled(1);
    CHECK(handled == 1);
    CHECK(call(64, (u64)fds[1], (u64)"x", 1) == 1);
    while (load(&phase) && results[0] == 0) sleep_ns(1000000);
    CHECK(results[0] == 1);
    put("restart: SA_RESTART ok\n");

    /* without it: EINTR */
    store(&phase, 0);
    await(&phase);
    sleep_ns(100000000);
    CHECK(tgkill(reader_tid, SIGUSR2) == 0);
    while (results[1] == 0) sleep_ns(1000000);
    CHECK(results[1] == -EINTR && handled == 2);
    store(&phase, 0);
    join(&ctid);
    put("restart: EINTR ok\n");

    /* nanosleep never restarts: EINTR and the time left. A thread stops the main thread's 30 s sleep, longer than that
       thread takes to start on a loaded machine */
    s64 pid = getpid();
    CHECK(call(129, (u64)pid, SIGUSR1, 0) == 0 && handled == 3); /* a signal to the process runs at once here */
    sleeper_tid = (u32)gettid();
    ctid = 1;
    CHECK(spawn_thread(THREAD_FLAGS, stack + sizeof stack, 0, 0, (u32*)&ctid, alarm, 0) > 0);
    s64 request[2] = {30, 0}, left[2] = {0, 0};
    CHECK(call(101, (u64)request, (u64)left, 0) == -EINTR && handled == 4);
    CHECK(left[0] >= 0 && left[0] < 30 && (left[0] > 0 || left[1] > 0));
    join(&ctid);
    put("restart: nanosleep ok\n");
    return 0;
}
