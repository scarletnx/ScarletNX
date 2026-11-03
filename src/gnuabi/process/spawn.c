/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest processes are host processes. On POSIX hosts clone is the host's fork: the guest's memory, CPU state and
   descriptor table are this process's, so the child has a copy of each, and its descriptors share the host's open
   files with the parent's, as Linux shares them. A vfork (CLONE_VFORK, how posix_spawn starts a program) is a fork
   whose parent waits until the child has loaded another program or exited, as the kernel makes it wait; the child
   has a copy of the memory instead of sharing it, which is all a vfork child may rely on before execve. execve loads
   the new program into the same host process (gnu_spawn_finish_exec), keeping its pid and descriptors.
   Windows has no fork: a vfork child runs in this process until its execve, which starts the new program in a new
   host process (another copy of this runtime) with the child's descriptors, and the parent goes on from its clone
   call. A plain fork is unsupported there. A fork from a thread copies that thread alone, as Linux's does, with every
   lock of the runtime held across the host's fork (thread.c). clone with CLONE_THREAD is a thread (thread.c). */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "gnuabi/process/process.h"
#include "gnuabi/kernel/signals.h"
#include "common/utilities/endian.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/filesystem/internal.h"
#include "common/utilities/common.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

enum {
GNU_CLONE_VM = 0x100,
GNU_CLONE_SIGHAND = 0x800,
GNU_CLONE_VFORK = 0x4000,
GNU_CLONE_THREAD = 0x10000,
GNU_CLONE_SETTLS = 0x80000,
GNU_CLONE_PARENT_SETTID = 0x100000,
GNU_CLONE_CHILD_CLEARTID = 0x200000,
GNU_CLONE_CHILD_SETTID = 0x1000000
};
#define ARGUMENTS_MAX 0x200000 /* bytes of arguments and environment, Linux's 2 MiB with the default stack */

/* ---- signals between guest and host numbering ---- */
#ifndef _WIN32
/* a host wait status in Linux's encoding */
static uint32_t guest_status(int status) {
    if (WIFEXITED(status)) return (uint32_t)(WEXITSTATUS(status) & 0xff) << 8;
    if (WIFSIGNALED(status)) {
#ifdef WCOREDUMP
        return (uint32_t)gnu_guest_signal(WTERMSIG(status)) | (WCOREDUMP(status) ? 0x80u : 0);
#else
        return (uint32_t)gnu_guest_signal(WTERMSIG(status));
#endif
    }
    if (WIFSTOPPED(status)) return (uint32_t)gnu_guest_signal(WSTOPSIG(status)) << 8 | 0x7f;
#ifdef WIFCONTINUED
    if (WIFCONTINUED(status)) return 0xffff;
#endif
    return 0;
}
#endif


void gnu_spawn_exit(GnuProcess* p) {
    fflush(stdout);
    fflush(stderr);
#ifndef _WIN32
    if (p->faulted && gnu_host_signal(p->faulted)) {
        /* the parent sees the signal, as it would of the program on Linux; no core of the runtime */
        struct rlimit none = {0, 0};
        int sig = gnu_host_signal(p->faulted);
        (void)setrlimit(RLIMIT_CORE, &none);
        signal(sig, SIG_DFL);
        if (sig != SIGKILL && sig != SIGSTOP) {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, sig);
            sigprocmask(SIG_UNBLOCK, &set, NULL);
        }
        raise(sig);
    }
#endif
    exit(p->exit_code);
}

/* ---- reading execve's arrays ---- */

static void free_strings(char** strings) {
    size_t i;
    if (!strings) return;
    for (i = 0; strings[i]; ++i) free(strings[i]);
    free(strings);
}

