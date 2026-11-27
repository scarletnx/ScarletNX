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
#include "nx/runtime/platform/tracy.h"
#include <stdlib.h>
#include <string.h>

#define DESCRIPTOR_BYTES (64ull << 20)
/* The tables that last from frame to frame: slots of the largest layout's size in one part of the arena's buffer, past
   the frames' parts, so that a frame binds them at its own address. A slot names the frame that used it last, and is
   written again only when that frame's slot was reset (vk_descriptors_reset: its fence and holds are done). It keeps
   the generations of the objects it names (vk_descriptor_object_gen): a forget of one ends it. In one game's world,
   177k of the 314k tables a 600 presents were the same as a table that an earlier frame wrote */
#define LASTING_BYTES (64ull << 20)
#define LASTING_OBJECTS 8
/* the least that each part of the one buffer takes: below it, each frame keeps a buffer of its own and no table
   lasts */
#define ARENA_LEAST (16ull << 20)
typedef struct {
    u64 h0, h1;
    u64 used_frame; /* the frame (vk_frame_id) that used it last plus 1, 0 for none: frame 0 is a frame */
    u32 used_slot;  /* and that frame's slot */
    u32 n;          /* the objects it names, 0 for an empty slot */
    u32 obj[LASTING_OBJECTS], gen[LASTING_OBJECTS];
} LastingTable;
/* the arenas' memory: the CPU writes the tables into it in order and never reads it (the encodings it copies from
   are in images and buffers, in the CPU's memory). In the GPU's memory where the CPU can map it (DM_WINDOW, where
   that window is all of it: vk_devmem_tables_at): in system memory, each descriptor the shaders read crossed the
   bus, 30% more GPU time in the passes of one game */
#define ARENA_MEMORY (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
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
        DevMem memory;
        u8* mapped;
        VkDeviceAddress address;
        VkDeviceSize used, capacity;
        VkDeviceSize start; /* its part's offset in the buffer that all frames share, 0 with a buffer of its own */
        u64 frame;          /* the frame (vk_frame_id) that wrote into it last */
    } frame[VK_FRAMES];
    u64 done[VK_FRAMES]; /* each slot's frame at its last reset: that frame's work is done */
    /* the tables that last: lasting_n slots of lasting_stride bytes at lasting_at in the shared buffer */
    LastingTable* lasting;
    u32 lasting_n;
    VkDeviceSize lasting_at, lasting_stride;
    u8* lasting_mapped;
    /* each recording stream's (g_vk_slot) command buffer that bound them, and vk_rec_set_binds then. A worker's list
       plays after the renderer thread's commands, both recorded at the same time (vk_descriptors_stream_reset) */
    u32 bound_cb[VK_RC_SLOTS];
    u64 bound_sets[VK_RC_SLOTS];
    VkDeviceAddress table; /* the texture table's own descriptor buffer (vk_descriptors_table), 0 for none */
    VkDescriptorStats stats, reported;
    u8* images;
    size_t image_stride;
    u8* buffers;
    size_t buffer_stride;
} g_descriptors;

VkResult vk_buffer_create(const VkBufferCreateInfo* info, const DevMemWant* allocation, VkBuffer* buffer,
                          DevMem* memory, DevMemInfo* details) {
    VkBufferCreateInfo create = *info;
    if ((g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support || g_vk.has_gbuf_address) &&
        (create.usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)))
        create.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return vk_devmem_buffer(&create, allocation, buffer, memory, details);
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
    if (slot < VK_FRAMES) {
        g_descriptors.frame[slot].used = 0;
        g_descriptors.done[slot] = g_descriptors.frame[slot].frame;
    }
}
int vk_descriptors_room(u32 slot) {
    if (vk_heaps()) return vk_descriptor_heaps_room(slot);
    if (!g_vk.has_descriptor_buffers || slot >= VK_FRAMES || !g_descriptors.frame[slot].buffer) return 1;
    /* a draw writes one table for each of its five stages at most */
    return g_descriptors.frame[slot].capacity - g_descriptors.frame[slot].used >= 5 * g_descriptors.largest;
}
static VkDeviceSize least(VkDeviceSize a, VkDeviceSize b) { return a < b ? a : b; }
/* One buffer for every frame's part and the lasting tables', where the device's limits let one binding reach all of
   it: the smallest of the largest allocation, the binding ranges of resources and samplers, and their address spaces
   (the tables hold combined image samplers), less the texture table's buffer, which vk_table_init made before the
   first draw and which is in both address spaces too. The reported limits go from 128 KiB to 1 TiB, so the parts take their
   size from them: 64 MiB each where it fits, else an equal share. 0: a buffer for each frame, as before */
