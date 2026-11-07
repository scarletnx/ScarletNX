/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_descriptor_heaps_init with mock Vulkan and VMA calls: every setup failure logs what failed, with its values, and
   the graphics backend the device keeps (buffers or sets), then leaves heaps off; the setup that succeeds logs its
   heap sizes. No Vulkan loader, device or GPU is used. */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VkState g_vk;
PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
PFN_vkCmdBindResourceHeapEXT vkCmdBindResourceHeapEXT;
PFN_vkCmdBindSamplerHeapEXT vkCmdBindSamplerHeapEXT;
PFN_vkCmdPushDataEXT vkCmdPushDataEXT;
PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress;
PFN_vkGetPhysicalDeviceDescriptorSizeEXT vkGetPhysicalDeviceDescriptorSizeEXT;
PFN_vkWriteResourceDescriptorsEXT vkWriteResourceDescriptorsEXT;
PFN_vkWriteSamplerDescriptorsEXT vkWriteSamplerDescriptorsEXT;

static struct {
    unsigned checks, failures;
    char log[4096];
    size_t used;
    VkDeviceSize sampler_size;
    VkResult buffer_result, pool_result;
    u8 mapped[256];
} g_test;
static void check(int pass, const char* what) {
    g_test.checks++;
    if (!pass && g_test.failures++ < 12) fprintf(stderr, "FAIL descriptor heap setup: %s\n", what);
}

void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_test.log + g_test.used, sizeof g_test.log - g_test.used, fmt, ap);
    va_end(ap);
    if (n > 0)
        g_test.used += (size_t)n < sizeof g_test.log - g_test.used ? (size_t)n : sizeof g_test.log - g_test.used - 1;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    (void)file;
    (void)line;
    if (r < 0) logf_(0, "vulkan: %s failed: %d\n", what, (int)r);
    return r >= 0;
}
u64 vk_hash(const void* p, u64 size) {
    u64 h = 1469598103934665603ull;
    for (u64 i = 0; i < size; i++) h = (h ^ ((const u8*)p)[i]) * 1099511628211ull;
    return h;
}
VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
u32 vk_cb_begun(void) { return 1; }
u64 vk_frame_id(void) { return 1; }
u32 vk_frame_slot(void) { return 0; }
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first, u32 n,
                               const VkDescriptorSet* sets, u32 ndyn, const u32* dyn) {
    (void)cb, (void)bp, (void)layout, (void)first, (void)n, (void)sets, (void)ndyn, (void)dyn;
}
void rec_CmdBindDescriptorHeapsEXT(VkCommandBuffer cb, const VkBindHeapInfoEXT* resource,
                                   const VkBindHeapInfoEXT* sampler) {
    (void)cb, (void)resource, (void)sampler;
}
void rec_CmdPushDataEXT(VkCommandBuffer cb, uint32_t offset, uint32_t size, const void* data) {
    (void)cb, (void)offset, (void)size, (void)data;
}
int vk_descriptor_view_info(VkImageView view, VkImageViewCreateInfo* out, void* chain, size_t size) {
    (void)view, (void)out, (void)chain, (void)size;
    return 0;
}
int vk_descriptor_sampler_info(VkSampler sampler, VkSamplerCreateInfo* out, void* chain, size_t size) {
    (void)sampler, (void)out, (void)chain, (void)size;
    return 0;
}
VkResult vmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* info,
                         const VmaAllocationCreateInfo* allocation, VkBuffer* buffer, VmaAllocation* memory,
                         VmaAllocationInfo* details) {
    (void)allocator, (void)allocation;
    if (g_test.buffer_result != VK_SUCCESS) return g_test.buffer_result;
    *buffer = (VkBuffer)(uintptr_t)0x1000;
    *memory = (VmaAllocation)(uintptr_t)0x2000;
    *details = (VmaAllocationInfo){.size = info->size, .pMappedData = g_test.mapped};
    return VK_SUCCESS;
}
void vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation memory) {
    (void)allocator, (void)buffer, (void)memory;
}