static int read_strings(GnuProcess* p, uint64_t address, char*** out, size_t* total) {
    size_t count = 0, capacity = 16;
    char** strings = (char**)calloc(capacity, sizeof *strings);
    if (!strings) return -GNU_ENOMEM;
    while (address) {
        uint64_t pointer;
        char* text;
        size_t n = 0;
        if (gnu_memory_read(&p->memory, address + count * 8, &pointer, 8)) {
            free_strings(strings);
            return -GNU_EFAULT;
        }
        pointer = snx_le64(pointer);
        if (!pointer) break;
        for (;;) {
            unsigned char* c = gnu_memory_access(&p->memory, pointer + n, 1, GNU_PROT_READ);
            if (!c) {
                free_strings(strings);
                return -GNU_EFAULT;
            }
            if (!*c) break;
            if (++n + *total > ARGUMENTS_MAX) {
                free_strings(strings);
                return -GNU_E2BIG;
            }
        }
        *total += n + 1;
        if (count + 2 > capacity) {
            char** bigger = (char**)realloc(strings, capacity * 2 * sizeof *strings);
            if (!bigger) {
                free_strings(strings);
                return -GNU_ENOMEM;
            }
            memset(bigger + capacity, 0, capacity * sizeof *strings);
            strings = bigger;
            capacity *= 2;
        }
        text = (char*)malloc(n + 1);
        if (!text) {
            free_strings(strings);
            return -GNU_ENOMEM;
        }
        memcpy(text, p->memory.base + pointer, n + 1);
        strings[count++] = text;
    }
    *out = strings;
    return 0;
}

static size_t count_strings(char** strings) {
    size_t n = 0;
    while (strings && strings[n]) ++n;
    return n;
}

/* "#!interpreter [argument]": the interpreter's argv before the script's */
static int shebang(char*** argv, const unsigned char* image, size_t size, const char* script, char* interpreter,
                   size_t capacity) {
    char line[256], *p, *argument = NULL, **next;
    size_t n = 0, i, old = count_strings(*argv), j = 0;
    while (n + 2 < size && n < sizeof line - 1 && image[n + 2] != '\n') {
        line[n] = (char)image[n + 2];
        ++n;
    }
    line[n] = 0;
    p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) return -GNU_ENOEXEC;
    for (i = 0; p[i] && p[i] != ' ' && p[i] != '\t'; ++i) {}
    if (p[i]) {
        p[i] = 0;
        argument = p + i + 1;
        while (*argument == ' ' || *argument == '\t') ++argument;
        n = strlen(argument);
        while (n && (argument[n - 1] == ' ' || argument[n - 1] == '\t' || argument[n - 1] == '\r'))
            argument[--n] = 0;
        if (!*argument) argument = NULL;
    }
    snprintf(interpreter, capacity, "%s", p);
    next = (char**)calloc(old + 4, sizeof *next);
    if (!next) return -GNU_ENOMEM;
    next[j++] = snx_strdup(p);
    if (argument) next[j++] = snx_strdup(argument);
    next[j++] = snx_strdup(script);
    for (i = 1; i < old; ++i) next[j++] = (*argv)[i];
    if (old) free((*argv)[0]);
    free(*argv);
    *argv = next;
    return 0;
}

