/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_PROCESS_H
#define SNX_GNU_PROCESS_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"
#include "gnuabi/memory/memory.h"
#include "gnuabi/loader/elf/elf.h"
#include "gnuabi/filesystem/vfs.h"
#include "gnuabi/abi/structures/signal.h"
#include "gnuabi/runtime/jit.h"
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
typedef struct GnuProcess {
    CPU cpu;
    SnxArmCPU arm; /* used when elf.bits == 32 */
    GnuMemory memory;
    GnuElf elf;
    GnuElf interp; /* the dynamic linker (PT_INTERP), image NULL for a static program */
    GnuVfs vfs;
    GnuJit jit;
    uint32_t pid, tid;
    uint64_t clear_tid, signal_mask;
    GnuSignalAction actions[65];
    jmp_buf exit_target;
    int running, exited, exit_code, faulted;
    GnuExec* exec;  /* an execve on its way */
    int vfork_fd;   /* a vfork child's end of the pipe its parent waits on until it execs or exits, -1 */
    /* Windows: the parent of the vfork child running in its place (spawn.c), and the children it started */
    struct GnuVfork* vfork_parent;
    struct GnuChild* children;
    size_t child_count;
    char diagnostic[256];
} GnuProcess;
int gnu_random_bytes(void* destination, size_t size);
int gnu_clock_gettime(int clock_id, int64_t* seconds, int64_t* nanoseconds);
void gnu_sleep(int64_t seconds, int64_t nanoseconds);
/* runtime.c: --sample-pc's watchdog for this process, once per host process (POSIX: Windows has none) */
void gnu_start_sampler(GnuProcess* process);
#ifndef _WIN32
void gnu_sample_pc(int on); /* --sample-pc, before the guest starts */
#endif
/* A process, its program the file at host path path. sysroot: the host-backed namespace's (gnu_vfs_set_root), NULL
   for the isolated one. The JIT must be initialized (gnu_jit_init) before */
int gnu_process_init(GnuProcess* process, const char* path, int argc, const char* const* argv,
                     const char* const* environment, const char* sysroot, char* error, size_t error_capacity);
/* the program image at guest path path into a fresh address space: the ELF, its dynamic linker, the stack */
int gnu_process_load(GnuProcess* process, const char* path, const unsigned char* image, size_t size, int argc,
                     const char* const* argv, const char* const* environment, char* error, size_t error_capacity);
int gnu_process_stack(GnuProcess* process, int argc, const char* const* argv, const char* const* environment);
int gnu_process_stack32(GnuProcess* process, int argc, const char* const* argv, const char* const* environment);
void gnu_process_destroy(GnuProcess* process);
void gnu_process_exit(GnuProcess* process, int status);
void gnu_process_fault(GnuProcess* process, int signal, const char* reason);
int64_t gnu_syscall(GnuProcess* process, uint64_t number, const uint64_t arguments[6]);
void gnu_syscall_trace(int on); /* syscalls.c: --strace, each syscall on stderr; before the guest starts */
/* spawn.c: processes (clone as fork and vfork, execve, wait4, waitid, kill), and pipes. false for another call */
bool gnu_spawn_syscall(GnuProcess* process, uint64_t number, const uint64_t arguments[6], int64_t* result);
/* execve's point of no return, at the top of the runtime: the old program goes, the new one is loaded */
int gnu_spawn_finish_exec(GnuProcess* process);
/* the guest's exit status for the host: a signal ends the host process the same way where the host can */
void gnu_spawn_exit(GnuProcess* process);
#ifdef _WIN32
/* Windows' vfork, execve, wait4 and kill (spawn.c) */
int64_t gnu_spawn_win32_vfork(GnuProcess* process, uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t tls,
                              uint64_t child_tid);
int64_t gnu_spawn_win32_exec(GnuProcess* process, GnuExec* exec);
void gnu_spawn_win32_child_exit(GnuProcess* process, int status);
int64_t gnu_spawn_win32_wait(GnuProcess* process, int32_t pid, uint64_t status, int no_hang, uint64_t usage);
int64_t gnu_spawn_win32_kill(GnuProcess* process, int32_t pid, int signal);
#endif
/* the options another copy of the runtime is started with for a program a guest execs (Windows) */
void gnu_spawn_set_options(int count, const char* const* options);
/* a guest sigaction's disposition (SIG_DFL 0, SIG_IGN 1, a handler) mirrored on the host's signal */
void gnu_spawn_signal_disposition(int signal, uint64_t handler);
extern const RtAPI gnu_rtapi;
#endif
