/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_SYNC_H
#define SNX_GNU_SYNC_H
/* The host's locks, condition variables and clocks, for the kernel library (kernel.h) and the GNU ABI runtime's shared
   state: the JIT, the memory map and the descriptor table */
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct {
    CRITICAL_SECTION section; /* recursive */
} GnuMutex;
typedef struct {
    CONDITION_VARIABLE cond;
} GnuCond;
#else
#include <pthread.h>
typedef struct {
    pthread_mutex_t mutex;
} GnuMutex;
typedef struct {
    pthread_cond_t cond; /* on the monotonic clock where the host has one for condition variables */
} GnuCond;
#endif
/* recursive: the owner can lock it again, and unlocks it as many times */
void gnu_mutex_init(GnuMutex* mutex, int recursive);
void gnu_mutex_destroy(GnuMutex* mutex);
void gnu_mutex_lock(GnuMutex* mutex);
void gnu_mutex_unlock(GnuMutex* mutex);
void gnu_cond_init(GnuCond* cond);
void gnu_cond_destroy(GnuCond* cond);
void gnu_cond_broadcast(GnuCond* cond);
/* until a broadcast or a spurious wakeup, or until gnu_monotonic_ns() reaches deadline (-1: no deadline): 1 when the
   deadline passed, else 0. mutex is locked once by the caller */
int gnu_cond_wait(GnuCond* cond, GnuMutex* mutex, int64_t deadline);
/* the host's monotonic and realtime clocks in nanoseconds */
int64_t gnu_monotonic_ns(void);
int64_t gnu_realtime_ns(void);
#endif
