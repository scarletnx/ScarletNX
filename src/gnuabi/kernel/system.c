/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The kernel's system calls that need no loader, file system or descriptor table (kernel.h): ids, clocks, random
   bytes, the scheduler's parameters, prctl, membarrier, madvise, uname and the resource queries. Each writes the
   guest's AArch64 structures through the checked access. Behavior as Linux on AArch64 documents it: getrandom(2),
   prctl(2), membarrier(2), madvise(2), sched_setscheduler(2), getrusage(2), prlimit(2), uname(2), sysinfo(2) */
#ifndef _WIN32
#define _GNU_SOURCE /* sched_getcpu, membarrier's numbers */
#endif
#include "gnuabi/kernel/kernel.h"
#include "common/utilities/endian.h"
#include <errno.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#endif
#endif

#if defined(__linux__) && defined(SYS_membarrier)
static struct {
    int barrier;   /* the host's membarrier: 1 there, -1 not (atomic) */
    int expedited; /* 1 registered for its private expedited barrier, -1 refused (atomic) */
} g_system;
#endif

void gnu_kernel_host_thread_name(char name[16]) {
    memset(name, 0, 16);
#if defined(__linux__)
    (void)prctl(PR_GET_NAME, name, 0, 0, 0); /* the host thread's comm */
    name[15] = 0;
#endif
}

static int64_t put(GnuKernel* k, uint64_t address, const void* bytes, size_t size) {
    return gnu_kernel_write(k, address, bytes, size) ? -GNU_EFAULT : 0;
}
/* a timespec {s64, s64}, a timeval {s64, s64} */
static int64_t put_pair(GnuKernel* k, uint64_t address, int64_t first, int64_t second) {
    uint8_t out[16];
    snx_st_le64(out, (uint64_t)first);
    snx_st_le64(out + 8, (uint64_t)second);
    return put(k, address, out, sizeof out);
}

/* whether pid names this process or one of its threads (0 is the caller) */
static int ours(GnuKernel* k, int32_t pid) {
    if (pid == 0 || (uint32_t)pid == k->pid) return 1;
    gnu_mutex_lock(&k->lock);
    int found = 0;
    for (size_t i = 0; i < k->thread_count && !found; i++) found = k->threads[i]->tid == (uint32_t)pid;
    gnu_mutex_unlock(&k->lock);
    return found;
}

static uint32_t host_id(int which) {
#ifdef _WIN32
    (void)which;
    return 1000;
#else
    return which == 0 ? (uint32_t)getuid() : which == 1 ? (uint32_t)geteuid() : which == 2 ? (uint32_t)getgid()
                                                                                          : (uint32_t)getegid();
#endif
}

/* ---- random bytes ---- */
/* getrandom(buf, count, flags): GRND_NONBLOCK 1, GRND_RANDOM 2, GRND_INSECURE 4 (not with GRND_RANDOM). The host's
   generator never blocks once seeded, so each one gives the bytes */
static int64_t getrandom_call(GnuKernel* k, uint64_t address, uint64_t count, uint64_t flags) {
    if ((flags & ~7ull) || (flags & 6) == 6) return -GNU_EINVAL;
    if (count > 0x1ffffff) count = 0x1ffffff; /* Linux's most for one call */
    uint8_t chunk[256];
    for (uint64_t done = 0; done < count;) {
        size_t n = count - done > sizeof chunk ? sizeof chunk : (size_t)(count - done);
        int e = gnu_random_bytes(chunk, n);
        if (!e) e = gnu_kernel_write(k, address + done, chunk, n);
        if (e) return done ? (int64_t)done : e;
        done += n;
    }
    return (int64_t)count;
}