static VKAPI_ATTR VkDeviceSize VKAPI_CALL descriptor_size(VkPhysicalDevice device, VkDescriptorType type) {
    (void)device;
    return type == VK_DESCRIPTOR_TYPE_SAMPLER ? g_test.sampler_size : 32;
}
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL buffer_address(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    (void)device, (void)info;
    return 0x100000;
}
static VKAPI_ATTR VkResult VKAPI_CALL write_resources(VkDevice device, uint32_t count,
                                                      const VkResourceDescriptorInfoEXT* infos,
                                                      const VkHostAddressRangeEXT* out) {
    (void)device, (void)count, (void)infos, (void)out;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL write_samplers(VkDevice device, uint32_t count, const VkSamplerCreateInfo* infos,
                                                     const VkHostAddressRangeEXT* out) {
    (void)device, (void)count, (void)infos, (void)out;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL bind_heap(VkCommandBuffer cb, const VkBindHeapInfoEXT* info) { (void)cb, (void)info; }
static VKAPI_ATTR void VKAPI_CALL push_data(VkCommandBuffer cb, const VkPushDataInfoEXT* info) { (void)cb, (void)info; }
static VKAPI_ATTR VkResult VKAPI_CALL set_layout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
                                                 const VkAllocationCallbacks* allocation, VkDescriptorSetLayout* out) {
    (void)device, (void)info, (void)allocation;
    *out = (VkDescriptorSetLayout)(uintptr_t)0x3000;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL pipeline_layout(VkDevice device, const VkPipelineLayoutCreateInfo* info,
                                                      const VkAllocationCallbacks* allocation, VkPipelineLayout* out) {
    (void)device, (void)info, (void)allocation;
    *out = (VkPipelineLayout)(uintptr_t)0x4000;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL pool(VkDevice device, const VkDescriptorPoolCreateInfo* info,
                                           const VkAllocationCallbacks* allocation, VkDescriptorPool* out) {
    (void)device, (void)info, (void)allocation;
    if (g_test.pool_result != VK_SUCCESS) return g_test.pool_result;
    *out = (VkDescriptorPool)(uintptr_t)0x5000;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate_set(VkDevice device, const VkDescriptorSetAllocateInfo* info,
                                                   VkDescriptorSet* out) {
    (void)device, (void)info;
    *out = (VkDescriptorSet)(uintptr_t)0x6000;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice device, VkDescriptorPool p, const VkAllocationCallbacks* a) {
    (void)device, (void)p, (void)a;
}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice device, VkDescriptorSetLayout l,
                                                 const VkAllocationCallbacks* a) {
    (void)device, (void)l, (void)a;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pipeline_layout(VkDevice device, VkPipelineLayout l,
                                                          const VkAllocationCallbacks* a) {
    (void)device, (void)l, (void)a;
}

static VkPhysicalDeviceDescriptorHeapPropertiesEXT good_properties(void) {
    return (VkPhysicalDeviceDescriptorHeapPropertiesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
        .samplerHeapAlignment = 64,
        .resourceHeapAlignment = 64,
        .maxSamplerHeapSize = 1ull << 30,
        .maxResourceHeapSize = 1ull << 30,
        .minSamplerHeapReservedRange = 256,
        .minResourceHeapReservedRange = 256,
        .samplerDescriptorAlignment = 16,
        .imageDescriptorAlignment = 32,
        .bufferDescriptorAlignment = 16,
        .maxPushDataSize = 256};
}
static void mocks(void) {
    vkAllocateDescriptorSets = allocate_set;
    vkCmdBindResourceHeapEXT = bind_heap;
    vkCmdBindSamplerHeapEXT = bind_heap;
    vkCmdPushDataEXT = push_data;
    vkCreateDescriptorPool = pool;
    vkCreateDescriptorSetLayout = set_layout;
    vkCreatePipelineLayout = pipeline_layout;
    vkDestroyDescriptorPool = destroy_pool;
    vkDestroyDescriptorSetLayout = destroy_layout;
    vkDestroyPipelineLayout = destroy_pipeline_layout;
    vkGetBufferDeviceAddress = buffer_address;
    vkGetPhysicalDeviceDescriptorSizeEXT = descriptor_size;
    vkWriteResourceDescriptorsEXT = write_resources;
    vkWriteSamplerDescriptorsEXT = write_samplers;
    g_test.sampler_size = 16;
    g_test.buffer_result = g_test.pool_result = VK_SUCCESS;
    g_vk.alloc_cap = 1ull << 30;
}
/* One setup: it must fail, and the log must hold what failed and the backend left */
static void refused(const VkPhysicalDeviceDescriptorHeapPropertiesEXT* props, int buffers, const char* why,
                    const char* what) {
    g_vk.has_descriptor_buffers = buffers;
    g_test.used = 0;
    g_test.log[0] = 0;
    char line[512];
    snprintf(line, sizeof line, "vulkan: descriptor heaps are not used: %s; graphics descriptors use %s\n", why,
             buffers ? "buffers" : "sets");
    int made = vk_descriptor_heaps_init(props);
    check(!made, what);
    int said = strstr(g_test.log, line) != NULL;
    if (!said) fprintf(stderr, "%s: expected \"%s\" in:\n%s", what, line, g_test.log);
    check(said, what);
    VkDescriptorHeapStats stats;
    vk_descriptor_heaps_get_stats(&stats);
    check(!stats.arena_allocated_bytes, "a refused setup keeps no heap memory");
    mocks();
}

int main(void) {
    mocks();
    VkPhysicalDeviceDescriptorHeapPropertiesEXT props = good_properties();
    refused(NULL, 1, "the device gave no descriptor heap properties", "no properties");
    vkWriteSamplerDescriptorsEXT = NULL;
    refused(&props, 1, "the driver has no vkWriteSamplerDescriptorsEXT", "an entry point missing");
    props.maxPushDataSize = 100;
    refused(&props, 0, "maxPushDataSize is 100 bytes, 168 needed", "too little push data");
    props = good_properties();
    g_test.sampler_size = 0;
    refused(&props, 1,
            "a sampler descriptor is 0 bytes aligned to 16: a size and a power of two of 1 to 0xffffffff are needed",
            "a descriptor of no size");
    props.bufferDescriptorAlignment = 24;
    refused(&props, 0,
            "a uniform buffer descriptor is 32 bytes aligned to 24: a size and a power of two of 1 to 0xffffffff are "
            "needed",
            "an alignment that is no power of two");
    props = good_properties();
    props.samplerHeapAlignment = 48;
    refused(&props, 1, "resourceHeapAlignment is 64 and samplerHeapAlignment 48: powers of two are needed",
            "a heap alignment that is no power of two");
    props = good_properties();
    props.maxResourceHeapSize = 200;
    refused(&props, 1,
            "no resource heap of 2 frames fits: 256 bytes reserved, maxResourceHeapSize 200, allocations of "
            "1073741824 bytes at most",
            "a resource heap too small for its reserved range");
    props = good_properties();
    g_test.buffer_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    char why[256];
    snprintf(why, sizeof why, "the resource heap's %llu-byte buffer could not be made",
             (unsigned long long)((1ull << 20) * VK_FRAMES + 256 + 63));
    refused(&props, 0, why, "the resource heap's buffer");
    g_test.pool_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    refused(&props, 1, "the empty descriptor set that leaves descriptor buffers: vkCreateDescriptorPool failed",
            "the transition set's pool");
    /* The same mocks with nothing refused: heaps are made, and the log says their sizes and no refusal */
    g_vk.has_descriptor_buffers = 1;
    g_test.used = 0;
    g_test.log[0] = 0;
    check(vk_descriptor_heaps_init(&props) == 1, "a setup nothing refuses");
    check(!strstr(g_test.log, "not used") && strstr(g_test.log, "vulkan: descriptor heaps: "),
          "a made setup logs its sizes");
    vk_descriptor_heaps_destroy();
    printf("descriptor heap setup: %u checks, %u failures\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
