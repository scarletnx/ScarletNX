/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* rt_sigsuspend and rt_sigtimedwait: a blocked signal that another thread sends ends sigsuspend through its handler,
   with EINTR and the old mask back; sigtimedwait takes a blocked signal without its handler, with its siginfo, and
   times out with EAGAIN. Exits 0 */
#include "threading.h"

static u8 stack[65536] __attribute__((aligned(16)));
static volatile u32 handled, main_tid, ready, ctid;

static void handler(int sig) {
    (void)sig;
    handled = handled + 1;
}
static int sender(u64 sig) {
    await(&ready);
    sleep_ns(50000000);
    if (sig == SIGUSR1)
        tgkill(main_tid, (int)sig);
    else
        call(129, (u64)getpid(), sig, 0); /* to the process: the main thread is the one that waits for it */
    return 0;
}

int guest_main(u64* sp) {
    (void)sp;
    main_tid = (u32)gettid();
    CHECK(sigaction(SIGUSR1, (void*)handler, 0, 0, 0) == 0);
    CHECK(sigaction(SIGUSR2, (void*)handler, 0, 0, 0) == 0);
    CHECK(sigprocmask(0, BIT(SIGUSR1) | BIT(SIGUSR2), 0) == 0);

    ctid = 1;
    CHECK(spawn_thread(THREAD_FLAGS, stack + sizeof stack, 0, 0, (u32*)&ctid, sender, SIGUSR1) > 0);
    raise_flag(&ready);
    u64 none = 0, now = 0;
    CHECK(syscall6(133, (u64)&none, 8, 0, 0, 0, 0) == -EINTR && handled == 1);
    CHECK(sigprocmask(0, 0, &now) == 0 && now == (BIT(SIGUSR1) | BIT(SIGUSR2)));
    join(&ctid);
    put("sigsuspend: ok\n");

    store(&ready, 0);
    ctid = 1;
    CHECK(spawn_thread(THREAD_FLAGS, stack + sizeof stack, 0, 0, (u32*)&ctid, sender, SIGUSR2) > 0);
    raise_flag(&ready);
    u64 set = BIT(SIGUSR2);
    u8 info[128] = {0};
    CHECK(syscall6(137, (u64)&set, (u64)info, 0, 8, 0, 0) == SIGUSR2);
    CHECK(*(u32*)info == SIGUSR2 && *(s32*)(info + 8) == 0 && *(u32*)(info + 16) == (u32)getpid() && handled == 1);
    join(&ctid);
    s64 brief[2] = {0, 20000000};
    CHECK(syscall6(137, (u64)&set, 0, (u64)brief, 8, 0, 0) == -EAGAIN);
    put("sigtimedwait: ok\n");
    return 0;
}
