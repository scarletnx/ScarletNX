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
typedef int64_t i64;
#define SNX_VK_H
#define SNX_LOG_H
#define VK_FRAMES                                              2
#define SNX_VKB_END                                            64
#define VK_OK(result)                                          ((result) == VK_SUCCESS)
#define DM_HOST                                                1
typedef u32 DevMem;
typedef struct {
    u32 at;
    VkMemoryPropertyFlags need;
    unsigned char mapped;
} DevMemWant;
typedef struct {
    void* ptr;
} DevMemInfo;
static struct {
    int has_descriptor_buffers, has_descriptor_heap_support, has_descriptor_heaps;
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_props;
    VkPhysicalDeviceFeatures feat;
    VkDevice dev;
    u64 alloc_cap;
} g_vk;
static struct {
    u32 queries, encoded;
    u64 incarnation;
    VkDeviceAddress address[128];
    VkDeviceSize range[128];
    VkFormat format[128];
} g_test;
VkResult vk_devmem_buffer(const VkBufferCreateInfo*, const DevMemWant*, VkBuffer*, DevMem*, DevMemInfo*);
void vk_mem_track(DevMem, u32);
VkCommandBuffer vk_cmd(void);
u32 vk_frame_slot(void);
u64 vk_frame_id(void);
void vk_frame_next(void);
u32 vk_cb_begun(void);
u64 vk_rec_set_binds(void);
void fatal(const char*, ...);
void logf_(int, const char*, ...);
void rec_CmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, u32, u32, const VkDescriptorSet*,
                               u32, const u32*);
#define LOG(...) logf_(1, __VA_ARGS__)
#include "nx/gpu/vk/descriptors.c"

#define HANDLE(type, value) ((type)(uintptr_t)(value))
void vk_descriptor_heaps_layout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding* bindings, u32 count) {
    (void)layout;
    (void)bindings;
    (void)count;
    abort();
}
void vk_descriptor_heaps_reset(u32 slot) {
    (void)slot;
    abort();
}
int vk_descriptor_heaps_room(u32 slot) {
    (void)slot;
    abort();
}
VkDescriptorSet vk_descriptor_heaps_write(VkDescriptorSetLayout layout, u32 count, const VkWriteDescriptorSet* writes) {
    (void)layout;
    (void)count;
    (void)writes;
    abort();
}
void vk_descriptor_heaps_bind(VkCommandBuffer cb, u32 first, u32 count, const VkDescriptorSet* sets) {
    (void)cb;
    (void)first;
    (void)count;
    (void)sets;
    abort();
}
void vk_descriptor_heaps_buffer_bound(u32 begun) { (void)begun; abort(); }
void vk_descriptor_heaps_stats(void) { abort(); }
void vk_descriptor_heaps_get_stats(VkDescriptorHeapStats* stats) {
    (void)stats;
    abort();
}
VkResult vk_devmem_buffer(const VkBufferCreateInfo* b, const DevMemWant* c, VkBuffer* d, DevMem* e,
                          DevMemInfo* f) {
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    abort();
}
void vk_mem_track(DevMem a, u32 owner) {
    (void)a, (void)owner;
    abort();
}
VkCommandBuffer vk_cmd(void) { return HANDLE(VkCommandBuffer, 1); }
u32 vk_frame_slot(void) { return 0; }
u64 vk_frame_id(void) { return 0; }
void vk_frame_next(void) { abort(); }
u32 vk_cb_begun(void) { return 1; }
u64 vk_rec_set_binds(void) { return 0; }
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

static const u8* encode(const VkWriteDescriptorSet* writes, u32 count, u32 queries, u32 descriptors) {
    g_test.queries = g_test.encoded = 0;
    VkDescriptorSet set = vk_descriptors_write(HANDLE(VkDescriptorSetLayout, 1), count, writes);
    assert(g_test.queries == queries && g_test.encoded == descriptors);
    return g_descriptors.frame[0].mapped + (uintptr_t)set - 1;
}
static u64 encoded_address(const u8* table, u32 binding, u32 element) {
    u64 address;
    memcpy(&address, table + g_descriptors.layouts[0].offset[binding] + element * sizeof address, sizeof address);
    return address;
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
        g_descriptors.layouts[0].count[i] = 4;
        g_descriptors.layouts[0].type[i] = i < 18 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        buffers[i] = (VkDescriptorBufferInfo){HANDLE(VkBuffer, 7), 16 + 8 * i, 16 + i};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = i < 18 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                    : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &buffers[i]};
    }
    const u8* table = encode(writes, 34, 1, 34);
    for (u32 i = 0; i < 34; i++) {
        assert(g_test.address[i] == 7 * 4096 + buffers[i].offset);
        assert(g_test.range[i] == buffers[i].range && g_test.format[i] == VK_FORMAT_UNDEFINED);
        assert(encoded_address(table, i, 0) == g_test.address[i]);
    }
    /* A/B/A must query again on the second A and keep each descriptor's own offset. */
    buffers[1].buffer = HANDLE(VkBuffer, 8);
    table = encode(writes, 3, 3, 1); /* both A encodings are cached; the B encoding is new */
    for (u32 i = 0; i < 3; i++) assert(encoded_address(table, i, 0) == (i == 1 ? 8 : 7) * 4096 + buffers[i].offset);
    /* A Vulkan handle reused in a later write call gets its new device address. */
    g_test.incarnation = 1ull << 40;
    table = encode(writes, 1, 1, 1);
    assert(encoded_address(table, 0, 0) == (1ull << 40) + 7 * 4096 + buffers[0].offset);
    g_test.incarnation = 0;
    /* Descriptor arrays share the same reuse; null descriptors never query the driver. */
    for (u32 i = 0; i < 4; i++) buffers[i].buffer = HANDLE(VkBuffer, i < 2 ? 7 : 0);
    writes[0].descriptorCount = 4;
    table = encode(writes, 1, 1, 2); /* the two non-null encodings are cached */
    for (u32 i = 0; i < 4; i++) assert(encoded_address(table, 0, i) == (i < 2 ? 7 * 4096 : 0) + buffers[i].offset);
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
    g_descriptors.layouts[0].count[1] = 1;
    g_descriptors.layouts[0].type[1] = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    table = encode(writes, 2, 1, 1);
    assert(encoded_address(table, 1, 0) == 7 * 4096 + 128);
    assert(g_test.range[0] == 64 && g_test.format[0] == VK_FORMAT_R32_UINT);
    view.buffer = HANDLE(VkBuffer, 9);
    table = encode(writes, 2, 2, 1);
    assert(encoded_address(table, 1, 0) == 9 * 4096 + 128);
    puts("PASS descriptor addresses: repeated buffers, switches, offsets, arrays, nulls and texel views");
    return 0;
}
