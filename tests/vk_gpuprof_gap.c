/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A gap of the graphics queue split as vk_gpu_profile reports it (gpu/vk/gpuprof_gap.h): the GPU idle before the
   submit, after it, and the part a submission's commands waited recorded and not submitted, up to its GPU time */
#include <stdio.h>
#include "nx/gpu/vk/gpuprof_gap.h"

static int g_failures;
static void check(const char* what, VkGpGap g, uint64_t before, uint64_t queued, uint64_t fill) {
    if (g.before == before && g.queued == queued && g.fill == fill) return;
    g_failures++;
    printf("FAIL %s: before %llu queued %llu fill %llu, expected %llu %llu %llu\n", what, (unsigned long long)g.before,
           (unsigned long long)g.queued, (unsigned long long)g.fill, (unsigned long long)before,
           (unsigned long long)queued, (unsigned long long)fill);
}

int main(void) {
    /* last end 100; begun 120, submitted 150, ran 160-200: idle 50 before the submit, 10 queued, 30 of it recorded */
    check("begun inside the gap", vk_gpuprof_gap(100, 120, 150, 160, 200), 50, 10, 30);
    /* begun before the last one ended: the whole gap before the submit held commands */
    check("begun before the gap", vk_gpuprof_gap(100, 40, 150, 160, 220), 50, 10, 50);
    /* begun after its submit can't be (the clocks): no fill, the gap still split */
    check("begun at the submit", vk_gpuprof_gap(100, 150, 150, 160, 200), 50, 10, 0);
    /* the fill is held to the submission's own GPU time: 5 */
    check("fill up to its GPU time", vk_gpuprof_gap(100, 40, 150, 160, 165), 50, 10, 5);
    /* submitted before the last end: all queued, nothing recorded waited */
    check("submitted before the last end", vk_gpuprof_gap(100, 40, 90, 130, 200), 0, 30, 0);
    /* the submit on the CPU's clock after the GPU's start (calibration): held to the start */
    check("submit after the start", vk_gpuprof_gap(100, 110, 170, 160, 200), 60, 0, 40);
    /* no gap: started at the last end, or no last end yet */
    check("no gap", vk_gpuprof_gap(100, 40, 150, 100, 200), 0, 0, 0);
    check("no last end", vk_gpuprof_gap(0, 40, 150, 160, 200), 0, 0, 0);
    /* an empty submission (no GPU time) fills nothing */
    check("no GPU time", vk_gpuprof_gap(100, 40, 150, 160, 160), 50, 10, 0);
    if (!g_failures) printf("vk_gpuprof_gap: all checks passed\n");
    return g_failures != 0;
}
