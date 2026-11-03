/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_SIGNAL_H
#define SNX_ANDROID_HLE_SIGNAL_H
/* The guest's signals (docs/android/NATIVE.md): bionic's sigaction, masks, kill and the waits, over the GNU ABI
   runtime's kernel (src/gnuabi/kernel/kernel.h), which keeps the actions, masks, queues and futexes and writes the
   frame Linux gives a handler (common/runtime/linux_signal.h). They are the guest's alone: the process is the JVM's
   too, whose own signals (SIGSEGV, SIGUSR2, SIGQUIT) the guest never sees or masks, and the host's dispositions and
   masks never change.

   A sender queues the signal in the kernel, which sets the target's CPU.poll and interrupt, wakes its wait in the
   kernel (a futex, sigsuspend, sigtimedwait) and, when it waits in a blocking HLE call or runs no guest code, sends
   it the host's carrier signal. The target runs the handler where its guest registers are all in its CPU structure:
   at RtAPI.poll, as an HLE call returns, or from the carrier's host handler when it waits in a blocking HLE call
   (signals_park) or runs no guest code, and holds no lock of the kernel. Generated code that the carrier
   interrupts goes on to its next poll */
#include "common/cpu/aarch64/cpu.h"
#include "gnuabi/kernel/kernel.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* the signal state of a thread that runs guest code (bridge.c's Thread) */
typedef struct GuestSignals {
    GnuKernelThread k; /* k.runtime points back here */
    CPU* cpu;
    pthread_t host;
    int parked;          /* atomic: in an HLE call that waits */
    int busy;            /* atomic: the depth of guest calls and deliveries on this thread */
    unsigned delivered;  /* atomic: the handlers this thread ran */
    uint64_t in_export;  /* atomic: the export it is in, for SNX_ANDROID_SAMPLE */
} GuestSignals;

/* the kernel and the carrier's host handler: once, from bridge_init */
void signals_init(void);
void signals_thread_start(GuestSignals* t, CPU* c);
void signals_thread_end(GuestSignals* t);
/* the mask that the threads the calling one creates start with: set in the new thread before its first guest call */
void signals_inherit(uint64_t mask);
uint64_t signals_mask(void);
/* the export the calling thread enters (0 as it leaves), for SNX_ANDROID_SAMPLE */
void signals_in_export(uint64_t address);
/* the signals ready for the calling thread run their handlers: c's registers must all be in place */
void signals_check(CPU* c);
/* around an HLE call that can wait: the previous state, which signals_unpark takes back */
int signals_park(CPU* c);
void signals_unpark(int previous);
/* around guest code that the host calls (bridge_call) */
void signals_enter(void);
void signals_leave(void);
/* a system call of the GNU ABI runtime's kernel (kernel.h's gnu_kernel_syscall: threads, futexes, signals, sleeps,
   ids, clocks, random bytes, prctl, membarrier, madvise...) from the HLE call that c makes (syscall(), or a bionic
   function that does the same work), with the handlers of the signals that stop it: the result as the kernel's, a
   negative errno for an error. *known (when not NULL) is false for a number the kernel doesn't have, -ENOSYS */
long hle_kernel_syscall(long number, const uint64_t arguments[6], CPU* c, bool* known);
#endif
