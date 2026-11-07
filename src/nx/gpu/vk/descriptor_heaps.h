/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_DESCRIPTOR_HEAPS_H
#define SNX_VK_DESCRIPTOR_HEAPS_H
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <string.h>

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
/* Within one image write, equal consumed fields select the same immutable heap indices. Padding and caller
   addresses never participate. Separate image descriptors never read their ignored sampler field. */
static inline uint32_t vk_descriptor_heap_image_run(VkDescriptorType type, const VkDescriptorImageInfo* images,
                                                     uint32_t count) {
    int combined = type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (!images || !count || (!combined && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                              type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) return 0;
    uint32_t run = 1;
    while (run < count && images[run].imageView == images[0].imageView &&
           images[run].imageLayout == images[0].imageLayout &&
           (!combined || images[run].sampler == images[0].sampler)) run++;
    return run;
}
/* The destination holds count4-byte indices. Write from the CPU value only; never read mapped GPU memory to
   replicate an earlier entry. Byte stores also support an unaligned CPU-model destination. */
static inline void vk_descriptor_heap_write_index_run(void* destination, uint32_t count, uint32_t value) {
    uint8_t* bytes = destination;
    for (uint32_t i = 0; i < count; i++) snx_st_le32(bytes + (size_t)i * 4, value);
}

/* The resource heap bytes a frame slot asks for. An open world's heaviest frames (one game on RADV, 2026-11-07)
   wrote 3,900 tables a frame: 2.1 MB of indices and descriptors. A slot of 1 MiB ran out of room once or twice in
   such a frame, and each time the frame moved on to the other slot (vk_descriptors_room), whose GPU work the GPU
   thread then waited for. The device's maxResourceHeapSize and allocation limit cut it down
   (vk_descriptor_heap_memory_plan_fit) */
#define VK_DESCRIPTOR_HEAP_RESOURCE_FRAME (8ull << 20)

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

#define VK_DESCRIPTOR_HEAP_BUFFER_BATCH 8u
typedef struct {
    uint64_t object, offset, range;
    uint32_t tag, heap_offset;
} VkDescriptorHeapResourceEntry;
typedef struct { uint64_t hash; uint32_t offset, used; } VkDescriptorHeapSamplerEntry;
#define VK_DESCRIPTOR_HEAP_RESOURCE_KEY_BYTES 28u
typedef char VkDescriptorHeapResourceEntryLayout[
    sizeof(VkDescriptorHeapResourceEntry) == 32 &&
    offsetof(VkDescriptorHeapResourceEntry, heap_offset) == VK_DESCRIPTOR_HEAP_RESOURCE_KEY_BYTES ? 1 : -1];
typedef char VkDescriptorHeapSamplerEntryLayout[sizeof(VkDescriptorHeapSamplerEntry) == 16 ? 1 : -1];
/* Nonnegative VkFormat values plus2 cannot reach the three image tags. Unformatted buffers have separate tags;
   formatted buffers currently support storage texel descriptors alone. An object handle of0 marks an empty slot. */
static inline int vk_descriptor_heap_resource_tag(VkDescriptorType type, VkFormat format, uint32_t* tag) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: *tag = 0; return 1;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: *tag = 1; return 1;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (format == VK_FORMAT_UNDEFINED || (uint32_t)format > INT32_MAX) return 0;
            *tag = (uint32_t)format + 2; return 1;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: *tag = UINT32_MAX; return 1;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: *tag = UINT32_MAX - 1; return 1;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: *tag = UINT32_MAX - 2; return 1;
        default: return 0;
    }
}
static inline int vk_descriptor_heap_resource_equal(const VkDescriptorHeapResourceEntry* a,
                                                     const VkDescriptorHeapResourceEntry* b) {
    return !memcmp(a, b, VK_DESCRIPTOR_HEAP_RESOURCE_KEY_BYTES);
}
/* Hashes only reject candidates. Every collision still compares all opaque bytes in the CPU-only shadow. */
static inline int vk_descriptor_heap_sampler_equal(const VkDescriptorHeapSamplerEntry* entry, uint64_t hash,
                                                    const uint8_t* shadow, size_t frame_base,
                                                    const void* encoding, size_t bytes) {
    return entry->used && entry->hash == hash &&
        !memcmp(shadow + ((size_t)entry->offset - frame_base), encoding, bytes);
}
typedef struct {
    VkDeviceSize used;
    uint32_t count, misses, changes, exhausted, offsets[VK_DESCRIPTOR_HEAP_BUFFER_BATCH];
    uint64_t hits, evictions, bytes;
    struct { VkDescriptorHeapResourceEntry entry; uint32_t bytes; uint64_t hash; } miss[VK_DESCRIPTOR_HEAP_BUFFER_BATCH];
    struct { uint32_t slot; VkDescriptorHeapResourceEntry entry; } change[VK_DESCRIPTOR_HEAP_BUFFER_BATCH];
} VkDescriptorHeapBufferBatch;
/* Lookups see pending replacements in input order. Immutable offsets are copied into each item, so replacing a
   pending cache slot cannot redirect an earlier descriptor. The unique miss list also deduplicates a key after
   another pending key evicts it. Planning changes no global cursor, cache entry or mapped heap byte. */
