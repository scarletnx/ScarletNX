/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The kernel's system calls (kernel.h's gnu_kernel_syscall): threads, futexes, sleeps and signals */
#include "gnuabi/kernel/kernel.h"
#include "gnuabi/kernel/futex.h"
#include "gnuabi/kernel/signals.h"
#include "common/utilities/endian.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

/* nanosleep and clock_nanosleep: a wait that a signal ends, with the time left in rem for a relative one */
static int64_t sleep_call(GnuKernelThread* t, uint64_t number, const uint64_t a[6]) {
    GnuKernel* k = t->kernel;
    int64_t req[2];
    uint64_t at = number == 101 ? a[0] : a[2], rem = number == 101 ? a[1] : a[3];
    int absolute = number == 115 && (a[1] & 1); /* TIMER_ABSTIME */
    /* CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME */
    if (number == 115 && a[0] != 0 && a[0] != 1 && a[0] != 4 && a[0] != 7) return -GNU_EINVAL;
    if (gnu_kernel_read(k, at, req, sizeof req)) return -GNU_EFAULT;
    int64_t seconds = (int64_t)snx_le64((uint64_t)req[0]), nanos = (int64_t)snx_le64((uint64_t)req[1]);
    if (seconds < 0 || nanos < 0 || nanos >= 1000000000) return -GNU_EINVAL;
    int64_t span = seconds > INT64_MAX / 1000000000 - 1 ? INT64_MAX : seconds * 1000000000 + nanos;
    if (absolute) { /* until the clock reads req */
        span -= a[0] == 0 ? gnu_realtime_ns() : gnu_monotonic_ns();
        if (span <= 0) return 0;
    }
    int64_t now = gnu_monotonic_ns(), deadline = span > INT64_MAX - now ? INT64_MAX : now + span;
    if (gnu_signal_sleep(t, deadline)) return 0;
    if (absolute) return -GNU_ERESTARTNOHAND; /* the same time again on a restart */
    if (rem) {
        int64_t left = deadline - gnu_monotonic_ns(), out[2];
        if (left < 0) left = 0;
        out[0] = (int64_t)snx_le64((uint64_t)(left / 1000000000));
        out[1] = (int64_t)snx_le64((uint64_t)(left % 1000000000));
        if (gnu_kernel_write(k, rem, out, sizeof out)) return -GNU_EFAULT;
    }
    return -GNU_ERESTART_RESTARTBLOCK; /* EINTR when a handler runs */
}

/* sched_getaffinity: the host's processors, up to 64 */
static int64_t affinity(GnuKernel* k, const uint64_t a[6]) {
    if (a[1] < 8 || (a[1] & 7)) return -GNU_EINVAL;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    long cpus = (long)info.dwNumberOfProcessors;
#else
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (cpus < 1) cpus = 1;
    if (cpus > 64) cpus = 64;
    unsigned char mask[8];
    snx_st_le64(mask, cpus == 64 ? ~0ull : (1ull << cpus) - 1);
    return gnu_kernel_write(k, a[2], mask, sizeof mask) ? -GNU_EFAULT : 8;
}

bool gnu_kernel_syscall(GnuKernelThread* t, uint64_t number, const uint64_t a[6], int64_t* result) {
    GnuKernel* k = t->kernel;
    if (gnu_signal_syscall(t, number, a, result)) return true;
    switch (number) {
        case 93: /* exit */
            gnu_kernel_thread_exit(t, (int)a[0]);
            *result = 0;
            return true;
        case 94: /* exit_group */
            k->ops->end(k->context, t, (int)a[0] & 255, 0, NULL);
            *result = 0;
            return true;
        case 96: /* set_tid_address */
            t->clear_tid = a[0];
            *result = t->tid;
            return true;
        case 98: *result = gnu_futex(t, a); return true;
        case 99: /* set_robust_list(head, len): walked at the thread's exit (thread.c) */
            if (a[1] != 24) {
                *result = -GNU_EINVAL;
                return true;
            }
            t->robust_list = a[0];
            *result = 0;
            return true;
        case 100: { /* get_robust_list(tid, head_ptr, len_ptr): this thread's */
            unsigned char head[8], length[8];
            snx_st_le64(head, t->robust_list);
            snx_st_le64(length, 24);
            *result = a[0] && (uint32_t)a[0] != t->tid ? -GNU_EPERM
                      : gnu_kernel_write(k, a[1], head, 8) || gnu_kernel_write(k, a[2], length, 8) ? -GNU_EFAULT
                                                                                                    : 0;
            return true;
        }
        case 101: /* nanosleep(req, rem) */
        case 115: /* clock_nanosleep(clock, flags, req, rem) */
            *result = sleep_call(t, number, a);
            return true;
        case 122: *result = 0; return true; /* sched_setaffinity: the host schedules the threads */
        case 123: *result = affinity(k, a); return true;
        case 124: /* sched_yield */
#ifdef _WIN32
            SwitchToThread();
#else
            sched_yield();
#endif
            *result = 0;
            return true;
        case 128: *result = -GNU_EINTR; return true; /* restart_syscall: the kernel restarts its calls itself */
        case 178: *result = t->tid; return true; /* gettid */
    }
    return false;
}
