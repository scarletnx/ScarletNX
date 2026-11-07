/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The test-only interception preserves source create infos, redirects only the cache, and adds the experimental
   flag to either ordinary or flags2 state. Unexpected batching or chain placement is rejected before the driver. */
#include "support/pipeline_probe_flags.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int requested, heaps, mismatches, calls;
static VkPipelineCache wanted_cache;
static VkPipelineRenderingCreateInfo rendering;
static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice device, VkPipelineCache cache, uint32_t count,
                                            const VkGraphicsPipelineCreateInfo* info,
                                            const VkAllocationCallbacks* allocator, VkPipeline* output) {
    (void)device, (void)allocator;
    calls++;
    mismatches += cache != wanted_cache || count != 1;
    if (heaps) {
        const VkPipelineCreateFlags2CreateInfo* flags = info->pNext;
        mismatches += flags->sType != VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO || info->flags != 0;
        mismatches += flags->flags != (VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT |
            VK_PIPELINE_CREATE_2_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
            (requested ? VK_PIPELINE_CREATE_2_DISABLE_OPTIMIZATION_BIT : 0));
        mismatches += flags->pNext != &rendering;
    } else {
        mismatches += info->flags != (VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
            (requested ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0));
        mismatches += info->pNext != &rendering;
    }
    *output = (VkPipeline)(uintptr_t)4;
    return VK_SUCCESS;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "pipeline_probe_flags:%d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    wanted_cache = (VkPipelineCache)(uintptr_t)6;
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    VkPipelineCreateFlags2CreateInfo flags2 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &rendering, .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT |
            VK_PIPELINE_CREATE_2_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    for (heaps = 0; heaps < 2; heaps++) {
        info.pNext = heaps ? (const void*)&flags2 : &rendering;
        info.flags = heaps ? 0 : VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
                                    VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
        VkGraphicsPipelineCreateInfo original = info;
        VkPipelineCreateFlags2CreateInfo original_flags = flags2;
        for (requested = 0; requested < 2; requested++) {
            g_pipeline_probe_intercept = (PipelineProbeIntercept){create, wanted_cache, requested, 0};
            VkPipeline output;
            CHECK(pipeline_probe_intercept(VK_NULL_HANDLE, (VkPipelineCache)(uintptr_t)9,
                                             1, &info, NULL, &output) == VK_SUCCESS);
            CHECK(output == (VkPipeline)(uintptr_t)4 && g_pipeline_probe_intercept.calls == 1);
            CHECK(!memcmp(&info, &original, sizeof info) && !memcmp(&flags2, &original_flags, sizeof flags2));
        }
    }
    CHECK(calls == 4 && !mismatches);
    CHECK(pipeline_probe_intercept(VK_NULL_HANDLE, wanted_cache, 2, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(calls == 4);
    VkBaseInStructure unexpected = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                    .pNext = (const VkBaseInStructure*)(const void*)&flags2};
    info.pNext = &unexpected;
    CHECK(pipeline_probe_intercept(VK_NULL_HANDLE, wanted_cache, 1, &info, NULL, NULL) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(calls == 4);
    puts("pipeline probe intercept preserves inputs and handles both descriptor backends");
    return 0;
}
