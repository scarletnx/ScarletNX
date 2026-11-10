/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's clocks (time.h), without SDL: the clocks are the ones SDL_GetTicksNS and SDL_GetCurrentTime read, so an
   interval the runtime took through SDL before reads the same here */
#include "common/platform/time.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#if defined(_WIN32)
/* ticks of the frequency freq in nanoseconds, without the product's overflow */
static uint64_t ticks_ns(uint64_t ticks, uint64_t freq) {
    return ticks / freq * 1000000000u + ticks % freq * 1000000000u / freq;
}
uint64_t plat_ns(void) {
    LARGE_INTEGER counter, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return ticks_ns((uint64_t)counter.QuadPart, (uint64_t)freq.QuadPart);
}
uint64_t plat_monotonic_ns(void) { return 0; }
uint64_t plat_qpc_ns(uint64_t ticks) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return ticks_ns(ticks, (uint64_t)freq.QuadPart);
}
int64_t plat_unix_ns(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    /* 100 ns units since 1601 */
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
    return (int64_t)ticks * 100;
}
/* the local time of now minus its UTC, as SDL_TimeToDateTime says it */
int64_t plat_tz_offset(void) {
    SYSTEMTIME utc, local;
    FILETIME fu, fl;
    GetSystemTime(&utc);
    if (!SystemTimeToTzSpecificLocalTime(NULL, &utc, &local) || !SystemTimeToFileTime(&utc, &fu) ||
        !SystemTimeToFileTime(&local, &fl))
        return 0;
    int64_t u = (int64_t)((uint64_t)fu.dwHighDateTime << 32 | fu.dwLowDateTime);
    int64_t l = (int64_t)((uint64_t)fl.dwHighDateTime << 32 | fl.dwLowDateTime);
    return (l - u) / 10000000;
}
#else
static uint64_t clock_ns(clockid_t id) {
    struct timespec ts;
    if (clock_gettime(id, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
uint64_t plat_ns(void) {
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); /* mach_absolute_time, in nanoseconds */
#elif defined(CLOCK_MONOTONIC_RAW) && !defined(__ANDROID__)
    return clock_ns(CLOCK_MONOTONIC_RAW); /* NTP doesn't slew it */
#else
    return clock_ns(CLOCK_MONOTONIC);
#endif
}
uint64_t plat_monotonic_ns(void) { return clock_ns(CLOCK_MONOTONIC); }
uint64_t plat_qpc_ns(uint64_t ticks) {
    (void)ticks;
    return 0;
}
int64_t plat_unix_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) return 0;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
int64_t plat_tz_offset(void) {
    time_t now = time(NULL);
    struct tm local;
    if (!localtime_r(&now, &local)) return 0;
    return (int64_t)local.tm_gmtoff;
}
#endif

int64_t plat_unix_time(void) { return plat_unix_ns() / 1000000000; }
