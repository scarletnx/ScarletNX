/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real descriptor encoder with a deterministic Vulkan driver. Repeated staging-buffer descriptors
   need one address query, and each encoded descriptor must still carry its own offset, range and format. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define SNX_VK_H
#define SNX_LOG_H
#define VK_FRAMES                                              2
#define SNX_VKB_END                                            64
#define VK_OK(result)                                          ((result) == VK_SUCCESS)
#define VMA_ALLOCATION_CREATE_MAPPED_BIT                       1
#define VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT 2
#define VMA_MEMORY_USAGE_AUTO_PREFER_HOST                      0
typedef void* VmaAllocator;
typedef void* VmaAllocation;
typedef struct {
    u32 flags;
    int usage;
    VkMemoryPropertyFlags requiredFlags;
} VmaAllocationCreateInfo;
typedef struct {
    void* pMappedData;
} VmaAllocationInfo;
static struct {
    int has_descriptor_buffers;
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_props;
    VkPhysicalDeviceFeatures feat;
    VkDevice dev;
    VmaAllocator vma;
    u64 alloc_cap;
} g_vk;
static struct {
    u32 queries, encoded;
    u64 incarnation;
    VkDeviceAddress address[128];
    VkDeviceSize range[128];
    VkFormat format[128];
} g_test;
VkResult vmaCreateBuffer(VmaAllocator, const VkBufferCreateInfo*, const VmaAllocationCreateInfo*, VkBuffer*,
                         VmaAllocation*, VmaAllocationInfo*);
void vk_mem_track(VmaAllocation);
VkCommandBuffer vk_cmd(void);
u32 vk_frame_slot(void);
u64 vk_frame_id(void);
void vk_frame_next(void);
u32 vk_cb_begun(void);
void fatal(const char*, ...);
void logf_(int, const char*, ...);
void rec_CmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, u32, u32, const VkDescriptorSet*,
                               u32, const u32*);
#define LOG(...) logf_(1, __VA_ARGS__)
#include "nx/gpu/vk/descriptors.c"

#define HANDLE(type, value) ((type)(uintptr_t)(value))
VkResult vmaCreateBuffer(VmaAllocator a, const VkBufferCreateInfo* b, const VmaAllocationCreateInfo* c, VkBuffer* d,
                         VmaAllocation* e, VmaAllocationInfo* f) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    abort();
}
void vk_mem_track(VmaAllocation a) {
    (void)a;
    abort();
}
VkCommandBuffer vk_cmd(void) { return HANDLE(VkCommandBuffer, 1); }
u32 vk_frame_slot(void) { return 0; }
u64 vk_frame_id(void) { return 0; }
void vk_frame_next(void) { abort(); }
u32 vk_cb_begun(void) { return 1; }
void fatal(const char* fmt, ...) {
    (void)fmt;
    abort();
}
void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
void rec_CmdBindDescriptorSets(VkCommandBuffer a, VkPipelineBindPoint b, VkPipelineLayout c, u32 d, u32 e,
                               const VkDescriptorSet* f, u32 g, const u32* h) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    (void)g;
    (void)h;
}
void rec_CmdBindDescriptorBuffersEXT(VkCommandBuffer cb, VkDeviceAddress a) {
    (void)cb;
    (void)a;
}
void rec_CmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, u32 count,
                                          const VkDeviceSize* offsets) {
    (void)cb;
    (void)layout;
    (void)first;
    (void)count;
    (void)offsets;
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    (void)device;
    assert(info->buffer);
    g_test.queries++;
    return (u64)(uintptr_t)info->buffer * 4096 + g_test.incarnation;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice device,
                                                               VkPhysicalDeviceMemoryProperties* props) {
    (void)device;
    *props = (VkPhysicalDeviceMemoryProperties){0};
}
VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSizeEXT(VkDevice device, VkDescriptorSetLayout layout,
                                                           VkDeviceSize* size) {
    (void)device;
    (void)layout;
    *size = 4096;
}
VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutBindingOffsetEXT(VkDevice device, VkDescriptorSetLayout layout,
                                                                    u32 binding, VkDeviceSize* offset) {
    (void)device;
    (void)layout;
    *offset = binding * 64;
}
VKAPI_ATTR void VKAPI_CALL vkGetDescriptorEXT(VkDevice device, const VkDescriptorGetInfoEXT* info, size_t size,
                                              void* destination) {
    (void)device;
    assert(g_test.encoded < 128);
    const VkDescriptorAddressInfoEXT* a = info->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? info->data.pUniformBuffer
                                          : info->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                              ? info->data.pStorageBuffer
                                              : info->data.pStorageTexelBuffer;
    g_test.address[g_test.encoded] = a->address;
    g_test.range[g_test.encoded] = a->range;
    g_test.format[g_test.encoded++] = a->format;
    memcpy(destination, &a->address, size);
}

