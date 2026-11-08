/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The compute pipelines of common/gpu/vk/vk_common.h, through fakes of the three Vulkan calls they make: a plain one
   asks nothing of its subgroups; snx_vk_compute_pipeline_subgroups asks for their size (32, as a program whose lanes
   are a warp's needs) and, when full, for whole ones. Each frees its module once, and a module that fails stops it */
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>

typedef struct {
    int modules, freed, pipelines;
    VkResult module_result;
    uint32_t size; /* requiredSubgroupSize, 0 with no VkPipelineShaderStageRequiredSubgroupSizeCreateInfo */
    VkPipelineShaderStageCreateFlags flags;
} Fake;
static Fake g_fake;

static VKAPI_ATTR VkResult VKAPI_CALL fake_module(VkDevice dev, const VkShaderModuleCreateInfo* info,
                                                  const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    (void)dev, (void)info, (void)alloc;
    g_fake.modules++;
    *out = VK_NULL_HANDLE;
    return g_fake.module_result;
}
static VKAPI_ATTR void VKAPI_CALL fake_free(VkDevice dev, VkShaderModule module, const VkAllocationCallbacks* alloc) {
    (void)dev, (void)module, (void)alloc;
    g_fake.freed++;
}
static VKAPI_ATTR VkResult VKAPI_CALL fake_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                     const VkComputePipelineCreateInfo* info,
                                                     const VkAllocationCallbacks* alloc, VkPipeline* out) {
    (void)dev, (void)cache, (void)alloc;
    g_fake.pipelines += (int)n;
    g_fake.size = 0;
    for (const VkBaseInStructure* s = info->stage.pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO)
            g_fake.size =
                ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*)(const void*)s)->requiredSubgroupSize;
    g_fake.flags = info->stage.flags;
    *out = VK_NULL_HANDLE;
    return VK_SUCCESS;
}

int main(void) {
    static const uint32_t words[5] = {0x07230203u};
    /* the subgroup size asked (0: the plain call), whole subgroups, then the size and the flags seen */
    static const struct {
        uint32_t subgroup;
        bool full, plain;
        uint32_t size;
        VkPipelineShaderStageCreateFlags flags;
    } cases[] = {
        {0, false, true, 0, 0},
        {0, false, false, 0, 0},
        {32, false, false, 32, 0},
        {32, true, false, 32, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT},
    };
    int failures = 0;
    vkCreateShaderModule = fake_module;
    vkDestroyShaderModule = fake_free;
    vkCreateComputePipelines = fake_pipelines;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        VkPipeline p;
        g_fake = (Fake){.module_result = VK_SUCCESS};
        VkResult r =
            cases[i].plain
                ? snx_vk_compute_pipeline(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, words, sizeof words, &p)
                : snx_vk_compute_pipeline_subgroups(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, words, sizeof words,
                                                    cases[i].subgroup, cases[i].full, &p);
        int ok = r == VK_SUCCESS && g_fake.modules == 1 && g_fake.freed == 1 && g_fake.pipelines == 1 &&
                 g_fake.size == cases[i].size && g_fake.flags == cases[i].flags;
        printf("%s subgroup=%u full=%d: result %d, size %u, flags %#x, %d modules made and %d freed, %d pipelines%s\n",
               cases[i].plain ? "snx_vk_compute_pipeline" : "snx_vk_compute_pipeline_subgroups", cases[i].subgroup,
               cases[i].full, (int)r, g_fake.size, (unsigned)g_fake.flags, g_fake.modules, g_fake.freed,
               g_fake.pipelines, ok ? "" : ": FAIL");
        if (!ok) {
            fprintf(stderr, "  size %u and flags %#x expected, one module made and freed, one pipeline\n",
                    cases[i].size, (unsigned)cases[i].flags);
            failures++;
        }
    }
    /* a module the driver refuses: its error, and no pipeline */
    VkPipeline p;
    g_fake = (Fake){.module_result = VK_ERROR_OUT_OF_HOST_MEMORY};
    VkResult r = snx_vk_compute_pipeline_subgroups(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, words, sizeof words,
                                                   32, true, &p);
    int ok = r == VK_ERROR_OUT_OF_HOST_MEMORY && g_fake.pipelines == 0 && g_fake.freed == 0;
    printf("a refused module: result %d, %d pipelines, %d modules freed%s\n", (int)r, g_fake.pipelines, g_fake.freed,
           ok ? "" : ": FAIL");
    failures += !ok;
    return failures != 0;
}