/* execve and execveat: everything that can fail is checked here, before the old program goes */
static int64_t exec(GnuThread* t, int dirfd, uint64_t path_address, uint64_t argv_address, uint64_t envp_address,
                    uint64_t flags) {
    char path[GNU_VFS_PATH_MAX], guest[GNU_VFS_PATH_MAX], interpreter[GNU_VFS_PATH_MAX];
    unsigned char* image = NULL;
    size_t size = 0, total = 0;
    char **argv = NULL, **envp = NULL;
    GnuExec* e;
    int error, depth;
    GnuProcess* p = gnu_process_of(t);
    if (flags & ~(uint64_t)0x1100) return -GNU_EINVAL; /* AT_EMPTY_PATH, AT_SYMLINK_NOFOLLOW */
    if ((error = gnu_memory_read_string(&p->memory, path_address, path, sizeof path))) return error;
    if (!path[0] && !(flags & 0x1000)) return -GNU_ENOENT;
    if ((error = read_strings(p, argv_address, &argv, &total)) ||
        (error = read_strings(p, envp_address, &envp, &total))) {
        free_strings(argv);
        return error;
    }
    if (!path[0]) {
        /* fexecve: the descriptor's file, by its path */
        snprintf(guest, sizeof guest, "/proc/self/fd/%d", dirfd);
        strcpy(path, guest);
        dirfd = GNU_AT_FDCWD;
    }
    error = gnu_vfs_load(&p->vfs, dirfd, path, &image, &size, guest, sizeof guest);
    for (depth = 0; !error && size >= 2 && image[0] == '#' && image[1] == '!'; ++depth) {
        if (depth == 4) {
            error = -GNU_ELOOP;
            break;
        }
        error = shebang(&argv, image, size, path, interpreter, sizeof interpreter);
        free(image);
        image = NULL;
        if (!error) {
            strcpy(path, interpreter);
            error = gnu_vfs_load(&p->vfs, GNU_AT_FDCWD, path, &image, &size, guest, sizeof guest);
        }
    }
    if (!error && (size < 52 || memcmp(image, "\177ELF", 4) || image[5] != 1 ||
                   !((image[4] == 2 && image[18] == 183) || (image[4] == 1 && image[18] == 40))))
        error = -GNU_ENOEXEC;
    if (!error && !count_strings(argv)) {
        /* Linux gives an empty argv an empty argv[0] */
        free(argv);
        argv = (char**)calloc(2, sizeof *argv);
        if (argv) argv[0] = snx_strdup("");
        if (!argv) error = -GNU_ENOMEM;
    }
    if (error || !(e = (GnuExec*)calloc(1, sizeof *e))) {
        free(image);
        free_strings(argv);
        free_strings(envp);
        return error ? error : -GNU_ENOMEM;
    }
    e->path = snx_strdup(guest);
    e->image = image;
    e->size = size;
    e->argv = argv;
    e->envp = envp;
#ifdef _WIN32
    /* the program's own copy of the runtime, with the process's descriptors: a vfork child's parent goes on, any
       other process waits for it and ends with its status, as MSYS2 replaces a process */
    return gnu_spawn_win32_exec(t, e);
#endif
    /* the point of no return: the other threads go (Linux's de_thread), unless one of them ends the process first */
    if (!gnu_kernel_claim_stop(&t->k, 2)) {
        free(e->path);
        free(e->image);
        free_strings(e->argv);
        free_strings(e->envp);
        free(e);
        gnu_thread_leave(t);
        return -GNU_EAGAIN;
    }
    p->exec = e;
    if (!t->running) return 0; /* tests */
    longjmp(t->exit_target, 2);
}

int gnu_spawn_finish_exec(GnuThread* t) {
    GnuProcess* p = gnu_process_of(t);
    GnuExec* e = p->exec;
    char error[512];
    int r;
    gnu_kernel_stop_others(&t->k, 1);
    p->exec = NULL;
    gnu_mutex_lock(&p->kernel.lock);
    t->k.tid = p->kernel.pid; /* the thread that execs takes the process's id, as on Linux */
    __atomic_store_n(&p->kernel.stopping, 0, __ATOMIC_SEQ_CST);
    p->kernel.stopper = NULL;
    gnu_mutex_unlock(&p->kernel.lock);
    gnu_vfs_close_on_exec(&p->vfs);
    /* handlers go back to the default action; what is ignored stays ignored, as the mask and pending signals stay */
    gnu_signal_exec(&t->k);
    t->k.clear_tid = 0;
    t->k.robust_list = 0;
    gnu_jit_reset(&p->jit);
    gnu_elf_destroy(&p->elf);
    gnu_elf_destroy(&p->interp);
    gnu_memory_destroy(&p->memory);
    r = gnu_process_load(p, t, e->path, e->image, e->size, (int)count_strings(e->argv), (const char* const*)e->argv,
                         (const char* const*)e->envp, error, sizeof error);
    if (r) {
        /* past the point of no return: Linux ends the process */
        fprintf(stderr, "scarletnx-gnuabi: %s: %s\n", e->path, error);
        p->faulted = 11;
        p->exited = 1;
        p->exit_code = 128 + 11;
    }
#ifndef _WIN32
    if (p->vfork_fd >= 0) {
        close(p->vfork_fd); /* the parent waiting in clone goes on */
        p->vfork_fd = -1;
    }
#endif
    gnu_start_sampler(p);
    free(e->path);
    free(e->image);
    free_strings(e->argv);
    free_strings(e->envp);
    free(e);
    return r;
}

