/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Existing set/binding decorations map to immutable arrays of byte offsets. Only reachable writes consume index
   words or descriptor encodings. Exact descriptor sizes and separate alignments avoid a worst-case global stride.
   Each frame slot owns a disjoint user range until cmd.c waits for its work and resets its command pool. The
   implementation's reserved range remains identical for every binding and is never touched by the application.
   Sampler bytes are deterministic for identical create infos (VK_EXT_descriptor_heap), so byte interning preserves
   sampler state while it removes duplicate heap copies. All cache comparisons use CPU memory, never mapped GPU memory. */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "common/utilities/endian.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

#define HEAP_RESOURCE_FRAME (1ull << 20)
#define HEAP_SAMPLER_FRAME (128ull << 10)
#define HEAP_RESOURCE_CACHE 4096u
#define HEAP_SAMPLER_CACHE 2048u
#define HEAP_SAMPLER_HANDLES 1024u
#define HEAP_CHAIN_BYTES 1024u

enum { HEAP_UBO, HEAP_SSBO, HEAP_IMAGE, HEAP_STORAGE_IMAGE, HEAP_TEXEL, HEAP_SAMPLER, HEAP_KINDS };
typedef struct {
    VkBuffer buffer;
    VmaAllocation memory;
    u8* mapped;
    VkDeviceAddress address;
    VkDeviceSize allocation_bytes;
    VkDescriptorHeapMemoryPlan plan;
    VkDeviceSize used[VK_FRAMES];
} Heap;
typedef struct {
    u64 key[4];
    u32 offset;
} ResourceEntry;
typedef struct {
    u64 hash[2];
    u32 offset, used;
} SamplerEntry;
typedef struct { VkSampler sampler; u32 offset; } SamplerHandle;
typedef struct {
    VkDescriptorSetLayout layout;
    u32 offset[SNX_VKB_END], sampler_offset[SNX_VKB_END], count[SNX_VKB_END], words;
    VkDeviceSize full_size;
    VkDescriptorType type[SNX_VKB_END];
} HeapLayout;
static struct {
    Heap resource, sampler;
    HeapLayout layouts[10];
    u32 nlayouts;
    VkDeviceSize sizes[HEAP_KINDS], alignment[HEAP_KINDS];
    struct {
        ResourceEntry resources[HEAP_RESOURCE_CACHE];
        SamplerEntry samplers[HEAP_SAMPLER_CACHE];
        SamplerHandle handles[HEAP_SAMPLER_HANDLES];
    } cache; /* CPU-only metadata, used synchronously by the current frame's writer. */
    VkDescriptorHeapCacheTag cache_frame;
    int exhausted[VK_FRAMES];
    u8 *scratch, *sampler_bytes;
    u32 begun;
    u64 epoch, bound_epoch;
    size_t scratch_size;
    VkDescriptorHeapStats stats, reported;
    int ready, active;
} g_heaps;

