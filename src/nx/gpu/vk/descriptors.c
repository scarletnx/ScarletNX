/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN texture and sampler pool registration writes indexed descriptor memory. The Vulkan path uses the public
   descriptor-buffer API to encode its own descriptors and keeps each version until its consumers finish.
   The graphics descriptor layouts live in addressable buffers. Each frame slot owns its arena until the graphics
   fence and all holds complete. Only then can cmd.c reset its cursor. A set is an internal offset plus one, passed
   only to vk_descriptors_bind, never to a Vulkan descriptor-set command. */
#include "nx/gpu/vk/descriptors.h"
#include "common/utilities/bits.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

#define DESCRIPTOR_BYTES (64ull << 20)
/* the arenas' memory: the CPU writes the tables into it, and cached memory keeps the reads of image encodings cheap */
#define ARENA_MEMORY                                                                                                   \
    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
#define IMAGE_ENCODINGS 4096
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
    u64 written, bytes;
    u8* images;
    size_t image_stride;
} g_descriptors;

VkResult vk_buffer_create(VmaAllocator allocator, const VkBufferCreateInfo* info,
                           const VmaAllocationCreateInfo* allocation, VkBuffer* buffer,
                           VmaAllocation* memory, VmaAllocationInfo* details) {
    VkBufferCreateInfo create = *info;
    if (g_vk.has_descriptor_buffers &&
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
    VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    return vkGetBufferDeviceAddress(g_vk.dev, &info);
}
void vk_descriptors_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding* bindings, u32 count) {
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
        vkGetDescriptorSetLayoutBindingOffsetEXT(g_vk.dev, layout, binding, &g_descriptors.layouts[slot].offset[binding]);
    }
}
void vk_descriptors_reset(u32 slot) {
    if (slot < VK_FRAMES) g_descriptors.frame[slot].used = 0;
}
int vk_descriptors_room(u32 slot) {
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
    vk_mem_track(g_descriptors.frame[slot].memory);
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
        vkGetDescriptorEXT(g_vk.dev, info, size, entry + 1);
        *entry = (ImageEncoding){image->imageView, image->sampler, frame, image->imageLayout, info->type};
    }
    memcpy(destination, entry + 1, size); /* opaque descriptor bytes, in host-cached memory */
}
VkDescriptorSet vk_descriptors_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes) {
    u32 index = 0;
    while (index < g_descriptors.nlayouts && g_descriptors.layouts[index].layout != layout) index++;
    if (index == g_descriptors.nlayouts) fatal("vulkan: unknown graphics descriptor layout");
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
        vk_cmd();
        slot = vk_frame_slot();
        arena(slot);
    }
    VkDeviceSize base = g_descriptors.frame[slot].used;
    g_descriptors.frame[slot].used += size;
    g_descriptors.written++;
    g_descriptors.bytes += size;
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        /* a texel buffer's write carries its view's create info, which names the buffer, range and format (render.c) */
        int texel = w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        const VkBufferViewCreateInfo* view = w->pNext;
        if (texel != !!view ||
            (texel && (w->descriptorCount != 1 || view->sType != VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO)) ||
            w->dstBinding >= SNX_VKB_END)
            fatal("vulkan: invalid graphics descriptor write");
        size_t stride = descriptor_size(w->descriptorType);
        VkDeviceSize offset = g_descriptors.layouts[index].offset[w->dstBinding] + (u64)w->dstArrayElement * stride;
        if (offset > size || w->descriptorCount > (size - offset) / stride)
            fatal("vulkan: graphics descriptor write exceeds its layout");
        for (u32 j = 0; j < w->descriptorCount; j++) {
            VkDescriptorGetInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT, .type = w->descriptorType};
            VkDescriptorAddressInfoEXT buffer = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                const VkDescriptorBufferInfo* b = &w->pBufferInfo[j];
                buffer.address = address(b->buffer) + b->offset;
                buffer.range = b->range;
                if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) info.data.pUniformBuffer = &buffer;
                else info.data.pStorageBuffer = &buffer;
            } else if (texel) {
                buffer.address = address(view->buffer) + view->offset;
                buffer.range = view->range;
                buffer.format = view->format;
                info.data.pStorageTexelBuffer = &buffer;
            } else if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                info.data.pCombinedImageSampler = &w->pImageInfo[j];
            else info.data.pStorageImage = &w->pImageInfo[j];
            void* destination = g_descriptors.frame[slot].mapped + base + offset + j * stride;
            if (w->pImageInfo) image_descriptor(&info, &w->pImageInfo[j], stride, destination);
            else vkGetDescriptorEXT(g_vk.dev, &info, stride, destination);
        }
    }
    return (VkDescriptorSet)(uintptr_t)(base + 1);
}
void vk_descriptors_bind(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, u32 count, const VkDescriptorSet* sets) {
    if (!g_vk.has_descriptor_buffers) {
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, count, sets, 0, NULL);
        return;
    }
    if (count > 5) fatal("vulkan: too many graphics descriptor stages");
    u32 begun = vk_cb_begun();
    if (g_descriptors.bound_cb != begun) {
        rec_CmdBindDescriptorBuffersEXT(cb, g_descriptors.frame[vk_frame_slot()].address);
        g_descriptors.bound_cb = begun;
    }
    VkDeviceSize offsets[5];
    for (u32 i = 0; i < count; i++) offsets[i] = (u64)(uintptr_t)sets[i] - 1;
    rec_CmdSetDescriptorBufferOffsetsEXT(cb, layout, first, count, offsets);
}

void vk_descriptors_stats(void) {
    if (!g_vk.has_descriptor_buffers) return;
    LOG("vulkan: %llu graphics descriptor tables written into buffers (%llu MB), no graphics descriptor sets allocated\n",
        (unsigned long long)g_descriptors.written, (unsigned long long)(g_descriptors.bytes >> 20));
    g_descriptors.written = g_descriptors.bytes = 0;
}