/* ---- clone ---- */

static int64_t clone_process(GnuThread* t, uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t tls,
                             uint64_t child_tid) {
    if (flags & GNU_CLONE_THREAD) return gnu_thread_clone(t, flags, stack, parent_tid, tls, child_tid);
    if ((flags & GNU_CLONE_SIGHAND) && !(flags & GNU_CLONE_VM)) return -GNU_EINVAL;
    /* a process that shares the memory without being a thread: only a vfork's, which execs or exits at once */
    if ((flags & GNU_CLONE_VM) && !(flags & GNU_CLONE_VFORK)) return -GNU_EINVAL;
#ifdef _WIN32
    if (!(flags & GNU_CLONE_VFORK)) return -GNU_ENOSYS;
    return gnu_spawn_win32_vfork(t, flags, stack, parent_tid, tls, child_tid);
#else
    {
        GnuProcess* p = gnu_process_of(t);
        int sync[2] = {-1, -1};
        pid_t pid;
        uint32_t id;
        if ((flags & GNU_CLONE_VFORK) && pipe(sync)) return gnu_host_errno(errno);
        fflush(stdout);
        fflush(stderr);
        gnu_fork_prepare(p);
        pid = fork();
        if (pid)
            gnu_fork_parent(p);
        else
            gnu_fork_child(t, (uint32_t)getpid());
        if (pid < 0) {
            int error = gnu_host_errno(errno);
            if (sync[0] >= 0) {
                close(sync[0]);
                close(sync[1]);
            }
            return error;
        }
        if (!pid) {
            /* the compiled code the child runs, its own (a PowerPC64 host shares the arena's pages with the parent
               otherwise) */
            if (snx_recompiler_forked(p->jit.recompiler)) _exit(127);
            if (sync[0] >= 0) close(sync[0]);
            p->vfork_fd = sync[1];
            gnu_vfs_set_pid(&p->vfs, p->kernel.pid);
            if (stack) t->cpu.x[31] = stack;
            if (flags & GNU_CLONE_SETTLS) t->cpu.tpidr = tls;
            id = snx_le32(p->kernel.pid);
            if (flags & GNU_CLONE_CHILD_SETTID) (void)gnu_memory_write(&p->memory, child_tid, &id, 4);
            t->k.clear_tid = (flags & GNU_CLONE_CHILD_CLEARTID) ? child_tid : 0;
            return 0;
        }
        id = snx_le32((uint32_t)pid);
        if (flags & GNU_CLONE_PARENT_SETTID) (void)gnu_memory_write(&p->memory, parent_tid, &id, 4);
        if (sync[0] >= 0) {
            char byte;
            close(sync[1]);
            while (read(sync[0], &byte, 1) < 0 && errno == EINTR) {}
            close(sync[0]);
        }
        return pid;
    }
#endif
}

/* ---- wait ---- */

static int64_t wait4_call(GnuProcess* p, const uint64_t a[6]) {
    int32_t pid = (int32_t)a[0];
    uint64_t options = a[2];
    if (options & ~(uint64_t)(1 | 2 | 8 | 0x20000000 | 0x40000000 | 0x80000000)) return -GNU_EINVAL;
#ifdef _WIN32
    return gnu_spawn_win32_wait(p, pid, a[1], (options & 1) != 0, a[3]);
#else
    {
        int status = 0, host_options = 0;
        pid_t r;
        if (options & 1) host_options |= WNOHANG;
        if (options & 2) host_options |= WUNTRACED;
#ifdef WCONTINUED
        if (options & 8) host_options |= WCONTINUED;
#endif
        r = waitpid((pid_t)pid, &status, host_options);
        if (r < 0) return gnu_host_errno(errno);
        if (r > 0 && a[1]) {
            uint32_t s = snx_le32(guest_status(status));
            if (gnu_memory_write(&p->memory, a[1], &s, 4)) return -GNU_EFAULT;
        }
        if (a[3]) {
            unsigned char usage[144];
            memset(usage, 0, sizeof usage);
            if (gnu_memory_write(&p->memory, a[3], usage, sizeof usage)) return -GNU_EFAULT;
        }
        return r;
    }
#endif
}

