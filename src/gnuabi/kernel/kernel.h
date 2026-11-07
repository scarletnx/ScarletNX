/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_KERNEL_H
#define SNX_GNU_KERNEL_H
/* The Linux kernel's threads, futexes and signals for a guest of AArch64 Linux's ABI, as a library
   (scarletnx-gnuabi-kernel) that more than one runtime uses: the GNU ABI runtime (src/gnuabi) and the Android bridge.
   It has no loader, file system or descriptor table: the runtime reaches the guest's memory, ends the process and
   stops a host call through GnuKernelOps.

   A runtime makes one GnuKernel for its process, and a GnuKernelThread for each guest thread, which it registers with
   the thread's CPU (gnu_kernel_thread_add). Its system calls go to gnu_kernel_syscall, which answers the ones it
   has. At each delivery point of a thread (RtAPI.poll, the end of a system call or HLE call), where all its guest
   registers are in its CPU, the runtime calls gnu_signal_deliver: GNU_DELIVER_HANDLER means the CPU now enters a
   handler, with its frame on the guest's stack and its return address the restorer (SA_RESTORER) or
   GnuKernel.sigreturn. The runtime runs the handler as it runs guest code; the handler's rt_sigreturn (139) through
   gnu_kernel_syscall, or the runtime's own return (gnu_signal_return), takes the frame back.

   Locks: GnuKernel.lock (recursive) guards the threads, the signal queues, masks and actions, and the end of the
   process; the futex table's lock its waiters; each thread's wait_lock its runtime wait. The order is the futex
   table's or the kernel's lock, then a thread's wait_lock. No lock is held while guest code runs or a host call
   blocks */
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/linux_signal.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/kernel/sync.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct GnuKernel;
struct GnuKernelThread;

/* Signals waiting for delivery, to a thread or to the process. A standard signal (1-31) has one entry at most, as on
   Linux; a real-time one has an entry each time it is sent, while there is room. The kernel's lock guards the
   entries; pending, a bit for each signal with an entry, is also read without it (atomic) */
#define GNU_SIGQUEUE_MAX 64
typedef struct GnuSigQueue {
    uint64_t pending;
    uint32_t count;
    SnxLinuxSiginfo entries[GNU_SIGQUEUE_MAX];
} GnuSigQueue;

/* A waiter of a futex, on the stack of the thread that waits: in the table's list while it waits */
typedef struct GnuFutexWaiter {
    struct GnuFutexWaiter *prev, *next;
    uint64_t address; /* the guest address of its word: a requeue changes it */
    uint32_t bitset;
    uint32_t woken; /* atomic */
    struct GnuKernelThread* thread;
} GnuFutexWaiter;
/* The futexes of a process: one lock and one list of waiters, in the order they came. A private futex and a shared
   one are the same: the library knows one process */
typedef struct GnuFutexTable {
    GnuMutex lock;
    GnuFutexWaiter* head;
    GnuFutexWaiter* tail;
} GnuFutexTable;

/* A guest thread's kernel state. The runtime owns its memory (it can be a member of the runtime's own thread) */
typedef struct GnuKernelThread {
    struct GnuKernel* kernel;
    CPU* regs;     /* the thread's registers, the runtime's CPU */
    void* runtime; /* the runtime's object for the thread */
    uint32_t tid;
    uint64_t clear_tid;   /* CLONE_CHILD_CLEARTID, set_tid_address: 0 written and woken at its exit */
    uint64_t robust_list; /* set_robust_list's head */
    uint64_t signal_mask; /* the thread changes it under the kernel's lock, and others read it under the lock */
    /* rt_sigsuspend's and ppoll's: the mask that the frame of the next handler restores, or the call at its end */
    uint64_t saved_mask;
    int saved_mask_valid;
    uint64_t wait_set; /* rt_sigtimedwait's signals: they wake the thread although it blocks them */
    GnuSigQueue queue;
    SnxLinuxAltStack altstack;
    /* atomics: another thread wants this one at a delivery point (a signal, or the end of the process); the thread
       is in a system call (gnu_kernel_enter_call), where only its runtime wait or GnuKernelOps.kick reaches it */
    uint32_t interrupt;
    char apart[60]; /* other threads write interrupt, this one blocking: a cache line each */
    uint32_t blocking;
    /* a wait in the kernel (futex, sleeps, rt_sigsuspend) sleeps on wake, under wait_lock */
    GnuMutex wait_lock;
    GnuCond wake;
    char name[16]; /* prctl's PR_SET_NAME, NUL-terminated: the host thread's at first */
    uint64_t timer_slack; /* prctl's PR_SET_TIMERSLACK, in nanoseconds */
} GnuKernelThread;

