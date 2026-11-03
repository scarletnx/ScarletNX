/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's signals (hle_signal.h): the GNU ABI runtime's kernel for the app's process, the delivery of its
   signals into the bridge's handler loop, and bionic's functions over the kernel's system calls */
#if defined(__linux__)
#define _GNU_SOURCE /* SIGRTMIN, syscall */
#endif
#include "android/bridge/hle_signal.h"
#include "android/bridge/hle_vulkan.h"
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/host.h"
#include "gnuabi/kernel/signals.h"
#include "common/utilities/endian.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
#include <sys/thr.h>
#endif

static struct {
    GnuKernel kernel; /* base 0: the guest's addresses are the host's */
    int carrier;  /* the host's signal that kicks a thread, 0 on Windows (an APC) */
    bool verbose; /* --log-signals: each action, send and delivery on stderr */
    bool log, sample; /* signals_set_options' */
} s;

static _Thread_local struct {
    GuestSignals* self;
    /* the mask that the threads this one creates start with (pthread_create) */
    uint64_t inherited;
    /* the depth of the kernel's calls on this thread: the carrier's handler delivers only outside them, where the
       thread holds none of the kernel's locks */
    int in_kernel;
} t_signal;

/* ---- the kernel's operations ---- */

/* the guest's memory, checked as the kernel checks it: a bad address is EFAULT, not a crash. Apps probe the system
   with calls Linux refuses (an rt_sigprocmask of an unmapped set) and expect its errors */
static int op_read(void* context, uint64_t address, void* out, size_t size) {
    (void)context;
    return host_copy_checked(out, (const void*)(uintptr_t)address, size) ? 0 : -GNU_EFAULT;
}
static int op_write(void* context, uint64_t address, const void* in, size_t size) {
    (void)context;
    return host_copy_checked((void*)(uintptr_t)address, in, size) ? 0 : -GNU_EFAULT;
}
static void carrier_deliver(void);
#ifdef _WIN32
/* Windows has no signals: an APC, which runs as the thread enters an alertable wait (SleepEx, the Ex waits). A wait
   that is not alertable (a lock, a condition variable, a read) takes the signal as its HLE call returns */
static void CALLBACK carrier_apc(ULONG_PTR unused) {
    (void)unused;
    carrier_deliver();
}
#endif
/* a thread that waits in a blocking HLE call or runs no guest code: the carrier. By tid where the host can (a thread
   the JVM ended without a word leaves its tid behind, which a pthread_t would make undefined) */
static void op_kick(void* context, GnuKernelThread* k) {
    (void)context;
#if defined(__linux__)
    syscall(SYS_tgkill, (long)getpid(), (long)k->tid, (long)s.carrier);
#elif defined(__FreeBSD__)
    thr_kill((long)k->tid, s.carrier);
#elif defined(__APPLE__)
    /* by pthread_t: macOS's pthread_kill checks the thread against its list, ESRCH for one that ended */
    GuestSignals* t = k->runtime;
    pthread_kill(t->host, s.carrier);
#elif defined(_WIN32)
    HANDLE h = OpenThread(THREAD_SET_CONTEXT, FALSE, (DWORD)k->tid);
    if (h) {
        QueueUserAPC(carrier_apc, h, 0);
        CloseHandle(h);
    }
#endif
}
/* a default action that ends the process, or exit_group */
static void op_end(void* context, GnuKernelThread* k, int status, int sig, const char* reason) {
    (void)context;
    (void)k;
    if (!sig) _exit(status);
    /* SIGQUIT: on Android, ART's signal catcher takes it (a dump of the threads), so an app never dies of it */
    if (sig == 3) return;
    fprintf(stderr, "scarletnx-android: guest signal %d ends the app: %s\n", sig, reason ? reason : "no handler");
    _exit(128 + sig);
}
static void op_leave(void* context, GnuKernelThread* k) {
    (void)context;
    (void)k; /* the bridge never stops its threads: a guest's end is the process's _exit */
}
static void op_stop(void* context) {
    (void)context;
    fprintf(stderr, "scarletnx-android: a guest stop signal would stop the app: ignored\n");
}
/* the guest's addresses are the host's: the host's advice, on the app's own memory */
static int64_t op_madvise(void* context, uint64_t address, uint64_t length, int advice) {
    (void)context;
#if defined(__linux__)
    return madvise((void*)(uintptr_t)address, (size_t)length, advice) ? gnu_host_errno(errno) : 0;
#elif defined(_WIN32)
    (void)address;
    (void)length;
    (void)advice;
    return 0; /* advice only: Windows keeps the pages and their contents */
#else
    /* Linux's numbers (MADV_NORMAL 0 to MADV_DONTNEED 4 are the BSDs' too, MADV_FREE 8 is their 5). The BSDs'
       MADV_DONTNEED keeps the contents, where Linux's gives zero pages again */
    int host = advice <= 4 ? advice : advice == 8 ? MADV_FREE : -1;
    if (host < 0) return 0;
    return madvise((void*)(uintptr_t)address, (size_t)length, host) ? gnu_host_errno(errno) : 0;
#endif
}
static const GnuKernelOps g_ops = {op_read, op_write, NULL, op_kick, op_end, op_leave, op_stop, NULL, NULL,
                                   op_madvise};