static int64_t waitid_call(GnuProcess* p, const uint64_t a[6]) {
#ifdef _WIN32
    (void)p;
    (void)a;
    return -GNU_ENOSYS;
#else
    idtype_t type;
    siginfo_t info;
    unsigned char out[128];
    int options = 0;
    if (a[0] == 0)
        type = P_ALL;
    else if (a[0] == 1)
        type = P_PID;
    else if (a[0] == 2)
        type = P_PGID;
    else
        return -GNU_EINVAL;
    if (a[3] & 1) options |= WNOHANG;
    if (a[3] & 2) options |= WSTOPPED;
    if (a[3] & 4) options |= WEXITED;
    if (a[3] & 8) options |= WCONTINUED;
    if (a[3] & 0x1000000) options |= WNOWAIT;
    memset(&info, 0, sizeof info);
    if (waitid(type, (id_t)a[1], &info, options)) return gnu_host_errno(errno);
    memset(out, 0, sizeof out);
    if (info.si_pid) {
        int code = info.si_code == CLD_EXITED ? 1 : info.si_code == CLD_KILLED ? 2 : info.si_code == CLD_DUMPED ? 3
                   : info.si_code == CLD_TRAPPED                                ? 4
                   : info.si_code == CLD_STOPPED                                ? 5
                                                                                : 6;
        int status = info.si_code == CLD_EXITED ? info.si_status : gnu_guest_signal(info.si_status);
        gnu_vfs_put32(out, 17); /* SIGCHLD */
        gnu_vfs_put32(out + 8, (uint32_t)code);
        gnu_vfs_put32(out + 16, (uint32_t)info.si_pid);
        gnu_vfs_put32(out + 20, (uint32_t)info.si_uid);
        gnu_vfs_put32(out + 24, (uint32_t)status);
    }
    if (a[2] && gnu_memory_write(&p->memory, a[2], out, sizeof out)) return -GNU_EFAULT;
    if (a[4]) {
        unsigned char usage[144];
        memset(usage, 0, sizeof usage);
        if (gnu_memory_write(&p->memory, a[4], usage, sizeof usage)) return -GNU_EFAULT;
    }
    return 0;
#endif
}

bool gnu_spawn_syscall(GnuThread* t, uint64_t number, const uint64_t a[6], int64_t* result) {
    GnuProcess* p = gnu_process_of(t);
    switch (number) {
        case 220: *result = clone_process(t, a[0], a[1], a[2], a[3], a[4]); return true;
        case 435: *result = -GNU_ENOSYS; return true; /* clone3: the C library falls back to clone */
        case 221: *result = exec(t, GNU_AT_FDCWD, a[0], a[1], a[2], 0); return true;
        case 281: *result = exec(t, (int)a[0], a[1], a[2], a[3], a[4]); return true;
        case 260: *result = wait4_call(p, a); return true;
        case 95: *result = waitid_call(p, a); return true;
        case 59: { /* pipe2(fds, flags) */
            int fds[2];
            int32_t out[2];
            int error = gnu_vfs_pipe(&p->vfs, (unsigned)a[1], fds);
            if (error) {
                *result = error;
                return true;
            }
            out[0] = (int32_t)snx_le32((uint32_t)fds[0]);
            out[1] = (int32_t)snx_le32((uint32_t)fds[1]);
            *result = gnu_memory_write(&p->memory, a[0], out, sizeof out);
            if (*result) {
                uint64_t close_args[6] = {0};
                int64_t ignored;
                close_args[0] = (uint64_t)fds[0];
                gnu_vfs_syscall(&p->vfs, &p->memory, 57, close_args, &ignored);
                close_args[0] = (uint64_t)fds[1];
                gnu_vfs_syscall(&p->vfs, &p->memory, 57, close_args, &ignored);
            }
            return true;
        }
#ifndef _WIN32
        case 154: *result = setpgid((pid_t)a[0], (pid_t)a[1]) ? gnu_host_errno(errno) : 0; return true;
        case 155: *result = getpgid((pid_t)a[0]) < 0 ? gnu_host_errno(errno) : getpgid((pid_t)a[0]); return true;
        case 156: *result = getsid((pid_t)a[0]) < 0 ? gnu_host_errno(errno) : getsid((pid_t)a[0]); return true;
        case 157: *result = setsid() < 0 ? gnu_host_errno(errno) : p->kernel.pid; return true;
        case 166: *result = (int64_t)umask((mode_t)(a[0] & 0777)); return true;
        case 173: *result = getppid(); return true;
#else
        case 154: *result = 0; return true;
        case 155:
        case 156: *result = p->kernel.pid; return true;
        case 157: *result = p->kernel.pid; return true;
        case 166: *result = 022; return true;
        case 173: *result = 1; return true;
#endif
    }
    return false;
}