/* ---- prctl ---- */
enum {
    GNU_PR_SET_PDEATHSIG = 1, GNU_PR_GET_PDEATHSIG = 2, GNU_PR_GET_DUMPABLE = 3, GNU_PR_SET_DUMPABLE = 4,
    GNU_PR_GET_KEEPCAPS = 7, GNU_PR_SET_KEEPCAPS = 8, GNU_PR_SET_NAME = 15, GNU_PR_GET_NAME = 16,
    GNU_PR_GET_SECCOMP = 21, GNU_PR_SET_SECCOMP = 22, GNU_PR_CAPBSET_READ = 23, GNU_PR_CAPBSET_DROP = 24,
    GNU_PR_SET_TIMERSLACK = 29, GNU_PR_GET_TIMERSLACK = 30, GNU_PR_SET_CHILD_SUBREAPER = 36,
    GNU_PR_GET_CHILD_SUBREAPER = 37, GNU_PR_SET_NO_NEW_PRIVS = 38, GNU_PR_GET_NO_NEW_PRIVS = 39,
    GNU_PR_GET_TID_ADDRESS = 40, GNU_PR_SET_THP_DISABLE = 41, GNU_PR_GET_THP_DISABLE = 42, GNU_PR_CAP_AMBIENT = 47,
    GNU_PR_SVE_SET_VL = 50, GNU_PR_SVE_GET_VL = 51, GNU_PR_GET_SPECULATION_CTRL = 52, GNU_PR_PAC_RESET_KEYS = 54,
    GNU_PR_SET_TAGGED_ADDR_CTRL = 55, GNU_PR_GET_TAGGED_ADDR_CTRL = 56, GNU_PR_SET_VMA = 0x53564d41,
};
#define CAP_LAST_CAP 40

static int64_t prctl_call(GnuKernelThread* t, const uint64_t a[6]) {
    GnuKernel* k = t->kernel;
    uint64_t option = (uint32_t)a[0];
    switch (option) {
        case GNU_PR_SET_PDEATHSIG: /* the parent of the process never dies first here */
            return a[1] > 64 ? -GNU_EINVAL : 0;
        case GNU_PR_GET_PDEATHSIG: {
            uint8_t zero[4] = {0};
            return put(k, a[1], zero, 4);
        }
        case GNU_PR_GET_DUMPABLE: return __atomic_load_n(&k->dumpable, __ATOMIC_RELAXED);
        case GNU_PR_SET_DUMPABLE:
            if (a[1] > 1) return -GNU_EINVAL;
            __atomic_store_n(&k->dumpable, (uint32_t)a[1], __ATOMIC_RELAXED);
            return 0;
        case GNU_PR_GET_KEEPCAPS: return 0;
        case GNU_PR_SET_KEEPCAPS: return a[1] > 1 ? -GNU_EINVAL : 0;
        case GNU_PR_SET_NAME: { /* the first 15 bytes of the string */
            char name[16] = {0};
            for (int i = 0; i < 15; i++) {
                if (gnu_kernel_read(k, a[1] + (uint64_t)i, &name[i], 1)) return -GNU_EFAULT;
                if (!name[i]) break;
            }
            memcpy(t->name, name, sizeof name);
            return 0;
        }
        case GNU_PR_GET_NAME: return put(k, a[1], t->name, sizeof t->name);
        case GNU_PR_GET_SECCOMP: return 0;
        case GNU_PR_SET_SECCOMP: return -GNU_EINVAL; /* a kernel without seccomp's filters */
        case GNU_PR_CAPBSET_READ: return a[1] > CAP_LAST_CAP ? -GNU_EINVAL : 1;
        case GNU_PR_CAPBSET_DROP: return a[1] > CAP_LAST_CAP ? -GNU_EINVAL : -GNU_EPERM;
        case GNU_PR_SET_TIMERSLACK: t->timer_slack = a[1] ? a[1] : 50000; return 0;
        case GNU_PR_GET_TIMERSLACK: return (int64_t)t->timer_slack;
        case GNU_PR_SET_CHILD_SUBREAPER: __atomic_store_n(&k->subreaper, a[1] != 0, __ATOMIC_RELAXED); return 0;
        case GNU_PR_GET_CHILD_SUBREAPER: {
            uint8_t out[4];
            snx_st_le32(out, __atomic_load_n(&k->subreaper, __ATOMIC_RELAXED));
            return put(k, a[1], out, 4);
        }
        case GNU_PR_SET_NO_NEW_PRIVS:
            if (a[1] != 1 || a[2] || a[3] || a[4]) return -GNU_EINVAL;
            __atomic_store_n(&k->no_new_privs, 1, __ATOMIC_RELAXED);
            return 0;
        case GNU_PR_GET_NO_NEW_PRIVS:
            return a[1] || a[2] || a[3] || a[4] ? -GNU_EINVAL : __atomic_load_n(&k->no_new_privs, __ATOMIC_RELAXED);
        case GNU_PR_GET_TID_ADDRESS: {
            uint8_t out[8];
            snx_st_le64(out, t->clear_tid);
            return put(k, a[1], out, 8);
        }
        case GNU_PR_SET_THP_DISABLE:
            if (a[2] || a[3] || a[4]) return -GNU_EINVAL;
            __atomic_store_n(&k->thp_disable, a[1] != 0, __ATOMIC_RELAXED);
            return 0;
        case GNU_PR_GET_THP_DISABLE:
            return a[1] || a[2] || a[3] || a[4] ? -GNU_EINVAL : __atomic_load_n(&k->thp_disable, __ATOMIC_RELAXED);
        case GNU_PR_CAP_AMBIENT: return a[1] == 1 /* IS_SET */ ? 0 : a[1] == 4 /* CLEAR_ALL */ ? 0 : -GNU_EPERM;
        case GNU_PR_GET_SPECULATION_CTRL: return a[1] > 2 ? -GNU_ENODEV : 0; /* GNU_PR_SPEC_NOT_AFFECTED */
        case GNU_PR_SET_VMA: /* PR_SET_VMA_ANON_NAME (0): a name for /proc/self/maps, which nothing here shows */
            return a[1] == 0 ? 0 : -GNU_EINVAL;
        case GNU_PR_SVE_SET_VL:
        case GNU_PR_SVE_GET_VL:
        case GNU_PR_PAC_RESET_KEYS:
        /* the CPU the recompiler gives has no SVE and no pointer authentication, and generated code does not take
           tagged addresses: a kernel without them refuses these */
        case GNU_PR_SET_TAGGED_ADDR_CTRL:
        case GNU_PR_GET_TAGGED_ADDR_CTRL: return -GNU_EINVAL;
    }
    return -GNU_EINVAL;
}