/* the blocking flag the kernel kicks by: in a blocking HLE call, or outside guest code */
static void update_blocking(GuestSignals* t) {
    int blocking = __atomic_load_n(&t->parked, __ATOMIC_RELAXED) || !__atomic_load_n(&t->busy, __ATOMIC_RELAXED);
    __atomic_store_n(&t->k.blocking, (uint32_t)blocking, __ATOMIC_SEQ_CST);
}

/* ---- delivery ---- */

static bool is_restart(int64_t r) {
    return r == -GNU_ERESTARTSYS || r == -GNU_ERESTARTNOINTR || r == -GNU_ERESTARTNOHAND ||
           r == -GNU_ERESTART_RESTARTBLOCK;
}

/* the kernel's gnu_signal_deliver with the depth counted. result NULL at a poll or after an HLE call */
static int kernel_deliver(GuestSignals* t, int64_t* result, uint64_t orig_x0) {
    t_signal.in_kernel++;
    int d = gnu_signal_deliver(&t->k, result, orig_x0);
    t_signal.in_kernel--;
    return d;
}

/* the handler that the kernel's delivery entered, run on the thread's guest stack until it returns to <sigreturn>,
   then its frame back into c (rt_sigreturn). False when it left by a longjmp through the end of the host's call
   into guest code: c is then that call's end */
static bool run_handler(CPU* c, GuestSignals* t) {
    int sig = (int)c->x[0];
    c->excl_on = 0; /* an exception clears the exclusive monitor */
    __atomic_add_fetch(&t->delivered, 1, __ATOMIC_RELAXED);
    if (s.verbose)
        fprintf(stderr, "scarletnx-android: signal %d to thread %d: its handler 0x%llx\n", sig, (int)t->k.tid,
                (unsigned long long)c->pc);
    if (!bridge_run_handler(c)) return false;
    t_signal.in_kernel++;
    gnu_signal_return(&t->k);
    t_signal.in_kernel--;
    c->excl_on = 0;
    return true;
}

/* every signal the thread can take, at a point where c's registers are all in place */
static void deliver(CPU* c, GuestSignals* t) {
    int saved_errno = errno;
    /* while it delivers, a carrier on this thread waits for the next point: the handler's own parked calls take it */
    int parked = __atomic_exchange_n(&t->parked, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&t->busy, 1, __ATOMIC_RELAXED);
    update_blocking(t);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    while (gnu_signal_maybe(&t->k) && kernel_deliver(t, NULL, 0) == GNU_DELIVER_HANDLER)
        if (!run_handler(c, t)) break;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&t->busy, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&t->parked, parked, __ATOMIC_RELAXED);
    update_blocking(t);
    errno = saved_errno;
}

