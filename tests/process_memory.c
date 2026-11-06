/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The process's memory as common.c reads it (snx_footprint, snx_memory_available), which memory_fit and the stats
   take: the footprint told on Linux, Android, macOS, iOS, Windows and FreeBSD, growing with the pages the process
   touches, and the memory available; neither elsewhere. And its processor time (snx_process_cpu_ns),
   which grows with the work it does */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const size_t size = (size_t)64 << 20;
    SnxU64 available = 0, before = snx_footprint(), after;
    unsigned char* block = malloc(size);
    /* a page at a time through volatile: a memset of a block freed unread is a store the compiler drops */
    volatile unsigned char* touch = block;
    assert(block);
    for (size_t i = 0; i < size; i += 4096) touch[i] = 1;
    after = snx_footprint();
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(__FreeBSD__)
    assert(before > 0 && after >= before + size / 2);
#else
    assert(!before && !after);
#endif
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(__FreeBSD__)
    assert(snx_memory_available(&available) && available > 0);
#else
    assert(!snx_memory_available(&available));
#endif
    printf("process memory: footprint %llu MiB, then %llu MiB with %zu MiB touched; %llu MiB available\n",
           (unsigned long long)(before >> 20), (unsigned long long)(after >> 20), size >> 20,
           (unsigned long long)(available >> 20));
    free(block);
    /* 20 ms of the processor's time, spent in a loop: a loaded machine takes longer, ten seconds at most. Windows counts
       it in the clock's ticks (15.6 ms): a process that has spent less than one reads 0, so the loop starts from the
       first tick */
    {
        double give_up = snx_seconds() + 10;
        volatile unsigned spin = 0;
        SnxU64 start, now;
        while (!(start = snx_process_cpu_ns()) && snx_seconds() < give_up) ++spin;
        while ((now = snx_process_cpu_ns()) < start + 20000000u && snx_seconds() < give_up) ++spin;
        assert(start > 0 && now >= start + 20000000u);
        printf("process time: %llu ms, then %llu ms\n", (unsigned long long)(start / 1000000),
               (unsigned long long)(now / 1000000));
    }
    return 0;
}
