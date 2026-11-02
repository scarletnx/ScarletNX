/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "nx/runtime/platform/scheduling.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <errno.h>
#include <string.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/rtprio.h>
#endif

static struct {
    unsigned logged;
} g_scheduling;

#if defined(__linux__) && !defined(__ANDROID__) && defined(SYS_sched_getattr) && defined(SYS_sched_setattr)
/* Linux's versioned syscall ABI. Do not include linux/sched/types.h: it redeclares libc's sched_param. */
typedef struct {
    unsigned size, policy;
    unsigned long long flags;
    int nice;
    unsigned priority;
    unsigned long long runtime, deadline, period;
    unsigned util_min, util_max;
} LinuxSchedAttr;
typedef char linux_sched_attr_size[sizeof(LinuxSchedAttr) == 56 ? 1 : -1];

static int sched_read(LinuxSchedAttr* attr) {
    *attr = (LinuxSchedAttr){0};
    return (int)syscall(SYS_sched_getattr, 0, attr, sizeof *attr, 0);
}
#endif

void snx_thread_performance(SnxThreadWork work) {
    static const char* const names[SNX_WORK_COUNT] = {"guest", "GPU", "Vulkan recorder"};
    int mode = g_settings.cpu_scheduling;
    if (!mode || (unsigned)work >= SNX_WORK_COUNT) return;
    unsigned bit = 1u << (unsigned)work;
    int report = !(__atomic_fetch_or(&g_scheduling.logged, bit, __ATOMIC_RELAXED) & bit);
#if defined(__linux__) && !defined(__ANDROID__) && defined(SYS_sched_getattr) && defined(SYS_sched_setattr)
    LinuxSchedAttr attr;
    if (sched_read(&attr)) {
        if (report) LOG("cpu: %s scheduling controls unavailable: %s\n", names[work], strerror(errno));
        return;
    }
    /* Respect an externally selected real-time, batch or idle policy. */
    if (attr.policy != SCHED_OTHER) {
        if (report) LOG("cpu: %s keeps its existing scheduling policy %u\n", names[work], attr.policy);
        return;
    }
    attr.size = attr.size < sizeof attr ? attr.size : (unsigned)sizeof attr;
    int boost_error = 0, slice_error = 0;
    if (attr.size >= sizeof attr) {
        LinuxSchedAttr boost = attr;
        boost.flags |= 0x60ull; /* SCHED_FLAG_UTIL_CLAMP_MIN | SCHED_FLAG_UTIL_CLAMP_MAX */
        boost.util_min = 1024;
        boost.util_max = 1024;
        if (syscall(SYS_sched_setattr, 0, &boost, 0))
            boost_error = errno;
        else
            attr = boost;
    } else
        boost_error = EOPNOTSUPP;
    if (mode >= 2) {
        LinuxSchedAttr latency = attr;
        /* Linux 6.12+: the minimum EEVDF request, in ns. Older kernels can accept and ignore this field. */
        latency.runtime = 100000;
        /* Use only the nice boost the user's existing resource limit permits. */
        struct rlimit limit;
        if (!getrlimit(RLIMIT_NICE, &limit) && limit.rlim_cur > 0) {
            int best = limit.rlim_cur >= 40 ? -20 : 20 - (int)limit.rlim_cur;
            if (best < latency.nice) latency.nice = best;
        }
        if (syscall(SYS_sched_setattr, 0, &latency, 0)) slice_error = errno;
    }
    LinuxSchedAttr actual;
    if (report && !sched_read(&actual)) {
        LOG("cpu: %s scheduling mode %d: task performance floor %u/1024, fair slice %llu ns, nice %d\n",
            names[work], mode, actual.util_min, actual.runtime, actual.nice);
        if (boost_error) LOG("cpu: %s performance-floor request unavailable: %s\n", names[work], strerror(boost_error));
        if (slice_error) LOG("cpu: %s short-slice request unavailable: %s\n", names[work], strerror(slice_error));
        else if (mode >= 2 && actual.runtime != 100000)
            LOG("cpu: %s kernel did not apply the requested 100000 ns fair slice\n", names[work]);
    }
#elif defined(__FreeBSD__)
    struct rtprio current = {0};
    if (rtprio_thread(RTP_LOOKUP, 0, &current)) {
        if (report) LOG("cpu: %s scheduling controls unavailable: %s\n", names[work], strerror(errno));
        return;
    }
    if (current.type != RTP_PRIO_NORMAL) {
        if (report) LOG("cpu: %s keeps its existing scheduling class %u\n", names[work], (unsigned)current.type);
        return;
    }
    /* Remain in the native timeshare scheduler. Zero requests its highest normal priority, never realtime. */
    struct rtprio desired = {0};
    desired.type = RTP_PRIO_NORMAL;
    if (rtprio_thread(RTP_SET, 0, &desired)) {
        if (report) LOG("cpu: %s normal priority request unavailable: %s\n", names[work], strerror(errno));
    } else if (report && !rtprio_thread(RTP_LOOKUP, 0, &current))
        LOG("cpu: %s native timeshare priority %u\n", names[work], (unsigned)current.prio);
#else
    if (report) LOG("cpu: %s uses the host scheduling defaults\n", names[work]);
#endif
}
