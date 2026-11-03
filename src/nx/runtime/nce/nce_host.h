/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE's machine layer, on an AArch64 Linux host or Android 10 and later on arm64: the game's code runs on the host's CPU, with the guest's registers
   in the host's registers and the guest's addresses as the host's. The stubs of the patcher (nce_patch.h) call the
   outcall here, which puts the guest's registers in its CPU and calls the runtime on the host's stack. A run of native
   code starts from the whole of a CPU, through a signal that the thread sends to itself: sigreturn loads each
   register, and no register is left for the jump. The same signal resumes the guest after the runtime stopped it.

   It knows nothing of the kernel or the services: the runtime (nce.c) gives it the functions to call, and it is
   tested alone (tests/nce_host.c). On other hosts only nce_host_init is defined, which says why there is no NCE */
#ifndef SNX_NCE_HOST_H
#define SNX_NCE_HOST_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

/* the hosts that run NCE, and the only ones the recompile makes an NCE recompile for (recompile.c). Android from API 29
   only (nce_host_check, nce_refused): its stubs reach the thread's CPU through bionic's TLS_SLOT_APP (nce_host.c) */
#ifndef SNX_NCE_HOST /* a test of nce.c's host-free part sets it to 0: the host part needs the whole runtime */
#if defined(__aarch64__) && defined(__linux__)
#define SNX_NCE_HOST 1
#else
#define SNX_NCE_HOST 0
#endif
#endif
/* how far up the guest's addresses move under NCE at least (mem_reserve_identity): Android's app process holds the low
   4 GiB, and Eden takes its base above 36 bits there too */
#if defined(__ANDROID__)
#define SNX_NCE_MOVE_FROM (64ull << 30)
#else
#define SNX_NCE_MOVE_FROM 0
#endif
/* C11's _Noreturn, which the C99 recompile (it reads SNX_NCE_HOST here) has not: plat.h's SNX_NORETURN */
#ifndef SNX_NORETURN
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define SNX_NORETURN _Noreturn
#else
#define SNX_NORETURN
#endif
#endif

/* What the runtime gives the machine layer. Each function runs on the host's stack of the thread's innermost
   nce_host_run, with the guest's state in c (its registers, sp, pc, NZCV, the vector registers, FPCR and FPSR).
   dispatch: an outcall. code is the stub's x16 (NCE_OUTCALL), and c->pc is where the stub goes on (0 for a hook's or
   a trap's stub, whose pc dispatch sets). When dispatch returns, the guest goes on at c->pc with the state of c.
   preempted: a kick (nce_host_kick) stopped the guest's code at c->pc. trapped: the guest's code raised sig (SIGILL,
   SIGTRAP, or SIGSYS for an SVC that the net caught, nce_host_net) at c->pc. When they return, the guest goes on with the state of c. lo and hi: the guest's addresses. A pc
   in them is the guest's code (its text and the pools of stubs), and any other pc is the host's */
typedef struct {
    void (*dispatch)(CPU* c, u64 code);
    void (*preempted)(CPU* c);
    void (*trapped)(CPU* c, int sig);
    u64 lo, hi;
} NceHostConfig;

/* once, before any thread runs native code: the signal handlers, and the values that the stubs need (NceEnv): *tls,
   the distance from the host's TPIDR_EL0 to the thread's CPU pointer, or to a pointer to it when *indirect is 1
   (Android), and *outcall, the outcall's address. 0, or -1 with the reason in why */
int nce_host_init(const NceHostConfig* config, u64* tls, int* indirect, u64* outcall, char* why, size_t size);
#if SNX_NCE_HOST
/* whether this thread can run NCE, before anything is reserved for it: on Android the API level (29 or later) and the
   thread's word that the stubs read (TLS_SLOT_APP: 0, or this layer's). 0, or -1 with what was found in why */
int nce_host_check(char* why, size_t size);
/* this host thread runs c's guest code from here: before its first nce_host_run. reads: the thread's count of counter
   reads, which the stubs of NCE_TICKS add to. 0, or -1 with the reason in why when another library of the process
   holds the thread's word that the stubs read */
int nce_host_attach(CPU* c, u64* reads, char* why, size_t size);
/* the net under the guest's code: from here a system call made from the guest's addresses (an SVC that no stub
   holds) is not made, and trapped gets SIGSYS at the SVC. Every system call of the process then runs a seccomp
   filter of NCE_NET_WORDS words. After nce_host_init. 0, or -1 with the reason in why: a kernel or qemu-user without
   seccomp filters */
int nce_host_net(char* why, size_t size);
/* this host thread's id, which nce_host_kick takes */
int nce_host_tid(void);
/* the guest code that thread tid runs, if it runs any, stops and calls preempted. Any thread */
void nce_host_kick(int tid);
/* runs the guest from the whole of c, until dispatch calls nce_host_leave. A dispatch can call it again (a hook's
   handler that calls the guest's own function): the runs nest */
void nce_host_run(CPU* c);
/* from dispatch: the innermost nce_host_run returns, with the guest's state in its CPU */
SNX_NORETURN void nce_host_leave(void);
/* from dispatch, preempted or trapped: the guest goes on at once from the whole of c */
SNX_NORETURN void nce_host_resume(CPU* c);
/* the guest's state in a signal's context (a ucontext_t) into c, when the signal stopped this thread's guest code: 1,
   else 0. For a crash report, which then shows the guest's registers */
int nce_host_context(const void* context, CPU* c);
#endif
#endif