static int arena_shared(void) {
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT* p = &g_vk.descriptor_props;
    const VkDeviceSize table = vk_table_descriptor_bytes(),
                       spaces = least(p->resourceDescriptorBufferAddressSpaceSize,
                                      p->samplerDescriptorBufferAddressSpaceSize);
    VkDeviceSize limit = least(least(g_vk.alloc_cap, p->maxResourceDescriptorBufferRange),
                               least(p->maxSamplerDescriptorBufferRange, spaces > table ? spaces - table : 0));
    const VkDeviceSize align = p->descriptorBufferOffsetAlignment ? p->descriptorBufferOffsetAlignment : 1;
    VkDeviceSize part = DESCRIPTOR_BYTES, lasting = LASTING_BYTES;
    if (part * VK_FRAMES + lasting > limit) part = lasting = limit / (VK_FRAMES + 1) / align * align;
    const VkDeviceSize stride = snx_align_up(g_descriptors.largest, align);
    if (part < ARENA_LEAST || !stride || lasting / stride < 64) {
        LOG("vulkan: descriptor buffers: a buffer for each frame and no tables that last: the device's limit is %llu "
            "bytes (allocations %llu, binding ranges %llu and %llu, address spaces %llu and %llu)\n",
            (unsigned long long)limit, (unsigned long long)g_vk.alloc_cap,
            (unsigned long long)p->maxResourceDescriptorBufferRange,
            (unsigned long long)p->maxSamplerDescriptorBufferRange,
            (unsigned long long)p->resourceDescriptorBufferAddressSpaceSize,
            (unsigned long long)p->samplerDescriptorBufferAddressSpaceSize);
        return 0;
    }
    const u32 n = (u32)least(lasting / stride, UINT32_MAX - 1) & ~1u; /* two ways for each hash */
    LastingTable* t = calloc(n, sizeof *t);
    if (!t) return 0;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = part * VK_FRAMES + lasting,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant allocation = {.at = vk_devmem_tables_at(), .mapped = 1, .need = ARENA_MEMORY};
    DevMemInfo details;
    VkBuffer buffer;
    DevMem memory;
    if (!VK_OK(vk_devmem_buffer(&info, &allocation, &buffer, &memory, &details))) {
        free(t);
        return 0;
    }
    vk_mem_track(memory, VK_OWN_DESCRIPTORS);
    const VkDeviceAddress at = address(buffer);
    for (u32 s = 0; s < VK_FRAMES; s++) {
        g_descriptors.frame[s].buffer = buffer;
        g_descriptors.frame[s].memory = memory;
        g_descriptors.frame[s].mapped = (u8*)details.ptr + s * part;
        g_descriptors.frame[s].address = at;
        g_descriptors.frame[s].start = s * part;
        g_descriptors.frame[s].capacity = part;
    }
    g_descriptors.lasting = t;
    g_descriptors.lasting_n = n;
    g_descriptors.lasting_at = part * VK_FRAMES;
    g_descriptors.lasting_stride = stride;
    g_descriptors.lasting_mapped = (u8*)details.ptr + part * VK_FRAMES;
    LOG("vulkan: descriptor buffers: one buffer of %u frame parts of %llu bytes and %u tables that last, of %llu bytes "
        "each, under the device's limit of %llu bytes\n", (unsigned)VK_FRAMES, (unsigned long long)part, (unsigned)n,
        (unsigned long long)stride, (unsigned long long)limit);
    return 1;
}
static void arena(u32 slot) {
    if (g_descriptors.frame[slot].buffer) return;
    static int tried;
    if (!tried) {
        tried = 1;
        if (arena_shared()) return;
    }
    VkDeviceSize capacity = DESCRIPTOR_BYTES < g_vk.alloc_cap ? DESCRIPTOR_BYTES : g_vk.alloc_cap;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = capacity,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant allocation = {.at = vk_devmem_tables_at(), .mapped = 1, .need = ARENA_MEMORY};
    DevMemInfo details;
    if (!VK_OK(vk_devmem_buffer(&info, &allocation, &g_descriptors.frame[slot].buffer,
                                &g_descriptors.frame[slot].memory, &details)))
        fatal("vulkan: cannot allocate graphics descriptor memory");
    vk_mem_track(g_descriptors.frame[slot].memory, VK_OWN_DESCRIPTORS);
    g_descriptors.frame[slot].mapped = details.ptr;
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
/* count copies of the encoding, stride bytes apart: each from the encoding, never from destination */
static void image_descriptor(const VkDescriptorGetInfoEXT* info, const VkDescriptorImageInfo* image,
                              size_t size, void* destination, size_t stride, u32 count) {
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
    for (u32 i = 0; i < count; i++) memcpy((u8*)destination + i * stride, entry + 1, size); /* opaque bytes */
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
/* the objects that the writes name, as generation counters (vk_descriptor_object_index): 0 where they are more than a
   lasting table keeps. Samplers last the run (the sampler cache keeps them) */
static u32 lasting_objects(u32 count, const VkWriteDescriptorSet* writes, u32* obj) {
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        const VkWriteDescriptorSet* w = &writes[i];
        const VkBufferViewCreateInfo* view = w->pNext;
        for (u32 j = 0; j < w->descriptorCount; j++) {
            u64 key = 0;
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                if (j) break;
#if VK_USE_64_BIT_PTR_DEFINES
                key = (u64)(uintptr_t)view->buffer;
#else
                key = (u64)view->buffer;
#endif
            } else if (w->pBufferInfo) {
#if VK_USE_64_BIT_PTR_DEFINES
                key = (u64)(uintptr_t)w->pBufferInfo[j].buffer;
#else
                key = (u64)w->pBufferInfo[j].buffer;
#endif
            } else if (w->pImageInfo) {
#if VK_USE_64_BIT_PTR_DEFINES
                key = (u64)(uintptr_t)w->pImageInfo[j].imageView;
#else
                key = (u64)w->pImageInfo[j].imageView;
#endif
            }
            if (!key) continue;
            const u32 o = vk_descriptor_object_index(key);
            u32 k = 0;
            while (k < n && obj[k] != o) k++;
            if (k < n) continue;
            if (n == LASTING_OBJECTS) return 0;
            obj[n++] = o;
        }
    }
    return n ? n : LASTING_OBJECTS + 1; /* none named: a table of null descriptors lasts too */
}
/* whether a lasting slot's last frame is done, so that the slot can take another table */
static int lasting_free(const LastingTable* e) {
    return !e->used_frame ||
           (g_descriptors.done[e->used_slot] + 1 >= e->used_frame && e->used_frame - 1 + VK_FRAMES <= vk_frame_id());
}
static VkDescriptorSet descriptors_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes,
                                         u32 cb_addr, const u64* hash);
