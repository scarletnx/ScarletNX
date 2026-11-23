/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* reproject.h: one compute pass (up_reproject.comp) from a depth image to a motion vectors image of ours (two 16-bit
   floats a pixel, at the largest render size). The caller's matrix is the frame before's view-projection times the
   inverse of this frame's, with the picture's coordinates (y down) and the depth's stored range folded in, so the pass
   knows no camera convention of its own. The descriptor sets go round SNX_UPSCALER_IN_FLIGHT evaluations, and the
   pass records plain vkCmd* calls with Vulkan 1.0 barriers */
#include "nx/gpu/vk/upscaler/reproject.h"
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/upscaler/upscaler_spv.h"

struct SnxReproject {
    VkDevice device;
    uint32_t size[2];
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorPool pool;
    VkDescriptorSet sets[SNX_UPSCALER_IN_FLIGHT];
    uint32_t next;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

/* up_reproject.comp's push constants */
typedef struct {
    float reproject[16];
    uint32_t render[2];
} Push;

static SnxReproject* refuse(SnxReproject* r, const char* what, const char** why) {
    *why = what;
    snx_reproject_destroy(r);
    return NULL;
}

SnxReproject* snx_reproject_create(VkDevice device, VkPhysicalDevice physical, const uint32_t render_max[2],
                                   const char** why) {
    SnxReproject* r = calloc(1, sizeof *r);
    if (!r) {
        *why = "no memory";
        return NULL;
    }
    r->device = device;
    r->size[0] = render_max[0], r->size[1] = render_max[1];
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    if (vkCreateSampler(device, &si, NULL, &r->sampler) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(device, &li, NULL, &r->set_layout) != VK_SUCCESS)
        return refuse(r, "the device refused the motion vectors pass's layout", why);
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &r->set_layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(device, &pi, NULL, &r->layout) != VK_SUCCESS)
        return refuse(r, "the device refused the motion vectors pass's layout", why);
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = (size_t)up_reproject_spv_size,
                                   .pCode = up_reproject_spv};
    VkShaderModule module;
    if (vkCreateShaderModule(device, &mi, NULL, &module) != VK_SUCCESS)
        return refuse(r, "the device refused the motion vectors pass", why);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = r->layout};
    VkResult made = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &r->pipeline);
    vkDestroyShaderModule(device, module, NULL);
    if (made != VK_SUCCESS) return refuse(r, "the device refused the motion vectors pass", why);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SNX_UPSCALER_IN_FLIGHT},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SNX_UPSCALER_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = SNX_UPSCALER_IN_FLIGHT,
                                     .poolSizeCount = 2,
                                     .pPoolSizes = ps};
    if (vkCreateDescriptorPool(device, &dp, NULL, &r->pool) != VK_SUCCESS)
        return refuse(r, "the device refused the motion vectors pass's descriptors", why);
    VkDescriptorSetLayout layouts[SNX_UPSCALER_IN_FLIGHT];
    for (int i = 0; i < SNX_UPSCALER_IN_FLIGHT; i++) layouts[i] = r->set_layout;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = r->pool,
                                      .descriptorSetCount = SNX_UPSCALER_IN_FLIGHT,
                                      .pSetLayouts = layouts};
    if (vkAllocateDescriptorSets(device, &ai, r->sets) != VK_SUCCESS)
        return refuse(r, "the device refused the motion vectors pass's descriptors", why);
    /* the image, device-local where the device has such memory */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16_SFLOAT,
                            .extent = {r->size[0], r->size[1], 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vkCreateImage(device, &ii, NULL, &r->image) != VK_SUCCESS)
        return refuse(r, "no memory for the motion vectors", why);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, r->image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (req.memoryTypeBits >> i & 1) {
            if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                type = i;
                break;
            }
            if (type == UINT32_MAX) type = i;
        }
    VkMemoryAllocateInfo ma = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(device, &ma, NULL, &r->memory) != VK_SUCCESS ||
        vkBindImageMemory(device, r->image, r->memory, 0) != VK_SUCCESS)
        return refuse(r, "no memory for the motion vectors", why);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = r->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R16G16_SFLOAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(device, &vi, NULL, &r->view) != VK_SUCCESS)
        return refuse(r, "no memory for the motion vectors", why);
    return r;
}

void snx_reproject_size(const SnxReproject* r, uint32_t out[2]) { out[0] = r->size[0], out[1] = r->size[1]; }

void snx_reproject_record(SnxReproject* r, VkCommandBuffer cb, const SnxUpscaleImage* depth, const uint32_t render[2],
                          const float reproject[16], SnxUpscaleImage* motion) {
    VkDescriptorSet set = r->sets[r->next];
    r->next = (r->next + 1) % SNX_UPSCALER_IN_FLIGHT;
    VkDescriptorImageInfo in = {r->sampler, depth->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo out = {VK_NULL_HANDLE, r->view, VK_IMAGE_LAYOUT_GENERAL};
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
                                  .pImageInfo = &out}};
    vkUpdateDescriptorSets(r->device, 2, w, 0, NULL);
    /* the image is all written each time: what it held doesn't matter */
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                              .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = r->image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
    Push push;
    memcpy(push.reproject, reproject, sizeof push.reproject);
    push.render[0] = render[0], push.render[1] = render[1];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, r->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(cb, (render[0] + 7) / 8, (render[1] + 7) / 8, 1);
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL, b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
    *motion = (SnxUpscaleImage){r->image, r->view, VK_FORMAT_R16G16_SFLOAT, r->size[0], r->size[1]};
}

void snx_reproject_destroy(SnxReproject* r) {
    if (!r) return;
    if (r->device) {
        vkDestroyImageView(r->device, r->view, NULL);
        vkDestroyImage(r->device, r->image, NULL);
        vkFreeMemory(r->device, r->memory, NULL);
        vkDestroyDescriptorPool(r->device, r->pool, NULL);
        vkDestroyPipeline(r->device, r->pipeline, NULL);
        vkDestroyPipelineLayout(r->device, r->layout, NULL);
        vkDestroyDescriptorSetLayout(r->device, r->set_layout, NULL);
        vkDestroySampler(r->device, r->sampler, NULL);
    }
    free(r);
}