/* the carrier: a thread that waits in an HLE call, or runs no guest code, takes its signals now, unless it is in the
   kernel, whose own waits end for the signal and deliver it as they return. Anything else (generated code, an HLE
   call that doesn't wait, a delivery) takes them at its next point */
static void carrier_deliver(void) {
    GuestSignals* t = t_signal.self;
    if (!t || t_signal.in_kernel) return;
    if ((__atomic_load_n(&t->parked, __ATOMIC_RELAXED) || !__atomic_load_n(&t->busy, __ATOMIC_RELAXED)) &&
        gnu_signal_maybe(&t->k))
        deliver(t->cpu, t);
}

void signals_in_export(uint64_t address) {
    if (t_signal.self) __atomic_store_n(&t_signal.self->in_export, address, __ATOMIC_RELAXED);
}

/* --sample: every 5 seconds, each guest thread's pc, x30 and the export it is in, for a stall. The CPU
   fields are read as another thread writes them: a sample, which may be torn between them */
static void* sampler(void* unused) {
    (void)unused;
    for (;;) {
        gnu_sleep(5, 0);
        fprintf(stderr, "sample: %llu Vulkan presents\n", (unsigned long long)hle_vulkan_presents());
        /* each thread in guest code writes its pc at its next poll (a backward branch): the pc of a loop, not of its
           last call. The poll delivers no signal, as none is pending */
        gnu_mutex_lock(&s.kernel.lock);
        for (size_t i = 0; i < s.kernel.thread_count; i++) {
            GuestSignals* t = s.kernel.threads[i]->runtime;
            __atomic_store_n(&t->cpu->poll, 1, __ATOMIC_RELAXED);
        }
        gnu_mutex_unlock(&s.kernel.lock);
        gnu_sleep(0, 20000000);
        gnu_mutex_lock(&s.kernel.lock);
        for (size_t i = 0; i < s.kernel.thread_count; i++) {
            GuestSignals* t = s.kernel.threads[i]->runtime;
            uint64_t pc = __atomic_load_n(&t->cpu->pc, __ATOMIC_RELAXED);
            uint64_t lr = __atomic_load_n(&t->cpu->x[30], __ATOMIC_RELAXED);
            uint64_t e = __atomic_load_n(&t->in_export, __ATOMIC_RELAXED);
            const char* name = e ? bridge_export_name(e) : NULL;
            uint64_t x0 = __atomic_load_n(&t->cpu->x[0], __ATOMIC_RELAXED);
            uint64_t x1 = __atomic_load_n(&t->cpu->x[1], __ATOMIC_RELAXED);
            uint64_t x2 = __atomic_load_n(&t->cpu->x[2], __ATOMIC_RELAXED);
            fprintf(stderr, "sample: thread %d pc 0x%llx lr 0x%llx x0 0x%llx x1 0x%llx x2 0x%llx %s%s\n",
                    (int)t->k.tid, (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)x0,
                    (unsigned long long)x1, (unsigned long long)x2, name ? "in " : "", name ? name : "");
        }
        gnu_mutex_unlock(&s.kernel.lock);
    }
    return NULL;
}

void signals_set_options(bool log, bool sample) {
    s.log = log;
    s.sample = sample;
}

#ifndef _WIN32
static void carrier(int sig, siginfo_t* information, void* context) {
    (void)sig;
    (void)information;
    (void)context;
    carrier_deliver();
}
#endif

void signals_init(void) {
    gnu_kernel_init(&s.kernel, &g_ops, NULL, NULL, host_pid());
    s.kernel.sigreturn = bridge_sigreturn();
    pthread_t thread;
    if (s.sample && !pthread_create(&thread, NULL, sampler, NULL)) pthread_detach(thread);
    s.verbose = s.log;
#ifndef _WIN32
    /* a signal that the JVM leaves alone: HotSpot takes SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGPIPE, SIGXFSZ, SIGQUIT and
       SIGUSR2 (its suspend and resume). On Linux and FreeBSD a real-time one (past glibc's own, which SIGRTMIN is
       after); macOS has none, and its HotSpot leaves SIGUSR1 alone */
#if defined(__APPLE__)
    s.carrier = SIGUSR1;
#else
    s.carrier = SIGRTMIN + 4;
#endif
    struct sigaction a = {0};
    a.sa_sigaction = carrier;
    /* SA_RESTART: a host call the carrier interrupts goes on after the guest's handler, as the guest's SA_RESTART
       ones do. SA_NODEFER: a handler's own parked calls take a nested signal */
    a.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER;
    sigemptyset(&a.sa_mask);
    sigaction(s.carrier, &a, NULL);
#endif
}