/* ---- membarrier ---- */
enum {
    MB_QUERY = 0, MB_GLOBAL = 1, MB_GLOBAL_EXPEDITED = 2, MB_REGISTER_GLOBAL_EXPEDITED = 4, MB_PRIVATE_EXPEDITED = 8,
    MB_REGISTER_PRIVATE_EXPEDITED = 16, MB_PRIVATE_EXPEDITED_SYNC_CORE = 32,
    MB_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE = 64, MB_PRIVATE_EXPEDITED_RSEQ = 128,
    MB_REGISTER_PRIVATE_EXPEDITED_RSEQ = 256, MB_GET_REGISTRATIONS = 512,
    MB_SUPPORTED = MB_GLOBAL | MB_GLOBAL_EXPEDITED | MB_REGISTER_GLOBAL_EXPEDITED | MB_PRIVATE_EXPEDITED |
                   MB_REGISTER_PRIVATE_EXPEDITED | MB_PRIVATE_EXPEDITED_SYNC_CORE |
                   MB_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE | MB_GET_REGISTRATIONS,
};
bool gnu_host_barrier(void) {
#if defined(_WIN32)
    return true; /* FlushProcessWriteBuffers */
#elif defined(__linux__) && defined(SYS_membarrier)
    int barrier = __atomic_load_n(&g_system.barrier, __ATOMIC_ACQUIRE);
    if (!barrier) { /* asked once: a kernel without it, or a seccomp filter that refuses it, has none */
        long commands = syscall(SYS_membarrier, MB_QUERY, 0, 0);
        barrier = commands > 0 && (commands & (MB_GLOBAL | MB_REGISTER_PRIVATE_EXPEDITED)) ? 1 : -1;
        __atomic_store_n(&g_system.barrier, barrier, __ATOMIC_RELEASE);
    }
    return barrier > 0;
#else
    return false; /* macOS, FreeBSD: no call puts a barrier on another thread */
#endif
}
/* a memory barrier on every thread of the host process, whose threads are the guest's, where gnu_host_barrier says
   the host has one: 0, or the host's error */
