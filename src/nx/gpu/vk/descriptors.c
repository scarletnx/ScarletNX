/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN texture and sampler pool registration writes indexed descriptor memory. The Vulkan path uses the public
   descriptor-buffer API to encode its own descriptors and keeps each version until its consumers finish.
   The graphics descriptor layouts live in addressable buffers. Each frame slot owns its arena until the graphics
   fence and all holds complete. Only then can cmd.c reset its cursor. A set is an internal offset plus one, passed
   only to vk_descriptors_bind, never to a Vulkan descriptor-set command. */
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/descriptor_encoding.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/ledger.h"
#include "common/utilities/bits.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

#define DESCRIPTOR_BYTES (64ull << 20)
/* the arenas' memory: the CPU writes the tables into it, and cached memory keeps the reads of image encodings cheap */
#define ARENA_MEMORY                                                                                                   \
    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
#define IMAGE_ENCODINGS 4096
#define BUFFER_ENCODING_BUCKETS 1024
/* Handles cannot be recycled within a frame: image views are deferred until its fence, and the sampler cache
   retains its objects. A new frame invalidates the encoding shortcut before any recycled handle can match it. */
typedef struct {
    VkImageView view;
    VkSampler sampler;
    u64 frame;
    VkImageLayout layout;
    VkDescriptorType type;
} ImageEncoding;
static struct {
    struct {
        VkDescriptorSetLayout layout;
        VkDeviceSize size;
        VkDeviceSize offset[SNX_VKB_END];
        u32 count[SNX_VKB_END];
        VkDescriptorType type[SNX_VKB_END];
    } layouts[10];
    u32 nlayouts;
    VkDeviceSize largest; /* the largest layout's size, aligned: what one stage of a draw takes at most */
    struct {
        VkBuffer buffer;
        VmaAllocation memory;
        u8* mapped;
        VkDeviceAddress address;
        VkDeviceSize used, capacity;
    } frame[VK_FRAMES];
    u32 bound_cb;
    u64 bound_sets; /* vk_rec_set_binds when they were bound */
    VkDescriptorStats stats, reported;
    u8* images;
    size_t image_stride;
    u8* buffers;
    size_t buffer_stride;
} g_descriptors;

