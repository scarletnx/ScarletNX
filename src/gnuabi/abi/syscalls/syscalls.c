/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/abi/syscalls/syscalls.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/process/process.h"
#include "gnuabi/kernel/signals.h"
#include "gnuabi/kernel/futex.h"
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
static int64_t dispatch(GnuThread* t, uint64_t number, const uint64_t a[6]);
static struct {
    int trace; /* gnu_syscall_trace's */
} g_syscalls;
void gnu_syscall_trace(int on) { g_syscalls.trace = on; }
/* --strace: every syscall on stderr, with its arguments and result, a line each, with the pid and tid */
int64_t gnu_syscall(GnuThread* t, uint64_t number, const uint64_t a[6]) {
    int trace = g_syscalls.trace;
    /* arm64 el0_svc_common takes an int syscall number from x8. */
    number = (uint32_t)number;
    if (trace)
        fprintf(stderr, "[%u:%u] %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx) ...\n", gnu_process_of(t)->kernel.pid, t->k.tid,
                (unsigned long long)number, (unsigned long long)a[0], (unsigned long long)a[1],
                (unsigned long long)a[2], (unsigned long long)a[3]);
    int64_t result = dispatch(t, number, a);
    if (trace)
        fprintf(stderr, "[%u:%u] %llu = %lld\n", gnu_process_of(t)->kernel.pid, t->k.tid, (unsigned long long)number,
                (long long)result);
    return result;
}
static int64_t dispatch(GnuThread* t, uint64_t number, const uint64_t a[6]) {
    GnuProcess* p = gnu_process_of(t);
    int64_t result;
    if (gnu_spawn_syscall(t, number, a, &result)) return result;
    if (gnu_kernel_syscall(&t->k, number, a, &result)) return result; /* threads, futexes, sleeps, signals */
    if (gnu_vfs_syscall(&p->vfs, &p->memory, number, a, &result)) return result;
    if (number == 22 || number == 72 || number == 73 || number == 441) {
        result = gnu_signal_call_mask(&t->k, number, a); /* the mask the wait has */
        if (result) return result;
    }
    if (gnu_net_syscall(p, number, a, &result)) return result;
    switch (number) {
        /* the ids, clocks, random bytes, prctl and the rest of the calls that need no runtime are the kernel's
           (gnu_kernel_syscall, src/gnuabi/kernel/system.c): here the memory map's */
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
        default: return -GNU_ENOSYS;
    }
}
