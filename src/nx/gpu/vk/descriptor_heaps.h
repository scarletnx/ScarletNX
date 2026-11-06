/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_DESCRIPTOR_HEAPS_H
#define SNX_VK_DESCRIPTOR_HEAPS_H
#include "common/utilities/bits.h"
#include <vulkan/vulkan.h>

/* Existing translated graphics shaders use push bytes0..127. One address per Vulkan set follows them. */
#define VK_DESCRIPTOR_HEAP_PUSH_OFFSET 128u
#define VK_DESCRIPTOR_HEAP_PUSH_SIZE (VK_DESCRIPTOR_HEAP_PUSH_OFFSET + 5u * 8u)

/* Formatted texel-buffer descriptors belong to the image class. The buffer class covers unformatted buffer
   addresses, so using its smaller alignment for a typed buffer can silently select the wrong descriptor. */
static inline VkDeviceSize vk_descriptor_heap_descriptor_alignment(const VkPhysicalDeviceDescriptorHeapPropertiesEXT* props,
                                                                    VkDescriptorType type) {
    if (!props) return 0;
    switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: return props->samplerDescriptorAlignment;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return props->bufferDescriptorAlignment;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return props->imageDescriptorAlignment;
        default: return 0;
    }
}

typedef struct {
    VkDeviceSize frame_size, reserved_offset, reserved_size, range_size, allocation_size, alignment;
} VkDescriptorHeapMemoryPlan;
/* A stable heap shares one reserved range across command buffers. Only its user range is split into frame slots.
   The extra address-alignment bytes belong to the backing buffer, outside the bound heap range. */
static inline int vk_descriptor_heap_memory_plan(VkDescriptorHeapMemoryPlan* plan, VkDeviceSize frame_size,
                                                  uint32_t frames, VkDeviceSize reserved, VkDeviceSize heap_alignment,
                                                  VkDeviceSize descriptor_alignment, VkDeviceSize heap_limit,
                                                  VkDeviceSize allocation_limit) {
    if (!plan || !frame_size || !frames || !heap_alignment || !descriptor_alignment ||
        frame_size > UINT64_MAX - descriptor_alignment + 1) return 0;
    frame_size = snx_align_up(frame_size, descriptor_alignment);
    if (frame_size > UINT32_MAX / frames) return 0; /* Indirect indices are byte offsets in32-bit words. */
    VkDeviceSize user = frame_size * frames;
    if (reserved > UINT64_MAX - user) return 0;
    VkDeviceSize range = user + reserved;
    if (range > heap_limit || range > UINT64_MAX - heap_alignment + 1 ||
        range + heap_alignment - 1 > allocation_limit) return 0;
    *plan = (VkDescriptorHeapMemoryPlan){frame_size, user, reserved, range, range + heap_alignment - 1, heap_alignment};
    return 1;
}

/* Device maxima include the reserved range. Reduce the requested user range instead of rejecting a useful small
   heap. Frame partitions retain descriptor alignment, including when the advertised heap limit is not aligned. */
static inline int vk_descriptor_heap_memory_plan_fit(VkDescriptorHeapMemoryPlan* plan, VkDeviceSize frame_size,
                                                      uint32_t frames, VkDeviceSize reserved, VkDeviceSize heap_alignment,
                                                      VkDeviceSize descriptor_alignment, VkDeviceSize heap_limit,
                                                      VkDeviceSize allocation_limit) {
    if (!frame_size || !frames || !heap_alignment || !descriptor_alignment || allocation_limit < heap_alignment - 1)
        return 0;
    VkDeviceSize range_limit = allocation_limit - (heap_alignment - 1);
    if (range_limit > heap_limit) range_limit = heap_limit;
    if (range_limit <= reserved) return 0;
    VkDeviceSize user_limit = range_limit - reserved;
    if (user_limit > UINT32_MAX) user_limit = UINT32_MAX;
    VkDeviceSize frame_limit = snx_align_down(user_limit / frames, descriptor_alignment);
    if (!frame_limit) return 0;
    if (frame_size > frame_limit) frame_size = frame_limit;
    return vk_descriptor_heap_memory_plan(plan, frame_size, frames, reserved, heap_alignment, descriptor_alignment,
                                           heap_limit, allocation_limit);
}
/* A failed allocation changes neither the cursor nor earlier live offsets. The caller retires a frame only at an
   external draw boundary, after its fence; an allocation never submits or changes the command buffer. */
static inline int vk_descriptor_heap_allocate(const VkDescriptorHeapMemoryPlan* plan, VkDeviceSize* used,
                                               uint32_t slot, VkDeviceSize size, VkDeviceSize alignment, uint32_t* offset) {
    if (!plan || !used || !offset || !plan->frame_size || !size || !alignment ||
        slot >= plan->reserved_offset / plan->frame_size || *used > plan->frame_size ||
        *used > UINT64_MAX - alignment + 1) return 0;
    VkDeviceSize at = snx_align_up(*used, alignment);
    if (at > plan->frame_size || size > plan->frame_size - at) return 0;
    VkDeviceSize absolute = slot * plan->frame_size + at;
    if (absolute > UINT32_MAX) return 0;
    *offset = (uint32_t)absolute;
    *used = at + size;
    return 1;
}

/* Only CPU lookup metadata belongs to this epoch. Encoded heap bytes remain in their frame partitions. The zero
   initializer is invalid, including for frame0; an explicit validity bit also handles frame counter wrap. */