VkResult vk_buffer_create(VmaAllocator allocator, const VkBufferCreateInfo* info,
                           const VmaAllocationCreateInfo* allocation, VkBuffer* buffer,
                           VmaAllocation* memory, VmaAllocationInfo* details) {
    VkBufferCreateInfo create = *info;
    if ((g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support) &&
        (create.usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)))
        create.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return vmaCreateBuffer(allocator, &create, allocation, buffer, memory, details);
}
int vk_descriptors_memory(VkPhysicalDevice phys) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; i++)
        if ((mp.memoryTypes[i].propertyFlags & ARENA_MEMORY) == ARENA_MEMORY) return 1;
    return 0;
}
static VkDeviceAddress address(VkBuffer buffer) {
    if (!buffer) return 0;
    g_descriptors.stats.address_queries++;
    VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    return vkGetBufferDeviceAddress(g_vk.dev, &info);
}
void vk_descriptors_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding* bindings, u32 count) {
    if (g_vk.has_descriptor_heap_support) vk_descriptor_heaps_layout(layout, bindings, count);
    if (!g_vk.has_descriptor_buffers) return;
    if (g_descriptors.nlayouts == 10) fatal("vulkan: too many graphics descriptor layouts");
    u32 slot = g_descriptors.nlayouts++;
    g_descriptors.layouts[slot].layout = layout;
    vkGetDescriptorSetLayoutSizeEXT(g_vk.dev, layout, &g_descriptors.layouts[slot].size);
    VkDeviceSize aligned =
        snx_align_up(g_descriptors.layouts[slot].size, g_vk.descriptor_props.descriptorBufferOffsetAlignment);
    if (aligned > g_descriptors.largest) g_descriptors.largest = aligned;
    for (u32 i = 0; i < count; i++) {
        u32 binding = bindings[i].binding;
        if (binding >= SNX_VKB_END) fatal("vulkan: invalid graphics descriptor binding");
        g_descriptors.layouts[slot].count[binding] = bindings[i].descriptorCount;
        g_descriptors.layouts[slot].type[binding] = bindings[i].descriptorType;
        vkGetDescriptorSetLayoutBindingOffsetEXT(g_vk.dev, layout, binding, &g_descriptors.layouts[slot].offset[binding]);
    }
}
void vk_descriptors_reset(u32 slot) {
    if (g_vk.has_descriptor_heap_support) vk_descriptor_heaps_reset(slot);
    if (slot < VK_FRAMES) g_descriptors.frame[slot].used = 0;
}
int vk_descriptors_room(u32 slot) {
    if (g_vk.has_descriptor_heaps) return vk_descriptor_heaps_room(slot);
    if (!g_vk.has_descriptor_buffers || slot >= VK_FRAMES || !g_descriptors.frame[slot].buffer) return 1;
    /* a draw writes one table for each of its five stages at most */
    return g_descriptors.frame[slot].capacity - g_descriptors.frame[slot].used >= 5 * g_descriptors.largest;
}
static void arena(u32 slot) {
    if (g_descriptors.frame[slot].buffer) return;
    VkDeviceSize capacity = DESCRIPTOR_BYTES < g_vk.alloc_cap ? DESCRIPTOR_BYTES : g_vk.alloc_cap;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = capacity,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo allocation = {.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = ARENA_MEMORY};
    VmaAllocationInfo details;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &info, &allocation, &g_descriptors.frame[slot].buffer,
                              &g_descriptors.frame[slot].memory, &details)))
        fatal("vulkan: cannot allocate graphics descriptor memory");
    vk_mem_track(g_descriptors.frame[slot].memory, VK_OWN_DESCRIPTORS);
    g_descriptors.frame[slot].mapped = details.pMappedData;
    g_descriptors.frame[slot].address = address(g_descriptors.frame[slot].buffer);
    g_descriptors.frame[slot].capacity = capacity;
}
static size_t descriptor_size(VkDescriptorType type) {
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT* p = &g_vk.descriptor_props;
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return g_vk.feat.robustBufferAccess ? p->robustUniformBufferDescriptorSize : p->uniformBufferDescriptorSize;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return g_vk.feat.robustBufferAccess ? p->robustStorageBufferDescriptorSize : p->storageBufferDescriptorSize;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return p->combinedImageSamplerDescriptorSize;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return p->storageImageDescriptorSize;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return g_vk.feat.robustBufferAccess ? p->robustStorageTexelBufferDescriptorSize
                                                : p->storageTexelBufferDescriptorSize;
        default: fatal("vulkan: unsupported graphics descriptor type %u", (u32)type); return 0;
    }
}
static void image_descriptor(const VkDescriptorGetInfoEXT* info, const VkDescriptorImageInfo* image,
                              size_t size, void* destination) {
    if (!g_descriptors.images) {
        size_t biggest = g_vk.descriptor_props.combinedImageSamplerDescriptorSize;
        if (g_vk.descriptor_props.storageImageDescriptorSize > biggest)
            biggest = g_vk.descriptor_props.storageImageDescriptorSize;
        if (biggest > SIZE_MAX - 2 * sizeof(ImageEncoding)) fatal("vulkan: descriptor encoding size overflow");
        g_descriptors.image_stride = (size_t)snx_align_up(sizeof(ImageEncoding) + biggest, sizeof(ImageEncoding));
        if (g_descriptors.image_stride > SIZE_MAX / IMAGE_ENCODINGS) fatal("vulkan: descriptor cache size overflow");
        g_descriptors.images = calloc(IMAGE_ENCODINGS, g_descriptors.image_stride);
        if (!g_descriptors.images) fatal("vulkan: cannot allocate image descriptor encodings");
    }
    u64 hash = snx_fnv_step(SNX_FNV_BASIS, (u64)(uintptr_t)image->imageView);
    hash = snx_fnv_step(hash, (u64)(uintptr_t)image->sampler);
    hash = snx_fnv_step(hash, (u64)image->imageLayout | (u64)(u32)info->type << 32);
    ImageEncoding* entry = (ImageEncoding*)(void*)(g_descriptors.images +
        ((hash ^ (hash >> 32)) & (IMAGE_ENCODINGS - 1)) * g_descriptors.image_stride);
    u64 frame = vk_frame_id() + 1;
    if (entry->frame != frame || entry->view != image->imageView || entry->sampler != image->sampler ||
        entry->layout != image->imageLayout || entry->type != info->type) {
        if (entry->frame == frame) g_descriptors.stats.image_evictions++;
        vkGetDescriptorEXT(g_vk.dev, info, size, entry + 1);
        g_descriptors.stats.image_driver_calls++;
        *entry = (ImageEncoding){image->imageView, image->sampler, frame, image->imageLayout, info->type};
    } else g_descriptors.stats.image_hits++;
    memcpy(destination, entry + 1, size); /* opaque descriptor bytes, in host-cached memory */
}
static void buffer_descriptor(const VkDescriptorGetInfoEXT* info, const VkDescriptorAddressInfoEXT* buffer,
                                VkBuffer identity, u64 offset, size_t size, void* destination) {
    if (!g_descriptors.buffers) {
        size_t biggest = descriptor_size(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        size_t storage = descriptor_size(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        size_t texel = descriptor_size(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
        if (biggest < storage) biggest = storage;
        if (biggest < texel) biggest = texel;
        if (biggest > SIZE_MAX - sizeof(VkBufferEncodingKey) - 8) fatal("vulkan: buffer encoding size overflow");
        g_descriptors.buffer_stride = (size_t)snx_align_up(sizeof(VkBufferEncodingKey) + biggest, 8);
        if (g_descriptors.buffer_stride > SIZE_MAX / (2 * BUFFER_ENCODING_BUCKETS))
            fatal("vulkan: buffer descriptor cache size overflow");
        g_descriptors.buffers = calloc(2 * BUFFER_ENCODING_BUCKETS, g_descriptors.buffer_stride);
        if (!g_descriptors.buffers) fatal("vulkan: cannot allocate buffer descriptor encodings");
    }
    u64 buffer_identity;
#if VK_USE_64_BIT_PTR_DEFINES
    buffer_identity = (u64)(uintptr_t)identity;
#else
    buffer_identity = (u64)identity;
#endif
    VkBufferEncodingKey key = {.frame = vk_frame_id() + 1, .device = (u64)(uintptr_t)g_vk.dev,
                                .buffer = buffer_identity, .address = buffer->address, .offset = offset,
                                .range = buffer->range, .format = (u32)buffer->format,
                                .type = (u32)info->type, .bytes = (u32)size};
    int hit, evicted;
    void* encoding = vk_buffer_encoding_slot(g_descriptors.buffers, g_descriptors.buffer_stride,
                                              BUFFER_ENCODING_BUCKETS, &key, &hit, &evicted);
    g_descriptors.stats.buffer_requests++;
    if (hit) g_descriptors.stats.buffer_hits++;
    else {
        vkGetDescriptorEXT(g_vk.dev, info, size, encoding);
        g_descriptors.stats.buffer_driver_calls++;
        g_descriptors.stats.buffer_evictions += evicted;
    }
    memcpy(destination, encoding, size);
}
VkDescriptorSet vk_descriptors_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes) {
    if (g_vk.has_descriptor_heaps) return vk_descriptor_heaps_write(layout, count, writes);
    u32 index = 0;
    while (index < g_descriptors.nlayouts && g_descriptors.layouts[index].layout != layout) index++;
    if (index == g_descriptors.nlayouts) fatal("vulkan: unknown graphics descriptor layout");
    if (count > SNX_VKB_END) fatal("vulkan: too many graphics descriptor writes");
    u64 offsets[SNX_VKB_END], active = 0;
    size_t strides[SNX_VKB_END];
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        int texel = w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        const VkBufferViewCreateInfo* view = w->pNext;
        if (texel != !!view ||
            (texel && (w->descriptorCount != 1 || view->sType != VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO)) ||
            w->dstBinding >= SNX_VKB_END || !w->descriptorCount)
            fatal("vulkan: invalid graphics descriptor write");
        u32 binding = w->dstBinding;
        strides[i] = descriptor_size(w->descriptorType);
        u64 end = 0;
        if (g_descriptors.layouts[index].type[binding] != w->descriptorType ||
            !vk_descriptor_active_end(g_descriptors.layouts[index].size, g_descriptors.layouts[index].offset[binding],
                                       g_descriptors.layouts[index].count[binding], w->dstArrayElement,
                                       w->descriptorCount, strides[i], &end))
            fatal("vulkan: graphics descriptor write exceeds its binding");
        offsets[i] = end - (u64)w->descriptorCount * strides[i];
        if (end > active) active = end;
    }
    vk_cmd();
    u32 slot = vk_frame_slot();
    arena(slot);
    VkDeviceSize align = g_vk.descriptor_props.descriptorBufferOffsetAlignment;
    VkDeviceSize size = snx_align_up(g_descriptors.layouts[index].size, align);
    if (size > g_descriptors.frame[slot].capacity) fatal("vulkan: graphics descriptor layout exceeds the arena");
    if (size > g_descriptors.frame[slot].capacity - g_descriptors.frame[slot].used) {
        /* vk_frame_check advances the frame before a draw's setup comes this far (vk_descriptors_room). Here the
           submission changes under the setup, which then starts again (render.c) */
        vk_frame_next();
        g_descriptors.stats.frame_advances++;
        vk_cmd();
        slot = vk_frame_slot();
        arena(slot);
    }
    VkDeviceSize base = g_descriptors.frame[slot].used;
    VkDeviceSize used = snx_align_up(active, align);
    /* Every binding remains in bounds at base+size. Only inactive tails overlap later tables; descriptors the
       shader can access are all in the written prefix, which never changes before this frame retires. */
    g_descriptors.frame[slot].used += used;
    g_descriptors.stats.tables++;
    g_descriptors.stats.full_layout_bytes += size;
    g_descriptors.stats.arena_bytes += used;
    if (g_descriptors.stats.peak_used_bytes < g_descriptors.frame[slot].used)
        g_descriptors.stats.peak_used_bytes = g_descriptors.frame[slot].used;
    /* Constant buffers usually share one staging buffer. Its address is fixed throughout these writes, so ask the
       driver only when the buffer changes. Keep this local: a later call may use a recycled Vulkan handle. */
    VkBuffer previous_buffer = VK_NULL_HANDLE;
    VkDeviceAddress previous_address = 0;
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        int texel = w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        const VkBufferViewCreateInfo* view = w->pNext;
        size_t stride = strides[i];
        VkDeviceSize offset = offsets[i];
        g_descriptors.stats.payload_bytes += (u64)w->descriptorCount * stride;
        if (w->pImageInfo) g_descriptors.stats.image_requests += w->descriptorCount;
        for (u32 j = 0; j < w->descriptorCount;) {
            VkDescriptorGetInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT, .type = w->descriptorType};
            VkDescriptorAddressInfoEXT buffer = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
            VkBuffer identity = VK_NULL_HANDLE;
            u64 buffer_offset = 0;
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                const VkDescriptorBufferInfo* b = &w->pBufferInfo[j];
                if (b->buffer != previous_buffer) {
                    previous_buffer = b->buffer;
                    previous_address = address(b->buffer);
                }
                buffer.address = previous_address + b->offset;
                buffer.range = b->range;
                identity = b->buffer;
                buffer_offset = b->offset;
                if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) info.data.pUniformBuffer = &buffer;
                else info.data.pStorageBuffer = &buffer;
            } else if (texel) {
                if (view->buffer != previous_buffer) {
                    previous_buffer = view->buffer;
                    previous_address = address(view->buffer);
                }
                buffer.address = previous_address + view->offset;
                buffer.range = view->range;
                buffer.format = view->format;
                identity = view->buffer;
                buffer_offset = view->offset;
                info.data.pStorageTexelBuffer = &buffer;
            } else if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                info.data.pCombinedImageSampler = &w->pImageInfo[j];
            else info.data.pStorageImage = &w->pImageInfo[j];
            void* destination = g_descriptors.frame[slot].mapped + base + offset + j * stride;
            u32 run = 1;
            if (w->pImageInfo) {
                const VkDescriptorImageInfo* image = &w->pImageInfo[j];
                while (run < w->descriptorCount - j) {
                    const VkDescriptorImageInfo* next = &w->pImageInfo[j + run];
                    if (image->sampler != next->sampler || image->imageView != next->imageView ||
                        image->imageLayout != next->imageLayout) break;
                    run++;
                }
                image_descriptor(&info, image, stride, destination);
                vk_descriptor_repeat(destination, stride, run);
                g_descriptors.stats.image_replicas += run - 1;
            } else buffer_descriptor(&info, &buffer, identity, buffer_offset, stride, destination);
            j += run;
        }
    }
    return (VkDescriptorSet)(uintptr_t)(base + 1);
}
void vk_descriptors_invalidate(void) { g_descriptors.bound_cb = 0; }
void vk_descriptors_bind(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, u32 count, const VkDescriptorSet* sets) {
    if (g_vk.has_descriptor_heaps) {
        vk_descriptors_invalidate();
        vk_descriptor_heaps_bind(cb, first, count, sets);
        return;
    }
    if (!g_vk.has_descriptor_buffers) {
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, count, sets, 0, NULL);
        return;
    }
    if (count > 5) fatal("vulkan: too many graphics descriptor stages");
    u32 begun = vk_cb_begun();
    u64 binds = vk_rec_set_binds();
    /* in this command buffer, and no descriptor set bound since: a compute pass's (a copy's, a conversion's) between
       two draws unbinds them, and the second draw's offsets went unbound */
    if (g_descriptors.bound_cb != begun || g_descriptors.bound_sets != binds) {
        rec_CmdBindDescriptorBuffersEXT(cb, g_descriptors.frame[vk_frame_slot()].address);
        if (g_vk.has_descriptor_heap_support) vk_descriptor_heaps_buffer_bound(begun);
        g_descriptors.bound_cb = begun;
        g_descriptors.bound_sets = binds;
    }
    VkDeviceSize offsets[5];
    for (u32 i = 0; i < count; i++) offsets[i] = (u64)(uintptr_t)sets[i] - 1;
    rec_CmdSetDescriptorBufferOffsetsEXT(cb, layout, first, count, offsets);
}