static inline int vk_descriptor_heap_buffer_plan(VkDescriptorHeapBufferBatch* batch,
                                                  const VkDescriptorHeapResourceEntry* cache, uint32_t capacity,
                                                  const VkDescriptorHeapMemoryPlan* plan, uint32_t frame,
                                                  const VkDescriptorHeapResourceEntry* key, uint64_t hash, uint32_t bytes,
                                                  VkDeviceSize alignment) {
    if (batch->count == VK_DESCRIPTOR_HEAP_BUFFER_BATCH || capacity < 4 || (capacity & (capacity - 1)) || !key->object)
        return 0;
    for (uint32_t i = 0; i < batch->misses; i++)
        if (batch->miss[i].hash == hash && vk_descriptor_heap_resource_equal(&batch->miss[i].entry, key)) {
            batch->offsets[batch->count++] = batch->miss[i].entry.heap_offset;
            batch->hits++;
            return 1;
        }
    uint32_t first = ((uint32_t)hash & (capacity / 4 - 1)) * 4;
    uint32_t replacement = first + (uint32_t)(hash >> 32) % 4;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t slot = first + i;
        const VkDescriptorHeapResourceEntry* entry = &cache[slot];
        for (uint32_t j = 0; j < batch->changes; j++)
            if (batch->change[j].slot == slot) { entry = &batch->change[j].entry; break; }
        if (!entry->object) { replacement = slot; break; }
        if (vk_descriptor_heap_resource_equal(entry, key)) {
            batch->offsets[batch->count++] = entry->heap_offset;
            batch->hits++;
            return 1;
        }
    }
    VkDeviceSize used = batch->used;
    uint32_t offset;
    if (!vk_descriptor_heap_allocate(plan, &used, frame, bytes, alignment, &offset)) {
        batch->exhausted = 1;
        return 0;
    }
    uint32_t change = 0;
    while (change < batch->changes && batch->change[change].slot != replacement) change++;
    const VkDescriptorHeapResourceEntry* previous = change < batch->changes
        ? &batch->change[change].entry : &cache[replacement];
    batch->evictions += previous->object != 0;
    VkDescriptorHeapResourceEntry entry = *key;
    entry.heap_offset = offset;
    batch->miss[batch->misses].entry = entry;
    batch->miss[batch->misses].hash = hash;
    batch->miss[batch->misses++].bytes = bytes;
    batch->change[change].slot = replacement;
    batch->change[change].entry = entry;
    if (change == batch->changes) batch->changes++;
    batch->offsets[batch->count++] = offset;
    batch->used = used;
    batch->bytes += bytes;
    return 1;
}
/* Encoding may fail after writing part of CPU scratch. Publish nothing in that case. A successful chunk writes
   immutable bytes from CPU scratch only, then installs its cache entries/cursor. Earlier chunks stay valid. */
static inline int vk_descriptor_heap_buffer_publish(const VkDescriptorHeapBufferBatch* batch,
                                                     VkDescriptorHeapResourceEntry* cache, VkDeviceSize* used,
                                                     void* mapped, const void* scratch, size_t stride, int encoded) {
    if (!encoded || batch->exhausted) return 0;
    for (uint32_t i = 0; i < batch->misses; i++)
        memcpy((uint8_t*)mapped + batch->miss[i].entry.heap_offset,
               (const uint8_t*)scratch + (size_t)i * stride, batch->miss[i].bytes);
    for (uint32_t i = 0; i < batch->changes; i++) cache[batch->change[i].slot] = batch->change[i].entry;
    *used = batch->used;
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

/* What keeps a device from descriptor heaps, for the log: the first piece missing, NULL when none is */
/* the vk_descriptors setting a device takes: 3, Automatic, is 2 (heaps, else buffers, else sets) on a GPU that shares
   the system's memory (uma: vk_common.h's SNX_VK_UMA, an integrated GPU, a CPU's, a phone's) and 0 (sets) on one
   with memory of its own. On a discrete RX 6700 XT, heaps wrote each draw's descriptors on the GPU thread, 0.75 ms
   a frame more than sets, whose recording thread writes them, and one open world's frames went from release to
   present 1.2 to 1.6 ms later. Every other value is taken as it is */
static inline int vk_descriptors_auto(int setting, int uma) { return setting == 3 ? (uma ? 2 : 0) : setting; }
static inline const char* vk_descriptor_heap_missing(int extension, int feature, int addresses, int flags) {
    return !extension   ? "no VK_EXT_descriptor_heap"
           : !feature   ? "its descriptorHeap feature is off"
           : !addresses ? "its bufferDeviceAddress feature is off"
           : !flags     ? "neither extendedFlags nor maintenance5"
                        : NULL;
}

/* Called after VMA and the extension entry points exist. A failure logs what failed and the backend the device
   keeps (buffers or sets), and leaves those backends available. */
int vk_descriptor_heaps_init(const VkPhysicalDeviceDescriptorHeapPropertiesEXT* props);
/* The device is idle before destruction; the dedicated transition set remains immutable until then. */
void vk_descriptor_heaps_destroy(void);
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
    uint64_t image_requests, image_driver_calls, image_hits, image_evictions, image_lookups, image_replicas;
    uint64_t sampler_requests, sampler_driver_calls, sampler_hits, sampler_evictions, sampler_lookups, sampler_replicas;
    uint64_t address_queries, peak_used_bytes;
    uint64_t arena_allocated_bytes, cache_allocated_bytes, reserved_bytes, resource_range_bytes, sampler_range_bytes;
    uint64_t buffer_native_calls; /* API invocations; buffer_driver_calls remains encoded-element count. */
} VkDescriptorHeapStats;
/* Counters are monotonic. Requests/payload include every logical element. On successful runs, requests equal
   lookups+replicas; sampler hits may overlap driver calls when byte interning follows native encoding.
   Memory values describe physical VMA allocations, reserved ranges and CPU cache storage. */
void vk_descriptor_heaps_get_stats(VkDescriptorHeapStats* stats);

/* Any legacy descriptor or push command invalidates heap state across both bind points. */
void vk_descriptor_heaps_invalidate(void);
/* Graphics descriptor-buffer offsets were actually bound in this command-buffer generation. */
void vk_descriptor_heaps_buffer_bound(uint32_t begun);
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
