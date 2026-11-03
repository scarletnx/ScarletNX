/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _GNU_SOURCE /* PTHREAD_MUTEX_RECURSIVE, pthread_condattr_setclock */
#endif
#include "gnuabi/kernel/sync.h"
#include <time.h>

#ifdef _WIN32
void gnu_mutex_init(GnuMutex* m, int recursive) {
    (void)recursive; /* a critical section always is */
    InitializeCriticalSection(&m->section);
}
void gnu_mutex_destroy(GnuMutex* m) { DeleteCriticalSection(&m->section); }
void gnu_mutex_lock(GnuMutex* m) { EnterCriticalSection(&m->section); }
void gnu_mutex_unlock(GnuMutex* m) { LeaveCriticalSection(&m->section); }
void gnu_cond_init(GnuCond* c) { InitializeConditionVariable(&c->cond); }
void gnu_cond_destroy(GnuCond* c) { (void)c; }
void gnu_cond_broadcast(GnuCond* c) { WakeAllConditionVariable(&c->cond); }
int64_t gnu_monotonic_ns(void) {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return counter.QuadPart / frequency.QuadPart * 1000000000 +
           counter.QuadPart % frequency.QuadPart * 1000000000 / frequency.QuadPart;
}
int64_t gnu_realtime_ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
    return (int64_t)(ticks * 100);
}
int gnu_cond_wait(GnuCond* c, GnuMutex* m, int64_t deadline) {
    DWORD ms = INFINITE;
    if (deadline >= 0) {
        int64_t left = deadline - gnu_monotonic_ns();
        if (left <= 0) return 1;
        /* rounded up, and in bounded steps: the caller waits again */
        ms = left / 1000000 >= 0x7fffffff ? 0x7fffffff : (DWORD)((left + 999999) / 1000000);
    }
    SleepConditionVariableCS(&c->cond, &m->section, ms);
    return deadline >= 0 && gnu_monotonic_ns() >= deadline;
}
#else
void gnu_mutex_init(GnuMutex* m, int recursive) {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    if (recursive) pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
}
void gnu_mutex_destroy(GnuMutex* m) { pthread_mutex_destroy(&m->mutex); }
void gnu_mutex_lock(GnuMutex* m) { pthread_mutex_lock(&m->mutex); }
void gnu_mutex_unlock(GnuMutex* m) { pthread_mutex_unlock(&m->mutex); }
/* macOS has no clock attribute for condition variables: its waits are on the realtime clock */
#if defined(__APPLE__)
#define COND_CLOCK CLOCK_REALTIME
#else
#define COND_CLOCK CLOCK_MONOTONIC
#endif
void gnu_cond_init(GnuCond* c) {
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&c->cond, &attributes);
    pthread_condattr_destroy(&attributes);
}
void gnu_cond_destroy(GnuCond* c) { pthread_cond_destroy(&c->cond); }
void gnu_cond_broadcast(GnuCond* c) { pthread_cond_broadcast(&c->cond); }
int64_t gnu_monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
int64_t gnu_realtime_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
int gnu_cond_wait(GnuCond* c, GnuMutex* m, int64_t deadline) {
    if (deadline < 0) {
        pthread_cond_wait(&c->cond, &m->mutex);
        return 0;
    }
    int64_t now = gnu_monotonic_ns();
    if (now >= deadline) return 1;
    struct timespec at;
    clock_gettime(COND_CLOCK, &at);
    /* the deadline on the condition variable's clock, the same distance away */
    int64_t ns = (int64_t)at.tv_nsec + (deadline - now) % 1000000000;
    int64_t seconds = (deadline - now) / 1000000000 + ns / 1000000000;
    const int64_t far = 0x7fffffff; /* a time_t of 32 bits stops here */
    at.tv_sec = seconds > far - (int64_t)at.tv_sec ? (time_t)far : at.tv_sec + (time_t)seconds;
    at.tv_nsec = (long)(ns % 1000000000);
    pthread_cond_timedwait(&c->cond, &m->mutex, &at);
    return gnu_monotonic_ns() >= deadline;
}
#endif
