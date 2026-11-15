/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* passes.h: one compute layout for every pass (the picture sampled at binding 0, the result stored at binding 1, the
   sizes and sharpness as push constants), a pipeline per pass and descriptor sets that go round
   SNX_UPSCALER_IN_FLIGHT evaluations. The spatial upscaler's EASU writes an image of ours at the output's size when RCAS
   follows, and the output itself when not. The passes record plain vkCmd* calls with Vulkan 1.0 barriers, so any
   device and command buffer take them */
#include "nx/gpu/vk/upscaler/passes.h"
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/upscaler/upscaler_spv.h"

enum { PASS_COPY, PASS_EASU, PASS_RCAS, PASSES };

struct SnxUpPasses {
    VkDevice device;
    int spatial;
    uint32_t output[2];
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline[PASSES];
    VkDescriptorPool pool;
    VkDescriptorSet sets[SNX_UPSCALER_IN_FLIGHT][2];
    uint32_t next;
    /* EASU's result before RCAS: in VK_IMAGE_LAYOUT_GENERAL once fresh is 0 */
    VkImage mid;
    VkDeviceMemory mid_memory;
    VkImageView mid_view;
    int fresh;
};

/* the push constants of up_*.comp */
typedef struct {
    uint32_t render[2], size[2], output[2];
    float sharpness, pad;
} Push;

static SnxUpPasses* refuse(SnxUpPasses* p, const char* what, const char** why) {
    *why = what;
    snx_up_passes_destroy(p);
    return NULL;
}

static VkPipeline pipeline(SnxUpPasses* p, const uint32_t* words, uint64_t bytes) {
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)bytes, .pCode = words};
    VkShaderModule module;
    VkPipeline made = VK_NULL_HANDLE;
    if (vkCreateShaderModule(p->device, &mi, NULL, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = p->layout};
    if (vkCreateComputePipelines(p->device, VK_NULL_HANDLE, 1, &ci, NULL, &made) != VK_SUCCESS) made = VK_NULL_HANDLE;
    vkDestroyShaderModule(p->device, module, NULL);
    return made;
}

/* EASU's result image, device-local where the device has such memory */
static int make_mid(SnxUpPasses* p, VkPhysicalDevice physical) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                            .extent = {p->output[0], p->output[1], 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vkCreateImage(p->device, &ii, NULL, &p->mid) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetImageMemoryRequirements(p->device, p->mid, &req);
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (req.memoryTypeBits >> i & 1) {
            if (type == UINT32_MAX) type = i;
            if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                type = i;
                break;
            }
        }
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(p->device, &ai, NULL, &p->mid_memory) != VK_SUCCESS ||
        vkBindImageMemory(p->device, p->mid, p->mid_memory, 0) != VK_SUCCESS)
        return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = p->mid,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = ii.format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(p->device, &vi, NULL, &p->mid_view) == VK_SUCCESS;
}

SnxUpPasses* snx_up_passes_create(const SnxUpscalerInfo* info, int spatial, const char** why) {
    SnxUpPasses* p = (SnxUpPasses*)calloc(1, sizeof *p);
    if (!p) {
        *why = "no memory";
        return NULL;
    }
    p->device = info->device;
    p->spatial = spatial;
    p->output[0] = info->output[0], p->output[1] = info->output[1];
    p->fresh = 1;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (vkCreateSampler(p->device, &si, NULL, &p->sampler) != VK_SUCCESS)
        return refuse(p, "the device refused the upscaler's sampler", why);
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    if (vkCreateDescriptorSetLayout(p->device, &li, NULL, &p->set_layout) != VK_SUCCESS)
        return refuse(p, "the device refused the upscaler's descriptor layout", why);
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &p->set_layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &range};
    if (vkCreatePipelineLayout(p->device, &pi, NULL, &p->layout) != VK_SUCCESS)
        return refuse(p, "the device refused the upscaler's pipeline layout", why);
    if (spatial) {
        p->pipeline[PASS_EASU] = pipeline(p, up_easu_spv, up_easu_spv_size);
        p->pipeline[PASS_RCAS] = pipeline(p, up_rcas_spv, up_rcas_spv_size);
        if (!p->pipeline[PASS_EASU] || !p->pipeline[PASS_RCAS])
            return refuse(p, "the device refused the spatial upscaler's pipelines", why);
        if (!make_mid(p, info->physical)) return refuse(p, "no memory for the spatial upscaler's image", why);
    } else if (!(p->pipeline[PASS_COPY] = pipeline(p, up_copy_spv, up_copy_spv_size)))
        return refuse(p, "the device refused the copy's pipeline", why);
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * SNX_UPSCALER_IN_FLIGHT},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * SNX_UPSCALER_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 2 * SNX_UPSCALER_IN_FLIGHT,
                                     .poolSizeCount = 2,
                                     .pPoolSizes = sizes};
    if (vkCreateDescriptorPool(p->device, &dp, NULL, &p->pool) != VK_SUCCESS)
        return refuse(p, "the device refused the upscaler's descriptor pool", why);
    VkDescriptorSetLayout layouts[2 * SNX_UPSCALER_IN_FLIGHT];
    for (uint32_t i = 0; i < 2 * SNX_UPSCALER_IN_FLIGHT; i++) layouts[i] = p->set_layout;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = p->pool,
                                      .descriptorSetCount = 2 * SNX_UPSCALER_IN_FLIGHT,
                                      .pSetLayouts = layouts};
    if (vkAllocateDescriptorSets(p->device, &ai, &p->sets[0][0]) != VK_SUCCESS)
        return refuse(p, "the device refused the upscaler's descriptor sets", why);
    return p;
}

