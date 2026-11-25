/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* passes.h: one compute layout for every pass (the picture sampled at binding 0, the result stored at binding 1, two
   more sampled images at 2 and 3 for SMAA, the sizes, sharpness, flags and shift as push constants), a pipeline per
   pass and descriptor sets that go round SNX_UPSCALER_IN_FLIGHT evaluations. The spatial upscaler's EASU writes an
   image of ours at the output's size when RCAS follows, and the output itself when not. Before either, an evaluation
   that asks for it copies the picture into images of ours at the render size, shifted back by its jitter, and runs
   SMAA 1x there (up_smaa.comp, with the reference's area and search textures, uploaded at its first use). The passes
   record plain vkCmd* calls with Vulkan 1.0 barriers, so any device and command buffer take them */
#include "nx/gpu/vk/upscaler/passes.h"
#include <stdlib.h>
#include <string.h>
#include "AreaTex.h"
#include "SearchTex.h"
#include "nx/gpu/vk/upscaler/upscaler_spv.h"

enum {
    PASS_COPY,
    PASS_EASU,
    PASS_RCAS,
    PASS_PREP,
    PASS_EDGES,
    PASS_WEIGHTS,
    PASS_WEIGHTS_FAST,
    PASS_BLEND,
    PASS_BLEND_T2X,
    PASS_VELOCITY,
    PASS_RESOLVE,
    PASSES
};
/* the images at the render size: the picture copied (and shifted), SMAA's edges and weights, and its result. T2x's,
   made at its first use: the result of the frame before (the two results take turns), the velocity and the resolve */
enum { PREP_COPY, PREP_EDGES, PREP_WEIGHTS, PREP_RESULT, PREP_RESULT2, PREP_VELOCITY, PREP_RESOLVED, PREPS };
enum { PREPS_1X = PREP_RESULT + 1 };
/* descriptor sets an evaluation takes at most: the six before the upscale, EASU and RCAS */
enum { SETS = 8, SET_UPSCALE = 6 };
/* up_smaa.comp's flags, and the edges' input from bit 4 */
enum { FLAG_HDR = 1, FLAG_DECODE = 2, FLAG_RESET = 4, FLAG_EDGES = 4 };
/* SMAA's presets (SnxUpscaleEval.smaa_quality): threshold, search steps, diagonal search steps, corner rounding.
   Low and Medium run without the diagonal and corner searches (PASS_WEIGHTS_FAST) */
static const struct {
    float threshold;
    int32_t steps, steps_diag, corner;
} k_presets[4] = {{0.15f, 4, 0, 0}, {0.1f, 8, 0, 0}, {0.1f, 16, 8, 25}, {0.05f, 32, 16, 25}};

/* an image of ours, device-local where the device has such memory */
typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} Image;

struct SnxUpPasses {
    VkDevice device;
    int spatial;
    uint32_t output[2];
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline[PASSES];
    VkDescriptorPool pool;
    VkDescriptorSet sets[SNX_UPSCALER_IN_FLIGHT][SETS];
    uint32_t next;
    /* EASU's result before RCAS: in VK_IMAGE_LAYOUT_GENERAL once fresh is 0 */
    VkImage mid;
    VkDeviceMemory mid_memory;
    VkImageView mid_view;
    int fresh;
    /* before the upscale, made at the first evaluation that asks: the images at the render size (the largest, in
       VK_IMAGE_LAYOUT_GENERAL from then on), SMAA's area and search textures (in
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL once uploaded is 1) and the buffer they come from */
    VkPhysicalDevice physical;
    uint32_t render_max[2];
    uint32_t flags;
    Image prep[PREPS], area, search;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    int prep_made, uploaded, t2x_made;
    /* T2x: the result image this frame writes (PREP_RESULT or PREP_RESULT2), and whether the other holds the frame
       before (1 after a T2x evaluation, 0 after a reset or another mode) */
    int current, history;
    int display_motion; /* the caller's motion vectors are at the output's size */
    /* motion vectors that include the jitter, and the shift of the last T2x evaluation: in our frame, taken back by
       the jitter, a vector moves by this frame's shift less the last one's */
    int jittered_motion;
    float last_shift[2];
};

