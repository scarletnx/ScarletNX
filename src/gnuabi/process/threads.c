/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GNU ABI runtime's guest threads over the kernel library's (gnuabi/kernel/kernel.h), and the kernel's
   operations on this runtime's process. A thread created by clone(CLONE_THREAD) is a host thread with a stack of the
   size the guest's main thread has (main.c), as the guest's calls are host calls. Its tid is the host thread's id on
   Linux, so tgkill from another process reaches it; elsewhere a number of the runtime's own */
#ifndef _WIN32
#define _GNU_SOURCE /* syscall(SYS_gettid) */
#endif
#include "gnuabi/process/process.h"
#include "gnuabi/runtime/execution.h"
#include "common/network/net.h"
#include "common/utilities/endian.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

/* the guest's host threads reserve this much stack, which the host commits as it is used */
#define GUEST_STACK ((size_t)1 << 30)

enum { GNU_CLONE_VM = 0x100, GNU_CLONE_SIGHAND = 0x800, GNU_CLONE_VFORK = 0x4000, GNU_CLONE_SETTLS = 0x80000,
       GNU_CLONE_PARENT_SETTID = 0x100000, GNU_CLONE_CHILD_CLEARTID = 0x200000, GNU_CLONE_CHILD_SETTID = 0x1000000 };

static struct {
    uint32_t next_tid; /* the tids of a host that has no thread ids of its own */
} g_threads;

GnuThread* gnu_thread_self(void) {
    GnuKernelThread* k = gnu_kernel_self();
    return k ? (GnuThread*)k->runtime : NULL;
}
void gnu_thread_set_self(GnuThread* t) {
    gnu_kernel_set_self(t ? &t->k : NULL);
    snx_net_wake_bind(t ? t->wake : NULL);
}

GnuThread* gnu_thread_new(GnuProcess* p, uint32_t tid) {
    GnuThread* t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->cpu.thread = t;
    t->wake = snx_net_wake_new(); /* NULL without one: a signal reaches a network wait through the host signal alone */
    if (gnu_kernel_thread_add(&p->kernel, &t->k, tid, &t->cpu, t)) {
        snx_net_wake_free(t->wake);
        free(t);
        return NULL;
    }
    return t;
}
void gnu_thread_free(GnuThread* t) {
    gnu_kernel_thread_destroy(&t->k);
    snx_net_wake_free(t->wake);
    free(t);
}

#if defined(__linux__)
static uint32_t host_tid(void) { return (uint32_t)syscall(SYS_gettid); }
#else
static uint32_t host_tid(void) { return 0; }
#endif

/* what the host thread of a guest thread does: the guest's code until the thread ends, or the process with it */
static void thread_main(GnuThread* t) {
    GnuProcess* p = gnu_process_of(t);
    GnuKernel* k = &p->kernel;
    gnu_thread_set_self(t);
    /* a new thread (gnu_thread_clone): its tid, and then the parent's words of it before it runs */
    gnu_mutex_lock(&k->lock);
#ifndef _WIN32
    t->host = pthread_self(); /* before any kick: the thread blocks in nothing before it */
#endif
    if (!t->started) {
        uint32_t tid = host_tid();
        if (!tid) {
            if (!g_threads.next_tid) g_threads.next_tid = k->pid + 1;
            tid = g_threads.next_tid++;
        }
        t->k.tid = tid;
        t->started = 1;
        gnu_cond_broadcast(&k->changed);
        while (t->started == 1) gnu_cond_wait(&k->changed, &k->lock, -1);
    }
    gnu_mutex_unlock(&k->lock);
    (void)gnu_runtime_run(t);
    OPENSSL_thread_stop(); /* its state for this thread (random bytes) */
    gnu_mutex_lock(&k->lock);
    int ends = p->exited && k->stopper == &t->k;
    gnu_mutex_unlock(&k->lock);
    if (ends) {
        if (p->faulted && p->diagnostic[0]) fprintf(stderr, "scarletnx-gnuabi: %s\n", p->diagnostic);
        gnu_spawn_exit(p); /* never returns */
    }
    gnu_kernel_thread_remove(&t->k);
    gnu_thread_set_self(NULL);
    gnu_thread_free(t);
}

