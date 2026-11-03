/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* Signal handlers without a libc: kill to the process, the siginfo, a mask that holds a signal back until
   sigprocmask lets it go, the registers and flags that the frame keeps across a handler (rt_sigreturn through the
   runtime's return, and through SA_RESTORER), a handler that moves the pc in its ucontext, SA_RESETHAND,
   sigaltstack with SA_ONSTACK, rt_sigqueueinfo's value, and a signal to another thread. Exits 0 */
#include "threading.h"

static volatile u32 count, last_sig, last_code, last_pid, order, unblocked_at, on_alt, alt_flags;
extern volatile u32 restorer_used; /* own_restorer's mark (thread_start.S) */
static volatile u64 last_value;
static volatile u32 thread_got, thread_tid, ctid, ready;
static u8 alt[65536] __attribute__((aligned(16)));
static u8 stack[65536] __attribute__((aligned(16)));

static void handler(int sig, u8* info, u8* uc) {
    (void)uc;
    count = count + 1;
    last_sig = (u32)sig;
    last_code = *(u32*)(info + 8);
    last_pid = *(u32*)(info + 16);
    last_value = *(u64*)(info + 24);
    clobber();
}
static void ordered(int sig) {
    (void)sig;
    unblocked_at = ++order;
}
static void skipper(int sig, u8* info, u8* uc) {
    (void)sig;
    (void)info;
    *(u64*)(uc + 176 + 264) += 4; /* uc_mcontext.pc: past the instruction after the svc */
}
static void on_stack(int sig) {
    (void)sig;
    u8 local;
    on_alt = &local >= alt && &local < alt + sizeof alt;
    u64 now[3];
    call(132, 0, (u64)now, 0);
    alt_flags = (u32)now[1];
}
static void thread_handler(int sig) {
    (void)sig;
    thread_got = (u32)gettid();
}
static int waiter(u64 unused) {
    (void)unused;
    thread_tid = (u32)gettid();
    raise_flag(&ready);
    while (!load(&thread_got)) sleep_ns(1000000);
    return 0;
}

int guest_main(u64* sp) {
    (void)sp;
    s64 pid = getpid();
    CHECK(sigaction(SIGUSR1, (void*)handler, SA_SIGINFO, 0, 0) == 0);
    CHECK(call(129, (u64)pid, SIGUSR1, 0) == 0);
    CHECK(count == 1 && last_sig == SIGUSR1 && last_code == 0 && last_pid == (u32)pid);
    put("signals: kill ok\n");

    /* x3-x17, vectors and flags come back from the frame; the handler changed them */
    CHECK(regs_survive((u64)pid, (u64)gettid(), SIGUSR1) == 0 && count == 2 && last_code == (u32)-6);
    CHECK(sigaction(SIGUSR1, (void*)handler, SA_SIGINFO | SA_RESTORER, 0, 0) == 0);
    CHECK(regs_survive((u64)pid, (u64)gettid(), SIGUSR1) == 0 && count == 3 && restorer_used == 1);
    put("signals: frame ok\n");

    /* a blocked signal waits, is pending, and comes as sigprocmask unblocks it */
    CHECK(sigaction(SIGUSR2, (void*)ordered, 0, 0, 0) == 0);
    CHECK(sigprocmask(0, BIT(SIGUSR2), 0) == 0);
    CHECK(call(129, (u64)pid, SIGUSR2, 0) == 0);
    order = 1;
    u64 pending = 0;
    CHECK(call(136, (u64)&pending, 8, 0) == 0 && pending == BIT(SIGUSR2) && unblocked_at == 0);
    CHECK(sigprocmask(1, BIT(SIGUSR2), 0) == 0);
    order = order + 1;
    CHECK(unblocked_at == 2 && order == 3);
    put("signals: mask ok\n");

    /* the handler's change to its ucontext is the state it returns to */
    CHECK(sigaction(SIGPROF, (void*)skipper, SA_SIGINFO, 0, 0) == 0);
    CHECK(skip_after((u64)pid, (u64)gettid(), SIGPROF) == 0);
    /* SA_RESETHAND: SIG_DFL again once it ran */
    struct ksigaction old;
    CHECK(sigaction(SIGPROF, (void*)skipper, SA_SIGINFO | SA_RESETHAND, 0, 0) == 0);
    CHECK(skip_after((u64)pid, (u64)gettid(), SIGPROF) == 0);
    CHECK(sigaction(SIGPROF, 0, 0, 0, &old) == 0 && old.handler == 0);
    put("signals: ucontext ok\n");

    /* the alternate stack */
    u64 ss[3] = {(u64)alt, 0, sizeof alt};
    CHECK(call(132, (u64)ss, 0, 0) == 0);
    CHECK(sigaction(SIGURG, (void*)on_stack, SA_ONSTACK, 0, 0) == 0);
    CHECK(tgkill(gettid(), SIGURG) == 0 && on_alt == 1 && alt_flags == 1);
    u64 small[3] = {(u64)alt, 0, 1024};
    CHECK(call(132, (u64)small, 0, 0) == -12); /* below MINSIGSTKSZ */
    put("signals: altstack ok\n");

    /* rt_sigqueueinfo: the value reaches the handler */
    u8 info[128] = {0};
    *(s32*)(info + 8) = -1; /* SI_QUEUE */
    *(u64*)(info + 24) = 0x1234;
    CHECK(syscall6(138, (u64)pid, SIGUSR1, (u64)info, 0, 0, 0) == 0);
    CHECK(count == 4 && last_code == (u32)-1 && last_value == 0x1234);
    put("signals: sigqueue ok\n");

    /* a signal to another thread runs there */
    CHECK(sigaction(SIGUSR2, (void*)thread_handler, 0, 0, 0) == 0);
    ctid = 1;
    CHECK(spawn_thread(THREAD_FLAGS, stack + sizeof stack, 0, 0, (u32*)&ctid, waiter, 0) > 0);
    await(&ready);
    CHECK(tgkill(thread_tid, SIGUSR2) == 0);
    join(&ctid);
    CHECK(thread_got == thread_tid && thread_tid != (u32)pid);
    CHECK(tgkill(12345678, SIGUSR2) == -3); /* ESRCH */
    put("signals: thread ok\n");
    return 0;
}
