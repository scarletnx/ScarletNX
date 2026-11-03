/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's signals (signals.h). The process's lock guards the actions, every queue and every mask.
   Behavior from Linux (kernel/signal.c, arch/arm64/kernel/signal.c) as signal(7), sigaction(2), sigaltstack(2),
   rt_sigtimedwait(2) and rt_sigqueueinfo(2) document it: one entry of a standard signal and one of each real-time
   one sent, the synchronous signals first, a handler's mask, the restart of a call that a signal stopped, a frame
   that cannot be written. Host signals that the guest handles come through a pipe that their host handler writes
   (the only thing it does) to a relay thread, which sends them to the process */
#ifndef _WIN32
#define _GNU_SOURCE /* sigqueue, syscall */
#endif
#include "gnuabi/kernel/signals.h"
#include "common/utilities/endian.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

#define STOP_SIGNALS (SNX_SIGBIT(19) | SNX_SIGBIT(20) | SNX_SIGBIT(21) | SNX_SIGBIT(22))

/* ---- numbers ---- */
#ifndef _WIN32
int gnu_host_signal(int s) {
    if (0) { /* ... */ }
#define LINUX_SIGNAL(guest, name) else if (guest == s) return name;
#include "gnuabi/abi/linux_signal.inc"
    return 0;
}
int gnu_guest_signal(int s) {
    if (0) { /* ... */ }
#define LINUX_SIGNAL(guest, name) else if (name == s) return guest;
#include "gnuabi/abi/linux_signal.inc"
    return 0;
}
#else
int gnu_host_signal(int s) { return s; }
int gnu_guest_signal(int s) { return s; }
#endif

/* ---- queues, under the process's lock ---- */
static void set_pending(GnuSigQueue* q, uint64_t pending) { __atomic_store_n(&q->pending, pending, __ATOMIC_SEQ_CST); }
static int enqueue(GnuSigQueue* q, const SnxLinuxSiginfo* info) {
    uint64_t bit = SNX_SIGBIT(info->signo);
    if (info->signo < 32 && (q->pending & bit)) return 0; /* a standard signal is pending once */
    if (q->count == GNU_SIGQUEUE_MAX) return info->signo < 32 ? 0 : -GNU_EAGAIN;
    q->entries[q->count++] = *info;
    set_pending(q, q->pending | bit);
    return 0;
}
/* the first entry of sig, out of q */
static SnxLinuxSiginfo dequeue(GnuSigQueue* q, int sig) {
    SnxLinuxSiginfo info = {sig, SNX_SI_KERNEL, 0, 0, 0, 0, 0};
    int more = 0;
    for (uint32_t i = 0; i < q->count; i++) {
        if (q->entries[i].signo != sig) continue;
        info = q->entries[i];
        memmove(q->entries + i, q->entries + i + 1, (q->count - i - 1) * sizeof *q->entries);
        q->count--;
        for (uint32_t j = i; j < q->count && !more; j++) more = q->entries[j].signo == sig;
        break;
    }
    if (!more) set_pending(q, q->pending & ~SNX_SIGBIT(sig));
    return info;
}
/* every entry of the signals in bits goes */
static void discard(GnuSigQueue* q, uint64_t bits) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < q->count; i++)
        if (!(SNX_SIGBIT(q->entries[i].signo) & bits)) q->entries[kept++] = q->entries[i];
    q->count = kept;
    set_pending(q, q->pending & ~bits);
}
static void discard_everywhere(GnuKernel* p, uint64_t bits) {
    discard(&p->queue, bits);
    for (size_t i = 0; i < p->thread_count; i++) discard(&p->threads[i]->queue, bits);
}

/* the signals t takes now: its mask, without the ones it waits for in rt_sigtimedwait */
static uint64_t blocked(const GnuKernelThread* t) { return t->signal_mask & ~t->wait_set & ~SNX_SIG_UNBLOCKABLE; }