typedef struct { uint64_t frame; int valid; } VkDescriptorHeapCacheTag;
static inline int vk_descriptor_heap_cache_frame(VkDescriptorHeapCacheTag* tag, uint64_t frame) {
    if (tag->valid && tag->frame == frame) return 0;
    *tag = (VkDescriptorHeapCacheTag){frame, 1};
    return 1;
}
static inline void vk_descriptor_heap_cache_reset(VkDescriptorHeapCacheTag* tag) { tag->valid = 0; }

/* Resource and sampler runs are contiguous for each binding, rather than reserving a sampler copy of the entire
   resource layout. A single scalar texture after34 buffer bindings then occupies36 words instead of a sparse tail. */
static inline int vk_descriptor_heap_table_append(uint32_t* words, uint32_t count, int combined,
                                                  uint32_t* resource_word, uint32_t* sampler_word) {
    if (!words || !resource_word || !sampler_word || !count || count > (UINT32_MAX - *words) / (combined ? 2u : 1u)) return 0;
    *resource_word = *words;
    *sampler_word = combined ? *words + count : 0;
    *words += count * (combined ? 2u : 1u);
    return 1;
}

/* Map one existing binding to separate byte-index arrays. Byte strides preserve exact per-type packing and avoid
   the20-bit image/12-bit sampler limit of combined packed indices. The actual index must respect its type alignment. */
static inline int vk_descriptor_heap_binding_mapping(VkDescriptorSetAndBindingMappingEXT* mapping,
                                                      uint32_t set, uint32_t binding, uint32_t word,
                                                      uint32_t sampler_word, VkDescriptorType type) {
    if (!mapping || set >= 5 || word > UINT32_MAX / 4 || sampler_word > UINT32_MAX / 4) return 0;
    VkSpirvResourceTypeFlagsEXT mask;
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: mask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT; break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            mask = VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT |
                   VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT; break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            mask = VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT; break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            mask = VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT; break;
        default: return 0;
    }
    VkDescriptorMappingSourceIndirectIndexArrayEXT source = {
        .pushOffset = VK_DESCRIPTOR_HEAP_PUSH_OFFSET + 8 * set,
        .addressOffset = 4 * word, .heapIndexStride = 1,
        .samplerPushOffset = VK_DESCRIPTOR_HEAP_PUSH_OFFSET + 8 * set,
        .samplerAddressOffset = 4 * sampler_word, .samplerHeapIndexStride = 1};
    *mapping = (VkDescriptorSetAndBindingMappingEXT){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
        .descriptorSet = set, .firstBinding = binding, .bindingCount = 1, .resourceMask = mask,
        .source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT,
        .sourceData.indirectIndexArray = source};
    return 1;
}

/* Called after VMA and the extension entry points exist. Failure leaves the legacy backends available. */
int vk_descriptor_heaps_init(const VkPhysicalDeviceDescriptorHeapPropertiesEXT* props);
void vk_descriptor_heaps_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding* bindings,
                                 uint32_t count);
/* The caller owns maps through pipeline creation. Attach mapping info to the corresponding shader stage. */
uint32_t vk_descriptor_heaps_mapping(VkDescriptorSetLayout layout, uint32_t set,
                                      VkDescriptorSetAndBindingMappingEXT* maps, uint32_t capacity);
VkDescriptorSet vk_descriptor_heaps_write(VkDescriptorSetLayout layout, uint32_t count,
                                           const VkWriteDescriptorSet* writes);
void vk_descriptor_heaps_bind(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkDescriptorSet* sets);
/* Writes returnNULL on exhaustion. The renderer retries the whole draw with its legacy backend; no write submits.
   Reset only after the slot's GPU work completes and its command pool is reset. */
void vk_descriptor_heaps_reset(uint32_t slot);
int vk_descriptor_heaps_room(uint32_t slot);
void vk_descriptor_heaps_stats(void);
typedef struct {
    uint64_t tables, table_bytes, full_layout_bytes, payload_bytes, arena_bytes, resource_bytes, sampler_bytes;
    uint64_t buffer_requests, buffer_driver_calls, buffer_hits, buffer_evictions;
    uint64_t image_requests, image_driver_calls, image_hits, image_evictions;
    uint64_t sampler_requests, sampler_driver_calls, sampler_hits, sampler_evictions;
    uint64_t address_queries, peak_used_bytes;
    uint64_t arena_allocated_bytes, cache_allocated_bytes, reserved_bytes, resource_range_bytes, sampler_range_bytes;
} VkDescriptorHeapStats;
/* Counters are monotonic. Memory values describe physical VMA allocations, reserved ranges and CPU cache storage. */
void vk_descriptor_heaps_get_stats(VkDescriptorHeapStats* stats);

/* Any legacy descriptor or push command invalidates heap state across both bind points. */
void vk_descriptor_heaps_invalidate(void);
uint64_t vk_descriptor_heaps_epoch(void);
void vk_descriptor_heaps_pipeline_state(int active);
int vk_descriptor_heaps_active(void);

/* Owned by the device registry. The copied pNext chain must remain valid in chain while the descriptor is encoded.
   Samplers return heap-compatible custom-border index state; unsupported chains return0. */
int vk_descriptor_view_info(VkImageView view, VkImageViewCreateInfo* out, void* chain, size_t size);
int vk_descriptor_sampler_info(VkSampler sampler, VkSamplerCreateInfo* out, void* chain, size_t size);
/* Owned by record.c: these copy all data before a worker can consume it. */
void rec_CmdBindDescriptorHeapsEXT(VkCommandBuffer cb, const VkBindHeapInfoEXT* resource,
                                   const VkBindHeapInfoEXT* sampler);
void rec_CmdPushDataEXT(VkCommandBuffer cb, uint32_t offset, uint32_t size, const void* data);
#endif
