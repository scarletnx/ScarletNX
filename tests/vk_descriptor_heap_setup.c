/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_descriptor_heaps_init with mock Vulkan and devmem.c calls: every setup failure logs what failed, with its values, and
   the graphics backend the device keeps (buffers or sets), then leaves heaps off; the setup that succeeds logs its
   heap sizes. No Vulkan loader, device or GPU is used. */
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/runtime/platform/log.h" /* snx_settings.h: g_settings */
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
    u8* made[4]; /* the buffers bigger than mapped, by their DevMem - 0x2000 */
    u64 frame, submit, last, done; /* the frame and submissions the mocks give */
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
u64 vk_frame_id(void) { return g_test.frame; }
u32 vk_frame_slot(void) { return 0; }
Settings g_settings;
int vk_lock_held(void) { return 1; }
u64 vk_submit_number(void) { return g_test.submit; }
u64 vk_last_submit(void) { return g_test.last; }
void vk_submit_progress(u64* submitted, u64* done) {
    *submitted = g_test.last;
    *done = g_test.done;
}
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
VkResult vk_devmem_buffer(const VkBufferCreateInfo* info, const DevMemWant* allocation, VkBuffer* buffer,
                          DevMem* memory, DevMemInfo* details) {
    (void)allocation;
    if (g_test.buffer_result != VK_SUCCESS) return g_test.buffer_result;
    *buffer = (VkBuffer)(uintptr_t)0x1000;
    *memory = 0x2000;
    u8* ptr = g_test.mapped;
    if (info->size > sizeof g_test.mapped) { /* a heap the lasting part's checks write descriptors into */
        u32 i = 0;
        while (i < 4 && g_test.made[i]) i++;
        if (i == 4 || !(ptr = calloc(1, (size_t)info->size))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        g_test.made[i] = ptr;
        *memory = 0x2001 + i;
    }
    *details = (DevMemInfo){.size = info->size, .ptr = ptr};
    return VK_SUCCESS;
}
void vk_mem_track(DevMem a, u32 owner) { (void)a, (void)owner; }
void vk_mem_untrack(DevMem a) { (void)a; }
void vk_devmem_buffer_destroy(VkBuffer buffer, DevMem memory) {
    (void)buffer;
    if (memory > 0x2000 && memory <= 0x2004) {
        free(g_test.made[memory - 0x2001]);
        g_test.made[memory - 0x2001] = NULL;
    }
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

/* The lasting part's reap (descriptor_heaps.c lasting_reap): the slot of a buffer that was forgotten is not free
   while the GPU can still read a table of the frame that named it, and is free once the GPU has done the submissions
   of that frame. The walk over the slots waiting is skipped while the GPU has not done the last frame */
static void lasting_reap_case(const VkPhysicalDeviceDescriptorHeapPropertiesEXT* props) {
    char what[128];
    g_vk.has_descriptor_buffers = 1;
    g_test.frame = g_test.submit = 1;
    g_test.last = g_test.done = 0;
    check(vk_descriptor_heaps_init(props) == 1, "the lasting part's setup");
    const VkDescriptorSetLayout layout = (VkDescriptorSetLayout)(uintptr_t)0x7000;
    const VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    vk_descriptor_heaps_layout(layout, &binding, 1);
    const VkBuffer buffer = (VkBuffer)(uintptr_t)0x8000;
    vk_descriptor_heaps_lasting(buffer);
    const VkDescriptorBufferInfo range = {buffer, 0, 256};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0,
                                        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        .pBufferInfo = &range};
    VkDescriptorHeapStats s;
    check(vk_descriptor_heaps_write(layout, 1, &write, 0) != VK_NULL_HANDLE,
          "a named buffer's descriptor made in the lasting part");
    vk_descriptor_heaps_get_stats(&s);
    check(s.lasting_made == 1, "a named buffer's descriptor made in the lasting part");
    vk_descriptor_heaps_forget_buffer(buffer);
    /* frames 2 and 3: the GPU has not done the submission of the frame before. A table of frame 1 can still be read */
    static const u64 steps[3][4] = {{2, 2, 1, 0}, {3, 3, 2, 1}, {4, 4, 3, 3}}; /* frame, submit, last, done */
    for (u32 i = 0; i < 3; i++) {
        g_test.frame = steps[i][0];
        g_test.submit = steps[i][1];
        g_test.last = steps[i][2];
        g_test.done = steps[i][3];
        check(vk_descriptor_heaps_write(layout, 0, NULL, 0) != VK_NULL_HANDLE, "an empty table of a new frame");
        vk_descriptor_heaps_get_stats(&s);
        snprintf(what, sizeof what, "frame %llu, GPU done %llu: the slot %s", (unsigned long long)g_test.frame,
                 (unsigned long long)g_test.done, i < 2 ? "stays" : "is freed");
        check(s.lasting_forgotten == 1 && s.lasting_freed == (i < 2 ? 0u : 1u), what);
        snprintf(what, sizeof what, "frame %llu: the reap's walks and slots looked at",
                 (unsigned long long)g_test.frame);
        check(s.lasting_reaps_skipped == (i < 2 ? i + 1u : 2u) && s.lasting_reaps == (i < 2 ? 0u : 1u) &&
                  s.lasting_reap_looked == (i < 2 ? 0u : 1u),
              what);
    }
    vk_descriptor_heaps_destroy();
    g_test.frame = g_test.submit = 1;
    g_test.last = g_test.done = 0;
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
             (unsigned long long)(VK_DESCRIPTOR_HEAP_RESOURCE_FRAME * VK_FRAMES + 256 +
                                  32ull * VK_DESCRIPTOR_HEAP_LASTING_SLOTS + 63)); /* the lasting part's 32-byte slots */
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
    lasting_reap_case(&props);
    printf("descriptor heap setup: %u checks, %u failures\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
