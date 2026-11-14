/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/descriptor_objects.h"
#include "nx/gpu/vk/capture.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "nx/gpu/shader/coverage_spirv.h"
#include "nx/gpu/shader/capture_tiles_spirv.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/memory/mem.h"
#include "nx/hle/services/nvdrv.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    u64 blend;
    u32 flags, coverage;
    VkPipeline pipeline;
} CapturePipeline;
typedef struct {
    u32 samples, components;
    VkFormat format;
    VkPipelineColorBlendAttachmentState blend;
    u32 logic;
} MixedKey;
typedef struct { MixedKey key; VkPipeline pipeline; } MixedPipeline;
typedef struct {
    VkCapture active;
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkPipeline finalize;
    CapturePipeline* pipelines;
    u32 count, capacity;
    VkDescriptorSetLayout mixed_descriptors;
    VkPipelineLayout mixed_layout;
    VkShaderModule mixed_vertex;
    MixedPipeline* mixed;
    u32 mixed_count, mixed_capacity;
    VkImageFormatProperties validity_r8;
    int validity_r8_checked, validity_r8_supported;
} CaptureState;
static CaptureState g_capture;

int vk_capture_tiles_supported(void) {
    return g_vk.feat.fragmentStoresAndAtomics && g_vk.props.limits.maxPerStageDescriptorStorageBuffers >= 18 &&
           g_vk.props.limits.maxDescriptorSetStorageBuffers >= 86;
}
static void capture_tiles_drop(VkCapture* capture, int defer) {
    if (capture->tile_buffer) {
        if (defer)
            vk_defer_buffer(capture->tile_buffer, capture->tile_memory);
        else {
            vk_mem_untrack(capture->tile_memory);
            vk_devmem_buffer_destroy(capture->tile_buffer, capture->tile_memory);
        }
    }
    capture->tile_buffer = VK_NULL_HANDLE;
    capture->tile_memory = 0;
    capture->tile_bytes = capture->tile_count = 0;
    capture->tile_dirty = 0;
}
static int capture_tiles_make(VkCapture* capture, const Surf* target) {
    if (!vk_capture_tiles_supported()) return 0;
    u64 tx = ((u64)target->sw + 7) / 8, ty = ((u64)target->sh + 7) / 8;
    u64 count = tx * ty;
    if (target->layers && count > UINT32_MAX / target->layers) return 0;
    count *= target->layers;
    u64 bytes = SH_CAPTURE_TILE_HEADER * 4ull + count * 8;
    if (!count || count > g_vk.props.limits.maxComputeWorkGroupCount[0] ||
        bytes > g_vk.props.limits.maxStorageBufferRange || bytes > g_vk.alloc_cap)
        return 0;
    if (capture->tile_buffer) return 1; /* the image shape and its tile buffer are retired together */
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = bytes,
                                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    DevMemWant memory = {.at = DM_DEVICE};
    if (!VK_OK(vk_buffer_create(&info, &memory, &capture->tile_buffer, &capture->tile_memory, NULL))) return 0;
    vk_mem_track(capture->tile_memory, VK_OWN_MISC);
    capture->tile_count = (u32)count;
    capture->tile_bytes = bytes;
    capture->tile_dirty = 1;
    const u32 header[SH_CAPTURE_TILE_HEADER] = {0, 1, 1, (u32)tx, (u32)ty, target->sw, target->sh, target->layers};
    VkBuffer staging;
    u64 offset;
    u8* data = vk_stage(sizeof header, 4, &staging, &offset);
    if (!data) {
        capture_tiles_drop(capture, 1);
        return 0;
    }
    memcpy(data, header, sizeof header);
    VkBufferCopy copy = {offset, 0, sizeof header};
    rec_CmdCopyBuffer(vk_cmd(), staging, capture->tile_buffer, 1, &copy);
    return 1;
}
static void capture_tiles_barrier(VkCapture* capture, VkPipelineStageFlags2 source, VkAccessFlags2 source_access,
                                    VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access) {
    VkBufferMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                       .srcStageMask = source,
                                       .srcAccessMask = source_access,
                                       .dstStageMask = destination,
                                       .dstAccessMask = destination_access,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .buffer = capture->tile_buffer,
                                       .offset = 0,
                                       .size = capture->tile_bytes};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                    .bufferMemoryBarrierCount = 1,
                                    .pBufferMemoryBarriers = &barrier};
    rec_CmdPipelineBarrier2(vk_cmd(), &dependency);
}

