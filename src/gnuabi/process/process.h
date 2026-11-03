/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_PROCESS_H
#define SNX_GNU_PROCESS_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"
#include "common/runtime/linux_signal.h"
#include "gnuabi/memory/memory.h"
#include "gnuabi/loader/elf/elf.h"
#include "gnuabi/filesystem/vfs.h"
#include "gnuabi/runtime/jit.h"
#include "gnuabi/kernel/kernel.h"
#include <setjmp.h>
#include <stdbool.h>
/* execve's new program, loaded once the old one's host frames are gone (gnu_runtime_run) */
typedef struct GnuExec {
    char* path;            /* guest path: /proc/self/exe, AT_EXECFN */
    unsigned char* image;  /* the ELF */
    size_t size;
    char** argv;           /* NULL-terminated, and the environment's after it */
    char** envp;
} GnuExec;

/* A guest thread of the GNU ABI runtime, on a host thread of its own (threads.c): its kernel state first, and what
   this runtime adds. CPU.thread points to it */
typedef struct GnuThread {
    GnuKernelThread k; /* k.runtime points back here */
    CPU cpu;
    SnxArmCPU arm; /* used when elf.bits == 32 */
    jmp_buf exit_target;
    int running;
    /* under the kernel's lock: 0 made, 1 a cloned thread runs and has its tid, 2 it may run its guest code */
    int started;
#ifndef _WIN32
    pthread_t host; /* for the host signal that stops its host call (Windows has none) */
#endif
    struct SnxNetWake* wake; /* ends its waits in the network stack (ppoll, sockets) at once, for a signal */
    uint32_t sample;         /* atomic: --sample-pc wants its pc */
} GnuThread;

typedef struct GnuProcess {
    GnuKernel kernel; /* the threads, futexes and signals, kernel.context points back here */
    GnuMemory memory;
    GnuElf elf;
    GnuElf interp; /* the dynamic linker (PT_INTERP), image NULL for a static program */
    GnuVfs vfs;
    GnuJit jit;
    int exited, exit_code, faulted;
    GnuExec* exec;  /* an execve on its way */
    int vfork_fd;   /* a vfork child's end of the pipe its parent waits on until it execs or exits, -1 */
    /* Windows: the parent of the vfork child running in its place (spawn.c), and the children it started */
    struct GnuVfork* vfork_parent;
    struct GnuChild* children;
    size_t child_count;
    char diagnostic[256];
} GnuProcess;
/* the GnuProcess of a thread */
static inline struct GnuProcess* gnu_process_of(const GnuThread* thread) {
    return (struct GnuProcess*)thread->k.kernel->context;
}
/* the guest address of the page that holds the runtime's return from a signal handler (mov x8, #139; svc #0), as
   the vDSO's __kernel_rt_sigreturn: the handlers return there without SA_RESTORER */
#define GNU_SIGRETURN_PAGE 0xfffff000ull

/* runtime.c: --sample-pc's watchdog for this process, once per host process (POSIX: Windows has none) */
void gnu_start_sampler(GnuProcess* process);
#ifndef _WIN32
void gnu_sample_pc(int on); /* --sample-pc, before the guest starts */
#endif
/* A process, its program the file at host path path, and its first thread (the runtime of process->kernel.threads[0]). sysroot: the
   host-backed namespace's (gnu_vfs_set_root), NULL for the isolated one. The JIT must be initialized (gnu_jit_init)
   before */
int gnu_process_init(GnuProcess* process, const char* path, int argc, const char* const* argv,
                     const char* const* environment, const char* sysroot, char* error, size_t error_capacity);
/* the program image at guest path path into a fresh address space: the ELF, its dynamic linker, the stack. thread:
   the one that runs it, whose registers start the program */
int gnu_process_load(GnuProcess* process, GnuThread* thread, const char* path, const unsigned char* image,
                     size_t size, int argc, const char* const* argv, const char* const* environment, char* error,
                     size_t error_capacity);