int gnu_signal_send(GnuKernel* p, GnuKernelThread* target, int signo, int code, int32_t pid, uint32_t uid,
                    uint64_t value) {
    if (signo < 1 || signo > 64) return -GNU_EINVAL;
    SnxLinuxSiginfo info = {signo, code, 0, pid, uid, value, 0};
    uint64_t bit = SNX_SIGBIT(signo);
    gnu_mutex_lock(&p->lock);
    /* an ignored signal is dropped as it is sent, unless the thread blocks it: the action may change before it
       unblocks it (Linux's sig_ignored) */
    if (snx_linux_signal_ignored(signo, &p->actions[signo]) && !(target && (blocked(target) & bit))) {
        gnu_mutex_unlock(&p->lock);
        return 0;
    }
    /* SIGCONT takes the pending stop signals, and a stop signal a pending SIGCONT */
    if (signo == SNX_SIGCONT) discard_everywhere(p, STOP_SIGNALS);
    if (bit & STOP_SIGNALS) discard_everywhere(p, SNX_SIGBIT(SNX_SIGCONT));
    int r = enqueue(target ? &target->queue : &p->queue, &info);
    if (!r) {
        /* a thread that takes it: the target, or for the process its main thread first, as Linux's
           complete_signal, then any other */
        GnuKernelThread* wake = target && !(blocked(target) & bit) ? target : NULL;
        for (size_t i = 0; !target && !wake && i < p->thread_count; i++)
            if (p->threads[i]->tid == p->pid && !(blocked(p->threads[i]) & bit)) wake = p->threads[i];
        for (size_t i = 0; !target && !wake && i < p->thread_count; i++)
            if (!(blocked(p->threads[i]) & bit)) wake = p->threads[i];
        if (wake) gnu_kernel_kick(wake);
    }
    gnu_mutex_unlock(&p->lock);
    return r;
}

/* the next signal t takes, under the lock: 0 for none */
static int next_locked(const GnuKernelThread* t) {
    const GnuKernel* p = t->kernel;
    return snx_linux_signal_next(t->queue.pending | p->queue.pending, blocked(t));
}

int gnu_signal_maybe(GnuKernelThread* t) {
    GnuKernel* p = t->kernel;
    return __atomic_load_n(&t->interrupt, __ATOMIC_RELAXED) || __atomic_load_n(&p->stopping, __ATOMIC_RELAXED) ||
           ((__atomic_load_n(&t->queue.pending, __ATOMIC_RELAXED) |
             __atomic_load_n(&p->queue.pending, __ATOMIC_RELAXED)) &
            ~t->signal_mask) != 0;
}

int gnu_signal_wanted(GnuKernelThread* t) {
    GnuKernel* p = t->kernel;
    gnu_mutex_lock(&p->lock);
    __atomic_store_n(&t->interrupt, 0, __ATOMIC_SEQ_CST);
    int wanted = next_locked(t) != 0 || (__atomic_load_n(&p->stopping, __ATOMIC_SEQ_CST) && p->stopper != t);
    gnu_mutex_unlock(&p->lock);
    return wanted;
}

int gnu_signal_sleep(GnuKernelThread* t, int64_t deadline) {
    if (gnu_signal_wanted(t)) return 0;
    gnu_mutex_lock(&t->wait_lock);
    for (;;) {
        if (__atomic_load_n(&t->interrupt, __ATOMIC_SEQ_CST)) {
            gnu_mutex_unlock(&t->wait_lock);
            if (gnu_signal_wanted(t)) return 0;
            gnu_mutex_lock(&t->wait_lock);
            continue;
        }
        if (gnu_cond_wait(&t->wake, &t->wait_lock, deadline)) break;
    }
    gnu_mutex_unlock(&t->wait_lock);
    return 1;
}

/* ---- delivery ---- */
static int is_restart(int64_t r) {
    return r == -GNU_ERESTARTSYS || r == -GNU_ERESTARTNOINTR || r == -GNU_ERESTARTNOHAND ||
           r == -GNU_ERESTART_RESTARTBLOCK;
}

/* t's registers onto its stack in a frame for the handler of sig, which it enters: 0, or -1 when the frame does not
   fit (Linux then forces SIGSEGV) */