static void capture_image_drop(Surf* image, int defer) {
    for (int i = 0; i < image->nsv; i++) {
        if (defer)
            vk_defer_image(VK_NULL_HANDLE, 0, image->sv[i].v, VK_NULL_HANDLE);
        else
            vk_image_view_destroy(g_vk.dev, image->sv[i].v, NULL);
    }
    if (image->img) {
        if (defer)
            vk_defer_image(image->img, image->mem, image->att, VK_NULL_HANDLE);
        else {
            if (image->att) vk_image_view_destroy(g_vk.dev, image->att, NULL);
            vk_image_destroy(image->img, image->mem);
        }
    }
    *image = (Surf){0};
}
static int capture_image_make(Surf* image, const Surf* target, VkFormat format, u32 bytes, VkImageUsageFlags usage) {
    VkImageFormatProperties properties;
    if (vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                 usage, 0, &properties) != VK_SUCCESS ||
        !(properties.sampleCounts & vk_surf_samples(target)) || target->sw > properties.maxExtent.width ||
        target->sh > properties.maxExtent.height || target->layers > properties.maxArrayLayers)
        return 0;
    image->w = target->w;
    image->h = target->h;
    image->sw = target->sw;
    image->sh = target->sh;
    image->layers = target->layers;
    image->levels = image->depth3d = 1;
    image->msaa = target->msaa;
    image->storage_capable = (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    image->type = image->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    image->f = (VkFmt){.vk = format, .bpp = (u8)bytes, .bw = 1, .bh = 1};
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {image->sw, image->sh, 1},
                              .mipLevels = 1,
                              .arrayLayers = image->layers,
                              .samples = vk_surf_samples(target),
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = usage};
    if (!vk_image_create(&info, &image->img, &image->mem)) return 0;
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = image->img,
                                  .viewType = image->type,
                                  .format = format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image->layers}};
    if (!VK_OK(vk_image_view_create(g_vk.dev, &view, NULL, &image->att))) {
        capture_image_drop(image, 0);
        return 0;
    }
    return 1;
}
static void capture_extra_drop(VkCapture* capture, u32 first, int defer) {
    for (u32 i = first; i < 6; i++) {
        capture_image_drop(&capture->extra[i].source, defer);
        capture->extra[i] = (VkCaptureExtra){0};
    }
}
Surf* vk_capture_mixed_source(VkCapture* capture, u32 index) {
    if (!capture) return NULL;
    return !index ? &capture->source
                  : index < 7 && index < capture->mixed_count ? &capture->extra[index - 1].source : NULL;
}
Surf* vk_capture_mixed_target(VkCapture* capture, u32 index) {
    if (!capture) return NULL;
    return !index ? capture->target
                  : index < 7 && index < capture->mixed_count ? capture->extra[index - 1].target : NULL;
}