static void encode(const VkWriteDescriptorSet* writes, u32 count, u32 queries, u32 descriptors) {
    g_test.queries = g_test.encoded = 0;
    vk_descriptors_write(HANDLE(VkDescriptorSetLayout, 1), count, writes);
    assert(g_test.queries == queries && g_test.encoded == descriptors);
}
int main(void) {
    static u8 arena_bytes[64 * 1024];
    g_vk.has_descriptor_buffers = 1;
    g_vk.descriptor_props = (VkPhysicalDeviceDescriptorBufferPropertiesEXT){.descriptorBufferOffsetAlignment = 1,
                                                                            .uniformBufferDescriptorSize = 8,
                                                                            .storageBufferDescriptorSize = 8,
                                                                            .storageTexelBufferDescriptorSize = 8};
    g_descriptors.nlayouts = 1;
    g_descriptors.layouts[0].layout = HANDLE(VkDescriptorSetLayout, 1);
    g_descriptors.layouts[0].size = 4096;
    g_descriptors.frame[0].buffer = HANDLE(VkBuffer, 1);
    g_descriptors.frame[0].mapped = arena_bytes;
    g_descriptors.frame[0].capacity = sizeof arena_bytes;
    VkDescriptorBufferInfo buffers[34];
    VkWriteDescriptorSet writes[34];
    for (u32 i = 0; i < 34; i++) {
        g_descriptors.layouts[0].offset[i] = i * 64;
        buffers[i] = (VkDescriptorBufferInfo){HANDLE(VkBuffer, 7), 16 + 8 * i, 16 + i};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = i < 18 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                    : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &buffers[i]};
    }
    encode(writes, 34, 1, 34);
    for (u32 i = 0; i < 34; i++) {
        assert(g_test.address[i] == 7 * 4096 + buffers[i].offset);
        assert(g_test.range[i] == buffers[i].range && g_test.format[i] == VK_FORMAT_UNDEFINED);
        u64 encoded;
        memcpy(&encoded, arena_bytes + i * 64, sizeof encoded);
        assert(encoded == g_test.address[i]);
    }
    /* A/B/A must query again on the second A and keep each descriptor's own offset. */
    buffers[1].buffer = HANDLE(VkBuffer, 8);
    encode(writes, 3, 3, 3);
    for (u32 i = 0; i < 3; i++) assert(g_test.address[i] == (i == 1 ? 8 : 7) * 4096 + buffers[i].offset);
    /* A Vulkan handle reused in a later write call gets its new device address. */
    g_test.incarnation = 1ull << 40;
    encode(writes, 1, 1, 1);
    assert(g_test.address[0] == (1ull << 40) + 7 * 4096 + buffers[0].offset);
    g_test.incarnation = 0;
    /* Descriptor arrays share the same reuse; null descriptors never query the driver. */
    for (u32 i = 0; i < 4; i++) buffers[i].buffer = HANDLE(VkBuffer, i < 2 ? 7 : 0);
    writes[0].descriptorCount = 4;
    encode(writes, 1, 1, 4);
    for (u32 i = 0; i < 4; i++) assert(g_test.address[i] == (i < 2 ? 7 * 4096 : 0) + buffers[i].offset);
    writes[0].descriptorCount = 1;
    /* A texel view of the same buffer can reuse the address across descriptor types. */
    VkBufferViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                                   .buffer = HANDLE(VkBuffer, 7),
                                   .offset = 128,
                                   .range = 64,
                                   .format = VK_FORMAT_R32_UINT};
    writes[1] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .pNext = &view,
                                       .dstBinding = 1,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    encode(writes, 2, 1, 2);
    assert(g_test.address[1] == 7 * 4096 + 128 && g_test.range[1] == 64 && g_test.format[1] == VK_FORMAT_R32_UINT);
    view.buffer = HANDLE(VkBuffer, 9);
    encode(writes, 2, 2, 2);
    assert(g_test.address[1] == 9 * 4096 + 128);
    puts("PASS descriptor addresses: repeated buffers, switches, offsets, arrays, nulls and texel views");
    return 0;
}