void signals_thread_start(GuestSignals* t, CPU* c) {
    t->cpu = c;
    t->host = pthread_self();
    t->parked = t->busy = 0;
    t->delivered = 0;
    t->in_export = 0;
    if (gnu_kernel_thread_add(&s.kernel, &t->k, host_tid(), c, t)) {
        fprintf(stderr, "scarletnx-android: no memory for a guest thread's signals\n");
        abort();
    }
    gnu_mutex_lock(&s.kernel.lock);
    t->k.signal_mask = t_signal.inherited & ~SNX_SIG_UNBLOCKABLE;
    gnu_mutex_unlock(&s.kernel.lock);
    update_blocking(t);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    t_signal.self = t;
    gnu_kernel_set_self(&t->k);
}

void signals_thread_end(GuestSignals* t) {
    t_signal.self = NULL;
    gnu_kernel_set_self(NULL);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    gnu_kernel_thread_remove(&t->k);
    gnu_kernel_thread_destroy(&t->k);
}

void signals_inherit(uint64_t mask) {
    t_signal.inherited = mask;
}
uint64_t signals_mask(void) {
    GuestSignals* t = t_signal.self;
    if (!t) return 0;
    gnu_mutex_lock(&s.kernel.lock);
    uint64_t mask = t->k.signal_mask;
    gnu_mutex_unlock(&s.kernel.lock);
    return mask;
}

void signals_check(CPU* c) {
    GuestSignals* t = t_signal.self;
    if (t && t->cpu == c && gnu_signal_maybe(&t->k)) deliver(c, t);
}

int signals_park(CPU* c) {
    GuestSignals* t = t_signal.self;
    if (!t) return 0;
    int previous = __atomic_exchange_n(&t->parked, 1, __ATOMIC_RELAXED);
    update_blocking(t);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    /* what came before the wait: the carrier that brought it is gone */
    if (gnu_signal_maybe(&t->k)) deliver(c, t);
    return previous;
}
void signals_unpark(int previous) {
    GuestSignals* t = t_signal.self;
    if (!t) return;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&t->parked, previous, __ATOMIC_RELAXED);
    update_blocking(t);
}

void signals_enter(void) {
    GuestSignals* t = t_signal.self;
    if (t) {
        __atomic_add_fetch(&t->busy, 1, __ATOMIC_RELAXED);
        update_blocking(t);
    }
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}
void signals_leave(void) {
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    GuestSignals* t = t_signal.self;
    if (t) {
        __atomic_sub_fetch(&t->busy, 1, __ATOMIC_RELAXED);
        update_blocking(t);
    }
}

/* ---- the kernel's system calls from an HLE call ---- */

/* number with arguments a (guest addresses are host ones), with the signals that stop it: their handlers run in the
   bridge's loop on c, and Linux's restart rules give the result, EINTR or the call again */