static VkFormat mixed_validity_format(const Surf* shape, VkImageUsageFlags usage) {
    if (!g_capture.validity_r8_checked) {
        g_capture.validity_r8_supported = vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, VK_FORMAT_R8_UNORM,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &g_capture.validity_r8) == VK_SUCCESS;
        g_capture.validity_r8_checked = 1;
    }
    return vk_capture_validity_format(g_capture.validity_r8_supported ? &g_capture.validity_r8 : NULL,
        (u32)vk_surf_samples(shape), shape->sw, shape->sh, shape->layers);
}
static VkCapture* capture_get(Surf* target, u32 msaa, int mixed) {
    if (!target || !target->img || target->f.depth || (!mixed && !target->storage_capable) || target->depth3d > 1 ||
        !target->layers || !target->sw || !target->sh ||
        (!mixed && (!g_vk.feat.shaderStorageImageReadWithoutFormat || !g_vk.feat.shaderStorageImageWriteWithoutFormat)) ||
        (!mixed && msaa && !g_vk.feat.shaderStorageImageMultisample))
        return NULL;
    Surf shape = *target;
    shape.msaa = (u8)msaa;
    VkCapture* capture = &g_capture.active;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             (mixed ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT);
    VkFormat validity = mixed ? mixed_validity_format(&shape, usage) : VK_FORMAT_R8G8B8A8_UNORM;
    Surf* source = &capture->source;
    if (!source->img || source->w != target->w || source->h != target->h || source->sw != target->sw ||
        source->sh != target->sh || source->layers != target->layers || source->msaa != msaa || capture->usage != usage ||
        capture->valid.f.vk != validity) {
        vk_render_end();
        capture->mixed_set = VK_NULL_HANDLE;
        capture->mixed_frame = 0;
        capture->usage = 0;
        capture_image_drop(&capture->source, 1);
        capture_image_drop(&capture->valid, 1);
        capture_extra_drop(capture, 0, 1);
        capture_tiles_drop(capture, 1);
        if (!capture_image_make(&capture->source, &shape, VK_FORMAT_R32G32B32A32_SFLOAT, 16, usage) ||
            !capture_image_make(&capture->valid, &shape, validity, validity == VK_FORMAT_R8_UNORM ? 1 : 4, usage)) {
            capture_image_drop(&capture->source, 1);
            capture_image_drop(&capture->valid, 1);
            capture->target = NULL;
            return NULL;
        }
        capture->usage = usage;
    }
    capture->target = target;
    capture->mixed_count = mixed ? 1 : 0;
    capture->flags = target->layers > 1 ? SH_BLEND_ARRAY : 0;
    if (msaa) capture->flags |= SH_BLEND_MULTISAMPLE | (u32)vk_surf_samples(&shape) << 8;
    if (!mixed && capture_tiles_make(capture, target)) capture->flags |= SH_CAPTURE_TILED;
    return capture;
}
VkCapture* vk_capture_get(Surf* target) { return capture_get(target, target ? target->msaa : 0, 0); }
VkCapture* vk_capture_get_mixed(Surf* target, u32 raster_samples) {
    if (!target || target->msaa || target->layers != 1 || target->f.is_int || !g_vk_preserve_infnan ||
        raster_samples < 2 || raster_samples > 16 || (raster_samples & (raster_samples - 1))) return NULL;
    u32 msaa = raster_samples == 2 ? DS_MSAA_2X1 : raster_samples == 4 ? DS_MSAA_2X2
                                                 : raster_samples == 8 ? DS_MSAA_4X2 : DS_MSAA_4X4;
    VkCapture* capture = capture_get(target, msaa, 1);
    if (capture) capture_extra_drop(capture, 0, 1);
    return capture;
}
typedef struct {
    u64 cpu[7], size[7];
    u32 count, self;
    int aliases;
} MixedRanges;
static void mixed_alias_peer(void* user, u64 at, u64 size) {
    MixedRanges* ranges = user;
    if (at > UINT64_MAX - size) { ranges->aliases = 1; return; }
    for (u32 i = 0; i < ranges->count; i++)
        if (i != ranges->self && snx_ranges_overlap(at, size, ranges->cpu[i], ranges->size[i])) ranges->aliases = 1;
}
VkCapture* vk_capture_get_mixed_targets(Surf* const* targets, u32 count, u32 raster_samples) {
    if (!targets || !count || count > vk_coverage_capture_limit(g_vk.props.limits.maxColorAttachments)) return NULL;
    if (count == 1) return vk_capture_get_mixed(targets[0], raster_samples);
    if (!g_vk_preserve_infnan || raster_samples < 2 || raster_samples > 16 ||
        (raster_samples & (raster_samples - 1))) return NULL;
    const Surf* first = targets[0];
    if (!first) return NULL;
    MixedRanges ranges = {.count = count};
    for (u32 i = 0; i < count; i++) {
        const Surf* target = targets[i];
        if (!target || !target->img || target->f.depth || target->f.is_int || target->msaa || target->volume ||
            target->depth3d > 1 || target->layers != 1 || target->w != first->w || target->h != first->h ||
            target->sw != first->sw || target->sh != first->sh || !target->gva) return NULL;
        ranges.size[i] = vk_surf_guest_size(target);
        /* A bounded proof for the first MRT path: fragmented or sparse backing remains unsupported. */
        if (!ranges.size[i] || target->gva > UINT64_MAX - ranges.size[i] ||
            gmmu_run(target->gva, ranges.size[i], &ranges.cpu[i]) != ranges.size[i] ||
            ranges.cpu[i] > UINT64_MAX - ranges.size[i]) return NULL;
        for (u32 j = 0; j < i; j++)
            if (target->img == targets[j]->img ||
                snx_ranges_overlap(target->gva, ranges.size[i], targets[j]->gva, ranges.size[j]) ||
                snx_ranges_overlap(ranges.cpu[i], ranges.size[i], ranges.cpu[j], ranges.size[j])) return NULL;
    }
    if (mem_alias_any())
        for (ranges.self = 0; ranges.self < count; ranges.self++) {
            mem_alias_peers(ranges.cpu[ranges.self], ranges.size[ranges.self], mixed_alias_peer, &ranges);
            if (ranges.aliases) return NULL;
        }
    u32 msaa = raster_samples == 2 ? DS_MSAA_2X1 : raster_samples == 4 ? DS_MSAA_2X2
                                                 : raster_samples == 8 ? DS_MSAA_4X2 : DS_MSAA_4X4;
    VkCapture* capture = capture_get(targets[0], msaa, 1);
    if (!capture) return NULL;
    capture_extra_drop(capture, count - 1, 1);
    for (u32 i = 1; i < count; i++) {
        VkCaptureExtra* extra = &capture->extra[i - 1];
        if (!extra->source.img) {
            Surf shape = *targets[i];
            shape.msaa = (u8)msaa;
            if (!capture_image_make(&extra->source, &shape, VK_FORMAT_R32G32B32A32_SFLOAT, 16, capture->usage))
                return NULL;
        }
        extra->target = targets[i];
    }
    capture->mixed_count = count;
    return capture;
}
int vk_capture_clear(VkCapture* capture) {
    if (!capture || !capture->valid.img) return 0;
    vk_render_end();
    VkCommandBuffer command = vk_cmd();
    vk_surf_use(command, &capture->valid, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue zero = {.float32 = {0, 0, 0, 0}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, capture->valid.layers};
    rec_CmdClearColorImage(command, capture->valid.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    if (capture->flags & SH_CAPTURE_TILED) {
        capture_tiles_barrier(capture,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        rec_CmdFillBuffer(command, capture->tile_buffer, 0, 4, 0);
        if (capture->tile_dirty)
            rec_CmdFillBuffer(command, capture->tile_buffer, SH_CAPTURE_TILE_HEADER * 4ull,
                               (u64)capture->tile_count * 4, 0);
        capture_tiles_barrier(capture, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        capture->tile_dirty = 1;
    }
    return 1;
}

static int capture_layout(void) {
    if (g_capture.layout) return 1;
    if (!g_capture.descriptors) {
        VkDescriptorSetLayoutBinding bindings[4];
        for (u32 i = 0; i < 4; i++)
            bindings[i] = (VkDescriptorSetLayoutBinding){i, i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                                         VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                        .bindingCount = 4,
                                                        .pBindings = bindings};
        if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &descriptors, NULL, &g_capture.descriptors))) return 0;
    }
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &g_capture.descriptors};
    return VK_OK(vkCreatePipelineLayout(g_vk.dev, &layout, NULL, &g_capture.layout));
}
static VkPipeline capture_pipeline(u64 blend, u32 flags, u32 coverage) {
    for (u32 i = 0; i < g_capture.count; i++) {
        const CapturePipeline* pipeline = &g_capture.pipelines[i];
        if (pipeline->blend == blend && pipeline->flags == flags && pipeline->coverage == coverage)
            return pipeline->pipeline;
    }
    if (!capture_layout()) return VK_NULL_HANDLE;
    if (g_capture.count == g_capture.capacity) {
        u32 capacity = g_capture.capacity ? g_capture.capacity * 2 : 16;
        size_t bytes = (size_t)capacity * sizeof *g_capture.pipelines;
        if (capacity < g_capture.capacity || bytes / sizeof *g_capture.pipelines != capacity) return VK_NULL_HANDLE;
        CapturePipeline* pipelines = realloc(g_capture.pipelines, bytes);
        if (!pipelines) return VK_NULL_HANDLE;
        g_capture.pipelines = pipelines;
        g_capture.capacity = capacity;
    }
    size_t words = 0;
    u32* code = coverage ? shader_coverage_resolve_spirv(flags, coverage == 2, &words)
                         : shader_blend_resolve_spirv(blend, flags, &words);
    if (!code) return VK_NULL_HANDLE;
    VkShaderModule module = vk_module_spv(code, words);
    free(code);
    if (!module) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                  .module = module,
                                                  .pName = "main"},
                                        .layout = g_capture.layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    int entered = snx_vk_compile_enter();
    VkResult result = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
    snx_vk_compile_leave(entered);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (!VK_OK(result)) return VK_NULL_HANDLE;
    g_capture.pipelines[g_capture.count++] = (CapturePipeline){blend, flags, coverage, pipeline};
    return pipeline;
}