#ifdef _WIN32
static DWORD WINAPI host_main(LPVOID t) {
    thread_main(t);
    return 0;
}
int gnu_thread_spawn(GnuThread* t) {
    HANDLE h = CreateThread(NULL, GUEST_STACK, host_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) return -GNU_EAGAIN;
    CloseHandle(h); /* nothing joins it */
    return 0;
}
#else
static void* host_main(void* t) {
    thread_main(t);
    return NULL;
}
int gnu_thread_spawn(GnuThread* t) {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, GUEST_STACK);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t host;
    int failed = pthread_create(&host, &attributes, host_main, t);
    pthread_attr_destroy(&attributes);
    return failed ? -GNU_EAGAIN : 0;
}
#endif

int64_t gnu_thread_clone(GnuThread* parent, uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t tls,
                         uint64_t child_tid) {
    GnuProcess* p = gnu_process_of(parent);
    GnuKernel* k = &p->kernel;
    /* Linux's rules: a thread shares the signal handlers, which need the shared memory */
    if (!(flags & GNU_CLONE_SIGHAND) || !(flags & GNU_CLONE_VM) || (flags & GNU_CLONE_VFORK)) return -GNU_EINVAL;
    GnuThread* t = gnu_thread_new(p, 0);
    if (!t) return -GNU_EAGAIN;
    t->cpu = parent->cpu; /* the registers of the clone call, which returns 0 in the child */
    t->cpu.thread = t;
    t->cpu.poll = 0;
    t->cpu.excl_on = 0;
    t->cpu.x[0] = 0;
    if (stack) t->cpu.x[31] = stack;
    if (flags & GNU_CLONE_SETTLS) t->cpu.tpidr = tls;
    t->k.clear_tid = (flags & GNU_CLONE_CHILD_CLEARTID) ? child_tid : 0;
    gnu_mutex_lock(&k->lock);
    t->k.signal_mask = parent->k.signal_mask; /* the alternate stack is not inherited (Linux's clone of a thread) */
    gnu_mutex_unlock(&k->lock);
    if (gnu_thread_spawn(t)) {
        gnu_kernel_thread_remove(&t->k);
        gnu_thread_free(t);
        return -GNU_EAGAIN;
    }
    gnu_mutex_lock(&k->lock);
    while (t->started != 1) gnu_cond_wait(&k->changed, &k->lock, -1);
    uint32_t tid = t->k.tid, word = snx_le32(tid);
    /* both words before either thread runs on, as Linux writes them */
    if (flags & GNU_CLONE_PARENT_SETTID) (void)gnu_memory_write(&p->memory, parent_tid, &word, 4);
    if (flags & GNU_CLONE_CHILD_SETTID) (void)gnu_memory_write(&p->memory, child_tid, &word, 4);
    t->started = 2;
    gnu_cond_broadcast(&k->changed);
    gnu_mutex_unlock(&k->lock);
    return tid;
}

void gnu_thread_leave(GnuThread* t) {
    if (t->running) longjmp(t->exit_target, 4);
}

void gnu_thread_drain(GnuThread* t) { snx_net_wake_drain(t->wake); }

int gnu_thread_kick_signal(void) {
#ifdef _WIN32
    return 0;
#elif defined(SIGRTMIN)
    return SIGRTMIN + 2;
#else
    return SIGUSR2;
#endif
}
#ifndef _WIN32
static void kicked(int signal) { (void)signal; }
#endif
void gnu_thread_install_kick(void) {
#ifndef _WIN32
    /* no SA_RESTART: the host call it stops fails with EINTR */
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = kicked;
    sigemptyset(&action.sa_mask);
    (void)sigaction(gnu_thread_kick_signal(), &action, NULL);
#endif
}

