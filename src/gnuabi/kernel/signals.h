/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_SIGNALS_H
#define SNX_GNU_SIGNALS_H
/* The guest's signals (signals.c): actions, masks, the queues of each thread and of the process, and their delivery
   into the guest's handlers through the Linux AArch64 frame (common/runtime/linux_signal.h).
   A signal is delivered only where all the guest's registers are in its CPU: at RtAPI.poll and at the end of each
   system call or HLE call. The sender sets the target's poll word and interrupt, wakes its wait in the kernel, and the
   runtime's GnuKernelOps.kick stops a host call it is in (thread.c's gnu_kernel_kick) */
#include "gnuabi/kernel/kernel.h"

/* rt_sigaction, rt_sigprocmask, kill, tkill, tgkill, sigaltstack, rt_sigsuspend, rt_sigpending, rt_sigtimedwait,
   rt_sigqueueinfo and rt_tgsigqueueinfo. false for another call */
bool gnu_signal_syscall(GnuKernelThread* thread, uint64_t number, const uint64_t arguments[6], int64_t* result);
/* ppoll, pselect6, epoll_pwait and epoll_pwait2 with a signal mask: the mask while the call waits, restored at its
   end or by the frame of the handler that ends it. 0, or -errno */
int64_t gnu_signal_call_mask(GnuKernelThread* thread, uint64_t number, const uint64_t arguments[6]);
/* a signal from the runtime (a sender in this process, or the relay of a host signal) to a thread, or with thread
   NULL to the process. 0, or -GNU_EAGAIN when the queue is full */
int gnu_signal_send(GnuKernel* process, GnuKernelThread* thread, int signo, int code, int32_t pid,
                    uint32_t uid, uint64_t value);
/* a fault of thread at an instruction (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP), with addr as the siginfo's
   si_addr, as Linux's force_sig_fault gives it: queued for thread when it has a handler for signo that it does not
   block, 1, and the next delivery point enters the handler. 0 when Linux would take the default action (no handler,
   SIG_IGN, or the signal blocked, which force_sig_fault makes SIG_DFL): the runtime ends the process itself */
int gnu_signal_fault(GnuKernelThread* thread, int signo, int code, uint64_t addr);
/* whether thread has a reason to leave its wait: a signal it takes, or the end of the process. Clears its interrupt
   first, so a signal sent after the check sets it again */
int gnu_signal_wanted(GnuKernelThread* thread);
/* the wait of a runtime call (a sleep, rt_sigsuspend): until deadline (gnu_monotonic_ns, -1 none), 1, or until
   gnu_signal_wanted, 0 */
int gnu_signal_sleep(GnuKernelThread* thread, int64_t deadline);

/* What a delivery point does: nothing, restart the system call (no handler ran), or the CPU enters a handler and the
   caller resumes it through the dispatch path */
enum { GNU_DELIVER_NONE, GNU_DELIVER_RESTART, GNU_DELIVER_HANDLER };
/* At a delivery point of thread, whose registers are all in its CPU. result: the result of the system call that ends
   here (x0 not written yet), or NULL at RtAPI.poll; orig_x0 is the call's first argument, which a restart gives it
   again. The default actions run here: a signal that ends the process ends it here */
int gnu_signal_deliver(GnuKernelThread* thread, int64_t* result, uint64_t orig_x0);
/* whether gnu_signal_deliver has anything to do, without a lock: an interrupt, the end of the process, or a pending
   signal that is not blocked */
int gnu_signal_maybe(GnuKernelThread* thread);
/* rt_sigreturn: thread's registers and mask from the frame at its stack pointer (where a handler's return to its
   restorer leaves it). A frame that cannot be read ends the process with SIGSEGV, as Linux's force_sigsegv. The
   runtime then resumes the thread at its restored pc */
void gnu_signal_return(GnuKernelThread* thread);
/* execve: handlers go back to SIG_DFL (ignored signals stay ignored), with the host's dispositions */
void gnu_signal_exec(GnuKernelThread* thread);
/* host and guest signal numbers (1-31), 0 for none */
int gnu_host_signal(int guest);
int gnu_guest_signal(int host);
#endif