int vk_capture_finalize_tiles(VkCapture* capture) {
    if (!capture || !capture->tile_buffer || !capture_layout()) return 0;
    if (!g_capture.finalize) {
        size_t words;
        u32* code = shader_capture_tiles_finalize_spirv(3, &words);
        if (!code) return 0;
        VkShaderModule module = vk_module_spv(code, words);
        free(code);
        if (!module) return 0;
        VkComputePipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                       .module = module, .pName = "main"}, .layout = g_capture.layout};
        int entered = snx_vk_compile_enter();
        VkResult result = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &info, NULL, &g_capture.finalize);
        snx_vk_compile_leave(entered);
        vkDestroyShaderModule(g_vk.dev, module, NULL);
        if (!VK_OK(result)) return 0;
    }
    VkDescriptorSet set = vk_desc_alloc(g_capture.descriptors);
    if (!set) return 0;
    VkDescriptorBufferInfo buffer = {capture->tile_buffer, 0, capture->tile_bytes};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer};
    vkUpdateDescriptorSets(g_vk.dev, 1, &write, 0, NULL);
    capture_tiles_barrier(capture, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    rec_CmdBindPipeline(vk_cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, g_capture.finalize);
    rec_CmdBindDescriptorSets(vk_cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, g_capture.layout, 0, 1, &set, 0, NULL);
    rec_CmdDispatch(vk_cmd(), 1, 1, 1);
    capture_tiles_barrier(capture, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    return 1;
}

int vk_capture_resolve(VkCapture* capture, u64 blend, u32 flags, int coverage) {
    if (!capture || !capture->target || !capture->source.img || !capture->valid.img ||
        !(capture->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return 0;
    Surf* target = capture->target;
    u32 samples = flags & SH_BLEND_MULTISAMPLE ? flags >> 8 & 63 : 1;
    if (samples != (u32)vk_surf_samples(target) || (target->layers > 1 && !(flags & SH_BLEND_ARRAY)) ||
        (coverage ? !target->f.is_int : target->f.is_int))
        return 0;
    flags &= SH_BLEND_MASK | SH_BLEND_SRGB | SH_BLEND_ARRAY | SH_BLEND_MULTISAMPLE | (63u << 8) | SH_CAPTURE_TILED | SH_CAPTURE_ADAPTIVE;
    if ((flags & SH_CAPTURE_TILED) && !(capture->flags & SH_CAPTURE_TILED)) return 0;
    if ((flags & SH_CAPTURE_ADAPTIVE) && !(flags & SH_CAPTURE_TILED)) return 0;
    if (coverage) flags &= ~(SH_BLEND_MASK | SH_BLEND_SRGB);
    VkPipeline pipeline = capture_pipeline(coverage ? 0 : blend, flags, coverage ? 1u + (target->f.sint != 0) : 0);
    if (!pipeline) return 0;
    VkImageViewType type = flags & SH_BLEND_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    Surf* images[3] = {&capture->source, &capture->valid, target};
    VkDescriptorImageInfo image_info[3];
    for (u32 i = 0; i < 3; i++) {
        VkImageView view = vk_tex_storage_view(images[i], type);
        if (!view) return 0;
        image_info[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkDescriptorSet set = vk_desc_alloc(g_capture.descriptors);
    if (!set) return 0;
    VkWriteDescriptorSet writes[4];
    for (u32 i = 0; i < 3; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           .pImageInfo = &image_info[i]};
    VkDescriptorBufferInfo tiles = {capture->tile_buffer, 0, capture->tile_bytes};
    u32 write_count = 3;
    if (flags & SH_CAPTURE_TILED) {
        writes[write_count++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                       .dstSet = set,
                                                       .dstBinding = 3,
                                                       .descriptorCount = 1,
                                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                       .pBufferInfo = &tiles};
    }
    vkUpdateDescriptorSets(g_vk.dev, write_count, writes, 0, NULL);
    vk_render_end();
    VkCommandBuffer command = vk_cmd();
    for (u32 i = 0; i < 3; i++)
        vk_surf_use(command, images[i], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | (i == 2 ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0));
    if ((flags & SH_CAPTURE_ADAPTIVE) && !vk_capture_finalize_tiles(capture)) return 0;
    rec_CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    rec_CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g_capture.layout, 0, 1, &set, 0, NULL);
    if (flags & SH_CAPTURE_TILED) {
        capture_tiles_barrier(capture, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        rec_CmdDispatchIndirect(command, capture->tile_buffer,
                                flags & SH_CAPTURE_ADAPTIVE ? SH_CAPTURE_ADAPTIVE_DISPATCH : 0);
        capture->tile_dirty = 0; /* every enqueued workgroup resets its unique flag */
    } else
        rec_CmdDispatch(command, (target->sw + 7) / 8, (target->sh + 7) / 8, target->layers);
    vk_surf_wrote(target);
    vk_rt_drawn(target);
    return 1;
}
static int mixed_layout(void) {
    if (g_capture.mixed_layout) return 1;
    if (!g_capture.mixed_descriptors) {
        VkDescriptorSetLayoutBinding bindings[2];
        for (u32 i = 0; i < 2; i++)
            bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                         VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = bindings};
        if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &info, NULL, &g_capture.mixed_descriptors))) return 0;
    }
    const VkPushConstantRange factors = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 * sizeof(float)};
    VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &g_capture.mixed_descriptors,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &factors};
    return VK_OK(vkCreatePipelineLayout(g_vk.dev, &info, NULL, &g_capture.mixed_layout));
}
static VkPipeline mixed_pipeline(const MixedKey* key) {
    for (u32 i = 0; i < g_capture.mixed_count; i++)
        if (!memcmp(&g_capture.mixed[i].key, key, sizeof *key)) return g_capture.mixed[i].pipeline;
    if (!mixed_layout()) return VK_NULL_HANDLE;
    if (g_capture.mixed_count == g_capture.mixed_capacity) {
        u32 capacity = g_capture.mixed_capacity ? g_capture.mixed_capacity * 2 : 16;
        size_t bytes = (size_t)capacity * sizeof *g_capture.mixed;
        if (capacity < g_capture.mixed_capacity || bytes / sizeof *g_capture.mixed != capacity) return VK_NULL_HANDLE;
        MixedPipeline* pipelines = realloc(g_capture.mixed, bytes);
        if (!pipelines) return VK_NULL_HANDLE;
        g_capture.mixed = pipelines;
        g_capture.mixed_capacity = capacity;
    }
    size_t words;
    if (!g_capture.mixed_vertex) {
        u32* code = shader_coverage_vertex_spirv(&words);
        if (!code) return VK_NULL_HANDLE;
        g_capture.mixed_vertex = vk_module_spv(code, words);
        free(code);
        if (!g_capture.mixed_vertex) return VK_NULL_HANDLE;
    }
    u32* code = shader_coverage_fragment_spirv(key->samples, key->components, &words);
    if (!code) return VK_NULL_HANDLE;
    VkShaderModule fragment = vk_module_spv(code, words);
    free(code);
    if (!fragment) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = g_capture.mixed_vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = key->logic != 0, .logicOp = key->logic ? (VkLogicOp)(key->logic - 1) : VK_LOGIC_OP_COPY,
        .attachmentCount = 1, .pAttachments = &key->blend};
    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                                    VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3, .pDynamicStates = states};
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &key->format};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport, .pRasterizationState = &raster,
        .pMultisampleState = &multisample, .pDepthStencilState = &depth, .pColorBlendState = &blend,
        .pDynamicState = &dynamic, .layout = g_capture.mixed_layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    int entered = snx_vk_compile_enter();
    VkResult result = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
    snx_vk_compile_leave(entered);
    vkDestroyShaderModule(g_vk.dev, fragment, NULL);
    if (!VK_OK(result)) return VK_NULL_HANDLE;
    g_capture.mixed[g_capture.mixed_count++] = (MixedPipeline){*key, pipeline};
    return pipeline;
}
typedef struct {
    Surf *target, *source;
    VkPipeline pipeline;
    VkImageView attachment;
    VkDescriptorSet set;
} MixedResolve;
static int mixed_prepare(VkCapture* capture, const VkCoverage* coverage, const DsState* state, u32 outputs, u32 index,
                          MixedResolve* prepared) {
    Surf* target = vk_capture_mixed_target(capture, index);
    Surf* source = vk_capture_mixed_source(capture, index);
    if (!target || !source || !source->img) return 0;
    if (target->msaa || target->f.depth || target->f.is_int || target->layers != 1 || target->depth3d > 1 ||
        vk_coverage_ratio(coverage->raster_samples, 1) <= 1 || coverage->mode > 3 ||
        !(capture->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || (capture->usage & VK_IMAGE_USAGE_STORAGE_BIT) ||
        (u32)vk_surf_samples(source) != coverage->raster_samples ||
        (u32)vk_surf_samples(&capture->valid) != coverage->raster_samples ||
        (state->blend.logic_op && !g_vk.feat.logicOp)) return 0;
    const DsBlend* color = &state->blend;
    u32 mask = color->mask[index] & (outputs >> (index * 4)) & 15;
    if (!mask) {
        *prepared = (MixedResolve){0}; /* No destination access, descriptor or final pass for an inert slot. */
        return 1;
    }
    int blend = color->en[index] && mask;
    if (blend && !color->logic_op &&
        (color->sc[index] >= DS_FACTOR_SRC1_COLOR || color->dc[index] >= DS_FACTOR_SRC1_COLOR ||
         color->sa[index] >= DS_FACTOR_SRC1_COLOR || color->da[index] >= DS_FACTOR_SRC1_COLOR)) return 0;
    MixedKey key;
    memset(&key, 0, sizeof key); /* includes native struct padding in the pipeline key */
    key.samples = coverage->raster_samples;
    key.components = vk_coverage_components(coverage->mode);
    key.format = state->tgt.srgb_write ? target->f.vk : vk_srgb_as(target->f.vk, 0);
    key.blend = (VkPipelineColorBlendAttachmentState){.blendEnable = blend,
        .srcColorBlendFactor = (VkBlendFactor)color->sc[index], .dstColorBlendFactor = (VkBlendFactor)color->dc[index],
        .colorBlendOp = (VkBlendOp)color->cop[index], .srcAlphaBlendFactor = (VkBlendFactor)color->sa[index],
        .dstAlphaBlendFactor = (VkBlendFactor)color->da[index], .alphaBlendOp = (VkBlendOp)color->aop[index],
        .colorWriteMask = mask};
    key.logic = color->logic_op;
    VkPipeline pipeline = mixed_pipeline(&key);
    VkImageView attachment = vk_rt_attachment(target, state->tgt.srgb_write);
    if (!pipeline || !attachment) return 0;
    Surf* images[2] = {source, &capture->valid};
    vk_cmd();
    u64 frame = vk_frame_id(); /* vk_cmd starts/recycles the frame before its cached set can be inspected */
    VkDescriptorSet* cached_set = index ? &capture->extra[index - 1].mixed_set : &capture->mixed_set;
    u64* cached_frame = index ? &capture->extra[index - 1].mixed_frame : &capture->mixed_frame;
    VkDescriptorSet set = *cached_set;
    if (!set || *cached_frame != frame) {
        VkDescriptorImageInfo image_info[2];
        const VkComponentMapping identity = {0};
        for (u32 i = 0; i < 2; i++) {
            VkImageView view = vk_tex_view(images[i], VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
            if (!view) return 0;
            image_info[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        set = vk_desc_alloc(g_capture.mixed_descriptors);
        if (!set) return 0;
        VkWriteDescriptorSet writes[2];
        for (u32 i = 0; i < 2; i++)
            writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &image_info[i]};
        vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
        *cached_set = set; /* never update an immutable set referenced by queued primitives */
        *cached_frame = frame;
    }
    *prepared = (MixedResolve){target, source, pipeline, attachment, set};
    return 1;
}
int vk_capture_mixed_resolve(VkCapture* capture, const VkCoverage* coverage, const DsState* state, u32 outputs) {
    if (!capture || !coverage || !state || !capture->valid.img) return 0;
    u32 count = capture->mixed_count ? capture->mixed_count : 1;
    if (count > vk_coverage_capture_limit(g_vk.props.limits.maxColorAttachments) ||
        !vk_capture_mixed_outputs_supported(outputs, count, &state->blend)) return 0;
    float factors[16] = {0};
    if (vk_coverage_table(coverage, 1, factors) != coverage->raster_samples) return 0;
    MixedResolve prepared[7];
    /* Prepare every pipeline, view and immutable set before changing any destination. A failed later slot must not
       leave the earlier targets with only part of this primitive resolved. */
    for (u32 index = 0; index < count; index++)
        if (!mixed_prepare(capture, coverage, state, outputs, index, &prepared[index])) return 0;
    vk_render_end();
    VkCommandBuffer command = vk_cmd();
    for (u32 index = 0; index < count; index++) {
        Surf* target = prepared[index].target;
        if (!target) continue;
        Surf* images[2] = {prepared[index].source, &capture->valid};
        for (u32 i = 0; i < 2; i++)
            vk_surf_use(command, images[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        vk_surf_use(command, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color_attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = prepared[index].attachment, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, {target->sw, target->sh}}, .layerCount = 1,
            .colorAttachmentCount = 1, .pColorAttachments = &color_attachment};
        vk_render_forget_state();
        rec_CmdBeginRendering(command, &rendering);
        rec_CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, prepared[index].pipeline);
        rec_CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g_capture.mixed_layout, 0, 1,
                                 &prepared[index].set, 0, NULL);
        VkViewport viewport = {0, 0, (float)target->sw, (float)target->sh, 0, 1};
        VkRect2D scissor = {{0, 0}, {target->sw, target->sh}};
        rec_CmdSetViewportWithCount(command, 1, &viewport);
        rec_CmdSetScissorWithCount(command, 1, &scissor);
        rec_CmdSetBlendConstants(command, state->dyn.blend_const);
        rec_CmdPushConstants(command, g_capture.mixed_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof factors, factors);
        rec_CmdDraw(command, 3, 1, 0, 0);
        rec_CmdEndRendering(command);
        vk_surf_wrote(target);
        vk_rt_drawn(target);
    }
    return 1;
}
void vk_capture_destroy(void) {
    capture_image_drop(&g_capture.active.source, 0);
    capture_image_drop(&g_capture.active.valid, 0);
    capture_extra_drop(&g_capture.active, 0, 0);
    capture_tiles_drop(&g_capture.active, 0);
    for (u32 i = 0; i < g_capture.mixed_count; i++) vkDestroyPipeline(g_vk.dev, g_capture.mixed[i].pipeline, NULL);
    free(g_capture.mixed);
    if (g_capture.mixed_vertex) vkDestroyShaderModule(g_vk.dev, g_capture.mixed_vertex, NULL);
    if (g_capture.mixed_layout) vkDestroyPipelineLayout(g_vk.dev, g_capture.mixed_layout, NULL);
    if (g_capture.mixed_descriptors) vkDestroyDescriptorSetLayout(g_vk.dev, g_capture.mixed_descriptors, NULL);
    for (u32 i = 0; i < g_capture.count; i++) vkDestroyPipeline(g_vk.dev, g_capture.pipelines[i].pipeline, NULL);
    free(g_capture.pipelines);
    if (g_capture.finalize) vkDestroyPipeline(g_vk.dev, g_capture.finalize, NULL);
    if (g_capture.layout) vkDestroyPipelineLayout(g_vk.dev, g_capture.layout, NULL);
    if (g_capture.descriptors) vkDestroyDescriptorSetLayout(g_vk.dev, g_capture.descriptors, NULL);
    g_capture = (CaptureState){0};
}
