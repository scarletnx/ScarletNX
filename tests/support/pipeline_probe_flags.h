/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Diagnostic-only interception of the actual graphics builder. The test quiesces workers, saves the native PFN,
   redirects one synchronous creation to its empty cache, and restores the PFN before checking the result. */
#ifndef SNX_TEST_PIPELINE_PROBE_FLAGS_H
#define SNX_TEST_PIPELINE_PROBE_FLAGS_H
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

typedef struct {
    PFN_vkCreateGraphicsPipelines native;
    VkPipelineCache cache;
    int disable_optimization;
    uint32_t calls;
} PipelineProbeIntercept;
static PipelineProbeIntercept g_pipeline_probe_intercept;
static VKAPI_ATTR VkResult VKAPI_CALL pipeline_probe_intercept(VkDevice device, VkPipelineCache ignored,
                                                               uint32_t count,
                                                               const VkGraphicsPipelineCreateInfo* infos,
                                                               const VkAllocationCallbacks* allocator,
                                                               VkPipeline* output) {
    (void)ignored;
    PipelineProbeIntercept* probe = &g_pipeline_probe_intercept;
    if (!probe->native || count != 1 || !infos ||
        (probe->disable_optimization != 0 && probe->disable_optimization != 1)) return VK_ERROR_INITIALIZATION_FAILED;
    VkGraphicsPipelineCreateInfo create = *infos;
    VkPipelineCreateFlags2CreateInfo flags2;
    const VkBaseInStructure* node = create.pNext;
    if (node && node->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO) {
        flags2 = *(const VkPipelineCreateFlags2CreateInfo*)(const void*)node;
        if (probe->disable_optimization) flags2.flags |= VK_PIPELINE_CREATE_2_DISABLE_OPTIMIZATION_BIT;
        create.pNext = &flags2;
    } else {
        /* The production descriptor-heap builder places flags2 first. Refuse an unexpected chain placement rather
           than adding a duplicate flags2 node or quietly testing a flag that flags2 overrides. */
        for (; node; node = node->pNext)
            if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO) return VK_ERROR_INITIALIZATION_FAILED;
        if (probe->disable_optimization) create.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    }
    probe->calls++;
    return probe->native(device, probe->cache, 1, &create, allocator, output);
}
#endif