static int64_t kernel_call(CPU* c, uint64_t number, const uint64_t a[6], bool* known_out) {
    GuestSignals* t = t_signal.self;
    if (!t) {
        bridge_cpu(); /* a thread's first guest state: its kernel thread too */
        t = t_signal.self;
    }
    for (;;) {
        int64_t r;
        t_signal.in_kernel++;
        bool known = gnu_kernel_syscall(&t->k, number, a, &r);
        t_signal.in_kernel--;
        if (known_out) *known_out = known;
        if (!known) return -GNU_ENOSYS;
        if (!is_restart(r) && !gnu_signal_maybe(&t->k)) return r;
        /* the HLE call's own registers: the handler's frame keeps them, with the pc 4 back for a restart */
        CPU saved = *c;
        int d = kernel_deliver(t, &r, a[0]);
        if (d == GNU_DELIVER_RESTART) continue;
        if (d != GNU_DELIVER_HANDLER) return is_restart(r) ? -GNU_EINTR : r;
        __atomic_add_fetch(&t->busy, 1, __ATOMIC_RELAXED);
        update_blocking(t);
        bool back = run_handler(c, t);
        __atomic_sub_fetch(&t->busy, 1, __ATOMIC_RELAXED);
        update_blocking(t);
        if (!back) return -GNU_EINTR;
        int again = c->pc == saved.pc - 4;
        int64_t final = (int64_t)c->x[0];
        c->pc = saved.pc;
        c->x[0] = saved.x[0];
        if (!again) return final; /* a signal still pending runs as the HLE call returns (call_export) */
    }
}

uint32_t hle_kernel_tid(void) {
    if (!t_signal.self) bridge_cpu(); /* a thread's first guest state: its kernel thread too */
    return t_signal.self->k.tid;
}

long hle_kernel_syscall(long number, const uint64_t a[6], CPU* c, bool* known) {
    if (s.verbose && ((number >= 129 && number <= 138) || number == 240))
        fprintf(stderr, "scarletnx-android: syscall(%ld, 0x%llx, 0x%llx, 0x%llx, 0x%llx) from 0x%llx\n", number,
                (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2],
                (unsigned long long)a[3], (unsigned long long)c->x[30]);
    uint64_t args[6];
    memcpy(args, a, sizeof args);
    /* kill(0, sig): the app's own process group is the JVM's too: the guest's signal stays the guest's */
    if (number == 129 && (int32_t)args[0] == 0) args[0] = host_pid();
    return (long)kernel_call(c, (uint64_t)number, args, known);
}

/* ---- bionic's functions ---- */

/* the kernel's result as bionic's: 0, or -1 with errno */
static void result(CPU* c, int64_t r) {
    if (r < 0) {
        hle_set_errno((int)-r);
        X(0) = (uint64_t)-1;
    } else
        X(0) = (uint64_t)r;
}
static int64_t call6(CPU* c, uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    const uint64_t a[6] = {a0, a1, a2, a3, 0, 0};
    return kernel_call(c, number, a, NULL);
}

/* bionic's struct sigaction on LP64: sa_flags (an int), the handler, sa_mask, sa_restorer */
static SnxLinuxSigaction bionic_action(uint64_t address) {
    uint8_t p[32];
    memcpy(p, (const void*)(uintptr_t)address, sizeof p);
    SnxLinuxSigaction a = {snx_ld_le64(p + 8), snx_ld_le32(p), snx_ld_le64(p + 24), snx_ld_le64(p + 16)};
    return a;
}
static void bionic_store(uint64_t address, const SnxLinuxSigaction* a) {
    uint8_t p[32] = {0};
    snx_st_le32(p, (uint32_t)a->flags);
    snx_st_le64(p + 8, a->handler);
    snx_st_le64(p + 16, a->mask);
    snx_st_le64(p + 24, a->restorer);
    memcpy((void*)(uintptr_t)address, p, sizeof p);
}

/* rt_sigaction with the kernel's structures: 0 or -errno */
static int64_t set_action(CPU* c, int sig, const SnxLinuxSigaction* action, SnxLinuxSigaction* old) {
    uint8_t in[32], out[32];
    if (action) snx_linux_sigaction_encode(in, action);
    int64_t r = call6(c, 134, (uint64_t)sig, action ? (uint64_t)(uintptr_t)in : 0, (uint64_t)(uintptr_t)out, 8);
    if (r) return r;
    if (old) *old = snx_linux_sigaction_decode(out);
    if (!action) return 0;
    if (s.verbose)
        fprintf(stderr, "scarletnx-android: signal %d: handler 0x%llx, flags 0x%llx, mask 0x%llx\n", sig,
                (unsigned long long)action->handler, (unsigned long long)action->flags,
                (unsigned long long)action->mask);
    /* compiled now: a handler that a waiting thread runs from the carrier compiles nothing there */
    if (action->handler > SNX_SIG_IGN) bridge_prepare(action->handler);
    return 0;
}