/* What the runtime gives the kernel */
typedef struct GnuKernelOps {
    /* the guest's memory, checked: 0, or -GNU_EFAULT for an address the guest cannot read or write */
    int (*read)(void* context, uint64_t address, void* out, size_t size);
    int (*write)(void* context, uint64_t address, const void* in, size_t size);
    /* whether the guest can write (write) or read the bytes, without a change: 0 or -GNU_EFAULT. NULL: read checks
       a futex word for both */
    int (*check)(void* context, uint64_t address, size_t size, int write);
    /* thread waits in a host call of the runtime (gnu_kernel_enter_call): stop it (a host signal, a wake of its
       poll). Under the kernel's lock. NULL: only the kernel's own waits end */
    void (*kick)(void* context, GnuKernelThread* thread);
    /* the process ends with status (exit_group, the last thread's exit), or because of signal (a fault, a default
       action), with reason. The runtime stops its other threads (gnu_kernel_stop_others) and leaves the thread's
       guest code. A return means the runtime ignores signal (a default action it doesn't take) */
    void (*end)(void* context, GnuKernelThread* thread, int status, int signal, const char* reason);
    /* the thread leaves its guest code for good: another thread ends the process or execs, or it exited. It does
       not return while the thread runs guest code */
    void (*leave)(void* context, GnuKernelThread* thread);
    /* a stop signal's default action (SIGSTOP, SIGTSTP...); NULL: none */
    void (*stop)(void* context);
    /* a guest's new disposition of sig (SIG_DFL 0, SIG_IGN 1, a handler), for a runtime that mirrors it on the host;
       NULL: the guest's signals are its own. Under the kernel's lock */
    void (*disposition)(void* context, int sig, uint64_t handler);
    /* kill(2), tgkill(2) and sigqueue of another process: NULL: the host's, by the host's numbers */
    int64_t (*kill)(void* context, int32_t pid, int32_t tid, int sig, int code, uint64_t value);
    /* madvise(2) of a range the kernel checked (page-aligned, a Linux advice): 0 or -errno. NULL: advice only, which
       a runtime whose memory is not the host's at the same addresses leaves alone */
    int64_t (*madvise)(void* context, uint64_t address, uint64_t length, int advice);
} GnuKernelOps;

typedef struct GnuKernel {
    const GnuKernelOps* ops;
    void* context;
    /* the host address of guest address 0 (0 when the guest's addresses are the host's): a futex word is an atomic
       at base + address */
    unsigned char* base;
    uint64_t sigreturn; /* where a handler without SA_RESTORER returns: code that makes rt_sigreturn */
    uint32_t pid;
    GnuMutex lock;
    GnuCond changed; /* a thread started or ended, under lock */
    GnuKernelThread** threads;
    size_t thread_count, thread_capacity;
    SnxLinuxSigaction actions[65];
    GnuSigQueue queue; /* signals to the process: the first thread that doesn't block one takes it */
    /* atomic: 1 while the process ends, 2 while an execve stops the other threads. stopper is the thread that does */
    uint32_t stopping;
    GnuKernelThread* stopper;
    GnuFutexTable futex;
    /* prctl's: PR_SET_DUMPABLE, PR_SET_NO_NEW_PRIVS, PR_SET_THP_DISABLE, PR_SET_CHILD_SUBREAPER (atomic) */
    uint32_t dumpable, no_new_privs, thp_disable, subreaper;
    uint32_t membarrier_registered; /* atomic: membarrier's REGISTER commands this process made */
} GnuKernel;

/* ---- the process and its threads (thread.c) ---- */
void gnu_kernel_init(GnuKernel* kernel, const GnuKernelOps* ops, void* context, unsigned char* base, uint32_t pid);
void gnu_kernel_destroy(GnuKernel* kernel);
/* thread into the kernel's list, its state zero but for these. 0, or -GNU_ENOMEM */
int gnu_kernel_thread_add(GnuKernel* kernel, GnuKernelThread* thread, uint32_t tid, CPU* regs, void* runtime);
/* thread out of the list: 1 when it was in it. The runtime frees it after */
int gnu_kernel_thread_remove(GnuKernelThread* thread);
void gnu_kernel_thread_destroy(GnuKernelThread* thread);
/* the thread of this host thread (the runtime sets it on each thread it runs), NULL for none */
GnuKernelThread* gnu_kernel_self(void);
void gnu_kernel_set_self(GnuKernelThread* thread);
/* exit(2): the clear_tid word, the robust futexes, and the thread leaves (the process ends with the last one) */
void gnu_kernel_thread_exit(GnuKernelThread* thread, int status);
/* another thread wants thread at a delivery point: its poll word, its kernel wait, GnuKernelOps.kick. Under the
   kernel's lock */
