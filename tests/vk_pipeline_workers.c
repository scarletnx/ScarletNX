/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How many workers make the new pipelines (common/gpu/vk/vk_common.h's snx_vk_pipeline_workers), for the Switch's
   renderer and the 3DS's alike, and how many of them run at the system's low priority (snx_vk_pipeline_low_workers):
   two at normal priority, the rest low, for pipeline_threads at Auto and set by the player, on 4, 12 and 64 CPUs */
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>

int main(void) {
    /* pipeline_threads, the CPUs, then the workers and how many of them are low */
    static const int cases[][4] = {
        {0, 12, 10, 8}, {0, 4, 2, 0},  {0, 64, 12, 10}, {0, 1, 2, 0},     {1, 12, 1, 0},   {1, 64, 1, 0},
        {2, 12, 2, 0},  {3, 12, 3, 1}, {3, 4, 3, 1},    {12, 12, 12, 10}, {12, 4, 12, 10}, {32, 4, 32, 30},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        int n = snx_vk_pipeline_workers(cases[i][0], cases[i][1]), low = snx_vk_pipeline_low_workers(n);
        int ok = n == cases[i][2] && low == cases[i][3];
        printf("pipeline_threads=%d on %d CPUs: %d workers, %d normal and %d low%s\n", cases[i][0], cases[i][1], n,
               n - low, low, ok ? "" : ": FAIL");
        if (!ok) {
            fprintf(stderr, "  %d workers, %d low expected\n", cases[i][2], cases[i][3]);
            failures++;
        }
    }
    return failures != 0;
}
