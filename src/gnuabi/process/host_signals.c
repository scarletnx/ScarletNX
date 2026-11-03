/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's side of the guest's signals (process.h): what the guest ignores, the host ignores; what it handles comes
   through a pipe that the host handler writes (the only thing it does) to a relay thread, which sends it to the
   kernel as a signal to the process; the rest has the host's default action, which ends the host process as Linux
   ends the guest's */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "gnuabi/process/process.h"
#include "gnuabi/kernel/signals.h"
#include <errno.h>
#include <signal.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef _WIN32
/* the relay: the process the signals go to, and the pipe from their handler */
static struct {
    GnuProcess* process;
    int pipe[2];
    uint32_t relay_for; /* the host process whose relay thread runs */
} g_signals = {NULL, {-1, -1}, 0};
#endif

#ifndef _WIN32
/* a host signal's record through the pipe: written whole by one write, which is atomic below PIPE_BUF */
typedef struct {
    int32_t signo, code, pid;
    uint32_t uid;
    uint64_t value;
} Relayed;
static void relay_handler(int host, siginfo_t* info, void* context) {
    (void)context;
    int saved = errno;
    Relayed r = {gnu_guest_signal(host), info->si_code, info->si_pid, info->si_uid,
                 (uint64_t)(uintptr_t)info->si_value.sival_ptr};
    if (host == SIGCHLD) r.value = (uint64_t)(uint32_t)info->si_status;
    if (g_signals.pipe[1] >= 0 && r.signo) (void)!write(g_signals.pipe[1], &r, sizeof r);
    errno = saved;
}
static void* relay_main(void* unused) {
    (void)unused;
    int fd = g_signals.pipe[0];
    for (;;) {
        Relayed r;
        ssize_t n = read(fd, &r, sizeof r);
        if (n < 0 && errno == EINTR) continue;
        if (n != (ssize_t)sizeof r) return NULL;
        /* the guest's code numbers: SI_USER, SI_QUEUE and SI_TKILL are Linux's on a Linux host, and a kernel's
           code (CLD_EXITED) is positive on every host */
        if (g_signals.process) gnu_signal_send(&g_signals.process->kernel, NULL, r.signo, r.code, r.pid, r.uid, r.value);
    }
}
/* the relay thread of this host process, once */
static void start_relay(void) {
    if (g_signals.relay_for == (uint32_t)getpid()) return;
    if (g_signals.pipe[0] < 0 && pipe(g_signals.pipe)) return;
    (void)fcntl(g_signals.pipe[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(g_signals.pipe[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(g_signals.pipe[1], F_SETFL, O_NONBLOCK); /* a full pipe drops the record, never blocks the handler */
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    /* the relay takes no host signal the guest handles: they stop the guest's host calls instead */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    if (!pthread_create(&thread, &attributes, relay_main, NULL)) g_signals.relay_for = (uint32_t)getpid();
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&attributes);
}
#endif

void gnu_host_signals_forked(void) {
#ifndef _WIN32
    if (g_signals.pipe[0] < 0) return;
    /* the parent's pipe is the parent's relay's: the child's handlers write a pipe of its own */
    int fresh[2];
    if (pipe(fresh)) return;
    close(g_signals.pipe[0]);
    close(g_signals.pipe[1]);
    g_signals.pipe[0] = fresh[0];
    g_signals.pipe[1] = fresh[1];
    start_relay();
#endif
}

/* what the host does with a guest's disposition: ignore what the guest ignores (a write to a closed pipe then fails
   with EPIPE, an ignored SIGCHLD leaves no zombies), relay what it handles, and the default action for the rest, which
   ends the host process as Linux ends the guest's. SIGSEGV and SIGBUS are the runtime's own (execution.c) */
void gnu_host_disposition(void* p, int sig, uint64_t handler) {
#ifdef _WIN32
    (void)p;
    (void)sig;
    (void)handler;
#else
    int host = gnu_host_signal(sig);
    if (!host || host == SIGKILL || host == SIGSTOP || host == SIGSEGV || host == SIGBUS) return;
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    if (handler > SNX_SIG_IGN) {
        g_signals.process = p;
        start_relay();
        action.sa_sigaction = relay_handler;
        action.sa_flags = SA_SIGINFO; /* no SA_RESTART: the guest's call decides with its own action */
    } else
        action.sa_handler = handler == SNX_SIG_IGN ? SIG_IGN : SIG_DFL;
    (void)sigaction(host, &action, NULL);
#endif
}