static u64 handle_key(VkBuffer buffer) {
#if VK_USE_64_BIT_PTR_DEFINES
    return (u64)(uintptr_t)buffer;
#else
    return (u64)buffer;
#endif
}
static VkDeviceAddress buffer_address(VkBuffer buffer) {
    if (!buffer) return 0;
    VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    g_heaps.stats.address_queries++;
    return vkGetBufferDeviceAddress(g_vk.dev, &info);
}
static int kind(VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return HEAP_UBO;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return HEAP_SSBO;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return HEAP_IMAGE;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return HEAP_STORAGE_IMAGE;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return HEAP_TEXEL;
        case VK_DESCRIPTOR_TYPE_SAMPLER: return HEAP_SAMPLER;
        default: return -1;
    }
}
static void heap_discard(Heap* heap) {
    if (heap->buffer) {
        vk_mem_untrack(heap->memory);
        vmaDestroyBuffer(g_vk.vma, heap->buffer, heap->memory);
    }
    *heap = (Heap){0};
}
static int heap_create(Heap* heap, const VkDescriptorHeapMemoryPlan* plan) {
    VkBufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = plan->allocation_size,
        .usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo allocation = {.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo details;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &create, &allocation, &heap->buffer, &heap->memory, &details))) return 0;
    vk_mem_track(heap->memory);
    VkDeviceAddress original = buffer_address(heap->buffer);
    VkDeviceSize padding = snx_align_up(original, plan->alignment) - original;
    if (!original || !details.pMappedData || padding > plan->allocation_size - plan->range_size) {
        heap_discard(heap);
        return 0;
    }
    heap->address = original + padding;
    heap->mapped = (u8*)details.pMappedData + padding;
    heap->plan = *plan;
    heap->allocation_bytes = details.size;
    return 1;
}
int vk_descriptor_heaps_init(const VkPhysicalDeviceDescriptorHeapPropertiesEXT* props) {
    if (g_heaps.ready) return 1;
    if (!props || props->maxPushDataSize < VK_DESCRIPTOR_HEAP_PUSH_SIZE || !vkWriteResourceDescriptorsEXT ||
        !vkWriteSamplerDescriptorsEXT || !vkGetPhysicalDeviceDescriptorSizeEXT || !vkCmdBindResourceHeapEXT ||
        !vkCmdBindSamplerHeapEXT || !vkCmdPushDataEXT) return 0;
    static const VkDescriptorType types[HEAP_KINDS] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER};
    VkDeviceSize largest = 0;
    for (u32 i = 0; i < HEAP_KINDS; i++) {
        g_heaps.sizes[i] = vkGetPhysicalDeviceDescriptorSizeEXT(g_vk.phys, types[i]);
        g_heaps.alignment[i] = vk_descriptor_heap_descriptor_alignment(props, types[i]);
        if (!g_heaps.sizes[i] || !g_heaps.alignment[i] || g_heaps.sizes[i] > UINT32_MAX ||
            g_heaps.alignment[i] > UINT32_MAX || (g_heaps.alignment[i] & (g_heaps.alignment[i] - 1))) return 0;
        if (g_heaps.sizes[i] > largest) largest = g_heaps.sizes[i];
    }
    VkDeviceSize resource_alignment = props->imageDescriptorAlignment > props->bufferDescriptorAlignment
                                           ? props->imageDescriptorAlignment : props->bufferDescriptorAlignment;
    if (resource_alignment < 8) resource_alignment = 8;
    VkDeviceSize resource_address_alignment = props->resourceHeapAlignment;
    if (resource_address_alignment < 8) resource_address_alignment = 8; /* Indirect table addresses are8-byte aligned. */
    if (resource_address_alignment < resource_alignment) resource_address_alignment = resource_alignment;
    VkDeviceSize sampler_address_alignment = props->samplerHeapAlignment;
    if (sampler_address_alignment < props->samplerDescriptorAlignment)
        sampler_address_alignment = props->samplerDescriptorAlignment;
    if (!props->resourceHeapAlignment || !props->samplerHeapAlignment ||
        (props->resourceHeapAlignment & (props->resourceHeapAlignment - 1)) ||
        (props->samplerHeapAlignment & (props->samplerHeapAlignment - 1))) return 0;
    VkDescriptorHeapMemoryPlan resources, samplers;
    VkDeviceSize sampler_frame = 5ull * (SH_MAXTEX + 2 * SNX_TEX_ARRAY) *
                                 snx_align_up(g_heaps.sizes[HEAP_SAMPLER], props->samplerDescriptorAlignment);
    if (sampler_frame < HEAP_SAMPLER_FRAME) sampler_frame = HEAP_SAMPLER_FRAME;
    if (!vk_descriptor_heap_memory_plan_fit(&resources, HEAP_RESOURCE_FRAME, VK_FRAMES,
            props->minResourceHeapReservedRange, resource_address_alignment, resource_alignment,
            props->maxResourceHeapSize, g_vk.alloc_cap) ||
        !vk_descriptor_heap_memory_plan_fit(&samplers, sampler_frame, VK_FRAMES,
            props->minSamplerHeapReservedRange, sampler_address_alignment, props->samplerDescriptorAlignment,
            props->maxSamplerHeapSize, g_vk.alloc_cap)) return 0;
    if (resources.frame_size < 8 + g_heaps.sizes[HEAP_UBO] ||
        samplers.frame_size < g_heaps.sizes[HEAP_SAMPLER] || largest > SIZE_MAX - samplers.frame_size) return 0;
    /* One CPU allocation owns temporary encoding bytes and the sampler comparison shadow. */
    g_heaps.scratch = calloc(1, (size_t)(largest + samplers.frame_size));
    if (!g_heaps.scratch) return 0;
    g_heaps.scratch_size = (size_t)(largest + samplers.frame_size);
    g_heaps.sampler_bytes = g_heaps.scratch + largest;
    if (!heap_create(&g_heaps.resource, &resources) || !heap_create(&g_heaps.sampler, &samplers)) {
        heap_discard(&g_heaps.resource);
        heap_discard(&g_heaps.sampler);
        free(g_heaps.scratch);
        g_heaps.scratch = g_heaps.sampler_bytes = NULL;
        g_heaps.scratch_size = 0;
        return 0;
    }
    g_heaps.ready = 1;
    LOG("vulkan: descriptor heaps: %llu resource bytes + %llu reserved, %llu sampler bytes + %llu reserved\n",
        (unsigned long long)resources.reserved_offset, (unsigned long long)resources.reserved_size,
        (unsigned long long)samplers.reserved_offset, (unsigned long long)samplers.reserved_size);
    return 1;
}
static HeapLayout* find_layout(VkDescriptorSetLayout layout) {
    for (u32 i = 0; i < g_heaps.nlayouts; i++)
        if (g_heaps.layouts[i].layout == layout) return &g_heaps.layouts[i];
    return NULL;
}
void vk_descriptor_heaps_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding* bindings, u32 count) {
    if (!g_heaps.ready) return;
    if (find_layout(layout)) return;
    if (g_heaps.nlayouts == 10) fatal("vulkan: too many descriptor heap layouts");
    HeapLayout* l = &g_heaps.layouts[g_heaps.nlayouts++];
    l->layout = layout;
    for (u32 i = 0; i < count; i++) {
        const VkDescriptorSetLayoutBinding* b = &bindings[i];
        int k = kind(b->descriptorType);
        if (b->binding >= SNX_VKB_END || !b->descriptorCount || k < 0 || l->count[b->binding] ||
            b->descriptorCount > UINT32_MAX - l->words) fatal("vulkan: invalid descriptor heap binding");
        if (!vk_descriptor_heap_table_append(&l->words, b->descriptorCount,
                b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &l->offset[b->binding], &l->sampler_offset[b->binding])) fatal("vulkan: descriptor heap table overflow");
        l->count[b->binding] = b->descriptorCount;
        l->type[b->binding] = b->descriptorType;
        l->full_size += (u64)b->descriptorCount * snx_align_up(g_heaps.sizes[k], g_heaps.alignment[k]);
        if (b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            l->full_size += (u64)b->descriptorCount *
                snx_align_up(g_heaps.sizes[HEAP_SAMPLER], g_heaps.alignment[HEAP_SAMPLER]);

    }
    if (l->words > UINT32_MAX / 4) fatal("vulkan: descriptor heap table is too large");
    l->full_size += 4ull * l->words;
}
uint32_t vk_descriptor_heaps_mapping(VkDescriptorSetLayout layout, u32 set,
                                      VkDescriptorSetAndBindingMappingEXT* maps, u32 capacity) {
    HeapLayout* l = find_layout(layout);
    if (!l || !maps || set >= 5) return 0;
    u32 count = 0;
    for (u32 binding = 0; binding < SNX_VKB_END; binding++) {
        if (!l->count[binding]) continue;
        if (count == capacity) return 0;
        if (!vk_descriptor_heap_binding_mapping(&maps[count], set, binding, l->offset[binding],
                                                  l->sampler_offset[binding], l->type[binding])) return 0;
        count++;
    }
    return count;
}
static int allocate(Heap* heap, u32 slot, VkDeviceSize size, VkDeviceSize alignment, u32* offset) {
    VkDeviceSize before = heap->used[slot];
    if (vk_descriptor_heap_allocate(&heap->plan, &heap->used[slot], slot, size, alignment, offset)) {
        g_heaps.stats.arena_bytes += heap->used[slot] - before;
        u64 live = g_heaps.resource.used[slot] + g_heaps.sampler.used[slot];
        if (live > g_heaps.stats.peak_used_bytes) g_heaps.stats.peak_used_bytes = live;
        return 1;
    }
    g_heaps.exhausted[slot] = 1;
    return 0;
}
static ResourceEntry* resource_entry(const u64 key[4], int* hit) {
    u64 hash = vk_hash(key, 4 * sizeof *key);
    u32 index = ((u32)hash & (HEAP_RESOURCE_CACHE / 4 - 1)) * 4;
    ResourceEntry* replacement = &g_heaps.cache.resources[index + (u32)(hash >> 32) % 4];
    for (u32 i = 0; i < 4; i++) {
        ResourceEntry* e = &g_heaps.cache.resources[index + i];
        if (!e->key[0]) { *hit = 0; return e; }
        if (!memcmp(e->key, key, 4 * sizeof *key)) { *hit = 1; return e; }
    }
    /* Replace only CPU metadata. Its former immutable descriptor bytes remain alive in this frame. */
    *hit = 0;
    return replacement;
}
static int resource_descriptor(u32 slot, const VkWriteDescriptorSet* write, u32 element,
                                VkBuffer* previous_buffer, VkDeviceAddress* previous_address, u32* offset) {
    int k = kind(write->descriptorType);
    if (k < 0 || k == HEAP_SAMPLER) return 0;
    int is_buffer = k == HEAP_UBO || k == HEAP_SSBO || k == HEAP_TEXEL;
    if (is_buffer) g_heaps.stats.buffer_requests++;
    else g_heaps.stats.image_requests++;
    g_heaps.stats.payload_bytes += g_heaps.sizes[k];
    u64 key[4] = {(u64)(u32)write->descriptorType + 1, 0, 0, 0};
    VkResourceDescriptorInfoEXT info = {.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
                                       .type = write->descriptorType};
    VkDeviceAddressRangeEXT range = {0};
    VkTexelBufferDescriptorInfoEXT texel = {.sType = VK_STRUCTURE_TYPE_TEXEL_BUFFER_DESCRIPTOR_INFO_EXT};
    VkImageDescriptorInfoEXT image = {.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    VkImageViewCreateInfo view;
    u64 chain[HEAP_CHAIN_BYTES / 8];
    if (k == HEAP_UBO || k == HEAP_SSBO || k == HEAP_TEXEL) {
        VkBuffer buffer;
        VkDeviceSize start, size;
        if (k == HEAP_TEXEL) {
            const VkBufferViewCreateInfo* v = write->pNext;
            if (!v || v->sType != VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO || write->descriptorCount != 1) return 0;
            buffer = v->buffer; start = v->offset; size = v->range;
            texel.format = v->format;
            key[0] |= (u64)(u32)v->format << 32;
        } else {
            if (!write->pBufferInfo) return 0;
            buffer = write->pBufferInfo[element].buffer;
            start = write->pBufferInfo[element].offset;
            size = write->pBufferInfo[element].range;
        }
        if (!buffer || !size || size == VK_WHOLE_SIZE) return 0;
        key[1] = handle_key(buffer); key[2] = start; key[3] = size;
        int hit;
        ResourceEntry* entry = resource_entry(key, &hit);
        if (hit) { *offset = entry->offset; g_heaps.stats.buffer_hits++; return 1; }
        if (*previous_buffer != buffer) { *previous_buffer = buffer; *previous_address = buffer_address(buffer); }
        if (!*previous_address || start > UINT64_MAX - *previous_address) return 0;
        range = (VkDeviceAddressRangeEXT){*previous_address + start, size};
        if (k == HEAP_TEXEL) { texel.addressRange = range; info.data.pTexelBuffer = &texel; }
        else info.data.pAddressRange = &range;
        g_heaps.stats.buffer_driver_calls++;
        if (!VK_OK(vkWriteResourceDescriptorsEXT(g_vk.dev, 1, &info,
                &(VkHostAddressRangeEXT){g_heaps.scratch, (size_t)g_heaps.sizes[k]})) ||
            !allocate(&g_heaps.resource, slot, g_heaps.sizes[k], g_heaps.alignment[k], offset)) return 0;
        memcpy(g_heaps.resource.mapped + *offset, g_heaps.scratch, (size_t)g_heaps.sizes[k]);
        if (entry) {
            g_heaps.stats.buffer_evictions += entry->key[0] != 0;
            memcpy(entry->key, key, sizeof entry->key); entry->offset = *offset;
        }
    } else {
        if (!write->pImageInfo) return 0;
        const VkDescriptorImageInfo* in = &write->pImageInfo[element];
        if (!in->imageView) return 0;
#if VK_USE_64_BIT_PTR_DEFINES
        key[1] = (u64)(uintptr_t)in->imageView;
#else
        key[1] = (u64)in->imageView;
#endif
        key[2] = (u32)in->imageLayout;
        int hit;
        ResourceEntry* entry = resource_entry(key, &hit);
        if (hit) { *offset = entry->offset; g_heaps.stats.image_hits++; return 1; }
        if (!vk_descriptor_view_info(in->imageView, &view, chain, sizeof chain)) return 0;
        image.pView = &view; image.layout = in->imageLayout;
        info.type = k == HEAP_IMAGE ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        info.data.pImage = &image;
        g_heaps.stats.image_driver_calls++;
        if (!VK_OK(vkWriteResourceDescriptorsEXT(g_vk.dev, 1, &info,
                &(VkHostAddressRangeEXT){g_heaps.scratch, (size_t)g_heaps.sizes[k]})) ||
            !allocate(&g_heaps.resource, slot, g_heaps.sizes[k], g_heaps.alignment[k], offset)) return 0;
        memcpy(g_heaps.resource.mapped + *offset, g_heaps.scratch, (size_t)g_heaps.sizes[k]);
        if (entry) {
            g_heaps.stats.image_evictions += entry->key[0] != 0;
            memcpy(entry->key, key, sizeof entry->key); entry->offset = *offset;
        }
    }
    g_heaps.stats.resource_bytes += g_heaps.sizes[k];
    return 1;
}
static int sampler_descriptor(u32 slot, VkSampler sampler, u32* offset) {
    if (!sampler) return 0;
    g_heaps.stats.sampler_requests++;
    g_heaps.stats.payload_bytes += g_heaps.sizes[HEAP_SAMPLER];
#if VK_USE_64_BIT_PTR_DEFINES
    u64 handle = (u64)(uintptr_t)sampler;
#else
    u64 handle = (u64)sampler;
#endif
    SamplerHandle* shortcut = &g_heaps.cache.handles[(handle ^ (handle >> 16)) & (HEAP_SAMPLER_HANDLES - 1)];
    if (shortcut->sampler == sampler) { *offset = shortcut->offset; g_heaps.stats.sampler_hits++; return 1; }
    VkSamplerCreateInfo create;
    u64 chain[HEAP_CHAIN_BYTES / 8];
    if (!vk_descriptor_sampler_info(sampler, &create, chain, sizeof chain)) return 0;
    g_heaps.stats.sampler_driver_calls++;
    if (!VK_OK(vkWriteSamplerDescriptorsEXT(g_vk.dev, 1, &create,
            &(VkHostAddressRangeEXT){g_heaps.scratch, (size_t)g_heaps.sizes[HEAP_SAMPLER]}))) return 0;
    u64 hash[2];
    vk_hash128(g_heaps.scratch, g_heaps.sizes[HEAP_SAMPLER], hash);
    u32 index = ((u32)hash[0] & (HEAP_SAMPLER_CACHE / 4 - 1)) * 4;
    SamplerEntry* replacement = &g_heaps.cache.samplers[index + (u32)(hash[1] >> 32) % 4];
    size_t shadow_base = (size_t)slot * (size_t)g_heaps.sampler.plan.frame_size;
    for (u32 i = 0; i < 4; i++) {
        SamplerEntry* entry = &g_heaps.cache.samplers[index + i];
        if (!entry->used) { replacement = entry; break; }
        if (entry->hash[0] == hash[0] && entry->hash[1] == hash[1] &&
            !memcmp(g_heaps.sampler_bytes + ((size_t)entry->offset - shadow_base), g_heaps.scratch,
                    (size_t)g_heaps.sizes[HEAP_SAMPLER])) {
            *offset = entry->offset;
            *shortcut = (SamplerHandle){sampler, *offset};
            g_heaps.stats.sampler_hits++;
            return 1;
        }
    }
    if (!allocate(&g_heaps.sampler, slot, g_heaps.sizes[HEAP_SAMPLER], g_heaps.alignment[HEAP_SAMPLER], offset)) return 0;
    memcpy(g_heaps.sampler.mapped + *offset, g_heaps.scratch, (size_t)g_heaps.sizes[HEAP_SAMPLER]);
    memcpy(g_heaps.sampler_bytes + ((size_t)*offset - shadow_base), g_heaps.scratch,
           (size_t)g_heaps.sizes[HEAP_SAMPLER]);
    g_heaps.stats.sampler_evictions += replacement->used != 0;
    *replacement = (SamplerEntry){{hash[0], hash[1]}, *offset, 1};
    *shortcut = (SamplerHandle){sampler, *offset};
    g_heaps.stats.sampler_bytes += g_heaps.sizes[HEAP_SAMPLER];
    return 1;
}
VkDescriptorSet vk_descriptor_heaps_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes) {
    HeapLayout* l = find_layout(layout);
    if (!l || !g_heaps.ready || (count && !writes)) return VK_NULL_HANDLE;
    VkDeviceSize words = 1;
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        if (w->dstBinding >= SNX_VKB_END || w->descriptorType != l->type[w->dstBinding] ||
            !l->count[w->dstBinding] || w->dstArrayElement > l->count[w->dstBinding] ||
            w->descriptorCount > l->count[w->dstBinding] - w->dstArrayElement) return VK_NULL_HANDLE;
        VkDeviceSize end = l->offset[w->dstBinding] + w->dstArrayElement + w->descriptorCount;
        if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            end = l->sampler_offset[w->dstBinding] + w->dstArrayElement + w->descriptorCount;
        if (end > words) words = end;
    }
    vk_cmd();
    u32 slot = vk_frame_slot(), table;
    /* Recording workers and the GPU retain copied addresses and immutable heap bytes, never this CPU metadata.
       Clear before any lookup, including writes that later fail and fall back to another descriptor backend. */
    if (vk_descriptor_heap_cache_frame(&g_heaps.cache_frame, vk_frame_id()))
        memset(&g_heaps.cache, 0, sizeof g_heaps.cache);
    if (g_heaps.exhausted[slot]) return VK_NULL_HANDLE;
    if (!allocate(&g_heaps.resource, slot, words * 4, 8, &table)) return VK_NULL_HANDLE;
    VkBuffer previous_buffer = VK_NULL_HANDLE;
    VkDeviceAddress previous_address = 0;
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        for (u32 j = 0; j < w->descriptorCount; j++) {
            u32 resource, sampler;
            if (!resource_descriptor(slot, w, j, &previous_buffer, &previous_address, &resource)) return VK_NULL_HANDLE;
            u32 word = l->offset[w->dstBinding] + w->dstArrayElement + j;
            snx_st_le32(g_heaps.resource.mapped + table + 4ull * word, resource);
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                if (!sampler_descriptor(slot, w->pImageInfo[j].sampler, &sampler)) return VK_NULL_HANDLE;
                snx_st_le32(g_heaps.resource.mapped + table + 4ull * (l->sampler_offset[w->dstBinding] + w->dstArrayElement + j), sampler);
            }
        }
    }
    g_heaps.stats.tables++;
    g_heaps.stats.table_bytes += words * 4;
    g_heaps.stats.full_layout_bytes += l->full_size;
    return (VkDescriptorSet)(uintptr_t)(table + 1);
}
void vk_descriptor_heaps_bind(VkCommandBuffer cb, u32 first, u32 count, const VkDescriptorSet* sets) {
    if (!g_heaps.ready || first > 5 || count > 5 - first || !count) return;
    u32 begun = vk_cb_begun();
    if (g_heaps.begun != begun || g_heaps.bound_epoch != g_heaps.epoch) {
        VkBindHeapInfoEXT resource = {.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
            .heapRange = {g_heaps.resource.address, g_heaps.resource.plan.range_size},
            .reservedRangeOffset = g_heaps.resource.plan.reserved_offset, .reservedRangeSize = g_heaps.resource.plan.reserved_size};
        VkBindHeapInfoEXT sampler = {.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
            .heapRange = {g_heaps.sampler.address, g_heaps.sampler.plan.range_size},
            .reservedRangeOffset = g_heaps.sampler.plan.reserved_offset, .reservedRangeSize = g_heaps.sampler.plan.reserved_size};
        rec_CmdBindDescriptorHeapsEXT(cb, &resource, &sampler);
        g_heaps.begun = begun;
        g_heaps.bound_epoch = g_heaps.epoch;
    }
    u32 addresses[10];
    for (u32 i = 0; i < count; i++) {
        u64 offset = (u64)(uintptr_t)sets[i];
        if (!offset || --offset >= g_heaps.resource.plan.reserved_offset) fatal("vulkan: invalid descriptor heap table");
        u64 address = g_heaps.resource.address + offset;
        addresses[2 * i] = (u32)address;
        addresses[2 * i + 1] = (u32)(address >> 32);
    }
    rec_CmdPushDataEXT(cb, VK_DESCRIPTOR_HEAP_PUSH_OFFSET + 8 * first, count * 8, addresses);
}
void vk_descriptor_heaps_reset(u32 slot) {
    if (slot >= VK_FRAMES) return;
    g_heaps.resource.used[slot] = g_heaps.sampler.used[slot] = 0;
    g_heaps.exhausted[slot] = 0;
    if (slot == vk_frame_slot()) vk_descriptor_heap_cache_reset(&g_heaps.cache_frame);
}
int vk_descriptor_heaps_room(u32 slot) {
    if (!g_heaps.ready || slot >= VK_FRAMES) return 1;
    /* Declared descriptor arrays can exceed a small heap even though only a few live elements are written. Keep
       a bounded margin for the next draw; an unusually large live write falls back without a mid-draw submission. */
    if (g_heaps.exhausted[slot]) return 0;
    VkDeviceSize resource_reserve = g_heaps.resource.plan.frame_size / 8;
    VkDeviceSize sampler_reserve = g_heaps.sampler.plan.frame_size / 8;
    if (resource_reserve > 16384) resource_reserve = 16384;
    if (sampler_reserve > 4096) sampler_reserve = 4096;
    return g_heaps.resource.plan.frame_size - g_heaps.resource.used[slot] >= resource_reserve &&
           g_heaps.sampler.plan.frame_size - g_heaps.sampler.used[slot] >= sampler_reserve;
}
void vk_descriptor_heaps_get_stats(VkDescriptorHeapStats* stats) {
    *stats = g_heaps.stats;
    stats->arena_allocated_bytes = g_heaps.resource.allocation_bytes + g_heaps.sampler.allocation_bytes;
    stats->cache_allocated_bytes = sizeof g_heaps.cache + sizeof g_heaps.cache_frame + sizeof g_heaps.exhausted +
                                     g_heaps.scratch_size;
    stats->reserved_bytes = g_heaps.resource.plan.reserved_size + g_heaps.sampler.plan.reserved_size;
    stats->resource_range_bytes = g_heaps.resource.plan.range_size;
    stats->sampler_range_bytes = g_heaps.sampler.plan.range_size;
}
void vk_descriptor_heaps_stats(void) {
    if (!g_heaps.ready) return;
    const VkDescriptorHeapStats *s = &g_heaps.stats, *p = &g_heaps.reported;
    LOG("vulkan: descriptor heaps: %llu tables, %llu index bytes, %llu resource bytes, %llu sampler bytes, %llu reused encodings\n",
        (unsigned long long)(s->tables - p->tables), (unsigned long long)(s->table_bytes - p->table_bytes),
        (unsigned long long)(s->resource_bytes - p->resource_bytes), (unsigned long long)(s->sampler_bytes - p->sampler_bytes),
        (unsigned long long)(s->buffer_hits + s->image_hits + s->sampler_hits - p->buffer_hits - p->image_hits - p->sampler_hits));
    g_heaps.reported = *s;
}
void vk_descriptor_heaps_invalidate(void) { g_heaps.epoch++; g_heaps.active = 0; }
uint64_t vk_descriptor_heaps_epoch(void) { return g_heaps.epoch; }
void vk_descriptor_heaps_pipeline_state(int active) { g_heaps.active = active && g_heaps.ready; }
int vk_descriptor_heaps_active(void) { return g_heaps.active; }