static int setup_frame(GnuKernelThread* t, const SnxLinuxSiginfo* info, const SnxLinuxSigaction* action) {
    GnuKernel* p = t->kernel;
    CPU* c = t->regs;
    uint64_t record, frame = snx_linux_sigframe_place(c->x[31], action->flags, &t->altstack, &record);
    uint8_t bytes[SNX_LINUX_SIGFRAME_SIZE], si[SNX_LINUX_SIGINFO_SIZE], link[16];
    snx_linux_siginfo_encode(si, info);
    gnu_mutex_lock(&p->lock);
    uint64_t restore = t->saved_mask_valid ? t->saved_mask : t->signal_mask;
    gnu_mutex_unlock(&p->lock);
    snx_linux_sigframe_build(bytes, c, si, restore, &t->altstack, 0);
    snx_linux_sigframe_record(link, c);
    if (frame >= record || gnu_kernel_write(p, frame, bytes, sizeof bytes) ||
        gnu_kernel_write(p, record, link, sizeof link))
        return -1;
    gnu_mutex_lock(&p->lock);
    t->signal_mask = snx_linux_handler_mask(t->signal_mask, info->signo, action);
    t->saved_mask_valid = 0;
    gnu_mutex_unlock(&p->lock);
    uint64_t restorer = (action->flags & SNX_SA_RESTORER) ? action->restorer : p->sigreturn;
    snx_linux_sigframe_enter(c, info->signo, frame, record, action->handler, restorer);
    return 0;
}

/* the process ends by sig (the runtime's GnuKernelOps.end); a return means the runtime ignores it */
static void ended_by(GnuKernelThread* t, int sig, const char* why) {
    char reason[96];
    if (!why) snprintf(reason, sizeof reason, "Guest ended by signal %d", sig);
    t->kernel->ops->end(t->kernel->context, t, 128 + sig, sig, why ? why : reason);
}

int gnu_signal_deliver(GnuKernelThread* t, int64_t* result, uint64_t orig_x0) {
    GnuKernel* p = t->kernel;
    CPU* c = t->regs;
    gnu_kernel_check_stop(t);
    while (gnu_signal_maybe(t)) {
        gnu_mutex_lock(&p->lock);
        __atomic_store_n(&t->interrupt, 0, __ATOMIC_SEQ_CST);
        int sig = next_locked(t);
        if (!sig) {
            gnu_mutex_unlock(&p->lock);
            break;
        }
        SnxLinuxSiginfo info = dequeue(t->queue.pending & SNX_SIGBIT(sig) ? &t->queue : &p->queue, sig);
        SnxLinuxSigaction action = p->actions[sig];
        if (action.handler > SNX_SIG_IGN && (action.flags & SNX_SA_RESETHAND)) p->actions[sig].handler = SNX_SIG_DFL;
        gnu_mutex_unlock(&p->lock);
        if (sig == SNX_SIGKILL || action.handler == SNX_SIG_DFL) {
            switch (snx_linux_signal_default(sig)) {
                case SNX_SIGDFL_IGNORE:
                case SNX_SIGDFL_CONT: continue;
                case SNX_SIGDFL_STOP:
                    if (p->ops->stop) p->ops->stop(p->context);
                    continue;
                default: ended_by(t, sig, NULL); continue; /* back only when the runtime ignores it */
            }
        }
        if (action.handler == SNX_SIG_IGN) continue;
        /* a handler: x0 and pc as the interrupted code sees them when it returns (arm64's do_signal) */
        if (result) {
            int64_t r = *result;
            int again = r == -GNU_ERESTARTNOINTR || (r == -GNU_ERESTARTSYS && (action.flags & SNX_SA_RESTART));
            if (again) {
                c->x[0] = orig_x0;
                c->pc -= 4;
            } else
                c->x[0] = (uint64_t)(is_restart(r) ? -GNU_EINTR : r);
        }
        if (setup_frame(t, &info, &action)) {
            ended_by(t, 11, "The guest's stack has no room for a signal frame");
            return GNU_DELIVER_NONE;
        }
        return GNU_DELIVER_HANDLER;
    }
    if (result && is_restart(*result)) return GNU_DELIVER_RESTART;
    if (result && t->saved_mask_valid) {
        /* the call that set a mask for its wait ends: the thread's own mask again */
        gnu_mutex_lock(&p->lock);
        t->signal_mask = t->saved_mask;
        t->saved_mask_valid = 0;
        gnu_mutex_unlock(&p->lock);
    }
    return GNU_DELIVER_NONE;
}