/* ---- the options a program's own copy of the runtime is started with (Windows' execve) ---- */

static char** forwarded; /* NULL-ended, as an argv */
void gnu_spawn_set_options(int count, const char* const* options) {
    int i;
    forwarded = (char**)calloc((size_t)count + 1, sizeof *forwarded);
    if (!forwarded) return;
    for (i = 0; i < count; ++i) forwarded[i] = snx_strdup(options[i]);
}

#ifdef _WIN32
typedef struct GnuVfork {
    CPU cpu; /* the parent's registers, at its clone call */
    uint64_t clear_tid, signal_mask;
    SnxLinuxSigaction actions[65];
    struct GnuVfsTable* descriptors;
    uint32_t pid;
} GnuVfork;
typedef struct GnuChild {
    uint32_t pid;
    HANDLE process; /* NULL once it is a status (a vfork child that exited before its exec) */
    uint32_t status;
} GnuChild;

static uint32_t next_pid;

static void add_child(GnuProcess* p, uint32_t pid, HANDLE process, uint32_t status) {
    GnuChild* bigger = (GnuChild*)realloc(p->children, (p->child_count + 1) * sizeof *bigger);
    if (!bigger) {
        if (process) CloseHandle(process);
        return;
    }
    p->children = bigger;
    p->children[p->child_count].pid = pid;
    p->children[p->child_count].process = process;
    p->children[p->child_count].status = status;
    ++p->child_count;
}

int64_t gnu_spawn_win32_vfork(GnuThread* t, uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t tls,
                              uint64_t child_tid) {
    GnuProcess* p = gnu_process_of(t);
    GnuVfork* v;
    uint32_t id;
    (void)parent_tid;
    if (p->vfork_parent) return -GNU_EAGAIN; /* one vfork child at a time runs in the process */
    v = (GnuVfork*)calloc(1, sizeof *v);
    if (!v) return -GNU_ENOMEM;
    v->cpu = t->cpu;
    v->clear_tid = t->k.clear_tid;
    v->signal_mask = t->k.signal_mask;
    memcpy(v->actions, p->kernel.actions, sizeof v->actions);
    v->pid = p->kernel.pid;
    v->descriptors = gnu_vfs_save_table(&p->vfs);
    if (!v->descriptors) {
        free(v);
        return -GNU_ENOMEM;
    }
    if (!next_pid) next_pid = p->kernel.pid * 16 + 1;
    p->vfork_parent = v;
    p->kernel.pid = t->k.tid = id = next_pid++;
    /* the child returns 0 from clone, on its own stack, with the registers its parent had */
    if (stack) t->cpu.x[31] = stack;
    if (flags & GNU_CLONE_SETTLS) t->cpu.tpidr = tls;
    if (flags & GNU_CLONE_CHILD_SETTID) (void)gnu_memory_write(&p->memory, child_tid, &id, 4);
    t->k.clear_tid = (flags & GNU_CLONE_CHILD_CLEARTID) ? child_tid : 0;
    return 0;
}

/* the parent again, its clone returning pid: the child's host frames go with a longjmp to the runtime's loop */
static void resume_parent(GnuThread* t, uint32_t pid) {
    GnuProcess* p = gnu_process_of(t);
    GnuVfork* v = p->vfork_parent;
    p->vfork_parent = NULL;
    t->cpu = v->cpu;
    t->cpu.x[0] = pid;
    t->k.clear_tid = v->clear_tid;
    t->k.signal_mask = v->signal_mask;
    memcpy(p->kernel.actions, v->actions, sizeof p->kernel.actions);
    p->kernel.pid = t->k.tid = v->pid;
    gnu_vfs_restore_table(&p->vfs, v->descriptors);
    free(v);
    longjmp(t->exit_target, 3);
}

