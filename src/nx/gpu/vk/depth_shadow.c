/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/depth_shadow.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/shader/depth_stencil_spirv.h"
#include "nx/memory/mem.h"
#include "nx/hle/services/nvdrv.h"
#include <string.h>

static struct {
    VkBuffer buffer;
    VmaAllocation memory;
    u64 bytes;
    Surf *active, *reserved;
    u64 reserved_bytes;
    u32 depth_offset, stencil_offset;
    VkImageAspectFlags writes;
} g_depth_shadow;

static void shadow_barrier(VkPipelineStageFlags2 source, VkAccessFlags2 source_access,
                            VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access) {
    VkBufferMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = source, .srcAccessMask = source_access,
        .dstStageMask = destination, .dstAccessMask = destination_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = g_depth_shadow.buffer, .offset = 0, .size = g_depth_shadow.bytes};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier};
    rec_CmdPipelineBarrier2(vk_cmd(), &dependency);
}
static int shadow_buffer(u64 bytes) {
    if (g_depth_shadow.buffer && g_depth_shadow.bytes >= bytes) return 1;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo memory_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    if (!VK_OK(vk_buffer_create(g_vk.vma, &info, &memory_info, &buffer, &memory, NULL))) return 0;
    vk_mem_track(memory, VK_OWN_MISC);
    if (g_depth_shadow.buffer) vk_defer_buffer(g_depth_shadow.buffer, g_depth_shadow.memory);
    g_depth_shadow.buffer = buffer;
    g_depth_shadow.memory = memory;
    g_depth_shadow.bytes = bytes;
    return 1;
}
static u32 stencil_control(const DsDynamic* dynamic, u32 face) {
    return dynamic->face[face].cmp | (u32)dynamic->face[face].fail << 3 |
           (u32)dynamic->face[face].pass << 6 | (u32)dynamic->face[face].zfail << 9;
}
static u32 stencil_values(const DsDynamic* dynamic, u32 face) {
    return (dynamic->face[face].ref & 255) | (dynamic->face[face].cmask & 255) << 8 |
           (dynamic->face[face].wmask & 255) << 16;
}
static int stencil_writes(const DsDynamic* dynamic, u32 face) {
    return (dynamic->face[face].wmask & 255) &&
        (dynamic->face[face].fail || dynamic->face[face].pass || dynamic->face[face].zfail);
}
static VkBufferImageCopy aspect_copy(const Surf* surface, VkImageAspectFlagBits aspect, u32 offset) {
    return (VkBufferImageCopy){.bufferOffset = (u64)offset * 4,
        .imageSubresource = {aspect, 0, 0, 1}, .imageExtent = {surface->sw, surface->sh, 1}};
}
static u64 shadow_shape(Surf* surface, u32* depth_offset, u32* stencil_offset) {
    if (!surface || !surface->img || surface->layers != 1 || surface->depth3d > 1 || surface->volume ||
        vk_surf_samples(surface) != 1 ||
        (surface->f.vk != VK_FORMAT_D32_SFLOAT && surface->f.vk != VK_FORMAT_D32_SFLOAT_S8_UINT) ||
        !g_vk.has_depth_range_unrestricted || !g_vk.has_fragment_interlock || !g_vk.feat.fragmentStoresAndAtomics)
        return 0;
    u64 bytes = shader_depth_stencil_layout(surface->sw, surface->sh,
        surface->f.vk == VK_FORMAT_D32_SFLOAT_S8_UINT, depth_offset, stencil_offset);
    return bytes <= g_vk.props.limits.maxStorageBufferRange && bytes <= g_vk.alloc_cap ? bytes : 0;
}
typedef struct { u64 cpu, size; int overlap; } DepthAlias;
static void shadow_alias_peer(void* user, u64 at, u64 size) {
    DepthAlias* range = user;
    if (at > UINT64_MAX - size || snx_ranges_overlap(at, size, range->cpu, range->size)) range->overlap = 1;
}
static u64 shadow_guest_span(const Surf* surface) {
    if (!surface || !surface->gva || !surface->w || !surface->h || !surface->layers || !surface->f.bpp) return 0;
    u64 size = vk_surf_guest_size(surface);
    /* A sampled texture can address other mip levels too; rt_size, in contrast, already includes its view. */
    if (surface->size > size) size = surface->size;
    return surface->gva <= UINT64_MAX - size ? size : 0;
}
int vk_depth_shadow_range_aliases(const Surf* depth, u64 gva, u64 bytes) {
    if (!depth || !depth->img || !gva || !bytes || gva > UINT64_MAX - bytes) return 1;
    u64 depth_size = shadow_guest_span(depth);
    if (!depth_size || snx_ranges_overlap(depth->gva, depth_size, gva, bytes)) return 1;
    u64 depth_cpu = 0, other_cpu = 0;
    /* Sparse or fragmented backing needs a larger proof. Refuse it here rather than miss a later aliased page. */
    if (gmmu_run(depth->gva, depth_size, &depth_cpu) != depth_size ||
        gmmu_run(gva, bytes, &other_cpu) != bytes ||
        depth_cpu > UINT64_MAX - depth_size || other_cpu > UINT64_MAX - bytes ||
        snx_ranges_overlap(depth_cpu, depth_size, other_cpu, bytes)) return 1;
    DepthAlias range = {other_cpu, bytes, 0};
    if (mem_alias_any()) mem_alias_peers(depth_cpu, depth_size, shadow_alias_peer, &range);
    return range.overlap;
}
int vk_depth_shadow_aliases(const Surf* depth, const Surf* other) {
    if (!depth || !other || depth == other || !depth->img || !other->img || depth->img == other->img) return 1;
    return vk_depth_shadow_range_aliases(depth, other->gva, shadow_guest_span(other));
}
int vk_depth_shadow_reserve(Surf* surface, VkDescriptorBufferInfo* descriptor) {
    if (descriptor) *descriptor = (VkDescriptorBufferInfo){0};
    if (!descriptor || g_depth_shadow.active) return 0;
    u32 depth_offset, stencil_offset;
    u64 bytes = shadow_shape(surface, &depth_offset, &stencil_offset);
    if (!bytes) return 0;
    vk_cmd(); /* retire a replaced allocation with the current frame before allocating descriptor state */
    if (!shadow_buffer(bytes)) return 0;
    g_depth_shadow.reserved = surface;
    g_depth_shadow.reserved_bytes = bytes;
    g_depth_shadow.depth_offset = depth_offset;
    g_depth_shadow.stencil_offset = stencil_offset;
    *descriptor = (VkDescriptorBufferInfo){g_depth_shadow.buffer, 0, bytes};
    return 1;
}
static void shadow_initialize(Surf* surface, int stencil) {
    int undefined = surface->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
        (surface->discarded && surface->discard_wseq == surface->wseq);
    if (!undefined) return;
    /* These contents are unspecified as a whole. Give native copies real initialized bits without changing
       any defined pixel of an existing surface. A merely predicated/partly undefined image is not cleared. */
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearDepthStencilValue value = {1.0f, 0};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
        0, 1, 0, 1};
    rec_CmdClearDepthStencilImage(vk_cmd(), surface->img, surface->layout, &value, 1, &range);
    surface->discarded = 0;
    surface->pred_undefined = 0;
    vk_surf_wrote(surface);
    vk_rt_drawn_full(surface);
}
int vk_depth_shadow_prepare(Surf* surface, const DsDynamic* dynamic, const DrawCall* call,
                              u32 fixed_mask, VkDescriptorBufferInfo* descriptor) {
    if (descriptor) *descriptor = (VkDescriptorBufferInfo){0};
    if (!surface || !dynamic || !call || !descriptor || g_depth_shadow.active ||
        g_depth_shadow.reserved != surface || !g_depth_shadow.buffer || dynamic->depth_cmp > 7)
        return 0;
    for (u32 face = 0; face < 2; face++)
        if (dynamic->face[face].cmp > 7 || dynamic->face[face].fail > 7 || dynamic->face[face].pass > 7 ||
            dynamic->face[face].zfail > 7) return 0;
    if ((call->state_flags & DS_DRAW_DEPTH_BOUNDS) &&
        !(call->depth_bounds[0] >= 0 && call->depth_bounds[0] <= call->depth_bounds[1] && call->depth_bounds[1] <= 1))
        return 0;
    u32 depth_offset, stencil_offset;
    u64 bytes = shadow_shape(surface, &depth_offset, &stencil_offset);
    if (!bytes || bytes != g_depth_shadow.reserved_bytes || depth_offset != g_depth_shadow.depth_offset ||
        stencil_offset != g_depth_shadow.stencil_offset) return 0;
    ShaderDepthStencilHeader header = {0};
    header.word[SH_DS_WIDTH] = surface->sw; header.word[SH_DS_HEIGHT] = surface->sh;
    header.word[SH_DS_DEPTH_OFFSET] = depth_offset; header.word[SH_DS_STENCIL_OFFSET] = stencil_offset;
    header.word[SH_DS_FLAGS] = (dynamic->depth_test ? SH_DS_DEPTH_TEST : 0) |
        (dynamic->depth_write ? SH_DS_DEPTH_WRITE : 0) | (dynamic->stencil_test && stencil_offset ? SH_DS_STENCIL_TEST : 0) |
        (call->state_flags & DS_DRAW_DEPTH_BOUNDS ? SH_DS_DEPTH_BOUNDS : 0) |
        (!(fixed_mask & 1) ? SH_DS_FIXED_MASK_OFF : 0) |
        (u32)dynamic->depth_cmp << SH_DS_COMPARE_SHIFT;
    header.word[SH_DS_BOUNDS_MAX] = 0x3f800000u;
    if (call->state_flags & DS_DRAW_DEPTH_BOUNDS) {
        memcpy(&header.word[SH_DS_BOUNDS_MIN], &call->depth_bounds[0], 4);
        memcpy(&header.word[SH_DS_BOUNDS_MAX], &call->depth_bounds[1], 4);
    }
    header.word[SH_DS_CLAMP_MAX] = 0x3f800000u;
    header.word[SH_DS_FRONT_CONTROL] = stencil_control(dynamic, 0);
    header.word[SH_DS_FRONT_VALUES] = stencil_values(dynamic, 0);
    header.word[SH_DS_BACK_CONTROL] = stencil_control(dynamic, 1);
    header.word[SH_DS_BACK_VALUES] = stencil_values(dynamic, 1);
    vk_render_end();
    vk_cmd(); /* resources and staging belong to the current recorded frame, never an early upload command buffer */
    VkBuffer staging;
    u64 staging_offset;
    u8* data = vk_stage(sizeof header, 4, &staging, &staging_offset);
    if (!data) return 0;
    memcpy(data, &header, sizeof header);
    shadow_barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferCopy copy = {staging_offset, 0, sizeof header};
    rec_CmdCopyBuffer(vk_cmd(), staging, g_depth_shadow.buffer, 1, &copy);
    u64 pixels = (u64)surface->sw * surface->sh;
    if (stencil_offset && (pixels & 3)) {
        /* Initialize the unused tail too: each atomic reads a full word even when the S8 copy ends mid-word. */
        rec_CmdFillBuffer(vk_cmd(), g_depth_shadow.buffer, (u64)stencil_offset * 4 + (pixels & ~3ull), 4, 0);
        shadow_barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    shadow_initialize(surface, stencil_offset != 0);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy aspects[2] = {aspect_copy(surface, VK_IMAGE_ASPECT_DEPTH_BIT, depth_offset)};
    u32 count = 1;
    if (stencil_offset) aspects[count++] = aspect_copy(surface, VK_IMAGE_ASPECT_STENCIL_BIT, stencil_offset);
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, surface->layout, g_depth_shadow.buffer, count, aspects);
    shadow_barrier(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    g_depth_shadow.active = surface;
    g_depth_shadow.depth_offset = depth_offset;
    g_depth_shadow.stencil_offset = stencil_offset;
    g_depth_shadow.writes = dynamic->depth_test && dynamic->depth_write && dynamic->depth_cmp != DS_CMP_NEVER
        ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
    if (stencil_offset && dynamic->stencil_test && (stencil_writes(dynamic, 0) || stencil_writes(dynamic, 1)))
        g_depth_shadow.writes |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!(fixed_mask & 1)) g_depth_shadow.writes = 0;
    *descriptor = (VkDescriptorBufferInfo){g_depth_shadow.buffer, 0, bytes};
    return 1;
}
int vk_depth_shadow_finish(Surf* surface) {
    if (!surface || g_depth_shadow.active != surface) return 0;
    vk_render_end();
    VkImageAspectFlags writes = g_depth_shadow.writes;
    if (writes) {
        /* Unchanged pixels still come from the import copy, including when draw setup failed after prepare. */
        shadow_barrier(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy aspects[2];
        u32 count = 0;
        if (writes & VK_IMAGE_ASPECT_DEPTH_BIT)
            aspects[count++] = aspect_copy(surface, VK_IMAGE_ASPECT_DEPTH_BIT, g_depth_shadow.depth_offset);
        if (writes & VK_IMAGE_ASPECT_STENCIL_BIT)
            aspects[count++] = aspect_copy(surface, VK_IMAGE_ASPECT_STENCIL_BIT, g_depth_shadow.stencil_offset);
        rec_CmdCopyBufferToImage(vk_cmd(), g_depth_shadow.buffer, surface->img, surface->layout, count, aspects);
        surface->pred_undefined = 0; /* callers reject conditional draws; the exported writes surely execute */
        vk_surf_wrote(surface);
        vk_rt_drawn(surface);
    }
    g_depth_shadow.active = NULL;
    g_depth_shadow.reserved = NULL;
    g_depth_shadow.reserved_bytes = 0;
    g_depth_shadow.writes = 0;
    return 1;
}
void vk_depth_shadow_cancel(void) {
    vk_render_end();
    g_depth_shadow.active = NULL;
    g_depth_shadow.reserved = NULL;
    g_depth_shadow.reserved_bytes = 0;
    g_depth_shadow.writes = 0;
}
void vk_depth_shadow_destroy(void) {
    if (g_depth_shadow.buffer) {
        vk_mem_untrack(g_depth_shadow.memory);
        vmaDestroyBuffer(g_vk.vma, g_depth_shadow.buffer, g_depth_shadow.memory);
    }
    g_depth_shadow.buffer = VK_NULL_HANDLE;
    g_depth_shadow.memory = VK_NULL_HANDLE;
    g_depth_shadow.bytes = 0;
    g_depth_shadow.active = NULL;
    g_depth_shadow.reserved = NULL;
    g_depth_shadow.reserved_bytes = 0;
    g_depth_shadow.writes = 0;
}