static void g_sigaction(CPU* c, void* data) {
    (void)data;
    SnxLinuxSigaction action, old;
    if (X(1)) action = bionic_action(X(1));
    int64_t r = set_action(c, (int)X(0), X(1) ? &action : NULL, &old);
    if (!r && X(2)) bionic_store(X(2), &old);
    result(c, r);
}
/* signal(), bsd_signal(): BSD's semantics, SA_RESTART. sysv_signal(): SA_RESETHAND and SA_NODEFER */
static void install(CPU* c, uint64_t flags) {
    SnxLinuxSigaction action = {X(1), flags, 0, 0}, old;
    if (set_action(c, (int)X(0), &action, &old)) {
        hle_set_errno(GNU_EINVAL);
        X(0) = (uint64_t)-1; /* SIG_ERR */
        return;
    }
    X(0) = old.handler;
}
static void g_signal(CPU* c, void* data) {
    (void)data;
    install(c, SNX_SA_RESTART);
}
static void g_sysv_signal(CPU* c, void* data) {
    (void)data;
    install(c, SNX_SA_RESETHAND | SNX_SA_NODEFER);
}
static void g_sigprocmask(CPU* c, void* data) {
    (void)data;
    result(c, call6(c, 135, X(0), X(1), X(2), 8));
}
/* pthread_sigmask(): the error number itself */
static void g_pthread_sigmask(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)-call6(c, 135, X(0), X(1), X(2), 8);
}
static void g_sigpending(CPU* c, void* data) {
    (void)data;
    result(c, call6(c, 136, X(0), 8, 0, 0));
}
static void g_sigsuspend(CPU* c, void* data) {
    (void)data;
    result(c, call6(c, 133, X(0), 8, 0, 0));
}
static void g_pause(CPU* c, void* data) {
    (void)data;
    uint8_t mask[8];
    snx_st_le64(mask, signals_mask());
    result(c, call6(c, 133, (uint64_t)(uintptr_t)mask, 8, 0, 0));
}
static void g_sigtimedwait(CPU* c, void* data) {
    (void)data;
    result(c, call6(c, 137, X(0), X(1), X(2), 8));
}
static void g_sigwaitinfo(CPU* c, void* data) {
    (void)data;
    result(c, call6(c, 137, X(0), X(1), 0, 8));
}
/* sigwait(): the error number, and no EINTR (it waits again) */
static void g_sigwait(CPU* c, void* data) {
    (void)data;
    uint64_t set = X(0), out = X(1);
    int64_t sig;
    while ((sig = call6(c, 137, set, 0, 0, 8)) == -GNU_EINTR) {}
    if (sig < 0) {
        X(0) = (uint64_t)-sig;
        return;
    }
    snx_st_le32((void*)(uintptr_t)out, (uint32_t)sig);
    X(0) = 0;
}
static void g_sigaltstack(CPU* c, void* data) {
    (void)data;
    result(c, call6(c, 132, X(0), X(1), 0, 0));
}

