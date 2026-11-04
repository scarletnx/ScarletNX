/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include "common/gpu/vk/vk_common.h"
#include "common/memory/host.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <SDL3/SDL_vulkan.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif


#if defined(__linux__)
/* a dma-buf over the board's memfd, for a driver whose host pointer import refuses a file-backed mapping (RADV at
   present). The kernel's udmabuf driver pins the pages, and the dma-buf is imported as external memory instead. */
struct snx_udmabuf_create {
    uint32_t memfd;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
};
#define SNX_UDMABUF_CREATE _IOW('u', 0x42, struct snx_udmabuf_create)
static int udmabuf_create(int memfd, uint64_t size) {
    int control = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    int fd;
    if (control < 0) {
        fprintf(stderr, "3ds gpu: udmabuf: open: %s\n", strerror(errno));
        return -1;
    }
    {
        struct snx_udmabuf_create create = {(uint32_t)memfd, 0, 0, size};
        fd = ioctl(control, SNX_UDMABUF_CREATE, &create);
    }
    close(control);
    if (fd < 0) fprintf(stderr, "3ds gpu: udmabuf: ioctl(%d, %llu): %s\n", memfd, (unsigned long long)size,
                        strerror(errno));
    return fd;
}
#else
static int udmabuf_create(int memfd, uint64_t size) {
    (void)memfd;
    (void)size;
    return -1;
}
#endif

static struct {
    SnxCtrVk vk;
    int up;
    char error[768];  /* why the last snx_ctr_vk_init failed (an import refused, then the device's memory's reason) */
    char device[512]; /* the device it chose and its driver, "" before one */
} g_device;

SnxCtrVk* snx_ctr_vk(void) { return g_device.up ? &g_device.vk : NULL; }
const char* snx_ctr_vk_error(void) { return g_device.error; }

