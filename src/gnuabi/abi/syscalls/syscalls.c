/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/abi/syscalls/syscalls.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/process/process.h"
#include "gnuabi/signals/signals.h"
#include "gnuabi/threading/futex.h"
#include "gnuabi/network/network.h"
#include "common/utilities/endian.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
static int64_t file_mmap(GnuProcess* p, const uint64_t a[6]) {
    if (a[2] & ~7ull) return -GNU_EINVAL;
    if (a[5] & 4095) return -GNU_EINVAL;
    /* Shared file writeback needs a coherent VFS mapping. A shared mapping nothing writes to is its copy */
    if ((a[3] & 3) != 2 && ((a[3] & 3) != 1 || (a[2] & 2))) return -GNU_ENOTSUP;
    unsigned char probe;
    int64_t r = gnu_vfs_pread(&p->vfs, (int)a[4], &probe, 0, a[5]);
    if (r < 0) return r;
    int64_t mapped = gnu_memory_mmap(&p->memory, a[0], a[1], 3, (a[3] & ~3ull) | 0x22);
    if (mapped < 0) return mapped;
    r = gnu_vfs_pread(&p->vfs, (int)a[4], p->memory.base + (uint64_t)mapped, (size_t)a[1], a[5]);
    if (r >= 0) r = gnu_memory_protect(&p->memory, (uint64_t)mapped, a[1], (unsigned)a[2]);
    if (r < 0) {
        gnu_memory_unmap(&p->memory, (uint64_t)mapped, a[1]);
        return r;
    }
    return mapped;
}
static int64_t dispatch(GnuProcess* p, uint64_t number, const uint64_t a[6]);
static struct {
    int trace; /* gnu_syscall_trace's */
} g_syscalls;
void gnu_syscall_trace(int on) { g_syscalls.trace = on; }
/* --strace: every syscall on stderr, with its arguments and result, a line each */
int64_t gnu_syscall(GnuProcess* p, uint64_t number, const uint64_t a[6]) {
    int trace = g_syscalls.trace;
    /* arm64 el0_svc_common takes an int syscall number from x8. */
    number = (uint32_t)number;
    if (trace)
        fprintf(stderr, "[%u] %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx) ...\n", p->pid, (unsigned long long)number,
                (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2],
                (unsigned long long)a[3]);
    int64_t result = dispatch(p, number, a);
    if (trace) fprintf(stderr, "[%u] %llu = %lld\n", p->pid, (unsigned long long)number, (long long)result);
    return result;
}
static int64_t dispatch(GnuProcess* p, uint64_t number, const uint64_t a[6]) {
    int64_t result;
    if (gnu_spawn_syscall(p, number, a, &result)) return result;
    if (gnu_vfs_syscall(&p->vfs, &p->memory, number, a, &result)) return result;
    if (gnu_net_syscall(p, number, a, &result)) return result;
    switch (number) {
        case GNU_NR_exit:
        case GNU_NR_exit_group: gnu_process_exit(p, (int)a[0]); return 0;
        case GNU_NR_getpid: return p->pid;
        case GNU_NR_gettid: return p->tid;
#ifdef _WIN32
        case 174:
        case 175:
        case 176:
        case 177: return 1000; /* uid/euid/gid/egid */
#else
        /* the host user's: the files the guest makes and stats are the host's */
        case 174: return getuid();
        case 175: return geteuid();
        case 176: return getgid();
        case 177: return getegid();
#endif
        case 158: /* getgroups: none beyond the group */
            return 0;
        case 99: return 0;              /* set_robust_list: one thread, which never dies with a robust lock */
        case 233: return 0;             /* madvise: advice */
        case 293: return -GNU_ENOSYS;   /* rseq */
        case 283: return -GNU_ENOSYS;   /* membarrier */
        case 101:                       /* nanosleep(req, rem) */
        case 115: {                     /* clock_nanosleep(clock, flags, req, rem) */
            int64_t t[2];
            uint64_t req = number == 101 ? a[0] : a[2];
            if (gnu_memory_read(&p->memory, req, t, sizeof t)) return -GNU_EFAULT;
            t[0] = (int64_t)snx_le64((uint64_t)t[0]);
            t[1] = (int64_t)snx_le64((uint64_t)t[1]);
            if (t[0] < 0 || t[1] < 0 || t[1] >= 1000000000) return -GNU_EINVAL;
            if (number == 115 && (a[1] & 1)) { /* TIMER_ABSTIME: until the clock reads t */
                int64_t now_s, now_ns;
                if (gnu_clock_gettime((int)a[0], &now_s, &now_ns)) return -GNU_EINVAL;
                t[0] -= now_s;
                t[1] -= now_ns;
                if (t[1] < 0) {
                    t[1] += 1000000000;
                    t[0]--;
                }
                if (t[0] < 0) return 0;
            }
            gnu_sleep(t[0], t[1]);
            return 0;
        }
        case 123: { /* sched_getaffinity(pid, size, mask): one CPU */
            unsigned char mask[8] = {1};
            if (a[1] < 8) return -GNU_EINVAL;
            return gnu_memory_write(&p->memory, a[2], mask, sizeof mask) ? -GNU_EFAULT : 8;
        }
        case 165: { /* getrusage: nothing measured */
            unsigned char usage[144] = {0};
            return gnu_memory_write(&p->memory, a[1], usage, sizeof usage) ? -GNU_EFAULT : 0;
        }
        case 153: { /* times */
            int64_t sec, ns;
            uint64_t ticks[4] = {0};
            if (!gnu_clock_gettime(2, &sec, &ns)) ticks[0] = snx_le64((uint64_t)(sec * 100 + ns / 10000000));
            if (a[0] && gnu_memory_write(&p->memory, a[0], ticks, sizeof ticks)) return -GNU_EFAULT;
            if (gnu_clock_gettime(1, &sec, &ns)) return 0;
            return sec * 100 + ns / 10000000;
        }
        case 179: { /* sysinfo */
            unsigned char info[112] = {0};
            int64_t sec, ns;
            if (!gnu_clock_gettime(1, &sec, &ns)) snx_st_le64(info, (uint64_t)sec);
            info[32] = 0; /* totalram etc.: an 8 GiB machine with 4 GiB free, in units of one byte */
            {
                snx_st_le64(info + 32, 8ull << 30);
                snx_st_le64(info + 40, 4ull << 30);
                snx_st_le16(info + 80, 1);  /* procs */
                snx_st_le32(info + 104, 1); /* mem_unit */
            }
            return gnu_memory_write(&p->memory, a[0], info, sizeof info) ? -GNU_EFAULT : 0;
        }
        case GNU_NR_set_tid_address: p->clear_tid = a[0]; return p->tid;
        case GNU_NR_futex: return gnu_futex(p, a);
        case GNU_NR_rt_sigaction: return gnu_sigaction(p, a);
        case GNU_NR_rt_sigprocmask: return gnu_sigprocmask(p, a);
        case GNU_NR_brk: return (int64_t)gnu_memory_brk(&p->memory, a[0]);
        case GNU_NR_munmap: return gnu_memory_unmap(&p->memory, a[0], a[1]);
        case GNU_NR_mprotect:
            if (a[2] & ~7ull) return -GNU_EINVAL;
            return gnu_memory_protect(&p->memory, a[0], a[1], (unsigned)a[2]);
        case GNU_NR_mmap:
            if (a[2] & ~7ull) return -GNU_EINVAL;
            if (!(a[3] & 0x20)) return file_mmap(p, a);
            if (a[5] & 4095) return -GNU_EINVAL;
            return gnu_memory_mmap(&p->memory, a[0], a[1], (unsigned)a[2], a[3]);
        case GNU_NR_uname: {
            char uts[6][65] = {0};
            strcpy(uts[0], "Linux");
            strcpy(uts[1], "scarletnx");
            strcpy(uts[2], "6.6.0");
            strcpy(uts[3], "ScarletNX GNU ABI");
            strcpy(uts[4], "aarch64");
            strcpy(uts[5], "(none)");
            return gnu_memory_write(&p->memory, a[0], uts, sizeof uts);
        }
        case GNU_NR_clock_gettime: {
            int64_t ts[2];
            result = gnu_clock_gettime((int)a[0], &ts[0], &ts[1]);
            if (result) return result;
            ts[0] = (int64_t)snx_le64((uint64_t)ts[0]);
            ts[1] = (int64_t)snx_le64((uint64_t)ts[1]);
            return gnu_memory_write(&p->memory, a[1], ts, sizeof ts);
        }
        case 114: { /* clock_getres */
            int64_t sec, ns;
            result = gnu_clock_gettime((int)a[0], &sec, &ns);
            if (result || !a[1]) return result;
            int64_t resolution[2] = {0, (int64_t)SNX_LE64(100)};
            return gnu_memory_write(&p->memory, a[1], resolution, sizeof resolution);
        }
        case GNU_NR_getrandom: {
            if ((a[2] & ~7ull) || ((a[2] & 6) == 6)) return -GNU_EINVAL;
            size_t count = a[1] > 0x2000000ull ? 0x2000000 : (size_t)a[1];
            void* destination = gnu_memory_access(&p->memory, a[0], count, GNU_PROT_WRITE);
            if (!destination) return -GNU_EFAULT;
            result = gnu_random_bytes(destination, count);
            return result ? result : (int64_t)count;
        }
        case 261: { /* prlimit64: query guest resource limits */
            if (a[0] && a[0] != p->pid) return -GNU_ESRCH;
            if (a[1] >= 16) return -GNU_EINVAL;
            if (a[2]) return 0; /* setting a limit: nothing here enforces one */
            uint64_t limits[2] = {UINT64_MAX, UINT64_MAX};
            if (a[1] == 3) limits[0] = 0x800000ull;
            if (a[1] == 7) limits[0] = limits[1] = 256;
            limits[0] = snx_le64(limits[0]);
            limits[1] = snx_le64(limits[1]);
            return a[3] ? gnu_memory_write(&p->memory, a[3], limits, sizeof limits) : 0;
        }
        case 167: {                   /* prctl */
            if (a[0] == 3) return 1;  /* PR_GET_DUMPABLE */
            if (a[0] == 21) return 0; /* PR_GET_SECCOMP */
            return -GNU_EINVAL;
        }
        case 124: return 0; /* sched_yield; one guest thread */
        default: return -GNU_ENOSYS;
    }
}