/* sigaltstack's change to the alternate stack, from the guest's stack_t: 0 or -errno */
static int set_altstack(GnuKernelThread* t, const uint8_t raw[24]) {
    SnxLinuxAltStack s = snx_linux_altstack_decode(raw);
    if (snx_linux_altstack_flags(&t->altstack, t->regs->x[31]) == SNX_SS_ONSTACK) return -GNU_EPERM;
    int32_t mode = s.flags & ~(int32_t)0x80000000; /* SS_AUTODISARM, which this runtime does not do */
    if (mode != 0 && mode != SNX_SS_DISABLE && mode != SNX_SS_ONSTACK) return -GNU_EINVAL;
    if (mode == SNX_SS_DISABLE) {
        SnxLinuxAltStack none = {0, SNX_SS_DISABLE, 0};
        t->altstack = none;
        return 0;
    }
    if (s.size < SNX_MINSIGSTKSZ) return -GNU_ENOMEM;
    s.flags = 0;
    t->altstack = s;
    return 0;
}

void gnu_signal_return(GnuKernelThread* t) {
    GnuKernel* p = t->kernel;
    uint8_t frame[SNX_LINUX_SIGFRAME_SIZE];
    uint64_t mask;
    /* the frame is at the stack pointer, where the handler's return to the restorer left it */
    if (gnu_kernel_read(p, t->regs->x[31], frame, sizeof frame) ||
        !snx_linux_sigframe_restore(t->regs, frame, &mask)) {
        ended_by(t, 11, "rt_sigreturn found no signal frame at the stack pointer");
        return;
    }
    gnu_mutex_lock(&p->lock);
    t->signal_mask = mask;
    gnu_mutex_unlock(&p->lock);
    (void)set_altstack(t, frame + SNX_LINUX_SIGFRAME_UCONTEXT + 16); /* uc_stack, as Linux's restore_altstack */
}

void gnu_signal_exec(GnuKernelThread* t) {
    GnuKernel* p = t->kernel;
    gnu_mutex_lock(&p->lock);
    for (int sig = 1; sig <= 64; ++sig)
        if (p->actions[sig].handler > SNX_SIG_IGN) {
            SnxLinuxSigaction none = {0};
            p->actions[sig] = none;
            if (p->ops->disposition) p->ops->disposition(p->context, sig, SNX_SIG_DFL);
        }
    SnxLinuxAltStack off = {0, SNX_SS_DISABLE, 0};
    t->altstack = off;
    t->saved_mask_valid = 0;
    gnu_mutex_unlock(&p->lock);
}

/* ---- the system calls ---- */
/* in Linux's order (kernel/signal.c's rt_sigaction): the size, the new action's read, the signal, then the old
   action's write */
static int64_t sigaction_call(GnuKernelThread* t, const uint64_t a[6]) {
    GnuKernel* p = t->kernel;
    int sig = (int)a[0];
    if (a[3] != 8) return -GNU_EINVAL;
    uint8_t raw[32];
    SnxLinuxSigaction action = {0};
    if (a[1]) {
        if (gnu_kernel_read(p, a[1], raw, sizeof raw)) return -GNU_EFAULT;
        action = snx_linux_sigaction_decode(raw);
        action.mask &= ~SNX_SIG_UNBLOCKABLE;
    }
    if (a[0] < 1 || a[0] > 64 || (a[1] && (sig == SNX_SIGKILL || sig == SNX_SIGSTOP))) return -GNU_EINVAL;
    gnu_mutex_lock(&p->lock);
    SnxLinuxSigaction old = p->actions[sig];
    if (a[1]) {
        p->actions[sig] = action;
        /* a pending signal whose action is now to ignore it goes (POSIX's sigaction) */
        if (snx_linux_signal_ignored(sig, &action)) discard_everywhere(p, SNX_SIGBIT(sig));
        if (p->ops->disposition) p->ops->disposition(p->context, sig, action.handler);
    }
    gnu_mutex_unlock(&p->lock);
    snx_linux_sigaction_encode(raw, &old);
    return a[2] && gnu_kernel_write(p, a[2], raw, sizeof raw) ? -GNU_EFAULT : 0;
}