static void g_raise(CPU* c, void* data) {
    (void)data;
    /* the handler runs before raise returns, as on Linux */
    result(c, call6(c, 131, host_pid(), host_tid(), X(0), 0));
}
static void g_kill(CPU* c, void* data) {
    (void)data;
    uint64_t pid = (int32_t)X(0) == 0 ? host_pid() : X(0); /* the process group is the JVM's too */
    result(c, call6(c, 129, pid, X(1), 0, 0));
}
static void g_tgkill(CPU* c, void* data) {
    (void)data;
    result(c, call6(c, 131, X(0), X(1), X(2), 0));
}
/* the tid of the guest thread on host thread target, 0 for none */
static uint32_t tid_of(pthread_t target) {
    uint32_t tid = 0;
    gnu_mutex_lock(&s.kernel.lock);
    for (size_t i = 0; i < s.kernel.thread_count && !tid; i++) {
        GuestSignals* t = s.kernel.threads[i]->runtime;
        if (pthread_equal(t->host, target)) tid = t->k.tid;
    }
    gnu_mutex_unlock(&s.kernel.lock);
    return tid;
}
static void g_pthread_kill(CPU* c, void* data) {
    (void)data;
    int sig = (int)X(1);
    if (sig < 0 || sig > 64) {
        X(0) = EINVAL;
        return;
    }
    uint32_t tid = tid_of((pthread_t)X(0));
    int64_t r = tid ? call6(c, 131, host_pid(), tid, (uint64_t)sig, 0) : 0;
    if (!tid && s.verbose)
        fprintf(stderr, "scarletnx-android: signal %d to a thread with no guest state: dropped\n", sig);
    /* a thread that runs no guest code, or that ended since: no handler of the guest's runs there, and it is alive */
    X(0) = r == -GNU_ESRCH ? 0 : (uint64_t)-r;
}
static void g_sigqueue(CPU* c, void* data) {
    (void)data;
    if ((uint32_t)X(0) != host_pid()) {
        result(c, -GNU_EPERM); /* another process: the host has no way to give it the value */
        return;
    }
    uint8_t info[SNX_LINUX_SIGINFO_SIZE] = {0};
#ifdef _WIN32
    uint32_t uid = 0;
#else
    uint32_t uid = (uint32_t)getuid();
#endif
    SnxLinuxSiginfo i = {(int32_t)X(1), SNX_SI_QUEUE, 0, (int32_t)host_pid(), uid, X(2), 0};
    snx_linux_siginfo_encode(info, &i);
    result(c, call6(c, 138, X(0), X(1), (uint64_t)(uintptr_t)info, 0));
}
static void g_rtmin(CPU* c, void* data) {
    (void)data;
    X(0) = 35; /* bionic keeps 32 to 34 (timers, the debugger, profilers) */
}
static void g_rtmax(CPU* c, void* data) {
    (void)data;
    X(0) = 64;
}

const HleSymbol hle_signal[] = {
    {"sigaction", g_sigaction, NULL},
    {"sigaction64", g_sigaction, NULL}, /* sigaction64 and sigset64_t are sigaction's on LP64 */
    {"signal", g_signal, NULL},
    {"bsd_signal", g_signal, NULL},
    {"sysv_signal", g_sysv_signal, NULL},
    {"sigprocmask", g_sigprocmask, NULL},
    {"sigprocmask64", g_sigprocmask, NULL},
    {"pthread_sigmask", g_pthread_sigmask, NULL},
    {"pthread_sigmask64", g_pthread_sigmask, NULL},
    {"sigpending", g_sigpending, NULL},
    {"sigpending64", g_sigpending, NULL},
    {"sigsuspend", g_sigsuspend, NULL},
    {"sigsuspend64", g_sigsuspend, NULL},
    {"pause", g_pause, NULL},
    {"sigtimedwait", g_sigtimedwait, NULL},
    {"sigtimedwait64", g_sigtimedwait, NULL},
    {"sigwaitinfo", g_sigwaitinfo, NULL},
    {"sigwaitinfo64", g_sigwaitinfo, NULL},
    {"sigwait", g_sigwait, NULL},
    {"sigwait64", g_sigwait, NULL},
    {"sigaltstack", g_sigaltstack, NULL},
    {"raise", g_raise, NULL},
    {"kill", g_kill, NULL},
    {"tgkill", g_tgkill, NULL},
    {"pthread_kill", g_pthread_kill, NULL},
    {"sigqueue", g_sigqueue, NULL},
    {"__libc_current_sigrtmin", g_rtmin, NULL},
    {"__libc_current_sigrtmax", g_rtmax, NULL},
    {"", NULL, NULL},
};