void gnu_spawn_win32_child_exit(GnuThread* t, int status) {
    GnuProcess* p = gnu_process_of(t);
    uint32_t pid = p->kernel.pid;
    p->exited = 0;
    add_child(p, pid, NULL, (uint32_t)(status & 0xff) << 8);
    resume_parent(t, pid);
}

/* a Windows command line argument, quoted as CommandLineToArgvW reads it back */
static void quote(wchar_t** line, size_t* used, size_t* capacity, const char* text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t* w = (wchar_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof *w);
    size_t i, needed;
    if (!w) return;
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, text, -1, w, n)) w[0] = 0;
    needed = *used + wcslen(w) * 2 + 4;
    if (needed > *capacity) {
        wchar_t* bigger;
        while (needed > *capacity) *capacity *= 2;
        bigger = (wchar_t*)realloc(*line, *capacity * sizeof *bigger);
        if (!bigger) {
            free(w);
            return;
        }
        *line = bigger;
    }
    if (*used) (*line)[(*used)++] = L' ';
    (*line)[(*used)++] = L'"';
    for (i = 0; w[i]; ++i) {
        size_t slashes = 0;
        while (w[i] == L'\\') {
            ++slashes;
            ++i;
        }
        if (!w[i] || w[i] == L'"') slashes *= 2;
        while (slashes--) (*line)[(*used)++] = L'\\';
        if (!w[i]) break;
        if (w[i] == L'"') (*line)[(*used)++] = L'\\';
        (*line)[(*used)++] = w[i];
    }
    (*line)[(*used)++] = L'"';
    (*line)[*used] = 0;
    free(w);
}

int64_t gnu_spawn_win32_exec(GnuThread* t, GnuExec* e) {
    GnuProcess* p = gnu_process_of(t);
    char host[8192], option[64];
    wchar_t self[32768], *line, *environment = NULL;
    size_t used = 0, capacity = 1024, total = 0, at = 0, i;
    HANDLE inherited[GNU_VFS_FD_MAX];
    int count = 0, fd, error;
    STARTUPINFOW startup;
    PROCESS_INFORMATION started;
    BOOL ok;
    error = gnu_vfs_host_path(&p->vfs, e->path, host, sizeof host);
    if (error || !GetModuleFileNameW(NULL, self, 32768)) {
        free(e->path);
        free(e->image);
        free_strings(e->argv);
        free_strings(e->envp);
        free(e);
        return error ? error : -GNU_ENOEXEC;
    }
    line = (wchar_t*)calloc(capacity, sizeof *line);
    if (!line) return -GNU_ENOMEM;
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, self, -1, NULL, 0, NULL, NULL);
        char* narrow = (char*)malloc((size_t)(n > 0 ? n : 1));
        if (narrow && n > 0 && WideCharToMultiByte(CP_UTF8, 0, self, -1, narrow, n, NULL, NULL)) quote(&line, &used, &capacity, narrow);
        free(narrow);
    }
    for (i = 0; forwarded && forwarded[i]; ++i) quote(&line, &used, &capacity, forwarded[i]);
    /* the child's descriptors, as inheritable handles the new runtime adopts at the same numbers */
    for (fd = 0; fd < GNU_VFS_FD_MAX; ++fd) {
        int crt = gnu_vfs_host_descriptor(&p->vfs, fd);
        HANDLE handle, copy;
        if (crt < 0 || (handle = (HANDLE)_get_osfhandle(crt)) == INVALID_HANDLE_VALUE) continue;
        if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
            continue;
        inherited[count++] = copy;
        snprintf(option, sizeof option, "--fd=%d:%llu", fd, (unsigned long long)(uintptr_t)copy);
        quote(&line, &used, &capacity, option);
    }
    quote(&line, &used, &capacity, "--cwd");
    quote(&line, &used, &capacity, gnu_vfs_cwd(&p->vfs));
    quote(&line, &used, &capacity, "-0");
    quote(&line, &used, &capacity, e->argv[0]);
    quote(&line, &used, &capacity, "--");
    quote(&line, &used, &capacity, host);
    for (i = 1; e->argv[i]; ++i) quote(&line, &used, &capacity, e->argv[i]);
    /* the environment block: VAR=VALUE strings, each NUL-terminated, and an empty one */
    for (i = 0; e->envp && e->envp[i]; ++i) total += (size_t)MultiByteToWideChar(CP_UTF8, 0, e->envp[i], -1, NULL, 0);
    environment = (wchar_t*)calloc(total + 2, sizeof *environment);
    for (i = 0; environment && e->envp && e->envp[i]; ++i)
        at += (size_t)MultiByteToWideChar(CP_UTF8, 0, e->envp[i], -1, environment + at, (int)(total - at + 1));
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    ok = CreateProcessW(self, line, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, environment, NULL, &startup,
                        &started);
    for (i = 0; i < (size_t)count; ++i) CloseHandle(inherited[i]);
    free(line);
    free(environment);
    free(e->path);
    free(e->image);
    free_strings(e->argv);
    free_strings(e->envp);
    free(e);
    if (!ok) return -GNU_ENOEXEC;
    CloseHandle(started.hThread);
    if (!p->vfork_parent) {
        DWORD code = 1;
        WaitForSingleObject(started.hProcess, INFINITE);
        GetExitCodeProcess(started.hProcess, &code);
        fflush(stdout);
        fflush(stderr);
        ExitProcess(code);
    }
    add_child(p, (uint32_t)started.dwProcessId, started.hProcess, 0);
    resume_parent(t, (uint32_t)started.dwProcessId);
    return 0;
}