bool snx_ctr_vk_window(uint32_t pa, uint64_t size, SnxCtrVkWindow* w, uint32_t* inside) {
    const SnxCtrVk* v = &g_device.vk;
    if (!size || size > UINT32_MAX) return false;
    VkDeviceSize offset;
    const SnxCtrVkRam* ram = snx_ctr_vk_ram(pa, (uint32_t)size, &offset);
    if (!ram) return false;
    VkDeviceSize alignment = v->properties.limits.minStorageBufferOffsetAlignment;
    VkDeviceSize start = offset / alignment * alignment;
    VkDeviceSize range = ram->size - (offset - ram->buffer_offset);
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
    const char* extensions[7];
    uint32_t n = 0;
    /* the RAM imported, or the device's memory that the RAM moves into (snx_ctr_vk_init): one of them */
    bool host = snx_vk_extensions_has(&list, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    bool dmabuf = snx_vk_extensions_has(&list, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                  snx_vk_extensions_has(&list, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    bool ok = host || dmabuf;
    if (host) extensions[n++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (dmabuf) {
        extensions[n++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        extensions[n++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    if (v->host.surface || v->second_surface) {
        ok = ok && snx_vk_extensions_has(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (snx_vk_extensions_has(&list, VK_EXT_HDR_METADATA_EXTENSION_NAME))
            extensions[n++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    }
    bool dma_buf = snx_vk_extensions_has(&list, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
                   snx_vk_extensions_has(&list, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (dma_buf) {
        extensions[n++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
        extensions[n++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    }
    bool index8 = snx_vk_extensions_has(&list, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
    if (index8) extensions[n++] = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME;
    /* a portability driver's subset must be enabled when it lists one (MoltenVK), as the Switch frontend does */
    bool portability = snx_vk_extensions_has(&list, "VK_KHR_portability_subset");
    if (portability) extensions[n++] = "VK_KHR_portability_subset";
    snx_vk_extensions_free(&list);
    if (!ok) return false;
    if (v->second_surface) { /* the same queue presents to both windows */
        VkBool32 presents = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(phys, family, v->second_surface, &presents) != VK_SUCCESS || !presents)
            return false;
    }
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
    v->dma_buf = dma_buf;
    v->logic_op = features.features.logicOp;
    v->clip_distance = features.features.shaderClipDistance;
    v->import_host = host;
    v->export_dmabuf = dmabuf;
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(phys, &properties);
    v->preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32;
    return true;
}
void snx_ctr_vk_import_error(const SnxCtrVkImport* f, const char* region, char* error, size_t error_size) {
    const char* call = "";
    switch (f->step) {
        case SNX_CTR_VK_IMPORT_ALIGNMENT: break;
        case SNX_CTR_VK_IMPORT_POINTER: call = "vkGetMemoryHostPointerPropertiesEXT"; break;
        case SNX_CTR_VK_IMPORT_BUFFER: call = "vkCreateBuffer"; break;
        case SNX_CTR_VK_IMPORT_TYPE: break;
        case SNX_CTR_VK_IMPORT_ALLOCATE: call = "vkAllocateMemory"; break;
        case SNX_CTR_VK_IMPORT_BIND: call = "vkBindBufferMemory"; break;
    }
    char why[256];
    if (f->step == SNX_CTR_VK_IMPORT_ALIGNMENT)
        snprintf(why, sizeof why, "its host address or its size is not a multiple of the alignment");
    else if (f->step == SNX_CTR_VK_IMPORT_TYPE)
        snprintf(why, sizeof why,
                 "no host-coherent memory type is both one of the host pointer's (0x%x) and one of the buffer's (0x%x)",
                 f->pointer_types, f->buffer_types);
    else if (f->step == SNX_CTR_VK_IMPORT_ALLOCATE || f->step == SNX_CTR_VK_IMPORT_BIND)
        snprintf(why, sizeof why,
                 "%s gave %s (%d) for memory type %u (the host pointer's types 0x%x, the buffer's 0x%x)", call,
                 snx_vk_result_str(f->result), (int)f->result, f->type, f->pointer_types, f->buffer_types);
    else
        snprintf(why, sizeof why, "%s gave %s (%d)", call, snx_vk_result_str(f->result), (int)f->result);
    snprintf(error, error_size,
             "the driver cannot import the %s region (0x%08x, 0x%x bytes at host 0x%llx, alignment %llu): %s", region,
             f->base, f->size, (unsigned long long)f->host, (unsigned long long)f->alignment, why);
}
void snx_ctr_vk_device_ram_error(const SnxCtrVkDeviceRam* f, const char* region, char* error, size_t error_size) {
    const char* result = snx_vk_result_str(f->result);
    char reason[128];
    switch (f->step) {
        case SNX_CTR_VK_DEVICE_RAM_EXTENSIONS:
            snprintf(error, error_size,
                     "the device has no VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf to export it");
            break;
        case SNX_CTR_VK_DEVICE_RAM_EXPORT:
            snprintf(error, error_size, "its buffers export a dma-buf %s (external memory features 0x%x)",
                     f->features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? "only from a dedicated allocation"
                                                                             : "never",
                     f->features);
            break;
        case SNX_CTR_VK_DEVICE_RAM_BUFFER:
            snprintf(error, error_size, "vkCreateBuffer gave %s (%d) for the %s region at offset 0x%llx", result,
                     (int)f->result, region, (unsigned long long)f->offset);
            break;
        case SNX_CTR_VK_DEVICE_RAM_ALIGNMENT:
            snprintf(error, error_size,
                     "the %s region's offset 0x%llx is not a multiple of its buffer's alignment %llu", region,
                     (unsigned long long)f->offset, (unsigned long long)f->alignment);
            break;
        case SNX_CTR_VK_DEVICE_RAM_TYPE:
            snprintf(error, error_size,
                     "no host-visible, coherent, cached memory type is one of its buffers' (0x%x; the host-visible "
                     "coherent ones, uncached: 0x%x)",
                     f->buffer_types, f->visible_types);
            break;
        case SNX_CTR_VK_DEVICE_RAM_ALLOCATE:
            snprintf(error, error_size, "vkAllocateMemory gave %s (%d) for 0x%llx bytes of memory type %u", result,
                     (int)f->result, (unsigned long long)f->allocation, f->type);
            break;
        case SNX_CTR_VK_DEVICE_RAM_BIND:
            snprintf(error, error_size, "vkBindBufferMemory gave %s (%d) for the %s region at offset 0x%llx", result,
                     (int)f->result, region, (unsigned long long)f->offset);
            break;
        case SNX_CTR_VK_DEVICE_RAM_EXPORT_FD:
            snprintf(error, error_size, "vkGetMemoryFdKHR gave %s (%d) for its dma-buf", result, (int)f->result);
            break;
        case SNX_CTR_VK_DEVICE_RAM_VIEWS:
            errno = f->error;
            snprintf(error, error_size, "the guest's views of the RAM do not map its dma-buf: %s",
                     snx_os_error(reason, sizeof reason));
            break;
    }
}
/* what the RAM's buffers are used for: the GPU reads and writes the guest's memory through them */
static const VkBufferUsageFlags ram_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
/* every RAM region's buffer gone, and the memory under them: imports, or the device's memory the RAM is in (which the
   RAM's dma-buf keeps, mapped at every view, after the device is gone) */
static void release_ram(SnxCtrVk* v) {
    for (unsigned i = 0; i < v->ram_count; ++i) {
        vkDestroyBuffer(v->device, v->ram[i].buffer, NULL);
        vkFreeMemory(v->device, v->ram[i].memory, NULL);
    }
    vkFreeMemory(v->device, v->ram_memory, NULL);
    v->ram_count = 0;
    v->ram_memory = VK_NULL_HANDLE;
}
/* one RAM region's host memory, at view, a buffer in place: false with the step it stopped at and the driver's
   answer in f, nothing of it left */
static bool import_region(SnxCtrVk* v, unsigned char* view, SnxCtrVkRam* ram, SnxCtrVkImport* f) {
    if ((f->host | f->size) & (f->alignment - 1)) return false;
    VkMemoryHostPointerPropertiesEXT pointer = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    f->step = SNX_CTR_VK_IMPORT_POINTER;
    f->result = vkGetMemoryHostPointerPropertiesEXT(v->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                    view, &pointer);
    if (f->result != VK_SUCCESS) return false;
    f->pointer_types = pointer.memoryTypeBits;
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                 .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .pNext = &external,
                             .size = f->size,
                             .usage = ram_usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    f->step = SNX_CTR_VK_IMPORT_BUFFER;
    if ((f->result = vkCreateBuffer(v->device, &bi, NULL, &ram->buffer)) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, ram->buffer, &req);
    f->buffer_types = req.memoryTypeBits;
    /* host-coherent: the guest's CPU writes before a submit are seen by it, its writes after the fence by the CPU */
    f->step = SNX_CTR_VK_IMPORT_TYPE;
    f->type = snx_ctr_vk_memory_type(req.memoryTypeBits & pointer.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImportMemoryHostPointerInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               .pHostPointer = view};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &import,
                               .allocationSize = f->size,
                               .memoryTypeIndex = f->type};
    if (f->type != UINT32_MAX) {
        f->step = SNX_CTR_VK_IMPORT_ALLOCATE;
        if ((f->result = vkAllocateMemory(v->device, &ai, NULL, &ram->memory)) == VK_SUCCESS) {
            f->step = SNX_CTR_VK_IMPORT_BIND;
            if ((f->result = vkBindBufferMemory(v->device, ram->buffer, ram->memory, 0)) == VK_SUCCESS) return true;
            vkFreeMemory(v->device, ram->memory, NULL);
        }
    }
    vkDestroyBuffer(v->device, ram->buffer, NULL);
    return false;
}
/* every RAM region a buffer of its host memory in place: false at the first one the driver does not take, with why in
   error (the call that failed, the driver's answer and the region), and none of them left */
static bool import_ram(SnxCtrVk* v, SnxCtrMemory* memory, char* error, size_t error_size) {
    const SnxCtrProfile* profile = snx_ctr_memory_profile(memory);
    if (!v->import_host) {
        snprintf(error, error_size, "the device has no VK_EXT_external_memory_host to import the RAM");
        return false;
    }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host};
    vkGetPhysicalDeviceProperties2(v->phys, &properties);
    uint64_t alignment = host.minImportedHostPointerAlignment ? host.minImportedHostPointerAlignment : 4096;
    for (size_t i = 0; i < profile->region_count; ++i) {
        unsigned char* view = snx_ctr_memory_region(memory, i);
        const SnxCtrRegion* r = &profile->regions[i];
        if (!view) continue;
        SnxCtrVkImport f = {.step = SNX_CTR_VK_IMPORT_ALIGNMENT,
                            .base = r->base,
                            .size = r->size,
                            .host = (uintptr_t)view,
                            .alignment = alignment};
        SnxCtrVkRam* ram = &v->ram[v->ram_count];
        if (!import_region(v, view, ram, &f)) {
            snx_ctr_vk_import_error(&f, r->name, error, error_size);
            release_ram(v);
            return false;
        }
        ram->base = r->base;
        ram->size = r->size;
        ++v->ram_count;
    }
    return true;
}
/* Every RAM region a buffer of one allocation of the device's own memory, host-visible, coherent (the GPU sees the
   CPU's writes before a submit, the CPU its writes after the fence) and cached (the guest's CPU runs in it: an uncached
   type reads at a hundredth of the speed), at the region's offset in the board's RAM; then the allocation's dma-buf
   becomes the RAM's object, so the guest's every view of the RAM maps the device's memory. The buffers are bound before
   the RAM moves, the one step there is no going back from. False with the step it stopped at in f, nothing of it left
   and the RAM where it was */
static bool device_ram(SnxCtrVk* v, SnxCtrMemory* memory, SnxCtrVkDeviceRam* f) {
    const SnxCtrProfile* profile = snx_ctr_memory_profile(memory);
    const unsigned char* ram = snx_ctr_memory_ram(memory, &f->size);
    f->step = SNX_CTR_VK_DEVICE_RAM_EXTENSIONS;
    if (!v->export_dmabuf) return false;
    VkPhysicalDeviceExternalBufferInfo info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                               .usage = ram_usage,
                                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkExternalBufferProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(v->phys, &info, &external);
    f->step = SNX_CTR_VK_DEVICE_RAM_EXPORT;
    f->features = external.externalMemoryProperties.externalMemoryFeatures;
    if (!(f->features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
        (f->features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT))
        return false;
    VkExternalMemoryBufferCreateInfo exported = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                 .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    uint64_t offset[8]; /* by buffer: its region's offset in the RAM, and the region */
    uint32_t region[8];
    f->buffer_types = UINT32_MAX;
    f->allocation = f->size;
    for (size_t i = 0; i < profile->region_count; ++i) {
        const unsigned char* view = snx_ctr_memory_region(memory, i);
        const SnxCtrRegion* r = &profile->regions[i];
        if (!view) continue;
        SnxCtrVkRam* b = &v->ram[v->ram_count];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .pNext = &exported,
                                 .size = r->size,
                                 .usage = ram_usage,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        f->region = region[v->ram_count] = (uint32_t)i;
        f->offset = offset[v->ram_count] = (uint64_t)(view - ram);
        f->step = SNX_CTR_VK_DEVICE_RAM_BUFFER;
        if ((f->result = vkCreateBuffer(v->device, &bi, NULL, &b->buffer)) != VK_SUCCESS) goto fail;
        b->memory = VK_NULL_HANDLE;
        b->base = r->base;
        b->size = r->size;
        ++v->ram_count;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(v->device, b->buffer, &req);
        f->step = SNX_CTR_VK_DEVICE_RAM_ALIGNMENT;
        f->alignment = req.alignment;
        if (f->offset % req.alignment) goto fail;
        f->buffer_types &= req.memoryTypeBits;
        if (f->offset + req.size > f->allocation) f->allocation = f->offset + req.size;
    }
    f->step = SNX_CTR_VK_DEVICE_RAM_TYPE;
    const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    f->type = snx_ctr_vk_memory_type(f->buffer_types, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (f->type == UINT32_MAX) {
        f->visible_types = 0;
        for (uint32_t t = 0; t < v->memory_properties.memoryTypeCount; ++t)
            if ((f->buffer_types >> t & 1) && (v->memory_properties.memoryTypes[t].propertyFlags & visible) == visible)
                f->visible_types |= 1u << t;
        goto fail;
    }
    VkExportMemoryAllocateInfo export_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                              .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &export_info,
                               .allocationSize = f->allocation,
                               .memoryTypeIndex = f->type};
    f->step = SNX_CTR_VK_DEVICE_RAM_ALLOCATE;
    if ((f->result = vkAllocateMemory(v->device, &ai, NULL, &v->ram_memory)) != VK_SUCCESS) goto fail;
    f->step = SNX_CTR_VK_DEVICE_RAM_BIND;
    for (unsigned i = 0; i < v->ram_count; ++i) {
        f->offset = offset[i];
        f->region = region[i];
        if ((f->result = vkBindBufferMemory(v->device, v->ram[i].buffer, v->ram_memory, offset[i])) != VK_SUCCESS)
            goto fail;
    }
    VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                               .memory = v->ram_memory,
                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    f->step = SNX_CTR_VK_DEVICE_RAM_EXPORT_FD;
    if ((f->result = vkGetMemoryFdKHR(v->device, &gi, &fd)) != VK_SUCCESS) goto fail;
    f->step = SNX_CTR_VK_DEVICE_RAM_VIEWS;
    if (!snx_ctr_memory_adopt(memory, fd)) {
        f->error = errno;
        SnxHostMemory refused = {.handle = fd};
        snx_host_memory_close(&refused);
        goto fail;
    }
    v->ram_type = f->type;
    return true;
fail:
    release_ram(v);
    return false;
}
static bool init(SnxCtrMemory* memory, char* error, size_t error_size) {
    SnxCtrVk* v = &g_device.vk;
    memset(v, 0, sizeof *v);
    /* a window first: a display server and a Vulkan surface make it the output; without one frames go to the frame
       dumps only */
    /* the top screen alone fills its window when the screens are apart, 400x240 at twice its size; the pair needs the
       taller window, 600x720 for the two at 1.5 times */
    if (!snx_vk_window_open("scarletnx-ctr", "ScarletNX 3DS", g_settings.ctr_separate_screens ? 800 : 600,
                            g_settings.ctr_separate_screens ? 480 : 720, &v->host, error, error_size))
        return false;
    /* another window for the bottom screen when the screens are apart, on this instance */
    if (g_settings.ctr_separate_screens && v->host.window) {
        v->second = SDL_CreateWindow("ScarletNX 3DS (Bottom)", 640, 480, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!v->second || !SDL_Vulkan_CreateSurface(v->second, v->host.instance.instance, NULL, &v->second_surface)) {
            if (v->second) SDL_DestroyWindow(v->second);
            v->second = NULL;
            v->second_surface = VK_NULL_HANDLE;
        }
    }
    /* each window keeps its screen's shape as it is resized, so the screen fills it: no bars, and no stretching */
    if (g_settings.ctr_separate_screens && v->host.window) {
        SDL_SetWindowAspectRatio(v->host.window, 400.0f / 240.0f, 400.0f / 240.0f);
        if (v->second) SDL_SetWindowAspectRatio(v->second, 320.0f / 240.0f, 320.0f / 240.0f);
    }
    const VkQueueFlags queue = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    bool made =
        snx_vk_first_device(v->host.instance.instance, VK_API_VERSION_1_3, queue, v->host.surface, make_device, v);
    if (!made && v->second) { /* no device presents to both windows: one window again, the pair's size */
        vkDestroySurfaceKHR(v->host.instance.instance, v->second_surface, NULL);
        SDL_DestroyWindow(v->second);
        v->second = NULL;
        v->second_surface = VK_NULL_HANDLE;
        if (v->host.window) SDL_SetWindowSize(v->host.window, 600, 720);
        made =
            snx_vk_first_device(v->host.instance.instance, VK_API_VERSION_1_3, queue, v->host.surface, make_device, v);
    }
    if (!made) {
        snprintf(
            error, error_size,
            "no Vulkan 1.3 device with 8-bit storage buffers, dynamic rendering and VK_EXT_external_memory_host or "
            "VK_EXT_external_memory_dma_buf");
        snx_ctr_vk_free();
        return false;
    }
    volkLoadDevice(v->device);
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 chosen = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(v->phys, &chosen);
    snprintf(g_device.device, sizeof g_device.device, "%s, driver %s%s%s", chosen.properties.deviceName,
             driver.driverName, driver.driverInfo[0] ? " " : "", driver.driverInfo);
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
    /* the RAM imported where it is; where the driver refuses that, the RAM moved into the device's memory, said with
       the refusal, which stays in the error when the device's memory cannot hold it either */
    char refused[384];
    if (!import_ram(v, memory, refused, sizeof refused)) {
        SnxCtrVkDeviceRam f = {0};
        if (!device_ram(v, memory, &f)) {
            char why[256];
            snx_ctr_vk_device_ram_error(&f, snx_ctr_memory_profile(memory)->regions[f.region].name, why, sizeof why);
            snprintf(error, error_size, "%s; nor can the board's RAM be the device's memory: %s", refused, why);
            snx_ctr_vk_free();
            return false;
        }
        fprintf(stderr,
                "3ds gpu: %s; the board's RAM is the device's memory instead: 0x%llx bytes of memory type %u "
                "(host-visible, coherent, cached), its dma-buf mapped at every guest view\n",
                refused, (unsigned long long)f.allocation, f.type);
    }
    /* the renderer answers for the pages the GPU keeps (memory/ctr_memory.h) */
    v->memory = memory;
    snx_ctr_track_handler(memory, snx_ctr_vk_track, NULL);
    return true;
}
bool snx_ctr_vk_init(SnxCtrMemory* memory, char* error, size_t error_size) {
    if (g_device.up) return true;
    /* the reason stays here, for what stops because the device did not start (the GSP's GX commands) */
    g_device.error[0] = g_device.device[0] = 0;
    if (init(memory, g_device.error, sizeof g_device.error)) return true;
    snprintf(error, error_size, "%s%s%s", g_device.error, g_device.device[0] ? "; device " : "", g_device.device);
    return false;
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
        release_ram(v);
        snx_vk_ring_free(&v->ring);
        vkDestroyDevice(v->device, NULL);
    }
    if (v->second_surface) vkDestroySurfaceKHR(v->host.instance.instance, v->second_surface, NULL);
    if (v->second) SDL_DestroyWindow(v->second);
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
            *offset = r->buffer_offset + (pa - r->base);
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