/* ---- the kernel's operations (kernel.h's GnuKernelOps) ---- */
static int op_read(void* context, uint64_t address, void* out, size_t size) {
    return gnu_memory_read(&((GnuProcess*)context)->memory, address, out, size);
}
static int op_write(void* context, uint64_t address, const void* in, size_t size) {
    return gnu_memory_write(&((GnuProcess*)context)->memory, address, in, size);
}
static int op_check(void* context, uint64_t address, size_t size, int write) {
    GnuProcess* p = context;
    unsigned want = GNU_PROT_READ | (write ? GNU_PROT_WRITE : 0);
    return gnu_memory_access(&p->memory, address, size, want) ? 0 : -GNU_EFAULT;
}
/* the thread's wait in the network stack ends through its wake, which stays set until it drains it (no race with the
   wait's start), and any other host call through the host signal. A host signal that comes after the thread set
   blocking but before its host call blocks is lost, and that call blocks until it ends on its own */
static void op_kick(void* context, GnuKernelThread* k) {
    (void)context;
    GnuThread* t = k->runtime;
    snx_net_wake_signal(t->wake);
#ifndef _WIN32
    if (t->started) pthread_kill(t->host, gnu_thread_kick_signal());
#endif
}
static void op_end(void* context, GnuKernelThread* k, int status, int signal, const char* reason) {
    (void)context;
    if (signal)
        gnu_process_fault(k->runtime, signal, reason);
    else
        gnu_process_exit(k->runtime, status);
}
static void op_leave(void* context, GnuKernelThread* k) {
    (void)context;
    gnu_thread_leave(k->runtime);
}
static void op_stop(void* context) {
    (void)context;
#ifndef _WIN32
    kill(getpid(), SIGSTOP); /* the whole host process, until SIGCONT */
#endif
}
#ifdef _WIN32
static int64_t op_kill(void* context, int32_t pid, int32_t tid, int sig, int code, uint64_t value) {
    (void)value;
    if (tid > 0 || code != 0) return -GNU_ENOSYS;
    return gnu_spawn_win32_kill(context, pid, sig);
}
#endif
const GnuKernelOps gnu_kernel_ops = {
    op_read, op_write, op_check, op_kick, op_end, op_leave, op_stop, gnu_host_disposition,
#ifdef _WIN32
    op_kill,
#else
    NULL,
#endif
    NULL, /* madvise: advice only (a range's MADV_DONTNEED keeps its bytes) */
};

/* the fork's locks, in the order every other path takes them: the descriptors, the memory map, the JIT, then the
   kernel's (futexes, process) */
void gnu_fork_prepare(GnuProcess* p) {
    gnu_vfs_lock(&p->vfs);
    gnu_memory_lock(&p->memory);
    gnu_jit_lock(&p->jit);
    gnu_kernel_fork_prepare(&p->kernel);
}
void gnu_fork_parent(GnuProcess* p) {
    gnu_kernel_fork_parent(&p->kernel);
    gnu_jit_unlock(&p->jit);
    gnu_memory_unlock(&p->memory);
    gnu_vfs_unlock(&p->vfs);
}
void gnu_fork_child(GnuThread* t, uint32_t pid) {
    GnuProcess* p = gnu_process_of(t);
    GnuKernel* k = &p->kernel;
    /* the other threads' host threads are not in the child: their records go, without their locks, which they may
       have held */
    for (size_t i = 0; i < k->thread_count; i++)
        if (k->threads[i] != &t->k) {
            GnuThread* other = k->threads[i]->runtime;
            snx_net_wake_free(other->wake);
            free(other);
        }
    gnu_kernel_fork_child(&t->k, pid);
    /* the wake's pipe is the parent's too: the child's own */
    snx_net_wake_free(t->wake);
    t->wake = snx_net_wake_new();
    snx_net_wake_bind(t->wake);
    /* the locks again, not unlocked: a recursive one knows its owner by the thread's id, which the child changed */
    gnu_mutex_init(&p->jit.lock, 0);
    gnu_mutex_init(&p->memory.lock, 1);
    gnu_vfs_forked(&p->vfs);
    gnu_host_signals_forked();
}
