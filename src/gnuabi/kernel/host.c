/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's clocks and random bytes as the guest's Linux has them (kernel.h) */
#ifndef _WIN32
#define _GNU_SOURCE /* CLOCK_BOOTTIME, getentropy */
#endif
#include "gnuabi/kernel/kernel.h"
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/random.h> /* Darwin's getentropy */
#endif

int gnu_random_bytes(void* destination, size_t size) {
    unsigned char* out = destination;
#ifdef _WIN32
    while (size) {
        ULONG n = size > 0x40000000 ? 0x40000000 : (ULONG)size;
        if (BCryptGenRandom(NULL, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return -GNU_EIO;
        out += n;
        size -= n;
    }
#else
    while (size) { /* getentropy gives at most 256 bytes a call */
        size_t n = size > 256 ? 256 : size;
        if (getentropy(out, n)) return -GNU_EIO;
        out += n;
        size -= n;
    }
#endif
    return 0;
}

/* Linux's clock ids: REALTIME 0, MONOTONIC 1, PROCESS_CPUTIME_ID 2, THREAD_CPUTIME_ID 3, MONOTONIC_RAW 4,
   REALTIME_COARSE 5, MONOTONIC_COARSE 6, BOOTTIME 7 */
int gnu_clock_gettime(int id, int64_t* seconds, int64_t* nanos) {
    if (id < 0 || id > 7) return -GNU_EINVAL;
    int64_t ns;
    if (id == 0 || id == 5)
        ns = gnu_realtime_ns();
    else if (id == 2 || id == 3) {
#ifdef _WIN32
        FILETIME created, exit_time, kernel, user;
        BOOL ok = id == 2 ? GetProcessTimes(GetCurrentProcess(), &created, &exit_time, &kernel, &user)
                          : GetThreadTimes(GetCurrentThread(), &created, &exit_time, &kernel, &user);
        if (!ok) return -GNU_EIO;
        uint64_t ticks = (((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                         (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime);
        ns = (int64_t)(ticks * 100);
#else
        struct timespec time;
        if (clock_gettime(id == 2 ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID, &time)) return -GNU_EIO;
        ns = (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
#endif
    } else {
#if defined(__linux__)
        /* BOOTTIME counts a suspend, as the guest's does */
        struct timespec time;
        if (clock_gettime(id == 7 ? CLOCK_BOOTTIME : id == 4 ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC, &time))
            return -GNU_EIO;
        ns = (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
#else
        ns = gnu_monotonic_ns();
#endif
    }
    *seconds = ns / 1000000000;
    *nanos = ns % 1000000000;
    return 0;
}

void gnu_sleep(int64_t seconds, int64_t nanoseconds) {
#ifdef _WIN32
    Sleep((DWORD)(seconds * 1000 + nanoseconds / 1000000));
#else
    struct timespec t;
    t.tv_sec = (time_t)seconds;
    t.tv_nsec = (long)nanoseconds;
    while (nanosleep(&t, &t)) {}
#endif
}
