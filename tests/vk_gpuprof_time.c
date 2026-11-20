/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The time between two timestamps of a queue (gpu/vk/gpuprof_time.h). A pass the queue began before the pass recorded
   before it ended gave a gap of the whole counter: one report said "gaps with barriers only: 44130966694.04 ms a
   frame" beside gaps of 10 ms */
#include <stdint.h>
#include <stdio.h>
#include "nx/gpu/vk/gpuprof_time.h"

static int g_failures;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            g_failures++;                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

int main(void) {
    CHECK(vk_gpuprof_ns(64, 1000, 1250, 1.0) == 250, "250 ticks of 1 ns are 250 ns");
    CHECK(vk_gpuprof_ns(64, 1000, 1250, 2.5) == 625, "250 ticks of 2.5 ns are 625 ns");
    CHECK(vk_gpuprof_ns(64, 1250, 1000, 1.0) == 0, "a start read 250 ticks before the end before it is a gap of 0");
    CHECK(vk_gpuprof_ns(64, 1000, 1000, 1.0) == 0, "the same tick is 0 ns");
    CHECK(vk_gpuprof_ticks(64, 1250, 1000) == -250, "to before from counts below 0");
    /* a counter of 36 valid bits wraps: 16 ticks before its end to 9 after its start */
    CHECK(vk_gpuprof_ticks(36, (1ull << 36) - 16, 9) == 25, "a wrap of a 36-bit counter is 25 ticks");
    CHECK(vk_gpuprof_ns(36, (1ull << 36) - 16, 9, 1.0) == 25, "a wrap of a 36-bit counter is 25 ns");
    CHECK(vk_gpuprof_ns(36, 9, (1ull << 36) - 16, 1.0) == 0, "the wrap read backwards is 0 ns");
    CHECK(vk_gpuprof_ns(0, 5, 9, 1.0) == 0, "a queue with no valid bits times nothing");
    if (g_failures) return 1;
    puts("vk_gpuprof_time: ok");
    return 0;
}
