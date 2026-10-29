/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include <stdio.h>
#include <string.h>

static struct {
    SnxCtrVk vk;
    int up;
} g_device;

SnxCtrVk* snx_ctr_vk(void) { return g_device.up ? &g_device.vk : NULL; }

bool snx_ctr_vk_window(uint32_t pa, uint64_t size, SnxCtrVkWindow* w, uint32_t* inside) {
    const SnxCtrVk* v = &g_device.vk;
    if (!size || size > UINT32_MAX) return false;
    VkDeviceSize offset;
    const SnxCtrVkRam* ram = snx_ctr_vk_ram(pa, (uint32_t)size, &offset);
    if (!ram) return false;
    VkDeviceSize alignment = v->properties.limits.minStorageBufferOffsetAlignment;
    VkDeviceSize start = offset / alignment * alignment;
    VkDeviceSize range = ram->size - start;
    if (range > v->properties.limits.maxStorageBufferRange) range = v->properties.limits.maxStorageBufferRange;
    if (offset - start + size > range) return false;
    *w = (SnxCtrVkWindow){ram->buffer, start, range};
    *inside = (uint32_t)(offset - start);
    return true;
}
uint32_t snx_ctr_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags properties) {
    return snx_vk_memory_type(&g_device.vk.memory_properties, bits, properties);
}
/* A device of this physical device and queue family, if it has what the 3DS needs (snx_vk_first_device's make) */
static bool make_device(VkPhysicalDevice phys, uint32_t family, void* user) {
    SnxCtrVk* v = user;
    SnxVkExtensions list;
    if (!snx_vk_device_extensions(phys, &list)) return false;
    const char* extensions[4];
    uint32_t n = 0;
    bool ok = snx_vk_extensions_has(&list, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    extensions[n++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (v->host.surface) {
        ok = ok && snx_vk_extensions_has(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    bool index8 = snx_vk_extensions_has(&list, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
    if (index8) extensions[n++] = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME;
    snx_vk_extensions_free(&list);
    if (!ok) return false;
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT index_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void* const query[] = {&features, &v12, &v13, index8 ? &index_features : NULL};
    snx_vk_chain(query, sizeof query / sizeof *query);
    vkGetPhysicalDeviceFeatures2(phys, &features);
    if (!v12.storageBuffer8BitAccess || !v13.dynamicRendering) return false;
    /* what the 3DS uses of them, nothing more */
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT want_index = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT,
        .indexTypeUint8 = index_features.indexTypeUint8};
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE,
                                               .synchronization2 = v13.synchronization2};
    VkPhysicalDeviceVulkan12Features want12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               .storageBuffer8BitAccess = VK_TRUE,
                                               .shaderInt8 = v12.shaderInt8,
                                               .uniformAndStorageBuffer8BitAccess =
                                                   v12.uniformAndStorageBuffer8BitAccess};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.features.logicOp = features.features.logicOp;
    want.features.fragmentStoresAndAtomics = features.features.fragmentStoresAndAtomics;
    want.features.shaderClipDistance = features.features.shaderClipDistance;
    want.features.shaderInt16 = features.features.shaderInt16;
    void* const enable[] = {&want, &want12, &want13, index8 ? &want_index : NULL};
    snx_vk_chain(enable, sizeof enable / sizeof *enable);
    if (snx_vk_device_create(phys, family, &want, extensions, n, &v->device) != VK_SUCCESS) return false;
    v->phys = phys;
    v->family = family;
    v->index8 = index8 && index_features.indexTypeUint8;
    v->logic_op = features.features.logicOp;
    v->clip_distance = features.features.shaderClipDistance;
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(phys, &properties);
    v->preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32;
    return true;
}
static bool import_ram(SnxCtrVk* v, SnxCtrMemory* memory, char* error, size_t error_size) {
    const SnxCtrProfile* profile = snx_ctr_memory_profile(memory);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host};
    vkGetPhysicalDeviceProperties2(v->phys, &properties);
    for (size_t i = 0; i < profile->region_count; ++i) {
        unsigned char* view = snx_ctr_memory_region(memory, i);
        const SnxCtrRegion* r = &profile->regions[i];
        if (!view) continue;
        uint64_t alignment = host.minImportedHostPointerAlignment ? host.minImportedHostPointerAlignment : 4096;
        if (((uintptr_t)view | r->size) & (alignment - 1)) {
            snprintf(error, error_size, "the %s region is not aligned to the %llu bytes the driver imports", r->name,
                     (unsigned long long)alignment);
            return false;
        }
        VkMemoryHostPointerPropertiesEXT pointer = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(v->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, view,
                                                &pointer) != VK_SUCCESS) {
            snprintf(error, error_size, "the driver cannot import the %s region", r->name);
            return false;
        }
        SnxCtrVkRam* ram = &v->ram[v->ram_count];
        VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                     .handleTypes =
                                                         VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .pNext = &external,
                                 .size = r->size,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        if (vkCreateBuffer(v->device, &bi, NULL, &ram->buffer) != VK_SUCCESS) {
            snprintf(error, error_size, "cannot make a buffer of the %s region", r->name);
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(v->device, ram->buffer, &req);
        /* host-coherent: the guest's CPU writes before a submit are seen by it, its writes after the fence by the CPU */
        uint32_t type = snx_ctr_vk_memory_type(req.memoryTypeBits & pointer.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkImportMemoryHostPointerInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                   .pHostPointer = view};
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .pNext = &import,
                                   .allocationSize = r->size,
                                   .memoryTypeIndex = type};
        if (type == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &ram->memory) != VK_SUCCESS ||
            vkBindBufferMemory(v->device, ram->buffer, ram->memory, 0) != VK_SUCCESS) {
            vkDestroyBuffer(v->device, ram->buffer, NULL);
            snprintf(error, error_size, "the driver cannot import the %s region", r->name);
            return false;
        }
        ram->base = r->base;
        ram->size = r->size;
        ++v->ram_count;
    }
    return true;
}
bool snx_ctr_vk_init(SnxCtrMemory* memory, char* error, size_t error_size) {
    SnxCtrVk* v = &g_device.vk;
    if (g_device.up) return true;
    memset(v, 0, sizeof *v);
    /* a window first: a display server and a Vulkan surface make it the output; without one frames go to the frame
       dumps only */
    if (!snx_vk_window_open("scarletnx-ctr", "ScarletNX 3DS", 600, 720, &v->host, error, error_size)) return false;
    const VkQueueFlags queue = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if (!snx_vk_first_device(v->host.instance.instance, VK_API_VERSION_1_3, queue, v->host.surface, make_device, v)) {
        snprintf(error, error_size,
                 "no Vulkan 1.3 device with 8-bit storage buffers, dynamic rendering and VK_EXT_external_memory_host");
        snx_ctr_vk_free();
        return false;
    }
    volkLoadDevice(v->device);
    vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    vkGetPhysicalDeviceProperties(v->phys, &v->properties);
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->memory_properties);
    /* a recording's sets: a draw takes two (three uniform blocks, three LUTs and three textures), a compute dispatch
       one; running out finishes the recording early (vk/render.c) */
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * 4096},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 3 * 4096}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 12288,
                                     .poolSizeCount = sizeof sizes / sizeof *sizes,
                                     .pPoolSizes = sizes};
    if (!snx_vk_ring_init(&v->ring, v->device, v->queue, v->family, SNX_CTR_VK_SLOTS, &dp)) {
        snprintf(error, error_size, "cannot make the Vulkan device's command pool");
        snx_ctr_vk_free();
        return false;
    }
    /* a recording opens the renderer's per-slot state, a rendering pass ends with its command buffer, and what the
       renderer kept for a submission goes when it is done */
    v->ring.begin = snx_ctr_vk_render_begin;
    v->ring.close = snx_ctr_vk_render_close;
    v->ring.retire = snx_ctr_vk_render_retire;
    g_device.up = 1;
    if (!import_ram(v, memory, error, error_size)) {
        snx_ctr_vk_free();
        return false;
    }
    /* the renderer answers for the pages the GPU keeps (memory/ctr_memory.h) */
    v->memory = memory;
    snx_ctr_track_handler(memory, snx_ctr_vk_track, NULL);
    return true;
}
void snx_ctr_vk_free(void) {
    SnxCtrVk* v = &g_device.vk;
    if (v->device) {
        snx_ctr_vk_render_free();
        vkDeviceWaitIdle(v->device);
        if (v->memory) { /* nothing is the GPU's any more: every page the CPU's, untrapped */
            snx_ctr_track_handler(v->memory, NULL, NULL);
            for (unsigned i = 0; i < v->ram_count; ++i)
                snx_ctr_track_set(v->memory, v->ram[i].base, v->ram[i].size, SNX_CTR_TRACK_NONE);
        }
        for (unsigned i = 0; i < v->ram_count; ++i) {
            vkDestroyBuffer(v->device, v->ram[i].buffer, NULL);
            vkFreeMemory(v->device, v->ram[i].memory, NULL);
        }
        snx_vk_ring_free(&v->ring);
        vkDestroyDevice(v->device, NULL);
    }
    snx_vk_window_close(&v->host);
    memset(v, 0, sizeof *v);
    g_device.up = 0;
}
uint64_t snx_ctr_vk_poll(void) { return g_device.up ? snx_vk_ring_poll(&g_device.vk.ring) : 0; }
bool snx_ctr_vk_wait(uint64_t serial, uint64_t timeout_ns) {
    return !g_device.up || snx_vk_ring_wait(&g_device.vk.ring, serial, timeout_ns);
}
VkCommandBuffer snx_ctr_vk_commands(void) { return snx_vk_ring_commands(&g_device.vk.ring); }
uint64_t snx_ctr_vk_serial(void) { return g_device.vk.ring.submitted + 1; }
unsigned snx_ctr_vk_slot(void) { return g_device.vk.ring.slot; }
uint64_t snx_ctr_vk_submit(void) { return snx_ctr_vk_submit_with(VK_NULL_HANDLE, VK_NULL_HANDLE); }
uint64_t snx_ctr_vk_submit_with(VkSemaphore wait, VkSemaphore signal) {
    return g_device.up ? snx_vk_ring_submit(&g_device.vk.ring, wait, signal) : 0;
}
bool snx_ctr_vk_finish(void) {
    if (!g_device.up) return true;
    return snx_ctr_vk_wait(snx_ctr_vk_submit(), UINT64_MAX);
}
VkDescriptorSet snx_ctr_vk_set(VkDescriptorSetLayout layout) { return snx_vk_ring_set(&g_device.vk.ring, layout); }
const SnxCtrVkRam* snx_ctr_vk_ram(uint32_t pa, uint32_t size, VkDeviceSize* offset) {
    const SnxCtrVk* v = &g_device.vk;
    for (unsigned i = 0; i < v->ram_count; ++i) {
        const SnxCtrVkRam* r = &v->ram[i];
        if (pa >= r->base && (uint64_t)pa - r->base + size <= r->size) {
            *offset = pa - r->base;
            return r;
        }
    }
    return NULL;
}
bool snx_ctr_vk_compute(const uint32_t* words, size_t count, unsigned buffers, unsigned images, uint32_t push,
                        SnxCtrVkCompute* out) {
    SnxCtrVk* v = &g_device.vk;
    VkDescriptorSetLayoutBinding bindings[16];
    *out = (SnxCtrVkCompute){0};
    if (buffers + images > 16) return false;
    for (unsigned i = 0; i < buffers + images; ++i)
        bindings[i] = (VkDescriptorSetLayoutBinding){
            i, i < buffers ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = buffers + images,
                                          .pBindings = bindings};
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pushConstantRangeCount = push ? 1 : 0,
                                     .pPushConstantRanges = &range};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = count * 4,
                                   .pCode = words};
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(v->device, &si, NULL, &out->set) != VK_SUCCESS) return false;
    li.pSetLayouts = &out->set;
    if (vkCreatePipelineLayout(v->device, &li, NULL, &out->layout) != VK_SUCCESS ||
        vkCreateShaderModule(v->device, &mi, NULL, &module) != VK_SUCCESS) {
        snx_ctr_vk_compute_free(out);
        return false;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = out->layout};
    bool ok = vkCreateComputePipelines(v->device, VK_NULL_HANDLE, 1, &ci, NULL, &out->pipeline) == VK_SUCCESS;
    vkDestroyShaderModule(v->device, module, NULL);
    if (!ok) snx_ctr_vk_compute_free(out);
    return ok;
}
void snx_ctr_vk_compute_free(SnxCtrVkCompute* c) {
    SnxCtrVk* v = &g_device.vk;
    if (!v->device) return;
    if (c->pipeline) vkDestroyPipeline(v->device, c->pipeline, NULL);
    if (c->layout) vkDestroyPipelineLayout(v->device, c->layout, NULL);
    if (c->set) vkDestroyDescriptorSetLayout(v->device, c->set, NULL);
    *c = (SnxCtrVkCompute){0};
}
void snx_ctr_vk_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                         0, NULL);
}
void snx_ctr_vk_compute_barrier(VkCommandBuffer cb) {
    snx_ctr_vk_render_close(); /* compute may follow a draw in the same recording */
    snx_ctr_vk_barrier(cb);
}
