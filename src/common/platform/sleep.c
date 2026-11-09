/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Sleep and yield (time.h). A file of its own, so a test that stubs them links the clocks without them */
#include "common/platform/time.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 /* Windows 10 1803 and later */
#endif
#else
#include <errno.h>
#include <sched.h>
#include <time.h>
#endif

#if defined(_WIN32)
void plat_yield(void) { SwitchToThread(); }
/* Each thread's own waitable timer, made on its first sleep and closed when the thread ends (a fiber-local slot's
   callback): one timer for every thread would let one thread's SetWaitableTimer move another's wait */
static struct {
    INIT_ONCE once;
    DWORD slot; /* FLS_OUT_OF_INDEXES when the system has no slot to give */
} g_sleep = {INIT_ONCE_STATIC_INIT, FLS_OUT_OF_INDEXES};
static void WINAPI timer_close(void* timer) {
    if (timer) CloseHandle(timer);
}
static BOOL CALLBACK slot_make(PINIT_ONCE once, void* param, void** ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    g_sleep.slot = FlsAlloc(timer_close);
    return TRUE;
}
static HANDLE thread_timer(void) {
    InitOnceExecuteOnce(&g_sleep.once, slot_make, NULL, NULL);
    if (g_sleep.slot == FLS_OUT_OF_INDEXES) return NULL;
    HANDLE timer = FlsGetValue(g_sleep.slot);
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (timer && !FlsSetValue(g_sleep.slot, timer)) {
            CloseHandle(timer);
            timer = NULL;
        }
    }
    return timer;
}
void plat_sleep_ns(int64_t ns) {
    if (ns <= 0) {
        plat_yield();
        return;
    }
    HANDLE timer = thread_timer();
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(ns / 100); /* relative, in 100 ns units */
    if (timer && due.QuadPart && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        return;
    }
    uint64_t ms = (uint64_t)ns / 1000000u;
    Sleep(ms >= 0xFFFFFFFEu ? 0xFFFFFFFEu : ms ? (DWORD)ms : 1);
}
#else
void plat_yield(void) { sched_yield(); }
/* nanosleep, going on with what is left after a signal interrupts it */
void plat_sleep_ns(int64_t ns) {
    if (ns <= 0) {
        plat_yield();
        return;
    }
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
#endif
