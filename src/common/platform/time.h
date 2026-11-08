/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's clocks, sleep and yield, the same for every driver. A guest's own clocks (the GNU ABI's clock ids, the 3DS's
   1900 epoch, the Switch's ticks) are made on top of these, in the driver */
#ifndef SNX_PLATFORM_TIME_H
#define SNX_PLATFORM_TIME_H
#include <stdint.h>

/* A monotonic clock in nanoseconds, the one for every interval the runtime measures: CLOCK_MONOTONIC_RAW on Linux,
   CLOCK_MONOTONIC on Android and the BSDs, mach_absolute_time on Apple systems, QueryPerformanceCounter on Windows
   (the clocks SDL_GetTicksNS reads), counted from the system's own start */
uint64_t plat_ns(void);
/* the system's CLOCK_MONOTONIC in nanoseconds (Vulkan's VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR, a POSIX guest's
   CLOCK_MONOTONIC), which plat_ns isn't everywhere: 0 where there is none (Windows) */
uint64_t plat_monotonic_ns(void);
int64_t plat_unix_ns(void);   /* the wall clock: nanoseconds since 1970 UTC */
int64_t plat_unix_time(void); /* seconds since 1970 UTC */
int64_t plat_tz_offset(void); /* local time minus UTC now, in seconds */
/* ---- sleep.c, a file of its own for the tests that stub these */
/* Sleeps ns nanoseconds, 0 or less yields. Windows waits on the thread's own high-resolution waitable timer (Windows
   10 1803 and later), as Sleep rounds to the system tick (15.6 ms unless raised); Sleep where that timer is missing */
void plat_sleep_ns(int64_t ns);
void plat_yield(void); /* gives the rest of the thread's slice to another thread */
#endif