VkDescriptorSet vk_descriptors_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes,
                                     u32 cb_addr) {
    return descriptors_write(layout, count, writes, cb_addr, NULL);
}
VkDescriptorSet vk_descriptors_write_lasting(VkDescriptorSetLayout layout, u32 count,
                                             const VkWriteDescriptorSet* writes, u32 cb_addr, const u64 hash[2]) {
    return descriptors_write(layout, count, writes, cb_addr, hash);
}
static VkDescriptorSet descriptors_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes,
                                         u32 cb_addr, const u64* hash) {
    if (vk_heaps()) return vk_descriptor_heaps_write(layout, count, writes, cb_addr);
    SNX_ZONE_V(z, "vk descriptor buffer write", count);
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
    g_descriptors.frame[slot].frame = vk_frame_id();
    VkDeviceSize used = snx_align_up(active, align);
    /* a table that lasts: bound again where an earlier frame wrote it and no object it names was forgotten since,
       else written into a free slot of the two for its hash, else into the frame's part */
    LastingTable* keep = NULL;
    u32 obj[LASTING_OBJECTS] = {0}, nobj = 0;
    if (hash && g_descriptors.lasting && size <= g_descriptors.lasting_stride &&
        (nobj = lasting_objects(count, writes, obj))) {
        const u32 way = (u32)(hash[0] >> 20) % (g_descriptors.lasting_n / 2) * 2;
        for (u32 k = 0; k < 2 && !keep; k++) {
            LastingTable* e = &g_descriptors.lasting[way + k];
            if (!e->n || e->h0 != hash[0] || e->h1 != hash[1]) continue;
            const u32 m = e->n > LASTING_OBJECTS ? 0 : e->n; /* LASTING_OBJECTS + 1: it names none */
            u32 j = 0;
            while (j < m && vk_descriptor_object_gen(e->obj[j]) == e->gen[j]) j++;
            if (j < m) {
                e->n = 0; /* an object it names was forgotten: the slot is free once its last frame is done */
                g_descriptors.stats.lasting_stale++;
                continue;
            }
            e->used_frame = vk_frame_id() + 1;
            e->used_slot = slot;
            g_descriptors.stats.lasting_hits++;
            return (VkDescriptorSet)(uintptr_t)(g_descriptors.lasting_at +
                                                (way + k) * g_descriptors.lasting_stride + 1);
        }
        LastingTable* a = &g_descriptors.lasting[way];
        LastingTable* b = a + 1;
        keep = lasting_free(a) && (!lasting_free(b) || a->used_frame <= b->used_frame) ? a
               : lasting_free(b)                                                         ? b
                                                                                         : NULL;
        if (!keep) g_descriptors.stats.lasting_full++;
    }
    VkDeviceSize base;
    u8* dest;
    if (keep) {
        const u32 k = (u32)(keep - g_descriptors.lasting);
        *keep = (LastingTable){
            .h0 = hash[0], .h1 = hash[1], .used_frame = vk_frame_id() + 1, .used_slot = slot, .n = nobj};
        for (u32 j = 0; j < nobj && j < LASTING_OBJECTS; j++) {
            keep->obj[j] = obj[j];
            keep->gen[j] = vk_descriptor_object_gen(obj[j]);
        }
        base = g_descriptors.lasting_at + k * g_descriptors.lasting_stride;
        dest = g_descriptors.lasting_mapped + k * g_descriptors.lasting_stride;
        g_descriptors.stats.lasting_made++;
    } else {
        base = g_descriptors.frame[slot].start + g_descriptors.frame[slot].used;
        dest = g_descriptors.frame[slot].mapped + g_descriptors.frame[slot].used;
        /* Every binding remains in bounds at base+size. Only inactive tails overlap later tables; descriptors the
           shader can access are all in the written prefix, which never changes before this frame retires. */
        g_descriptors.frame[slot].used += used;
    }
    g_descriptors.stats.tables++;
    g_descriptors.stats.full_layout_bytes += size;
    g_descriptors.stats.arena_bytes += used;
    if (g_descriptors.stats.peak_used_bytes < g_descriptors.frame[slot].used)
        g_descriptors.stats.peak_used_bytes = g_descriptors.frame[slot].used;
    SNX_ZONE_TEXT(z, "%u writes, %llu words into the host descriptor buffer, layout %u", count,
                  (unsigned long long)(active >> 2), index);
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
                if (b->buffer && b->buffer != previous_buffer) {
                    previous_buffer = b->buffer;
                    previous_address = address(b->buffer);
                }
                /* a null buffer (vk_null_descriptors): address 0 and the whole size, nullDescriptor's form */
                buffer.address = b->buffer ? previous_address + b->offset : 0;
                buffer.range = b->buffer ? b->range : VK_WHOLE_SIZE;
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
            void* destination = dest + offset + j * stride;
            u32 run = 1;
            if (w->pImageInfo) {
                const VkDescriptorImageInfo* image = &w->pImageInfo[j];
                while (run < w->descriptorCount - j) {
                    const VkDescriptorImageInfo* next = &w->pImageInfo[j + run];
                    if (image->sampler != next->sampler || image->imageView != next->imageView ||
                        image->imageLayout != next->imageLayout) break;
                    run++;
                }
                image_descriptor(&info, image, stride, destination, stride, run);
                g_descriptors.stats.image_replicas += run - 1;
            } else buffer_descriptor(&info, &buffer, identity, buffer_offset, stride, destination);
            j += run;
        }
    }
    return (VkDescriptorSet)(uintptr_t)(base + 1);
}
void vk_descriptors_invalidate(void) { g_descriptors.bound_cb[g_vk_slot] = 0; }
void vk_descriptors_stream_reset(u32 slot) {
    if (slot < VK_RC_SLOTS) g_descriptors.bound_cb[slot] = 0;
    if (g_vk.has_descriptor_heap_support) vk_descriptor_heaps_stream_reset(slot);
}
/* the frame's arena (and the texture table's buffer) bound in this command buffer, and no descriptor set bound since:
   a compute pass's (a copy's, a conversion's) between two draws unbinds them, and the second draw's offsets went
   unbound */