/* in Linux's order (kernel/signal.c's rt_sigprocmask): the size, the new set's read, how, then the old set's write */
static int64_t sigprocmask_call(GnuKernelThread* t, const uint64_t a[6]) {
    GnuKernel* p = t->kernel;
    if (a[3] != 8) return -GNU_EINVAL;
    gnu_mutex_lock(&p->lock);
    uint64_t old = t->signal_mask;
    gnu_mutex_unlock(&p->lock);
    if (a[1]) {
        uint8_t raw[8];
        if (gnu_kernel_read(p, a[1], raw, 8)) return -GNU_EFAULT;
        uint64_t mask = snx_ld_le64(raw) & ~SNX_SIG_UNBLOCKABLE;
        int32_t how = (int32_t)a[0];
        if (how < 0 || how > 2) return -GNU_EINVAL;
        gnu_mutex_lock(&p->lock);
        t->signal_mask = how == 0 ? t->signal_mask | mask : how == 1 ? t->signal_mask & ~mask : mask;
        gnu_mutex_unlock(&p->lock);
    }
    uint8_t out[8];
    snx_st_le64(out, old);
    if (a[2] && gnu_kernel_write(p, a[2], out, 8)) return -GNU_EFAULT;
    return 0; /* a signal it unblocked comes at the end of this call */
}

/* a mask for a wait (rt_sigsuspend, ppoll): the thread's own one comes back at the end, or from the handler's frame */
static void temporary_mask(GnuKernelThread* t, uint64_t mask) {
    GnuKernel* p = t->kernel;
    gnu_mutex_lock(&p->lock);
    if (!t->saved_mask_valid) { /* a restarted call keeps the mask it saved first */
        t->saved_mask = t->signal_mask;
        t->saved_mask_valid = 1;
    }
    t->signal_mask = mask & ~SNX_SIG_UNBLOCKABLE;
    gnu_mutex_unlock(&p->lock);
}

int64_t gnu_signal_call_mask(GnuKernelThread* t, uint64_t number, const uint64_t a[6]) {
    GnuKernel* p = t->kernel;
    uint64_t address = 0, size = 8;
    if (number == 73) { /* ppoll(fds, n, timeout, sigmask, size) */
        address = a[3];
        size = a[4];
    } else if (number == 22 || number == 441) { /* epoll_pwait(fd, events, max, timeout, sigmask, size) */
        address = a[4];
        size = a[5];
    } else if (number == 72 && a[5]) { /* pselect6's sixth argument: {const sigset_t*, size_t} */
        uint8_t pair[16];
        if (gnu_kernel_read(p, a[5], pair, sizeof pair)) return -GNU_EFAULT;
        address = snx_ld_le64(pair);
        size = snx_ld_le64(pair + 8);
    }
    if (!address) return 0;
    if (size != 8) return -GNU_EINVAL;
    uint8_t raw[8];
    if (gnu_kernel_read(p, address, raw, 8)) return -GNU_EFAULT;
    temporary_mask(t, snx_ld_le64(raw));
    return 0;
}

static int64_t sigsuspend_call(GnuKernelThread* t, const uint64_t a[6]) {
    if (a[1] != 8) return -GNU_EINVAL;
    uint8_t raw[8];
    if (gnu_kernel_read(t->kernel, a[0], raw, 8)) return -GNU_EFAULT;
    temporary_mask(t, snx_ld_le64(raw));
    while (gnu_signal_sleep(t, -1)) {}
    return -GNU_ERESTARTNOHAND; /* EINTR once the handler ran */
}

static int64_t sigpending_call(GnuKernelThread* t, const uint64_t a[6]) {
    GnuKernel* p = t->kernel;
    if (a[1] > 8) return -GNU_EINVAL;
    gnu_mutex_lock(&p->lock);
    uint64_t pending = (t->queue.pending | p->queue.pending) & t->signal_mask;
    gnu_mutex_unlock(&p->lock);
    uint8_t out[8];
    snx_st_le64(out, pending);
    return gnu_kernel_write(p, a[0], out, (size_t)a[1]) ? -GNU_EFAULT : 0;
}

/* a siginfo written for the guest (rt_sigtimedwait) */
static int put_siginfo(GnuKernel* p, uint64_t address, const SnxLinuxSiginfo* info) {
    uint8_t out[SNX_LINUX_SIGINFO_SIZE];
    snx_linux_siginfo_encode(out, info);
    return address && gnu_kernel_write(p, address, out, sizeof out) ? -GNU_EFAULT : 0;
}

