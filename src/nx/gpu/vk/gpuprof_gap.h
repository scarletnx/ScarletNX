/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One gap of the graphics queue split by what the CPU was doing in it (gpuprof.c's idle report), on the CPU's clock */
#ifndef SNX_VK_GPUPROF_GAP_H
#define SNX_VK_GPUPROF_GAP_H
#include <stdint.h>

typedef struct {
    uint64_t before; /* from the last end until the submit: the GPU had nothing to do */
    uint64_t queued; /* from the submit until the start: semaphores, the recording thread */
    /* the part of before after the submission's begin, up to its own GPU time: the GPU idled while commands it could
       have run were recorded and not submitted, what submitting them as they were recorded could have run */
    uint64_t fill;
} VkGpGap;

/* The gap before a submission that began recording at cpu0, was submitted at cpu and ran on the GPU from b to e, the
   last one having ended at last_end. All zero when it started before last_end (no gap) */
static inline VkGpGap vk_gpuprof_gap(uint64_t last_end, uint64_t cpu0, uint64_t cpu, uint64_t b, uint64_t e) {
    VkGpGap g = {0, 0, 0};
    if (!last_end || b <= last_end) return g;
    const uint64_t cut = cpu < last_end ? last_end : cpu > b ? b : cpu, from = cpu0 > last_end ? cpu0 : last_end;
    g.before = cut - last_end;
    g.queued = b - cut;
    g.fill = cut > from ? cut - from : 0;
    if (e > b && g.fill > e - b) g.fill = e - b;
    if (e <= b) g.fill = 0;
    return g;
}
#endif