/* the push constants of up_*.comp: the first 32 bytes are every pass's, the rest up_smaa.comp's */
typedef struct {
    uint32_t render[2], size[2], output[2];
    float sharpness;
    uint32_t flags;
    float shift[2];
    float threshold;
    int32_t steps, steps_diag, corner;
    float motion_scale[2], motion_size[2];
    float motion_bias[2];
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

/* a memory type of bits with want, else any of bits: UINT32_MAX for none */
static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags want, int must) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits >> i & 1) {
            if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
            if (type == UINT32_MAX && !must) type = i;
        }
    return type;
}

/* an image of format at width x height for usage, device-local where the device has such memory */
static int make_image(SnxUpPasses* p, Image* m, VkFormat format, uint32_t width, uint32_t height,
                      VkImageUsageFlags usage) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage};
    if (vkCreateImage(p->device, &ii, NULL, &m->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(p->device, m->image, &req);
    uint32_t type = memory_type(p->physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(p->device, &ai, NULL, &m->memory) != VK_SUCCESS ||
        vkBindImageMemory(p->device, m->image, m->memory, 0) != VK_SUCCESS)
        return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = m->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(p->device, &vi, NULL, &m->view) == VK_SUCCESS;
}
static void free_image(SnxUpPasses* p, Image* m) {
    vkDestroyImageView(p->device, m->view, NULL);
    vkDestroyImage(p->device, m->image, NULL);
    vkFreeMemory(p->device, m->memory, NULL);
}

/* EASU's result image */
static int make_mid(SnxUpPasses* p) {
    Image m = {0};
    int made = make_image(p, &m, VK_FORMAT_R16G16B16A16_SFLOAT, p->output[0], p->output[1],
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    p->mid = m.image, p->mid_memory = m.memory, p->mid_view = m.view;
    return made;
}

/* The images before the upscale, SMAA's pipelines and its textures with the buffer they are uploaded from: 1, else 0
   and why */
static int make_prep(SnxUpPasses* p, const char** why) {
    static const VkFormat formats[PREPS_1X] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,
                                               VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    p->prep_made = 1;
    for (int i = 0; i < PREPS_1X; i++)
        if (!make_image(p, &p->prep[i], formats[i], p->render_max[0], p->render_max[1],
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            *why = "no memory for the anti-aliasing's images";
            return 0;
        }
    if (!make_image(p, &p->area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !make_image(p, &p->search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        *why = "no memory for SMAA's textures";
        return 0;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = sizeof areaTexBytes + sizeof searchTexBytes,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkMemoryRequirements req;
    void* mapped;
    if (vkCreateBuffer(p->device, &bi, NULL, &p->staging) != VK_SUCCESS) {
        *why = "the device refused SMAA's upload buffer";
        return 0;
    }
    vkGetBufferMemoryRequirements(p->device, p->staging, &req);
    uint32_t type = memory_type(p->physical, req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(p->device, &ai, NULL, &p->staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(p->device, p->staging, p->staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(p->device, p->staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        *why = "no memory for SMAA's upload buffer";
        return 0;
    }
    memcpy(mapped, areaTexBytes, sizeof areaTexBytes);
    memcpy((unsigned char*)mapped + sizeof areaTexBytes, searchTexBytes, sizeof searchTexBytes);
    vkUnmapMemory(p->device, p->staging_memory);
    p->pipeline[PASS_PREP] = pipeline(p, up_smaa_prep_spv, up_smaa_prep_spv_size);
    p->pipeline[PASS_EDGES] = pipeline(p, up_smaa_edges_spv, up_smaa_edges_spv_size);
    p->pipeline[PASS_WEIGHTS] = pipeline(p, up_smaa_weights_spv, up_smaa_weights_spv_size);
    p->pipeline[PASS_WEIGHTS_FAST] = pipeline(p, up_smaa_weights_fast_spv, up_smaa_weights_fast_spv_size);
    p->pipeline[PASS_BLEND] = pipeline(p, up_smaa_blend_spv, up_smaa_blend_spv_size);
    p->pipeline[PASS_BLEND_T2X] = pipeline(p, up_smaa_blend_t2x_spv, up_smaa_blend_t2x_spv_size);
    p->pipeline[PASS_VELOCITY] = pipeline(p, up_smaa_velocity_spv, up_smaa_velocity_spv_size);
    p->pipeline[PASS_RESOLVE] = pipeline(p, up_smaa_resolve_spv, up_smaa_resolve_spv_size);
    int made = 1;
    for (int i = PASS_PREP; i < PASSES; i++) made &= p->pipeline[i] != VK_NULL_HANDLE;
    if (!made) {
        *why = "the device refused the anti-aliasing's pipelines";
        return 0;
    }
    return 1;
}

/* T2x's images, at its first evaluation, into VK_IMAGE_LAYOUT_GENERAL: 1, else 0 and why */
static int make_t2x(SnxUpPasses* p, VkCommandBuffer cb, const char** why) {
    p->t2x_made = 1;
    for (int i = PREPS_1X; i < PREPS; i++) {
        if (!make_image(p, &p->prep[i], VK_FORMAT_R16G16B16A16_SFLOAT, p->render_max[0], p->render_max[1],
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
            *why = "no memory for SMAA T2x's images";
            return 0;
        }
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = p->prep[i].image,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b);
    }
    return 1;
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
    p->physical = info->physical;
    p->render_max[0] = info->render_max[0], p->render_max[1] = info->render_max[1];
    p->flags = info->flags & SNX_UP_FLAG_HDR ? FLAG_HDR : 0;
    p->display_motion = (info->flags & SNX_UP_FLAG_DISPLAY_MOTION) != 0;
    p->jittered_motion = (info->flags & SNX_UP_FLAG_JITTERED_MOTION) != 0;
    p->current = PREP_RESULT;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (vkCreateSampler(p->device, &si, NULL, &p->sampler) != VK_SUCCESS)
        return refuse(p, "the device refused the upscaler's sampler", why);
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
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
        if (!make_mid(p)) return refuse(p, "no memory for the spatial upscaler's image", why);
    } else if (!(p->pipeline[PASS_COPY] = pipeline(p, up_copy_spv, up_copy_spv_size)))
        return refuse(p, "the device refused the copy's pipeline", why);
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * SETS * SNX_UPSCALER_IN_FLIGHT},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS * SNX_UPSCALER_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = SETS * SNX_UPSCALER_IN_FLIGHT,
                                     .poolSizeCount = 2,
                                     .pPoolSizes = sizes};
    if (vkCreateDescriptorPool(p->device, &dp, NULL, &p->pool) != VK_SUCCESS)
        return refuse(p, "the device refused the upscaler's descriptor pool", why);
    VkDescriptorSetLayout layouts[SETS * SNX_UPSCALER_IN_FLIGHT];
    for (uint32_t i = 0; i < SETS * SNX_UPSCALER_IN_FLIGHT; i++) layouts[i] = p->set_layout;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = p->pool,
                                      .descriptorSetCount = SETS * SNX_UPSCALER_IN_FLIGHT,
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
    for (int i = 0; i < PREPS; i++) free_image(p, &p->prep[i]);
    free_image(p, &p->area);
    free_image(p, &p->search);
    vkDestroyBuffer(p->device, p->staging, NULL);
    vkFreeMemory(p->device, p->staging_memory, NULL);
    free(p);
}

/* one pass: the picture sampled from view in layout (and SMAA's second and third images from ours, VK_NULL_HANDLE for
   none), the result stored to out (VK_IMAGE_LAYOUT_GENERAL) */
static void pass3(SnxUpPasses* p, VkCommandBuffer cb, int which, VkDescriptorSet set, VkImageView view,
                  VkImageLayout layout, VkImageView out, VkImageView second, VkImageLayout second_layout,
                  VkImageView third, VkImageLayout third_layout, const Push* push) {
    VkDescriptorImageInfo in[3] = {
        {p->sampler, view, layout}, {p->sampler, second, second_layout}, {p->sampler, third, third_layout}};
    VkDescriptorImageInfo store = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[4] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &in[0]},
                                 {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 1,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  .pImageInfo = &store},
                                 {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 2,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &in[1]},
                                 {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 3,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &in[2]}};
    vkUpdateDescriptorSets(p->device, second ? (third ? 4 : 3) : 2, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline[which]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *push, push);
    vkCmdDispatch(cb, (push->output[0] + 7) / 8, (push->output[1] + 7) / 8, 1);
}
static void pass(SnxUpPasses* p, VkCommandBuffer cb, int which, VkDescriptorSet set, VkImageView view,
                 VkImageLayout layout, VkImageView out, const Push* push) {
    pass3(p, cb, which, set, view, layout, out, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_NULL_HANDLE,
          VK_IMAGE_LAYOUT_UNDEFINED, push);
}

/* every compute write and read before against every one after, our images in VK_IMAGE_LAYOUT_GENERAL */
static void compute_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                         NULL, 0, NULL);
}
static void image_barrier(VkCommandBuffer cb, VkImage image, VkPipelineStageFlags from_stage, VkAccessFlags from,
                          VkImageLayout old, VkPipelineStageFlags to_stage, VkAccessFlags to, VkImageLayout now) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = from,
                              .dstAccessMask = to,
                              .oldLayout = old,
                              .newLayout = now,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, from_stage, to_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* SMAA's textures from the buffer, and the images at the render size into VK_IMAGE_LAYOUT_GENERAL, once */
static void upload(SnxUpPasses* p, VkCommandBuffer cb) {
    const VkImage images[2] = {p->area.image, p->search.image};
    const uint32_t sizes[2][2] = {{AREATEX_WIDTH, AREATEX_HEIGHT}, {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT}};
    for (int i = 0; i < 2; i++) {
        VkBufferImageCopy c = {.bufferOffset = i ? sizeof areaTexBytes : 0,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {sizes[i][0], sizes[i][1], 1}};
        image_barrier(cb, images[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cb, p->staging, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        image_barrier(cb, images[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (int i = 0; i < PREPS_1X; i++)
        image_barrier(cb, p->prep[i].image, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    p->uploaded = 1;
}

/* The picture into our images at the render size, shifted back by e->shift, then SMAA there when e->smaa: the view
   the upscale then takes the picture from (VK_IMAGE_LAYOUT_GENERAL), VK_NULL_HANDLE and why when it can't */
static VkImageView before_upscale(SnxUpPasses* p, const SnxUpscaleEval* e, VkDescriptorSet* sets, const char** why) {
    if (e->render[0] > p->render_max[0] || e->render[1] > p->render_max[1]) {
        *why = "a render size past the context's largest";
        return VK_NULL_HANDLE;
    }
    if (!p->prep_made && !make_prep(p, why)) return VK_NULL_HANDLE;
    if (!p->pipeline[PASS_BLEND]) {
        *why = "the anti-aliasing couldn't be set up";
        return VK_NULL_HANDLE;
    }
    if (!p->uploaded) upload(p, e->cb);
    /* every pass over the whole of our images, so SMAA's searches past the render size read pixels of this frame */
    int quality = e->smaa_quality >= 0 && e->smaa_quality < 4 ? e->smaa_quality : 2;
    int edges = e->smaa_edges == 2 && !e->depth.image ? 0 : e->smaa_edges < 0 || e->smaa_edges > 2 ? 0 : e->smaa_edges;
    int t2x = e->smaa == 2 && e->motion.image;
    Push push = {{e->render[0], e->render[1]},
                 {p->render_max[0], p->render_max[1]},
                 {p->render_max[0], p->render_max[1]},
                 0,
                 p->flags | (uint32_t)edges << FLAG_EDGES,
                 {e->shift[0], e->shift[1]},
                 k_presets[quality].threshold,
                 k_presets[quality].steps,
                 k_presets[quality].steps_diag,
                 k_presets[quality].corner,
                 {e->motion_scale[0], e->motion_scale[1]},
                 {(float)(p->display_motion ? p->output[0] : e->render[0]),
                  (float)(p->display_motion ? p->output[1] : e->render[1])},
                 {0, 0}};
    if (t2x && p->jittered_motion && p->history)
        push.motion_bias[0] = e->shift[0] - p->last_shift[0], push.motion_bias[1] = e->shift[1] - p->last_shift[1];
    if (t2x && !p->t2x_made && !make_t2x(p, e->cb, why)) return VK_NULL_HANDLE;
    if (t2x && !p->prep[PREP_RESOLVED].view) t2x = 0; /* its images couldn't be made before: 1x */
    if (!t2x) p->history = 0;
    compute_barrier(e->cb);
    pass(p, e->cb, PASS_PREP, sets[0], e->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, p->prep[PREP_COPY].view,
         &push);
    if (!e->smaa) {
        compute_barrier(e->cb);
        return p->prep[PREP_COPY].view;
    }
    compute_barrier(e->cb);
    pass3(p, e->cb, PASS_EDGES, sets[1], p->prep[PREP_COPY].view, VK_IMAGE_LAYOUT_GENERAL, p->prep[PREP_EDGES].view,
          edges == 2 ? e->depth.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_NULL_HANDLE,
          VK_IMAGE_LAYOUT_UNDEFINED, &push);
    compute_barrier(e->cb);
    pass3(p, e->cb, quality >= 2 ? PASS_WEIGHTS : PASS_WEIGHTS_FAST, sets[2], p->prep[PREP_EDGES].view,
          VK_IMAGE_LAYOUT_GENERAL, p->prep[PREP_WEIGHTS].view, p->area.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          p->search.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &push);
    compute_barrier(e->cb);
    if (!t2x) {
        push.flags |= FLAG_DECODE;
        pass3(p, e->cb, PASS_BLEND, sets[3], p->prep[PREP_COPY].view, VK_IMAGE_LAYOUT_GENERAL,
              p->prep[PREP_RESULT].view, p->prep[PREP_WEIGHTS].view, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE,
              VK_IMAGE_LAYOUT_UNDEFINED, &push);
        compute_barrier(e->cb);
        return p->prep[PREP_RESULT].view;
    }
    /* T2x: the velocity, this frame's blend with it packed (still encoded), then the resolve with the frame before,
       which decodes */
    int now = p->current, before = now == PREP_RESULT ? PREP_RESULT2 : PREP_RESULT;
    pass(p, e->cb, PASS_VELOCITY, sets[3], e->motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         p->prep[PREP_VELOCITY].view, &push);
    compute_barrier(e->cb);
    pass3(p, e->cb, PASS_BLEND_T2X, sets[4], p->prep[PREP_COPY].view, VK_IMAGE_LAYOUT_GENERAL, p->prep[now].view,
          p->prep[PREP_WEIGHTS].view, VK_IMAGE_LAYOUT_GENERAL, p->prep[PREP_VELOCITY].view, VK_IMAGE_LAYOUT_GENERAL,
          &push);
    compute_barrier(e->cb);
    push.flags |= FLAG_DECODE | (e->reset || !p->history ? FLAG_RESET : 0);
    pass3(p, e->cb, PASS_RESOLVE, sets[5], p->prep[now].view, VK_IMAGE_LAYOUT_GENERAL, p->prep[PREP_RESOLVED].view,
          p->prep[before].view, VK_IMAGE_LAYOUT_GENERAL, p->prep[PREP_VELOCITY].view, VK_IMAGE_LAYOUT_GENERAL, &push);
    compute_barrier(e->cb);
    p->current = before;
    p->history = 1;
    p->last_shift[0] = e->shift[0], p->last_shift[1] = e->shift[1];
    return p->prep[PREP_RESOLVED].view;
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
                 0,
                 {0, 0},
                 0,
                 0,
                 0,
                 0,
                 {0, 0},
                 {0, 0},
                 {0, 0}};
    VkDescriptorSet* sets = p->sets[p->next++ % SNX_UPSCALER_IN_FLIGHT];
    /* the picture the upscale takes: the caller's, or ours at the render size after the shift and SMAA */
    VkImageView picture = e->color.view;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (e->smaa || e->shift[0] != 0 || e->shift[1] != 0) {
        if (!(picture = before_upscale(p, e, sets, why))) return 0;
        layout = VK_IMAGE_LAYOUT_GENERAL;
        push.size[0] = p->render_max[0], push.size[1] = p->render_max[1];
    }
    if (!p->spatial) {
        pass(p, e->cb, PASS_COPY, sets[SET_UPSCALE], picture, layout, e->output.view, &push);
        return 1;
    }
    if (!(push.sharpness > 0)) {
        pass(p, e->cb, PASS_EASU, sets[SET_UPSCALE], picture, layout, e->output.view, &push);
        return 1;
    }
    /* EASU into ours (after the last evaluation's RCAS read it), then RCAS from it into the output */
    mid_barrier(p, e->cb, p->fresh, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    p->fresh = 0;
    pass(p, e->cb, PASS_EASU, sets[SET_UPSCALE], picture, layout, p->mid_view, &push);
    mid_barrier(p, e->cb, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    pass(p, e->cb, PASS_RCAS, sets[SET_UPSCALE + 1], p->mid_view, VK_IMAGE_LAYOUT_GENERAL, e->output.view, &push);
    return 1;
}