static int64_t sigtimedwait_call(GnuKernelThread* t, const uint64_t a[6]) {
    GnuKernel* p = t->kernel;
    if (a[3] != 8) return -GNU_EINVAL;
    uint8_t raw[16];
    if (gnu_kernel_read(p, a[0], raw, 8)) return -GNU_EFAULT;
    uint64_t set = snx_ld_le64(raw) & ~SNX_SIG_UNBLOCKABLE;
    int64_t deadline = -1;
    if (a[2]) {
        if (gnu_kernel_read(p, a[2], raw, 16)) return -GNU_EFAULT;
        int64_t seconds = (int64_t)snx_ld_le64(raw), nanos = (int64_t)snx_ld_le64(raw + 8);
        if (seconds < 0 || nanos < 0 || nanos >= 1000000000) return -GNU_EINVAL;
        int64_t now = gnu_monotonic_ns();
        deadline = seconds > (INT64_MAX - now) / 1000000000 - 1 ? INT64_MAX : now + seconds * 1000000000 + nanos;
    }
    int64_t result;
    for (;;) {
        gnu_mutex_lock(&p->lock);
        uint64_t ready = (t->queue.pending | p->queue.pending) & set;
        if (ready) {
            int sig = snx_linux_signal_next(ready, 0);
            SnxLinuxSiginfo info = dequeue(t->queue.pending & SNX_SIGBIT(sig) ? &t->queue : &p->queue, sig);
            t->wait_set = 0;
            gnu_mutex_unlock(&p->lock);
            result = put_siginfo(p, a[1], &info);
            return result ? result : sig;
        }
        int other = next_locked(t) != 0;
        t->wait_set = set;
        gnu_mutex_unlock(&p->lock);
        if (other) {
            result = -GNU_ERESTARTNOHAND; /* another signal's handler: EINTR */
            break;
        }
        if (gnu_signal_sleep(t, deadline)) {
            result = -GNU_EAGAIN;
            break;
        }
        gnu_mutex_lock(&p->lock);
        int stop = __atomic_load_n(&p->stopping, __ATOMIC_SEQ_CST) && p->stopper != t;
        gnu_mutex_unlock(&p->lock);
        if (stop) {
            result = -GNU_ERESTARTNOHAND;
            break;
        }
    }
    gnu_mutex_lock(&p->lock);
    t->wait_set = 0;
    gnu_mutex_unlock(&p->lock);
    return result;
}

static int64_t sigaltstack_call(GnuKernelThread* t, const uint64_t a[6]) {
    GnuKernel* p = t->kernel;
    uint8_t old[24], in[24];
    SnxLinuxAltStack now = t->altstack;
    now.flags = snx_linux_altstack_flags(&t->altstack, t->regs->x[31]);
    if (now.flags == SNX_SS_DISABLE) now.sp = now.size = 0;
    snx_linux_altstack_encode(old, &now);
    if (a[0]) {
        if (gnu_kernel_read(p, a[0], in, sizeof in)) return -GNU_EFAULT;
        int e = set_altstack(t, in);
        if (e) return e;
    }
    return a[1] && gnu_kernel_write(p, a[1], old, sizeof old) ? -GNU_EFAULT : 0;
}

static GnuKernelThread* find_locked(GnuKernel* p, uint32_t tid) {
    for (size_t i = 0; i < p->thread_count; i++)
        if (p->threads[i]->tid == tid) return p->threads[i];
    return NULL;
}
/* a signal to the thread tid of this process (tkill, tgkill, rt_tgsigqueueinfo): -GNU_ESRCH when there is none. The
   lock is held across the send, so the thread cannot go between them */
static int64_t to_thread(GnuKernel* p, uint32_t tid, int sig, int code, uint64_t value) {
    gnu_mutex_lock(&p->lock); /* recursive: gnu_signal_send takes it again */
    GnuKernelThread* target = find_locked(p, tid);
    int64_t r = !target ? -GNU_ESRCH : sig ? gnu_signal_send(p, target, sig, code, (int32_t)p->pid, 0, value) : 0;
    gnu_mutex_unlock(&p->lock);
    return r;
}

static uint32_t user_id(void) {
#ifdef _WIN32
    return 1000;
#else
    return (uint32_t)getuid();
#endif
}

/* a signal to another process (kill, tgkill with tid > 0, sigqueue with SI_QUEUE): the runtime's, or the host's by
   the host's numbers */