void gnu_kernel_kick(GnuKernelThread* thread);
/* thread starts to end the process (kind 1) or to exec (kind 2): 0 when another thread does already, and then thread
   must leave */
int gnu_kernel_claim_stop(GnuKernelThread* thread, uint32_t kind);
/* the other threads leave their guest code: all, or (exit_group) all but those in a system call */
void gnu_kernel_stop_others(GnuKernelThread* thread, int all);
/* the end of the stop: the other threads are gone (execve) */
void gnu_kernel_stop_done(GnuKernelThread* thread);
/* at a delivery point: the thread leaves when another thread ends the process or execs */
void gnu_kernel_check_stop(GnuKernelThread* thread);
/* around a system call or HLE call that can block: kick reaches the thread there */
void gnu_kernel_enter_call(GnuKernelThread* thread);
void gnu_kernel_leave_call(GnuKernelThread* thread);
/* the host's fork from thread: the kernel's locks held across it, and in the child only thread is left (the
   runtime frees the others' records with gnu_kernel_forked's list) */
void gnu_kernel_fork_prepare(GnuKernel* kernel);
void gnu_kernel_fork_parent(GnuKernel* kernel);
void gnu_kernel_fork_child(GnuKernelThread* thread, uint32_t pid);

/* system.c: the host thread's name (16 bytes with the NUL), "" where the host has none */
void gnu_kernel_host_thread_name(char name[16]);
/* whether the host has a memory barrier on every thread of the process: Linux's membarrier (asked once), Windows'
   FlushProcessWriteBuffers; macOS and FreeBSD have none */
bool gnu_host_barrier(void);
/* membarrier(2) on a host that has that barrier (host, gnu_host_barrier's answer): Linux's results. Without one every
   command is ENOSYS, as from a kernel without the call, and the guest takes its own way */
int64_t gnu_kernel_membarrier(GnuKernel* kernel, bool host, uint64_t command, uint64_t flags);

/* ---- system calls (syscall.c) ---- */
/* the calls the kernel has: exit, exit_group, gettid, set_tid_address, set_robust_list, get_robust_list, futex,
   sched_yield, sched_getaffinity, sched_setaffinity, nanosleep, clock_nanosleep, restart_syscall, the signal calls
   (signals.h) and gnu_kernel_system_call's. false for another call. A call that a signal stops returns a
   GNU_ERESTART* code, which gnu_signal_deliver turns into EINTR or a restart */
bool gnu_kernel_syscall(GnuKernelThread* thread, uint64_t number, const uint64_t arguments[6], int64_t* result);
/* system.c, for gnu_kernel_syscall: the calls that need no runtime but the memory: clock_gettime, clock_getres,
   gettimeofday, times, getrusage, sysinfo, uname, getrlimit, setrlimit, prlimit64, prctl, getcpu, getpid, getppid,
   getuid, geteuid, getgid, getegid, getgroups, setpriority, getpriority, sched_setparam, sched_setscheduler,
   sched_getscheduler, sched_getparam, sched_get_priority_max, sched_get_priority_min, sched_rr_get_interval,
   madvise, getrandom, membarrier, rseq (ENOSYS) */
bool gnu_kernel_system_call(GnuKernelThread* thread, uint64_t number, const uint64_t arguments[6], int64_t* result);
/* the guest's memory through GnuKernelOps */
int gnu_kernel_read(GnuKernel* kernel, uint64_t address, void* out, size_t size);
int gnu_kernel_write(GnuKernel* kernel, uint64_t address, const void* in, size_t size);
/* host.c: the host's random bytes (0 or -GNU_EIO), a Linux clock's time (0 or -GNU_EINVAL for an id Linux doesn't
   have), and a sleep that no signal ends */
int gnu_random_bytes(void* destination, size_t size);
int gnu_clock_gettime(int clock_id, int64_t* seconds, int64_t* nanoseconds);
void gnu_sleep(int64_t seconds, int64_t nanoseconds);
/* a host errno as the guest's, never the host's numbering (Windows' differs) (errno.c) */
int gnu_host_errno(int error);
#endif