void vk_descriptors_stats(void) {
    if (g_vk.has_descriptor_heaps) {
        vk_descriptor_heaps_stats();
        return;
    }
    if (!g_vk.has_descriptor_buffers) return;
    const VkDescriptorStats* s = &g_descriptors.stats;
    const VkDescriptorStats* p = &g_descriptors.reported;
    LOG("vulkan: %llu graphics descriptor tables, %llu MB arena cursor (%llu MB full layouts), %llu buffer encodings "
        "made, %llu reused, %llu image encodings made, %llu reused, %llu repeated image elements\n",
        (unsigned long long)(s->tables - p->tables), (unsigned long long)((s->arena_bytes - p->arena_bytes) >> 20),
        (unsigned long long)((s->full_layout_bytes - p->full_layout_bytes) >> 20),
        (unsigned long long)(s->buffer_driver_calls - p->buffer_driver_calls),
        (unsigned long long)(s->buffer_hits - p->buffer_hits),
        (unsigned long long)(s->image_driver_calls - p->image_driver_calls),
        (unsigned long long)(s->image_hits - p->image_hits),
        (unsigned long long)(s->image_replicas - p->image_replicas));
    g_descriptors.reported = *s;
}
void vk_descriptors_get_stats(VkDescriptorStats* stats) {
    if (g_vk.has_descriptor_heaps) {
        VkDescriptorHeapStats heap;
        vk_descriptor_heaps_get_stats(&heap);
        *stats = (VkDescriptorStats){
            .tables = heap.tables, .full_layout_bytes = heap.full_layout_bytes, .arena_bytes = heap.arena_bytes,
            .payload_bytes = heap.payload_bytes, .buffer_requests = heap.buffer_requests,
            .buffer_driver_calls = heap.buffer_driver_calls, .buffer_hits = heap.buffer_hits,
            .buffer_evictions = heap.buffer_evictions, .image_requests = heap.image_requests,
            .image_driver_calls = heap.image_driver_calls, .image_hits = heap.image_hits,
            .image_evictions = heap.image_evictions, .image_replicas = heap.image_replicas,
            .address_queries = heap.address_queries,
            .peak_used_bytes = heap.peak_used_bytes, .arena_allocated_bytes = heap.arena_allocated_bytes,
            .cache_allocated_bytes = heap.cache_allocated_bytes};
        return;
    }
    *stats = g_descriptors.stats;
    stats->arena_allocated_bytes = 0;
    for (u32 slot = 0; slot < VK_FRAMES; slot++) stats->arena_allocated_bytes += g_descriptors.frame[slot].capacity;
    stats->cache_allocated_bytes = IMAGE_ENCODINGS * g_descriptors.image_stride +
                                  2 * BUFFER_ENCODING_BUCKETS * g_descriptors.buffer_stride;
}