static void buffers_bound(VkCommandBuffer cb) {
    const u32 begun = vk_cb_begun();
    const u64 binds = vk_rec_set_binds();
    if (g_descriptors.bound_cb[g_vk_slot] != begun || g_descriptors.bound_sets[g_vk_slot] != binds) {
        rec_CmdBindDescriptorBuffersEXT(cb, g_descriptors.frame[vk_frame_slot()].address, g_descriptors.table);
        if (g_vk.has_descriptor_heap_support) vk_descriptor_heaps_buffer_bound(begun);
        g_descriptors.bound_cb[g_vk_slot] = begun;
        g_descriptors.bound_sets[g_vk_slot] = binds;
    }
}
void vk_descriptors_table(VkDeviceAddress table) { g_descriptors.table = table; }
void vk_descriptors_bind_table(VkCommandBuffer cb, VkPipelineLayout layout, u32 at) {
    buffers_bound(cb);
    const VkDeviceSize zero = 0;
    rec_CmdSetDescriptorBufferOffsetsBP(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, at, 1, &zero, 1);
}
void vk_descriptors_bind(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, u32 count, const VkDescriptorSet* sets,
                         u32 ndyn, const u32* dyn) {
    if (vk_heaps()) {
        vk_descriptors_invalidate();
        vk_descriptor_heaps_bind(cb, first, count, sets);
        return;
    }
    if (!g_vk.has_descriptor_buffers) {
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, count, sets, ndyn, dyn);
        return;
    }
    if (ndyn) fatal("vulkan: dynamic descriptors with descriptor buffers or heaps");
    if (count > 5) fatal("vulkan: too many graphics descriptor stages");
    buffers_bound(cb);
    VkDeviceSize offsets[5];
    for (u32 i = 0; i < count; i++) offsets[i] = (u64)(uintptr_t)sets[i] - 1;
    rec_CmdSetDescriptorBufferOffsetsEXT(cb, layout, first, count, offsets);
}

