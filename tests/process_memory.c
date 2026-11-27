/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The process's memory as common.c reads it (snx_footprint, snx_memory_available), which memory_fit and the stats
   take: the footprint told on Linux, Android, macOS, iOS, Windows and FreeBSD, growing with the pages the process
   touches, and the memory available; neither elsewhere. Its peak (snx_footprint_peak), which holds what was touched,
   and the line a recompile ends with, which says it (snx_phases_line). And its processor time (snx_process_cpu_ns),
   which grows with the work it does */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    /* the peak holds the block while it is touched. FreeBSD counts it at its clock's ticks, so it is read until it
       shows, ten seconds at most */
    {
        double give_up = snx_seconds() + 10;
        SnxU64 peak;
        while ((peak = snx_footprint_peak()) < size / 2 && snx_seconds() < give_up) touch[0] = 1;
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || defined(__FreeBSD__)
        assert(peak >= size / 2);
#else
        assert(!peak);
#endif
        printf("process memory: peak %llu MiB\n", (unsigned long long)(peak >> 20));
    }
    free(block);
    /* the line: each phase's seconds, the units' with the threads they ran on, and the peak in MiB; a pack's write
       in the link's and the write's place; a system that tells no peak said so */
    {
        SnxRecompilePhases p = {0};
        char line[512];
        p.discovery = 1.24, p.compile = 3.11, p.units = 12.44, p.descriptor = 0.2, p.link = 1.52, p.write = 0.61;
        p.threads = 4;
        p.peak = (SnxU64)2230 << 20;
        snx_phases_line(line, sizeof line, &p);
        assert(!strcmp(line, "code: discovery 1.2 s, compile 3.1 s (12.4 s of units on 4 threads), descriptor "
                             "0.2 s, link 1.5 s, write 0.6 s; peak 2230 MiB"));
        p.threads = 1, p.pack = 1, p.peak = 0;
        snx_phases_line(line, sizeof line, &p);
        assert(!strcmp(line, "code: discovery 1.2 s, compile 3.1 s (12.4 s of units on 1 thread), descriptor "
                             "0.2 s, pack 1.5 s; the system does not say the peak"));
        p.pack = 0, p.streamed = 1;
        snx_phases_line(line, sizeof line, &p);
        assert(!strcmp(line, "code: discovery 1.2 s, compile 3.1 s (12.4 s of units on 1 thread), descriptor "
                             "0.2 s, link and write 2.1 s; the system does not say the peak"));
        snx_recompile_line(line, sizeof line, 2.94, 4.91, 0.26);
        assert(!strcmp(line, "recompile: 8.1 s in all: 2.9 s before the code, 4.9 s the code, 0.3 s after it"));
        /* the SSA's: its instructions, how many a guest instruction, and both sizes in MiB */
        const SnxSsaCount ssa = {5850000, (SnxU64)3 << 30, (SnxU64)700 << 20};
        snx_ssa_line(line, sizeof line, &ssa, 1000000);
        assert(!strcmp(line, "ssa: 5850000 instructions for 1000000 guest instructions (5.85 each), 3072.0 MiB as "
                             "the back end holds it, 700.0 MiB packed"));
    }
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