static int64_t other_process(GnuKernel* p, int32_t pid, int32_t tid, int sig, int code, uint64_t value) {
    if (p->ops->kill) return p->ops->kill(p->context, pid, tid, sig, code, value);
#ifdef _WIN32
    (void)pid;
    (void)tid;
    (void)sig;
    (void)code;
    (void)value;
    return -GNU_ENOSYS;
#else
    int host = sig ? gnu_host_signal(sig) : 0;
    if (sig && !host) return -GNU_EINVAL;
    union sigval v;
    v.sival_ptr = (void*)(uintptr_t)value;
    if (tid > 0) {
#if defined(__linux__)
        if (code == SNX_SI_TKILL) return syscall(SYS_tgkill, pid, tid, host) ? gnu_host_errno(errno) : 0;
        siginfo_t info;
        memset(&info, 0, sizeof info);
        info.si_signo = host;
        info.si_code = code;
        info.si_pid = (pid_t)p->pid;
        info.si_uid = (uid_t)user_id();
        info.si_value = v;
        return syscall(SYS_rt_tgsigqueueinfo, pid, tid, host, &info) ? gnu_host_errno(errno) : 0;
#else
        return -GNU_ESRCH;
#endif
    }
    if (code == SNX_SI_USER) return kill((pid_t)pid, host) ? gnu_host_errno(errno) : 0;
    return sigqueue((pid_t)pid, host, v) ? gnu_host_errno(errno) : 0;
#endif
}

/* rt_sigqueueinfo and rt_tgsigqueueinfo: the guest's siginfo. Linux refuses a code that claims the kernel or tkill
   for another process */
static int64_t queueinfo(GnuKernelThread* t, int32_t tgid, int32_t tid, int sig, uint64_t address) {
    GnuKernel* p = t->kernel;
    uint8_t raw[SNX_LINUX_SIGINFO_SIZE];
    if (sig < 1 || sig > 64) return -GNU_EINVAL;
    if (gnu_kernel_read(p, address, raw, sizeof raw)) return -GNU_EFAULT;
    int32_t code = (int32_t)snx_ld_le32(raw + 8);
    uint64_t value = snx_ld_le64(raw + 24);
    if ((uint32_t)tgid == p->pid) {
        if (tid > 0) return to_thread(p, (uint32_t)tid, sig, code, value);
        return gnu_signal_send(p, NULL, sig, code, (int32_t)snx_ld_le32(raw + 16), snx_ld_le32(raw + 20), value);
    }
    if (code >= 0 || code == SNX_SI_TKILL) return -GNU_EPERM;
    return other_process(p, tgid, tid, sig, code, value);
}

bool gnu_signal_syscall(GnuKernelThread* t, uint64_t number, const uint64_t a[6], int64_t* result) {
    GnuKernel* p = t->kernel;
    switch (number) {
        case 129: { /* kill(pid, sig) */
            int sig = (int)a[1];
            int32_t pid = (int32_t)a[0];
            if (sig < 0 || sig > 64) *result = -GNU_EINVAL;
            else if ((uint32_t)pid == p->pid)
                *result = sig ? gnu_signal_send(p, NULL, sig, SNX_SI_USER, (int32_t)p->pid, user_id(), 0) : 0;
            else
                *result = other_process(p, pid, 0, sig, SNX_SI_USER, 0);
            return true;
        }
        case 130: /* tkill(tid, sig) */
        case 131: { /* tgkill(tgid, tid, sig) */
            int32_t tgid = number == 131 ? (int32_t)a[0] : (int32_t)p->pid, tid = (int32_t)a[number == 131];
            int sig = (int)a[number == 131 ? 2 : 1];
            if (tgid <= 0 || tid <= 0 || sig < 0 || sig > 64)
                *result = -GNU_EINVAL;
            else if ((uint32_t)tgid == p->pid)
                *result = to_thread(p, (uint32_t)tid, sig, SNX_SI_TKILL, 0);
            else
                *result = other_process(p, tgid, tid, sig, SNX_SI_TKILL, 0);
            return true;
        }
        case 132: *result = sigaltstack_call(t, a); return true;
        case 133: *result = sigsuspend_call(t, a); return true;
        case 134: *result = sigaction_call(t, a); return true;
        case 135: *result = sigprocmask_call(t, a); return true;
        case 136: *result = sigpending_call(t, a); return true;
        case 137: *result = sigtimedwait_call(t, a); return true;
        case 138: *result = queueinfo(t, (int32_t)a[0], 0, (int)a[1], a[2]); return true;
        case 240: *result = queueinfo(t, (int32_t)a[0], (int32_t)a[1], (int)a[2], a[3]); return true;
    }
    return false;
}