int gnu_process_stack(GnuThread* thread, int argc, const char* const* argv, const char* const* environment);
int gnu_process_stack32(GnuThread* thread, int argc, const char* const* argv, const char* const* environment);
void gnu_process_destroy(GnuProcess* process);
/* exit_group: the process ends with status. On thread's host thread, which leaves its guest code when it runs it */
void gnu_process_exit(GnuThread* thread, int status);
/* the process ends with signal, as Linux ends it with the signal's default action */
void gnu_process_fault(GnuThread* thread, int signal, const char* reason);
/* the kernel's operations on this runtime's process (threads.c) */
extern const GnuKernelOps gnu_kernel_ops;

/* ---- threads.c: the runtime's threads over the kernel's ---- */
/* the thread of this host thread, NULL on one that runs no guest */
GnuThread* gnu_thread_self(void);
void gnu_thread_set_self(GnuThread* thread);
/* a new thread of process, in the kernel's list, with no host thread yet. NULL when memory runs out */
GnuThread* gnu_thread_new(GnuProcess* process, uint32_t tid);
void gnu_thread_free(GnuThread* thread);
/* clone with CLONE_THREAD: a new thread from thread's registers, on stack, with its TLS and tid words. Its tid, or
   -errno */
int64_t gnu_thread_clone(GnuThread* thread, uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t tls,
                         uint64_t child_tid);
/* starts the host thread of a thread that gnu_thread_new made (the first one, main.c). 0, or -errno */
int gnu_thread_spawn(GnuThread* thread);
/* the thread leaves its guest code and ends */
void gnu_thread_leave(GnuThread* thread);
/* the signal wake of a thread's network waits, drained at a delivery point before the kernel looks at its signals */
void gnu_thread_drain(GnuThread* thread);
/* the host's fork from thread: every lock of the runtime held across it, and in the child only thread is left */
void gnu_fork_prepare(GnuProcess* process);
void gnu_fork_parent(GnuProcess* process);
void gnu_fork_child(GnuThread* thread, uint32_t pid);
/* the host signal that stops a thread's host call (EINTR), installed once */
void gnu_thread_install_kick(void);
/* host_signals.c: the host's side of the guest's signals: a disposition mirrored on the host, and the relay of host
   signals the guest handles into the kernel; a fork's child gets a relay of its own */
void gnu_host_disposition(void* process, int sig, uint64_t handler);
void gnu_host_signals_forked(void);
int64_t gnu_syscall(GnuThread* thread, uint64_t number, const uint64_t arguments[6]);
void gnu_syscall_trace(int on); /* syscalls.c: --strace, each syscall on stderr; before the guest starts */
/* spawn.c: processes (clone as fork, vfork and threads, execve, wait4, waitid), and pipes. false for another
   call */
bool gnu_spawn_syscall(GnuThread* thread, uint64_t number, const uint64_t arguments[6], int64_t* result);
/* execve's point of no return, at the top of the runtime: the old program goes, the new one is loaded */
int gnu_spawn_finish_exec(GnuThread* thread);
/* the guest's exit status for the host: a signal ends the host process the same way where the host can */
void gnu_spawn_exit(GnuProcess* process);
#ifdef _WIN32
/* Windows' vfork, execve, wait4 and kill (spawn.c) */
int64_t gnu_spawn_win32_vfork(GnuThread* thread, uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t tls,
                              uint64_t child_tid);
int64_t gnu_spawn_win32_exec(GnuThread* thread, GnuExec* exec);
void gnu_spawn_win32_child_exit(GnuThread* thread, int status);
int64_t gnu_spawn_win32_wait(GnuProcess* process, int32_t pid, uint64_t status, int no_hang, uint64_t usage);
int64_t gnu_spawn_win32_kill(GnuProcess* process, int32_t pid, int signal);
#endif
/* the options another copy of the runtime is started with for a program a guest execs (Windows) */
void gnu_spawn_set_options(int count, const char* const* options);
extern const RtAPI gnu_rtapi;
#endif