int64_t gnu_spawn_win32_wait(GnuProcess* p, int32_t pid, uint64_t status_address, int no_hang, uint64_t usage) {
    size_t i, n = 0;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    size_t which[MAXIMUM_WAIT_OBJECTS];
    for (;;) {
        /* a child's status that is there already: one that exited before exec, or one that ended */
        for (i = 0; i < p->child_count; ++i) {
            GnuChild* c = &p->children[i];
            DWORD code;
            if (pid > 0 && c->pid != (uint32_t)pid) continue;
            if (c->process && WaitForSingleObject(c->process, 0) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(c->process, &code)) {
                c->status = (uint32_t)(code & 0xff) << 8;
                CloseHandle(c->process);
                c->process = NULL;
            }
            if (!c->process) {
                uint32_t found = c->pid, s = c->status;
                memmove(c, c + 1, (p->child_count - i - 1) * sizeof *c);
                --p->child_count;
                if (status_address && gnu_memory_write(&p->memory, status_address, &s, 4)) return -GNU_EFAULT;
                if (usage) {
                    unsigned char none[144] = {0};
                    if (gnu_memory_write(&p->memory, usage, none, sizeof none)) return -GNU_EFAULT;
                }
                return found;
            }
        }
        n = 0;
        for (i = 0; i < p->child_count && n < MAXIMUM_WAIT_OBJECTS; ++i)
            if (pid <= 0 || p->children[i].pid == (uint32_t)pid) {
                which[n] = i;
                handles[n++] = p->children[i].process;
            }
        if (!n) return -GNU_ECHILD;
        if (no_hang) return 0;
        if (WaitForMultipleObjects((DWORD)n, handles, FALSE, INFINITE) == WAIT_FAILED) return -GNU_ECHILD;
        (void)which;
    }
}

int64_t gnu_spawn_win32_kill(GnuProcess* p, int32_t pid, int sig) {
    size_t i;
    for (i = 0; i < p->child_count; ++i)
        if (p->children[i].pid == (uint32_t)pid) {
            if (!sig) return 0;
            SnxSigDefault action = snx_linux_signal_default(sig);
            if (p->children[i].process && (action == SNX_SIGDFL_TERM || action == SNX_SIGDFL_CORE))
                TerminateProcess(p->children[i].process, 128 + (unsigned)sig);
            return 0;
        }
    return -GNU_ESRCH;
}
#endif