void snx_up_passes_destroy(SnxUpPasses* p) {
    if (!p) return;
    for (int i = 0; i < PASSES; i++) vkDestroyPipeline(p->device, p->pipeline[i], NULL);
    vkDestroyDescriptorPool(p->device, p->pool, NULL);
    vkDestroyPipelineLayout(p->device, p->layout, NULL);
    vkDestroyDescriptorSetLayout(p->device, p->set_layout, NULL);
    vkDestroySampler(p->device, p->sampler, NULL);
    vkDestroyImageView(p->device, p->mid_view, NULL);
    vkDestroyImage(p->device, p->mid, NULL);
    vkFreeMemory(p->device, p->mid_memory, NULL);
    free(p);
}

/* one pass: the picture sampled from view in layout, the result stored to out (VK_IMAGE_LAYOUT_GENERAL) */
static void pass(SnxUpPasses* p, VkCommandBuffer cb, int which, VkDescriptorSet set, VkImageView view,
                 VkImageLayout layout, VkImageView out, const Push* push) {
    VkDescriptorImageInfo in = {p->sampler, view, layout}, store = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &in},
                                 {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 1,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  .pImageInfo = &store}};
    vkUpdateDescriptorSets(p->device, 2, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline[which]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *push, push);
    vkCmdDispatch(cb, (push->output[0] + 7) / 8, (push->output[1] + 7) / 8, 1);
}

/* the mid image from its last use (or none) to what comes next: a write by EASU, or RCAS's read of it */
static void mid_barrier(SnxUpPasses* p, VkCommandBuffer cb, int fresh, VkAccessFlags from, VkAccessFlags to) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = from,
                              .dstAccessMask = to,
                              .oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = p->mid,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}

int snx_up_passes_record(SnxUpPasses* p, const SnxUpscaleEval* e, const char** why) {
    if (!e->color.image || !e->output.image) {
        *why = "an upscale without its color or output";
        return 0;
    }
    if (e->output.width < p->output[0] || e->output.height < p->output[1]) {
        *why = "the output image is smaller than the context's output size";
        return 0;
    }
    Push push = {{e->render[0], e->render[1]},
                 {e->color.width, e->color.height},
                 {p->output[0], p->output[1]},
                 e->sharpness > 1 ? 1 : e->sharpness,
                 0};
    VkDescriptorSet* sets = p->sets[p->next++ % SNX_UPSCALER_IN_FLIGHT];
    if (!p->spatial) {
        pass(p, e->cb, PASS_COPY, sets[0], e->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, e->output.view,
             &push);
        return 1;
    }
    if (!(push.sharpness > 0)) {
        pass(p, e->cb, PASS_EASU, sets[0], e->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, e->output.view,
             &push);
        return 1;
    }
    /* EASU into ours (after the last evaluation's RCAS read it), then RCAS from it into the output */
    mid_barrier(p, e->cb, p->fresh, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    p->fresh = 0;
    pass(p, e->cb, PASS_EASU, sets[0], e->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, p->mid_view, &push);
    mid_barrier(p, e->cb, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    pass(p, e->cb, PASS_RCAS, sets[1], p->mid_view, VK_IMAGE_LAYOUT_GENERAL, e->output.view, &push);
    return 1;
}
