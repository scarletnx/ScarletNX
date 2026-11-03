/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* What the thread and signal fixtures share: the system calls, a futex lock, the kernel's sigaction, and a check
   that names the line where it failed (exit status 1) */
#ifndef SNX_GNUABI_TEST_THREADING_H
#define SNX_GNUABI_TEST_THREADING_H
#include "guest.h"
typedef int s32;

/* clone's flags as glibc's pthread_create gives them */
#define THREAD_FLAGS (0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x80000 | 0x100000 | 0x200000)
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGURG 23
#define SIGPROF 27
#define SA_SIGINFO 4u
#define SA_ONSTACK 0x08000000u
#define SA_RESTART 0x10000000u
#define SA_NODEFER 0x40000000u
#define SA_RESETHAND 0x80000000u
#define SA_RESTORER 0x04000000u
#define EINTR 4
#define EAGAIN 11
#define ETIMEDOUT 110

long spawn_thread(u64 flags, void* stack_top, u32* ptid, u64 tls, u32* ctid, int (*fn)(u64), u64 arg);
void own_restorer(void);
void clobber(void);
u64 regs_survive(u64 tgid, u64 tid, u64 sig);
u64 skip_after(u64 tgid, u64 tid, u64 sig);

static inline void put(const char* s) {
    u64 n = 0;
    while (s[n]) n++;
    call(64, 1, (u64)s, n);
}
static inline void fail(int line) {
    char text[] = "check failed at line 0000\n";
    for (int i = 0; i < 4; i++, line /= 10) text[24 - i] = (char)('0' + line % 10);
    put(text);
    call(94, 1, 0, 0);
}
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) fail(__LINE__);                                                                                      \
    } while (0)

static inline s64 getpid(void) { return call(172, 0, 0, 0); }
static inline s64 gettid(void) { return call(178, 0, 0, 0); }
static inline s64 tgkill(s64 tid, int sig) { return call(131, (u64)getpid(), (u64)tid, (u64)sig); }
static inline s64 futex(volatile u32* word, int op, u32 value, u64 timeout, volatile u32* word2, u32 value3) {
    return syscall6(98, (u64)word, (u64)op, value, timeout, (u64)word2, value3);
}
static inline void sleep_ns(s64 ns) {
    s64 t[2] = {ns / 1000000000, ns % 1000000000};
    while (call(101, (u64)t, (u64)t, 0) == -EINTR) {}
}
static inline u32 load(volatile u32* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void store(volatile u32* p, u32 v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
/* until a thread's CLONE_CHILD_CLEARTID word is 0 */
static inline void join(volatile u32* ctid) {
    for (u32 v; (v = load(ctid)) != 0;) futex(ctid, 0, v, 0, 0, 0);
}
/* until *flag is nonzero */
static inline void await(volatile u32* flag) {
    while (!load(flag)) futex(flag, 0, 0, 0, 0, 0);
}
static inline void raise_flag(volatile u32* flag) {
    store(flag, 1);
    futex(flag, 1, 0x7fffffff, 0, 0, 0);
}

/* the kernel's sigaction */
struct ksigaction {
    u64 handler, flags, restorer, mask;
};
static inline s64 sigaction(int sig, void* handler, u64 flags, u64 mask, struct ksigaction* old) {
    struct ksigaction a = {(u64)handler, flags, (flags & SA_RESTORER) ? (u64)own_restorer : 0, mask};
    return syscall6(134, (u64)sig, (u64)&a, (u64)old, 8, 0, 0);
}
static inline s64 sigprocmask(int how, u64 set, u64* old) {
    return syscall6(135, (u64)how, (u64)&set, (u64)old, 8, 0, 0);
}
#define BIT(sig) (1ul << ((sig) - 1))
#endif