static int64_t barrier_everywhere(void) {
#if defined(_WIN32)
    FlushProcessWriteBuffers();
    return 0;
#elif defined(__linux__) && defined(SYS_membarrier)
    /* registered once; a host that refuses it gets the slow global barrier */
    int expedited = __atomic_load_n(&g_system.expedited, __ATOMIC_ACQUIRE);
    if (!expedited) {
        expedited = syscall(SYS_membarrier, MB_REGISTER_PRIVATE_EXPEDITED, 0, 0) ? -1 : 1;
        __atomic_store_n(&g_system.expedited, expedited, __ATOMIC_RELEASE);
    }
    return syscall(SYS_membarrier, expedited > 0 ? MB_PRIVATE_EXPEDITED : MB_GLOBAL, 0, 0) ? gnu_host_errno(errno) : 0;
#else
    return -GNU_ENOSYS;
#endif
}
int64_t gnu_kernel_membarrier(GnuKernel* k, bool host, uint64_t command, uint64_t flags) {
    if (!host) return -GNU_ENOSYS;
    if (flags) return -GNU_EINVAL; /* only the RSEQ commands take a flag, and they are not here */
    uint32_t registered = __atomic_load_n(&k->membarrier_registered, __ATOMIC_ACQUIRE);
    switch (command) {
        case MB_QUERY: return MB_SUPPORTED;
        case MB_GET_REGISTRATIONS: return registered;
        case MB_REGISTER_GLOBAL_EXPEDITED:
        case MB_REGISTER_PRIVATE_EXPEDITED:
        case MB_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
            __atomic_fetch_or(&k->membarrier_registered, (uint32_t)command, __ATOMIC_ACQ_REL);
            return 0;
        case MB_PRIVATE_EXPEDITED:
        case MB_PRIVATE_EXPEDITED_SYNC_CORE:
            /* Linux: EPERM before the process registered for it */
            if (!(registered & (uint32_t)(command << 1))) return -GNU_EPERM;
            return barrier_everywhere();
        case MB_GLOBAL:
        case MB_GLOBAL_EXPEDITED: return barrier_everywhere();
    }
    return -GNU_EINVAL;
}

/* ---- madvise ---- */
static int64_t madvise_call(GnuKernel* k, uint64_t address, uint64_t length, uint64_t advice) {
    if (address & 4095) return -GNU_EINVAL;
    if (length > UINT64_MAX - 4095 || address + ((length + 4095) & ~4095ull) < address) return -GNU_EINVAL;
    /* Linux's advice: NORMAL to DONTNEED (0-4), FREE to DONTNEED_LOCKED and COLLAPSE (8-25), HWPOISON, SOFT_OFFLINE */
    if (!(advice <= 4 || (advice >= 8 && advice <= 25) || advice == 100 || advice == 101)) return -GNU_EINVAL;
    if (advice >= 100) return -GNU_EPERM; /* CAP_SYS_ADMIN's */
    if (!length) return 0;
    return k->ops->madvise ? k->ops->madvise(k->context, address, length, (int)advice) : 0;
}

/* ---- the scheduler's parameters: SCHED_OTHER 0, FIFO 1, RR 2, BATCH 3, IDLE 5 ---- */
static int policy_valid(int64_t policy) {
    policy &= ~0x40000000ll; /* SCHED_RESET_ON_FORK */
    return policy == 0 || policy == 1 || policy == 2 || policy == 3 || policy == 5;
}
static int64_t setscheduler(GnuKernel* k, int32_t pid, int64_t policy, uint64_t param) {
    if (pid < 0 || !param) return -GNU_EINVAL;
    uint8_t raw[4];
    if (gnu_kernel_read(k, param, raw, 4)) return -GNU_EFAULT;
    int32_t priority = (int32_t)snx_ld_le32(raw);
    if (!ours(k, pid)) return -GNU_ESRCH;
    if (policy >= 0 && !policy_valid(policy)) return -GNU_EINVAL;
    int realtime = (policy & ~0x40000000ll) == 1 || (policy & ~0x40000000ll) == 2;
    if (realtime ? priority < 1 || priority > 99 : priority != 0) return -GNU_EINVAL;
    return realtime ? -GNU_EPERM : 0; /* an unprivileged process: the real-time policies are root's */
}

/* ---- the resource queries ---- */
/* getrusage's struct rusage (144 bytes): the CPU time the clocks give, the rest 0 */
static int64_t rusage_call(GnuKernel* k, int64_t who, uint64_t address) {
    if (who != 0 && who != -1 && who != 1) return -GNU_EINVAL; /* SELF, CHILDREN, THREAD */
    uint8_t usage[144] = {0};
    int64_t seconds, nanos;
    if (who != -1 && !gnu_clock_gettime(who == 1 ? 3 : 2, &seconds, &nanos)) {
        snx_st_le64(usage, (uint64_t)seconds); /* ru_utime: all of it user time */
        snx_st_le64(usage + 8, (uint64_t)(nanos / 1000));
    }
    return put(k, address, usage, sizeof usage);
}
/* prlimit64's limits: RLIMIT_STACK 3 is the initial stack's 8 MiB, RLIMIT_NOFILE 7 a table's 256 descriptors, the
   others unlimited. Nothing enforces a new one */
