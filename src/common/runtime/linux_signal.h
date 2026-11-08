/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_COMMON_LINUX_SIGNAL_H
#define SNX_COMMON_LINUX_SIGNAL_H
/* The signal ABI of Linux on AArch64, which the GNU ABI runtime (src/gnuabi) and the Android bridge (bionic is Linux's
   ABI) both give their guests: the kernel's sigaction, siginfo and the frame a handler runs on (rt_sigframe, from
   arch/arm64/kernel/signal.c's documented layout, the uapi headers' structures). The frame is built in a host buffer
   in the guest's little-endian bytes, and each runtime writes it to the guest's stack with its own checked access.

   A runtime delivers a signal only where the guest's registers are all in its CPU structure: at RtAPI.poll (each
   backward branch and dispatch of generated code), at the end of a system call or HLE call, or in one that waits.
   Never in a host signal handler that interrupted generated code, whose registers are partly in host ones. */
#include "common/cpu/aarch64/cpu.h"
#include <stdbool.h>
#include <stdint.h>

/* the kernel's sigaction on AArch64 (8-byte sigset): handler (SIG_DFL 0, SIG_IGN 1), flags, restorer, mask */
typedef struct {
    uint64_t handler, flags, restorer, mask;
} SnxLinuxSigaction;

#define SNX_SIG_DFL 0
#define SNX_SIG_IGN 1

#define SNX_SA_NOCLDSTOP 0x00000001u
#define SNX_SA_NOCLDWAIT 0x00000002u
#define SNX_SA_SIGINFO   0x00000004u
#define SNX_SA_RESTORER  0x04000000u
#define SNX_SA_ONSTACK   0x08000000u
#define SNX_SA_RESTART   0x10000000u
#define SNX_SA_NODEFER   0x40000000u
#define SNX_SA_RESETHAND 0x80000000u

#define SNX_SIGILL 4
#define SNX_SIGKILL 9
#define SNX_SIGSTOP 19
#define SNX_SIGCONT 18
#define SNX_SIGCHLD 17

/* si_code values */
#define SNX_SI_USER   0
#define SNX_SI_KERNEL 0x80
#define SNX_SI_QUEUE  (-1)
#define SNX_SI_TIMER  (-2)
#define SNX_SI_TKILL  (-6)
#define SNX_ILL_ILLOPC 1 /* SIGILL's: an illegal opcode */

/* a signal's bit in a mask: signal 1 is bit 0 */
#define SNX_SIGBIT(sig) (1ull << ((sig) - 1))
/* the signals no mask blocks */
#define SNX_SIG_UNBLOCKABLE (SNX_SIGBIT(SNX_SIGKILL) | SNX_SIGBIT(SNX_SIGSTOP))

/* the guest's siginfo_t (128 bytes), built from these fields: the kill form (pid, uid), the queue and timer forms'
   value, the fault form's address */
typedef struct {
    int32_t signo, code, error;
    int32_t pid;   /* or the timer's id */
    uint32_t uid;  /* or the timer's overrun */
    uint64_t value; /* sigval, for SI_QUEUE and SI_TIMER; the exit status for SIGCHLD */
    uint64_t addr; /* the fault address, for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP */
} SnxLinuxSiginfo;
#define SNX_LINUX_SIGINFO_SIZE 128
void snx_linux_siginfo_encode(uint8_t out[SNX_LINUX_SIGINFO_SIZE], const SnxLinuxSiginfo* info);

/* the guest's stack_t: ss_sp, ss_flags (SS_ONSTACK 1, SS_DISABLE 2), ss_size */
typedef struct {
    uint64_t sp;
    int32_t flags;
    uint64_t size;
} SnxLinuxAltStack;
#define SNX_SS_ONSTACK 1
#define SNX_SS_DISABLE 2
#define SNX_MINSIGSTKSZ 5120
/* stack_t's 24 bytes, both ways */
void snx_linux_altstack_encode(uint8_t out[24], const SnxLinuxAltStack* stack);
SnxLinuxAltStack snx_linux_altstack_decode(const uint8_t in[24]);
/* sigaltstack's flags for the current stack pointer: SS_DISABLE, SS_ONSTACK when sp is in it, or 0 */
int32_t snx_linux_altstack_flags(const SnxLinuxAltStack* stack, uint64_t sp);

/* the kernel's sigaction structure (32 bytes), both ways */
void snx_linux_sigaction_encode(uint8_t out[32], const SnxLinuxSigaction* action);
SnxLinuxSigaction snx_linux_sigaction_decode(const uint8_t in[32]);

/* what a signal does with SIG_DFL */
typedef enum { SNX_SIGDFL_TERM, SNX_SIGDFL_CORE, SNX_SIGDFL_IGNORE, SNX_SIGDFL_STOP, SNX_SIGDFL_CONT } SnxSigDefault;
SnxSigDefault snx_linux_signal_default(int sig);
/* whether a signal with this action is dropped as it is raised: SIG_IGN, or SIG_DFL whose action is to ignore */
bool snx_linux_signal_ignored(int sig, const SnxLinuxSigaction* action);

/* the next signal to deliver of pending that mask doesn't block, 0 for none: the synchronous ones first (SIGSEGV,
   SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGSYS), then the lowest number, as the kernel picks */
int snx_linux_signal_next(uint64_t pending, uint64_t mask);

/* The frame: siginfo (128), the ucontext (4560: flags, link, stack, sigmask, the sigcontext with x0-x30, sp, pc,
   pstate, and its records: FP/SIMD, a fault's ESR, the terminator), 16-byte aligned, below a frame record {x29, x30}
   that x29 points at, as the kernel's */
#define SNX_LINUX_SIGFRAME_SIZE 4688
#define SNX_LINUX_SIGFRAME_UCONTEXT 128
/* where the frame goes for a handler of these flags, from the stack pointer: on the alternate stack when the
   handler asks for it and the thread is not on it already. *record gets the frame record's address */
uint64_t snx_linux_sigframe_place(uint64_t sp, uint64_t flags, const SnxLinuxAltStack* stack, uint64_t* record);
/* the frame of c's state (with pc where the handler returns to the interrupted code) into out, with the mask to
   restore and the alternate stack's state; esr nonzero for a fault (the ESR_EL1 the kernel would report) */
void snx_linux_sigframe_build(uint8_t out[SNX_LINUX_SIGFRAME_SIZE], const CPU* c, const uint8_t siginfo[128],
                              uint64_t mask, const SnxLinuxAltStack* stack, uint64_t esr);
/* the frame record's 16 bytes: the interrupted x29 and pc, so a backtrace walks through the handler */
void snx_linux_sigframe_record(uint8_t out[16], const CPU* c);
/* c enters the handler of sig with its frame at frame (and record): x0 = sig, x1 = &siginfo, x2 = &ucontext, sp,
   x29, x30 = the return (the restorer), pc = the handler */
void snx_linux_sigframe_enter(CPU* c, int sig, uint64_t frame, uint64_t record, uint64_t handler, uint64_t restorer);
/* rt_sigreturn: c's state back from a frame the handler may have changed, its mask in *mask. False for a frame
   whose records are malformed, where Linux sends SIGSEGV */
bool snx_linux_sigframe_restore(CPU* c, const uint8_t frame[SNX_LINUX_SIGFRAME_SIZE], uint64_t* mask);

/* the mask while a handler runs: the action's, the signal itself unless SA_NODEFER, over the thread's */
uint64_t snx_linux_handler_mask(uint64_t thread_mask, int sig, const SnxLinuxSigaction* action);
#endif