/* a compute dispatch's set, at the compute bind point: the graphics draws' sets bind at the graphics one. The heap
   path is the draws' (a heap's bind is the graphics pipeline's), so the compute-task path asks for sets or buffers */
void vk_descriptors_bind_compute(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, VkDescriptorSet set) {
    if (!g_vk.has_descriptor_buffers) {
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, first, 1, &set, 0, NULL);
        return;
    }
    /* the buffers are the bind points' together: bound again only where they are not, as a bind of them disturbs
       the graphics offsets, the texture table's too, which the draws set only when the table changes (render.c) */
    buffers_bound(cb);
    VkDeviceSize offset = (u64)(uintptr_t)set - 1;
    rec_CmdSetDescriptorBufferOffsetsBP(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, first, 1, &offset, 0);
}
void vk_descriptors_stats(void) {
    if (vk_heaps()) {
        vk_descriptor_heaps_stats();
        return;
    }
    if (!g_vk.has_descriptor_buffers) return;
    const VkDescriptorStats* s = &g_descriptors.stats;
    const VkDescriptorStats* p = &g_descriptors.reported;
    LOG("vulkan: %llu graphics descriptor tables, %llu MB arena cursor (%llu MB full layouts), %llu buffer encodings "
        "made, %llu reused, %llu image encodings made, %llu reused, %llu repeated image elements; tables that last: "
        "%llu bound again, %llu written, %llu with a forgotten object, %llu with no free slot\n",
        (unsigned long long)(s->tables - p->tables), (unsigned long long)((s->arena_bytes - p->arena_bytes) >> 20),
        (unsigned long long)((s->full_layout_bytes - p->full_layout_bytes) >> 20),
        (unsigned long long)(s->buffer_driver_calls - p->buffer_driver_calls),
        (unsigned long long)(s->buffer_hits - p->buffer_hits),
        (unsigned long long)(s->image_driver_calls - p->image_driver_calls),
        (unsigned long long)(s->image_hits - p->image_hits),
        (unsigned long long)(s->image_replicas - p->image_replicas),
        (unsigned long long)(s->lasting_hits - p->lasting_hits),
        (unsigned long long)(s->lasting_made - p->lasting_made),
        (unsigned long long)(s->lasting_stale - p->lasting_stale),
        (unsigned long long)(s->lasting_full - p->lasting_full));
    g_descriptors.reported = *s;
}
void vk_descriptors_get_stats(VkDescriptorStats* stats) {
    if (vk_heaps()) {
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
    stats->arena_allocated_bytes += (u64)g_descriptors.lasting_n * g_descriptors.lasting_stride;
    stats->cache_allocated_bytes = IMAGE_ENCODINGS * g_descriptors.image_stride +
                                  2 * BUFFER_ENCODING_BUCKETS * g_descriptors.buffer_stride;
}