static int64_t limits(GnuKernel* k, int32_t pid, uint64_t resource, uint64_t set, uint64_t old) {
    if (pid < 0 || (pid && !ours(k, pid))) return -GNU_ESRCH;
    if (resource >= 16) return -GNU_EINVAL;
    if (set) {
        uint8_t in[16];
        if (gnu_kernel_read(k, set, in, sizeof in)) return -GNU_EFAULT;
        if (snx_ld_le64(in) > snx_ld_le64(in + 8)) return -GNU_EINVAL;
    }
    if (!old) return 0;
    uint64_t current = UINT64_MAX, most = UINT64_MAX;
    if (resource == 3) current = 0x800000ull;
    if (resource == 7) current = most = 256;
    uint8_t out[16];
    snx_st_le64(out, current);
    snx_st_le64(out + 8, most);
    return put(k, old, out, sizeof out);
}

bool gnu_kernel_system_call(GnuKernelThread* t, uint64_t number, const uint64_t a[6], int64_t* result) {
    GnuKernel* k = t->kernel;
    int64_t r;
    switch (number) {
        case 113: { /* clock_gettime(clock, ts) */
            int64_t seconds, nanos;
            r = gnu_clock_gettime((int)a[0], &seconds, &nanos);
            *result = r ? r : put_pair(k, a[1], seconds, nanos);
            return true;
        }
        case 114: { /* clock_getres(clock, res): the high-resolution clocks' 1 ns, the coarse ones' tick (4 ms) */
            int64_t seconds, nanos;
            r = gnu_clock_gettime((int)a[0], &seconds, &nanos);
            *result = r || !a[1] ? r : put_pair(k, a[1], 0, a[0] == 5 || a[0] == 6 ? 4000000 : 1);
            return true;
        }
        case 169: { /* gettimeofday(tv, tz) */
            int64_t seconds, nanos;
            (void)gnu_clock_gettime(0, &seconds, &nanos);
            r = a[0] ? put_pair(k, a[0], seconds, nanos / 1000) : 0;
            if (!r && a[1]) {
                uint8_t zone[8] = {0}; /* UTC, no daylight saving */
                r = put(k, a[1], zone, sizeof zone);
            }
            *result = r;
            return true;
        }
        case 153: { /* times(buf): clock ticks of 100 Hz */
            int64_t seconds, nanos;
            uint8_t ticks[32] = {0};
            if (!gnu_clock_gettime(2, &seconds, &nanos))
                snx_st_le64(ticks, (uint64_t)(seconds * 100 + nanos / 10000000));
            r = a[0] ? put(k, a[0], ticks, sizeof ticks) : 0;
            if (!r && !gnu_clock_gettime(7, &seconds, &nanos)) r = seconds * 100 + nanos / 10000000;
            *result = r;
            return true;
        }
        case 165: *result = rusage_call(k, (int32_t)a[0], a[1]); return true;
        case 179: { /* sysinfo: the host's memory on Linux, else an 8 GiB machine with 4 GiB free */
            uint8_t info[112] = {0};
            int64_t seconds, nanos;
            if (!gnu_clock_gettime(7, &seconds, &nanos)) snx_st_le64(info, (uint64_t)seconds);
            snx_st_le64(info + 32, 8ull << 30);
            snx_st_le64(info + 40, 4ull << 30);
            snx_st_le16(info + 80, (uint16_t)k->thread_count); /* procs */
            snx_st_le32(info + 104, 1);                        /* mem_unit */
#if defined(__linux__)
            struct sysinfo host;
            if (!sysinfo(&host)) {
                const unsigned long counts[] = {host.loads[0], host.loads[1], host.loads[2], host.totalram,
                                                host.freeram, host.sharedram, host.bufferram, host.totalswap,
                                                host.freeswap};
                for (int i = 0; i < 9; i++) snx_st_le64(info + 8 + 8 * i, (uint64_t)counts[i]);
                snx_st_le16(info + 80, host.procs);
                snx_st_le64(info + 88, (uint64_t)host.totalhigh);
                snx_st_le64(info + 96, (uint64_t)host.freehigh);
                snx_st_le32(info + 104, host.mem_unit);
            }
#endif
            *result = put(k, a[0], info, sizeof info);
            return true;
        }
        case 160: { /* uname */
            char uts[6][65] = {{0}};
            strcpy(uts[0], "Linux");
            strcpy(uts[1], "scarletnx");
            strcpy(uts[2], "6.6.0");
            strcpy(uts[3], "ScarletNX GNU ABI");
            strcpy(uts[4], "aarch64");
            strcpy(uts[5], "(none)");
            *result = put(k, a[0], uts, sizeof uts);
            return true;
        }
        case 163: *result = limits(k, 0, a[0], 0, a[1]); return true; /* getrlimit */
        case 164: *result = limits(k, 0, a[0], a[1], 0); return true; /* setrlimit */
        case 261: *result = limits(k, (int32_t)a[0], a[1], a[2], a[3]); return true; /* prlimit64 */
        case 167: *result = prctl_call(t, a); return true;
        case 168: { /* getcpu(cpu, node, cache) */
#if defined(_WIN32)
            uint32_t cpu = (uint32_t)GetCurrentProcessorNumber();
#elif defined(__linux__)
            int host = sched_getcpu();
            uint32_t cpu = host < 0 ? 0 : (uint32_t)host;
#else
            uint32_t cpu = 0;
#endif
            uint8_t out[4];
            snx_st_le32(out, cpu);
            r = a[0] ? put(k, a[0], out, 4) : 0;
            snx_st_le32(out, 0); /* one NUMA node */
            if (!r && a[1]) r = put(k, a[1], out, 4);
            *result = r;
            return true;
        }
        case 172: *result = k->pid; return true; /* getpid */
        case 173: /* getppid */
#ifdef _WIN32
            *result = 1;
#else
            *result = getppid();
#endif
            return true;
        case 174: *result = host_id(0); return true; /* getuid: the host user's, whose files the guest makes */
        case 175: *result = host_id(1); return true;
        case 176: *result = host_id(2); return true;
        case 177: *result = host_id(3); return true;
        case 158: *result = 0; return true; /* getgroups: none beyond the group */
        case 140: /* setpriority(which, who, prio): the host thread's own, for PRIO_PROCESS of a guest thread */
        case 141: { /* getpriority: 20 - nice, as the system call gives it */
            if (a[0] > 2) {
                *result = -GNU_EINVAL;
                return true;
            }
            if (a[0] != 0 || !ours(k, (int32_t)a[1])) {
                *result = -GNU_ESRCH;
                return true;
            }
#if defined(__linux__)
            id_t who = a[1] ? (id_t)a[1] : (id_t)t->tid;
            if (number == 140) {
                int nice = (int)(int32_t)a[2];
                nice = nice < -20 ? -20 : nice > 19 ? 19 : nice;
                *result = setpriority(PRIO_PROCESS, who, nice) ? gnu_host_errno(errno) : 0;
            } else {
                long raw = syscall(SYS_getpriority, PRIO_PROCESS, who);
                *result = raw < 0 ? gnu_host_errno(errno) : raw;
            }
#else
            *result = number == 140 ? 0 : 20;
#endif
            return true;
        }
        case 118: /* sched_setparam(pid, param) */
            *result = setscheduler(k, (int32_t)a[0], -1, a[1]);
            return true;
        case 119: *result = setscheduler(k, (int32_t)a[0], (int64_t)(int32_t)a[1], a[2]); return true;
        case 120: /* sched_getscheduler: SCHED_OTHER */
            *result = (int32_t)a[0] < 0 ? -GNU_EINVAL : ours(k, (int32_t)a[0]) ? 0 : -GNU_ESRCH;
            return true;
        case 121: { /* sched_getparam: priority 0 */
            uint8_t zero[4] = {0};
            *result = (int32_t)a[0] < 0 || !a[1] ? -GNU_EINVAL : !ours(k, (int32_t)a[0]) ? -GNU_ESRCH
                                                                                         : put(k, a[1], zero, 4);
            return true;
        }
        case 125: /* sched_get_priority_max */
        case 126: { /* sched_get_priority_min */
            int64_t policy = (int32_t)a[0];
            int realtime = policy == 1 || policy == 2;
            *result = !policy_valid(policy) || (policy & 0x40000000) ? -GNU_EINVAL
                      : realtime                                     ? (number == 125 ? 99 : 1)
                                                                     : 0;
            return true;
        }
        case 127: /* sched_rr_get_interval: Linux's 100 ms slice */
            *result = (int32_t)a[0] < 0 ? -GNU_EINVAL : !ours(k, (int32_t)a[0]) ? -GNU_ESRCH
                                                                              : put_pair(k, a[1], 0, 100000000);
            return true;
        case 233: *result = madvise_call(k, a[0], a[1], a[2]); return true;
        case 278: *result = getrandom_call(k, a[0], a[1], a[2]); return true;
        case 283: *result = gnu_kernel_membarrier(k, gnu_host_barrier(), a[0], a[1]); return true;
        case 293: *result = -GNU_ENOSYS; return true; /* rseq: the C library runs without it */
    }
    return false;
}
