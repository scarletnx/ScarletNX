/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vulkan of GLES 1.1, 2 and 3 (gles_vk.h): the device, the surfaces and their frames. The library is a Vulkan app
   of its own on the host window: it makes its instance, surface, device and swapchain through the pass-through's
   commands (hle_vulkan_host.h), so its presents get the renderer's layer and the --shot copies as a guest's own
   Vulkan presents do */
#include "android/bridge/gles_vk.h"
#include "android/bridge/gles2.h"
#include "android/bridge/hle_vulkan_host.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GlesVk g_gles_vk = {.lock = PTHREAD_MUTEX_INITIALIZER};
#define g g_gles_vk

uint32_t gles_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

VkShaderModule gles_vk_shader(const uint32_t* words, size_t bytes) {
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = bytes,
                                     .pCode = words};
    VkShaderModule m = VK_NULL_HANDLE;
    if (g.vk.vkCreateShaderModule(g.device, &info, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static bool has_extension(const VkExtensionProperties* list, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(list[i].extensionName, name)) return true;
    return false;
}

/* the device: Vulkan 1.3's dynamic rendering for GLES 2 and 3 when the device has it, and the features GLES's
   limits need (large points, wide lines, anisotropy, the compressed formats, storage in the vertex stage) */
static bool make_device(void) {
    uint32_t n = 0;
    g.vk.vkEnumerateDeviceExtensionProperties(g.physical, NULL, &n, NULL);
    VkExtensionProperties* exts = calloc(n ? n : 1, sizeof *exts);
    if (!exts) return false;
    g.vk.vkEnumerateDeviceExtensionProperties(g.physical, NULL, &n, exts);
    const char* names[8];
    uint32_t count = 0;
    names[count++] = "VK_KHR_swapchain";
    g.v13 = g.props.apiVersion >= VK_API_VERSION_1_3;
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT uint8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 all = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    bool has_clip = has_extension(exts, n, "VK_EXT_depth_clip_control");
    bool has_uint8 = has_extension(exts, n, "VK_EXT_index_type_uint8");
    free(exts);
    void* chain = NULL;
    if (g.v13) {
        v13.pNext = chain;
        chain = &v13;
    }
    if (has_clip) {
        clip.pNext = chain;
        chain = &clip;
    }
    if (has_uint8) {
        uint8.pNext = chain;
        chain = &uint8;
    }
    all.pNext = chain;
    g.vk.vkGetPhysicalDeviceFeatures2(g.physical, &all);
    const VkPhysicalDeviceFeatures* have = &all.features;
    VkPhysicalDeviceFeatures want = {
        .independentBlend = have->independentBlend,
        .largePoints = have->largePoints,
        .wideLines = have->wideLines,
        .samplerAnisotropy = have->samplerAnisotropy,
        .textureCompressionETC2 = have->textureCompressionETC2,
        .textureCompressionASTC_LDR = have->textureCompressionASTC_LDR,
        .imageCubeArray = have->imageCubeArray,
        .fragmentStoresAndAtomics = have->fragmentStoresAndAtomics,
        .vertexPipelineStoresAndAtomics = have->vertexPipelineStoresAndAtomics,
        .shaderStorageImageExtendedFormats = have->shaderStorageImageExtendedFormats,
        .sampleRateShading = have->sampleRateShading,
        .fullDrawIndexUint32 = have->fullDrawIndexUint32,
        .depthBiasClamp = have->depthBiasClamp,
        .shaderClipDistance = have->shaderClipDistance,
    };
    g.features = want;
    VkPhysicalDeviceVulkan13Features v13_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, .depthClipControl = VK_TRUE};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT uint8_on = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, .indexTypeUint8 = VK_TRUE};
    g.v13 = g.v13 && v13.dynamicRendering;
    g.depth_clip_control = has_clip && clip.depthClipControl;
    g.index_uint8 = has_uint8 && uint8.indexTypeUint8;
    chain = NULL;
    if (g.v13) {
        v13_on.pNext = chain;
        chain = &v13_on;
    }
    if (g.depth_clip_control) {
        clip_on.pNext = chain;
        chain = &clip_on;
        names[count++] = "VK_EXT_depth_clip_control";
    }
    if (g.index_uint8) {
        uint8_on.pNext = chain;
        chain = &uint8_on;
        names[count++] = "VK_EXT_index_type_uint8";
    }
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = g.family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkDeviceCreateInfo dinfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                .pNext = chain,
                                .queueCreateInfoCount = 1,
                                .pQueueCreateInfos = &queue,
                                .enabledExtensionCount = count,
                                .ppEnabledExtensionNames = names,
                                .pEnabledFeatures = &want};
    return hle_vulkan_create_device(g.physical, &dinfo, &g.device) == VK_SUCCESS;
}

bool gles_vk_device(VkSurfaceKHR* surface) {
    if (g.tried) {
        if (!g.up) return false;
        return hle_vulkan_create_surface(g.instance, surface) == VK_SUCCESS;
    }
    g.tried = true;
    static const char* const extensions[] = {"VK_KHR_android_surface"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "GLES",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app,
                                 .enabledExtensionCount = 1,
                                 .ppEnabledExtensionNames = extensions};
    if (hle_vulkan_create_instance(&info, &g.instance) != VK_SUCCESS) return false;
#define GLES_LOAD(name) g.vk.name = (PFN_##name)vkGetInstanceProcAddr(g.instance, #name);
    GLES_INSTANCE_COMMANDS(GLES_LOAD)
#undef GLES_LOAD
    if (hle_vulkan_create_surface(g.instance, surface) != VK_SUCCESS) return false;
    uint32_t n = 0;
    g.vk.vkEnumeratePhysicalDevices(g.instance, &n, NULL);
    VkPhysicalDevice physicals[16];
    if (n > 16) n = 16;
    g.vk.vkEnumeratePhysicalDevices(g.instance, &n, physicals);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; ++i) {
        uint32_t families = 0;
        g.vk.vkGetPhysicalDeviceQueueFamilyProperties(physicals[i], &families, NULL);
        VkQueueFamilyProperties props[16];
        if (families > 16) families = 16;
        g.vk.vkGetPhysicalDeviceQueueFamilyProperties(physicals[i], &families, props);
        for (uint32_t f = 0; f < families && !found; ++f) {
            VkBool32 present = VK_FALSE;
            g.vk.vkGetPhysicalDeviceSurfaceSupportKHR(physicals[i], f, *surface, &present);
            if ((props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (props[f].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                present) {
                g.physical = physicals[i];
                g.family = f;
                found = true;
            }
        }
    }
    if (!found) {
        fprintf(stderr, "scarletnx-android: GLES: no Vulkan device presents to the window\n");
        return false;
    }
    g.vk.vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
    g.vk.vkGetPhysicalDeviceProperties(g.physical, &g.props);
    g.max_texture = g.props.limits.maxImageDimension2D < 8192 ? g.props.limits.maxImageDimension2D : 8192;
    if (!make_device()) return false;
#define GLES_LOAD(name) g.vk.name = (PFN_##name)g.vk.vkGetDeviceProcAddr(g.device, #name);
    GLES_DEVICE_COMMANDS(GLES_LOAD)
    if (g.v13) {
        GLES_DEVICE13_COMMANDS(GLES_LOAD)
    }
#undef GLES_LOAD
    hle_vulkan_get_queue(g.device, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = g.family};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    g.up = g.vk.vkCreateCommandPool(g.device, &pool, NULL, &g.commands) == VK_SUCCESS &&
           g.vk.vkCreateFence(g.device, &fence, NULL, &g.flush_fence) == VK_SUCCESS;
    if (g.up)
        fprintf(stderr, "scarletnx-android: GLES over Vulkan on %s (GLES 2 and 3: %s)\n", g.props.deviceName,
                g.v13 ? "yes" : "no, the device has no Vulkan 1.3");
    return g.up;
}

bool gles_vk_image(const VkImageCreateInfo* info, VkImageViewType view_type, VkImageAspectFlags aspect,
                   GlesImage* out) {
    *out = (GlesImage){0};
    if (g.vk.vkCreateImage(g.device, info, NULL, &out->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    g.vk.vkGetImageMemoryRequirements(g.device, out->image, &req);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex =
                                      gles_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (alloc.memoryTypeIndex == UINT32_MAX) alloc.memoryTypeIndex = gles_vk_memory_type(req.memoryTypeBits, 0);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        g.vk.vkAllocateMemory(g.device, &alloc, NULL, &out->memory) != VK_SUCCESS ||
        g.vk.vkBindImageMemory(g.device, out->image, out->memory, 0) != VK_SUCCESS) {
        gles_vk_destroy((GlesGarbage){.image = out->image, .memory = out->memory});
        *out = (GlesImage){0};
        return false;
    }
    if (view_type == VK_IMAGE_VIEW_TYPE_MAX_ENUM) return true;
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = out->image,
                                  .viewType = view_type,
                                  .format = info->format,
                                  .subresourceRange = {aspect, 0, info->mipLevels, 0, info->arrayLayers}};
    if (g.vk.vkCreateImageView(g.device, &view, NULL, &out->view) != VK_SUCCESS) {
        gles_vk_destroy((GlesGarbage){.image = out->image, .memory = out->memory});
        *out = (GlesImage){0};
        return false;
    }
    return true;
}

void gles_vk_destroy(GlesGarbage x) {
    if (x.pipeline) g.vk.vkDestroyPipeline(g.device, x.pipeline, NULL);
    if (x.layout) g.vk.vkDestroyPipelineLayout(g.device, x.layout, NULL);
    if (x.set_layout) g.vk.vkDestroyDescriptorSetLayout(g.device, x.set_layout, NULL);
    if (x.buffer_view) g.vk.vkDestroyBufferView(g.device, x.buffer_view, NULL);
    if (x.view) g.vk.vkDestroyImageView(g.device, x.view, NULL);
    if (x.image) g.vk.vkDestroyImage(g.device, x.image, NULL);
    if (x.buffer) g.vk.vkDestroyBuffer(g.device, x.buffer, NULL);
    if (x.memory) g.vk.vkFreeMemory(g.device, x.memory, NULL);
}

void gles_vk_garbage(GlesSlot* s, GlesGarbage item) {
    if (s->garbage_count == s->garbage_capacity) {
        size_t cap = s->garbage_capacity ? s->garbage_capacity * 2 : 16;
        GlesGarbage* grown = realloc(s->garbage, cap * sizeof *grown);
        if (!grown) return; /* it leaks rather than being destroyed while the GPU reads it */
        s->garbage = grown;
        s->garbage_capacity = cap;
    }
    s->garbage[s->garbage_count++] = item;
}

/* the retired objects whose serial completed, destroyed */
static void collect(void) {
    size_t kept = 0;
    for (size_t i = 0; i < g.retired_count; ++i) {
        if (g.retired[i].serial <= g.completed) gles_vk_destroy(g.retired[i].item);
        else g.retired[kept++] = g.retired[i];
    }
    g.retired_count = kept;
}

void gles_vk_retire(GlesGarbage item) {
    if (g.retired_count == g.retired_capacity) {
        size_t cap = g.retired_capacity ? g.retired_capacity * 2 : 64;
        void* grown = realloc(g.retired, cap * sizeof *g.retired);
        if (!grown) return;
        g.retired = grown;
        g.retired_capacity = cap;
    }
    g.retired[g.retired_count].item = item;
    g.retired[g.retired_count++].serial = gles_vk_pending();
}

void gles_vk_shutdown(void) {
    if (!g.device) {
        g.tried = g.up = false;
        return;
    }
    g.vk.vkDeviceWaitIdle(g.device);
    /* every retired object, those of the serial still pending too: the device is idle */
    for (size_t i = 0; i < g.retired_count; ++i) gles_vk_destroy(g.retired[i].item);
    if (g.flush_fence) g.vk.vkDestroyFence(g.device, g.flush_fence, NULL);
    if (g.commands) g.vk.vkDestroyCommandPool(g.device, g.commands, NULL);
    if (g.pass_clear) g.vk.vkDestroyRenderPass(g.device, g.pass_clear, NULL);
    if (g.pass_load) g.vk.vkDestroyRenderPass(g.device, g.pass_load, NULL);
    hle_vulkan_destroy_device(g.device);
    hle_vulkan_destroy_instance(g.instance);
    free(g.retired);
    /* everything but the lock, which the caller holds */
    memset(&g.tried, 0, sizeof g - offsetof(GlesVk, tried));
}

void gles_vk_wait(uint64_t serial) {
    if (serial <= g.completed) return;
    if (serial > g.submitted) return; /* not submitted: the caller flushes first */
    /* the queue signals in order: waiting for all of it is waiting for serial */
    g.vk.vkDeviceWaitIdle(g.device);
    g.completed = g.submitted;
    collect();
}

/* a host-visible buffer of size bytes for an arena: of every use that GLES 1.1 and 2 put a frame's bytes to, as the
   first caller of a slot's arena makes it for the others too */
static bool arena_make(GlesArena* a, VkDeviceSize size, VkBufferUsageFlags usage) {
    *a = (GlesArena){0};
    usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    if (g.vk.vkCreateBuffer(g.device, &info, NULL, &a->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    g.vk.vkGetBufferMemoryRequirements(g.device, a->buffer, &req);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = gles_vk_memory_type(req.memoryTypeBits,
                                                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    void* map = NULL;
    if (alloc.memoryTypeIndex == UINT32_MAX || g.vk.vkAllocateMemory(g.device, &alloc, NULL, &a->memory) != VK_SUCCESS ||
        g.vk.vkBindBufferMemory(g.device, a->buffer, a->memory, 0) != VK_SUCCESS ||
        g.vk.vkMapMemory(g.device, a->memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        gles_vk_destroy((GlesGarbage){.buffer = a->buffer, .memory = a->memory});
        *a = (GlesArena){0};
        return false;
    }
    a->map = map;
    a->size = size;
    return true;
}

VkDeviceSize gles_vk_arena_take(GlesSlot* s, GlesArena* a, VkDeviceSize size, VkDeviceSize align, VkDeviceSize least,
                                VkBufferUsageFlags usage) {
    VkDeviceSize at = (a->used + align - 1) & ~(align - 1);
    if (!a->buffer || at + size > a->size) {
        if (a->buffer) gles_vk_garbage(s, (GlesGarbage){.buffer = a->buffer, .memory = a->memory});
        VkDeviceSize want = size > least ? size : least;
        if (a->size * 2 > want) want = a->size * 2;
        if (!arena_make(a, want, usage)) return VK_WHOLE_SIZE;
        at = 0;
    }
    a->used = at + size;
    return at;
}

VkDescriptorSet gles_vk_set(GlesSlot* s, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &layout};
    while (true) {
        if (s->pool_used == s->pool_count) {
            VkDescriptorPool* grown = realloc(s->pools, (s->pool_count + 1) * sizeof *grown);
            if (!grown) return VK_NULL_HANDLE;
            s->pools = grown;
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256}};
            VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                               .maxSets = 1024,
                                               .poolSizeCount = sizeof sizes / sizeof *sizes,
                                               .pPoolSizes = sizes};
            if (g.vk.vkCreateDescriptorPool(g.device, &info, NULL, &s->pools[s->pool_count]) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            s->pool_count++;
        }
        alloc.descriptorPool = s->pools[s->pool_used];
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult r = g.vk.vkAllocateDescriptorSets(g.device, &alloc, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return VK_NULL_HANDLE;
        s->pool_used++;
    }
}

void gles_image_barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                        VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
                        VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = src_access,
                              .dstAccessMask = dst_access,
                              .oldLayout = from,
                              .newLayout = to,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                                                   VK_REMAINING_ARRAY_LAYERS}};
    g.vk.vkCmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- the surfaces ---- */

/* the render passes of GLES 1.1 on a format: one that clears the image first, and one that keeps what the frame
   drew. Its pipelines are made for pass_clear (hle_gles.c), and are made again when the format changes */
static bool passes(VkFormat format) {
    if (g.format == format && g.pass_clear) return true;
    if (g.pass_clear) {
        g.vk.vkDeviceWaitIdle(g.device);
        g.vk.vkDestroyRenderPass(g.device, g.pass_clear, NULL);
        g.vk.vkDestroyRenderPass(g.device, g.pass_load, NULL);
        gles1_format_changed();
    }
    g.format = format;
    for (int load = 0; load < 2; ++load) {
        VkAttachmentDescription color = {.format = format,
                                         .samples = VK_SAMPLE_COUNT_1_BIT,
                                         .loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                         .initialLayout = load ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                                         .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &ref};
        /* after the acquire's wait, and after the texture copies before the pass */
        VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL,
                                   .dstSubpass = 0,
                                   .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
        VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                       .attachmentCount = 1,
                                       .pAttachments = &color,
                                       .subpassCount = 1,
                                       .pSubpasses = &sub,
                                       .dependencyCount = 1,
                                       .pDependencies = &dep};
        if (g.vk.vkCreateRenderPass(g.device, &info, NULL, load ? &g.pass_load : &g.pass_clear) != VK_SUCCESS)
            return false;
    }
    return true;
}

static void depth_free(GlesSurface* s) {
    gles_vk_destroy((GlesGarbage){.image = s->depth, .view = s->depth_view, .memory = s->depth_memory});
    s->depth = VK_NULL_HANDLE;
    s->depth_view = VK_NULL_HANDLE;
    s->depth_memory = VK_NULL_HANDLE;
    s->depth_ready = false;
}

static void swapchain_free(GlesSurface* s) {
    for (uint32_t i = 0; i < s->image_count; ++i) {
        if (s->framebuffers[i]) g.vk.vkDestroyFramebuffer(g.device, s->framebuffers[i], NULL);
        if (s->views[i]) g.vk.vkDestroyImageView(g.device, s->views[i], NULL);
        if (s->rendered[i]) g.vk.vkDestroySemaphore(g.device, s->rendered[i], NULL);
        s->framebuffers[i] = VK_NULL_HANDLE;
        s->views[i] = VK_NULL_HANDLE;
        s->rendered[i] = VK_NULL_HANDLE;
    }
    if (s->pbuffer) {
        gles_vk_destroy((GlesGarbage){.image = s->images[0], .memory = s->own_memory});
        s->images[0] = VK_NULL_HANDLE;
        s->own_memory = VK_NULL_HANDLE;
    }
    depth_free(s);
    s->own_ready = false;
    s->image_count = 0;
}

/* the images and views of the surface's images, and GLES 1.1's framebuffers of them */
static bool image_views(GlesSurface* s, uint32_t images) {
    for (uint32_t i = 0; i < images; ++i) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = s->images[i],
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = s->format,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (g.vk.vkCreateImageView(g.device, &view, NULL, &s->views[i]) != VK_SUCCESS) return false;
        if (!s->pbuffer) {
            VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = g.pass_clear,
                                          .attachmentCount = 1,
                                          .pAttachments = &s->views[i],
                                          .width = s->extent.width,
                                          .height = s->extent.height,
                                          .layers = 1};
            g.vk.vkCreateFramebuffer(g.device, &fb, NULL, &s->framebuffers[i]);
            VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            g.vk.vkCreateSemaphore(g.device, &sem, NULL, &s->rendered[i]);
        }
    }
    s->image_count = images;
    s->generation++;
    s->stale = false;
    return true;
}

/* the swapchain at the surface's current size, replacing the one it has */
static bool swapchain_make(GlesSurface* s) {
    g.vk.vkDeviceWaitIdle(g.device);
    VkSurfaceCapabilitiesKHR caps;
    if (hle_vulkan_surface_capabilities(g.physical, s->surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent = (VkExtent2D){(uint32_t)s->width, (uint32_t)s->height};
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    if (!extent.width || !extent.height) return false;
    uint32_t n = 0;
    g.vk.vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, s->surface, &n, NULL);
    VkSurfaceFormatKHR formats[64];
    if (n > 64) n = 64;
    g.vk.vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, s->surface, &n, formats);
    if (!n) return false;
    /* UNORM: GLES writes its colors to the window as they are */
    VkSurfaceFormatKHR format = formats[0];
    for (uint32_t i = 0; i < n; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            format = formats[i];
            break;
        }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > GLES_MAX_IMAGES) count = GLES_MAX_IMAGES;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t bit = 1; bit <= 8; bit <<= 1)
            if (caps.supportedCompositeAlpha & bit) {
                alpha = (VkCompositeAlphaFlagBitsKHR)bit;
                break;
            }
    /* GLES 2 copies from the window (glReadPixels, glBlitFramebuffer, glCopyTexImage2D) and into it */
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              (caps.supportedUsageFlags &
                               (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    VkSwapchainKHR old = s->swapchain;
    VkSwapchainCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                     .surface = s->surface,
                                     .minImageCount = count,
                                     .imageFormat = format.format,
                                     .imageColorSpace = format.colorSpace,
                                     .imageExtent = extent,
                                     .imageArrayLayers = 1,
                                     .imageUsage = usage,
                                     .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                     .preTransform = caps.currentTransform,
                                     .compositeAlpha = alpha,
                                     .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                     .clipped = VK_TRUE,
                                     .oldSwapchain = old};
    VkSwapchainKHR made = VK_NULL_HANDLE;
    VkResult r = hle_vulkan_create_swapchain(g.device, &info, &made);
    swapchain_free(s);
    if (old) hle_vulkan_destroy_swapchain(g.device, old);
    s->swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS || !passes(format.format)) return false;
    s->swapchain = made;
    s->format = format.format;
    s->extent = extent;
    uint32_t images = GLES_MAX_IMAGES;
    if (hle_vulkan_swapchain_images(g.device, made, &images, s->images) < 0) return false;
    return image_views(s, images);
}

/* a pbuffer's image: RGBA8, which every device renders to and samples */
static bool pbuffer_make(GlesSurface* s) {
    s->format = VK_FORMAT_R8G8B8A8_UNORM;
    s->extent = (VkExtent2D){(uint32_t)s->width, (uint32_t)s->height};
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = s->format,
                              .extent = {s->extent.width, s->extent.height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    GlesImage image;
    if (!gles_vk_image(&info, VK_IMAGE_VIEW_TYPE_MAX_ENUM, 0, &image)) return false;
    s->images[0] = image.image;
    s->own_memory = image.memory;
    return image_views(s, 1);
}

static void surface_free(GlesSurface* s);

static bool slots_make(GlesSurface* s) {
    for (unsigned i = 0; i < GLES_SLOTS; ++i) {
        GlesSlot* slot = &s->slots[i];
        VkCommandBufferAllocateInfo cb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = g.commands,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GLES_MAX_SETS},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, GLES_MAX_SETS / 8},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, GLES_MAX_SETS / 8}};
        VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .maxSets = GLES_MAX_SETS,
                                           .poolSizeCount = 3,
                                           .pPoolSizes = sizes};
        if (g.vk.vkAllocateCommandBuffers(g.device, &cb, &slot->cb) != VK_SUCCESS ||
            g.vk.vkCreateFence(g.device, &fence, NULL, &slot->fence) != VK_SUCCESS ||
            g.vk.vkCreateSemaphore(g.device, &sem, NULL, &slot->acquire) != VK_SUCCESS ||
            g.vk.vkCreateDescriptorPool(g.device, &pool, NULL, &slot->pool) != VK_SUCCESS)
            return false;
    }
    return true;
}

GlesSurface* gles_window_surface(int width, int height) {
    pthread_mutex_lock(&g.lock);
    VkSurfaceKHR vs = VK_NULL_HANDLE;
    GlesSurface* s = NULL;
    if (gles_vk_device(&vs) && (s = calloc(1, sizeof *s))) {
        s->surface = vs;
        s->width = width > 0 ? width : 1;
        s->height = height > 0 ? height : 1;
        if (!swapchain_make(s) || !slots_make(s)) {
            fprintf(stderr, "scarletnx-android: GLES: no swapchain on the window\n");
            pthread_mutex_unlock(&g.lock);
            gles_surface_free(s);
            return NULL;
        }
    } else if (vs) {
        g.vk.vkDestroySurfaceKHR(g.instance, vs, NULL);
    }
    pthread_mutex_unlock(&g.lock);
    return s;
}

GlesSurface* gles_pbuffer_make(int width, int height) {
    VkSurfaceKHR vs = VK_NULL_HANDLE;
    if (!g.tried) {
        /* the first surface brings the device up, which takes a window surface: this one is let go */
        if (gles_vk_device(&vs) && vs) g.vk.vkDestroySurfaceKHR(g.instance, vs, NULL);
    }
    GlesSurface* s = g.up ? calloc(1, sizeof *s) : NULL;
    if (!s) return NULL;
    s->pbuffer = true;
    s->width = width > 0 ? width : 1;
    s->height = height > 0 ? height : 1;
    if (!pbuffer_make(s) || !slots_make(s)) {
        surface_free(s);
        return NULL;
    }
    return s;
}

GlesSurface* gles_pbuffer_surface(int width, int height) {
    pthread_mutex_lock(&g.lock);
    GlesSurface* s = gles_pbuffer_make(width, height);
    pthread_mutex_unlock(&g.lock);
    return s;
}

static void surface_free(GlesSurface* s) {
    if (g.device) g.vk.vkDeviceWaitIdle(g.device);
    if (s->recording) {
        gles_pass_end(s);
        g.vk.vkEndCommandBuffer(s->slots[s->slot].cb);
    }
    gles2_surface_gone(s);
    for (unsigned i = 0; i < GLES_SLOTS; ++i) {
        GlesSlot* slot = &s->slots[i];
        for (size_t k = 0; k < slot->garbage_count; ++k) gles_vk_destroy(slot->garbage[k]);
        free(slot->garbage);
        gles_vk_destroy((GlesGarbage){.buffer = slot->vertices.buffer, .memory = slot->vertices.memory});
        gles_vk_destroy((GlesGarbage){.buffer = slot->staging.buffer, .memory = slot->staging.memory});
        for (uint32_t k = 0; k < slot->pool_count; ++k) g.vk.vkDestroyDescriptorPool(g.device, slot->pools[k], NULL);
        free(slot->pools);
        if (slot->cb) g.vk.vkFreeCommandBuffers(g.device, g.commands, 1, &slot->cb);
        if (slot->fence) g.vk.vkDestroyFence(g.device, slot->fence, NULL);
        if (slot->acquire) g.vk.vkDestroySemaphore(g.device, slot->acquire, NULL);
        if (slot->pool) g.vk.vkDestroyDescriptorPool(g.device, slot->pool, NULL);
    }
    swapchain_free(s);
    if (s->swapchain) hle_vulkan_destroy_swapchain(g.device, s->swapchain);
    if (s->surface) g.vk.vkDestroySurfaceKHR(g.instance, s->surface, NULL);
    free(s);
}

void gles_surface_free(GlesSurface* s) {
    if (!s) return;
    pthread_mutex_lock(&g.lock);
    surface_free(s);
    pthread_mutex_unlock(&g.lock);
}

void gles_surface_size(const GlesSurface* s, int* width, int* height) {
    *width = (int)s->extent.width;
    *height = (int)s->extent.height;
}

void gles_surface_depth(GlesSurface* s, int depth, int stencil) {
    s->depth_bits = depth;
    s->stencil_bits = stencil;
}

bool gles_frame_begin(GlesSurface* s) {
    if (s->recording) return true;
    if (!s->pbuffer) {
        if (s->stale && !swapchain_make(s)) return false;
        if (!s->swapchain) return false;
    }
    GlesSlot* slot = &s->slots[s->slot];
    g.vk.vkWaitForFences(g.device, 1, &slot->fence, VK_TRUE, UINT64_MAX);
    if (slot->serial > g.completed) g.completed = slot->serial;
    collect();
    for (size_t k = 0; k < slot->garbage_count; ++k) gles_vk_destroy(slot->garbage[k]);
    slot->garbage_count = 0;
    slot->vertices.used = slot->staging.used = 0;
    g.vk.vkResetDescriptorPool(g.device, slot->pool, 0);
    for (uint32_t k = 0; k < slot->pool_used + 1 && k < slot->pool_count; ++k)
        g.vk.vkResetDescriptorPool(g.device, slot->pools[k], 0);
    slot->pool_used = 0;
    g.vk.vkResetCommandBuffer(slot->cb, 0);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    g.vk.vkBeginCommandBuffer(slot->cb, &begin);
    s->recording = true;
    s->open = GLES_OPEN_NONE;
    s->acquired = s->drew = s->waited = s->touched = false;
    if (s->pbuffer) s->acquired = s->touched = true; /* its one image, always there */
    return true;
}

void gles_pass_end(GlesSurface* s) {
    if (s->open == GLES_OPEN_PASS) g.vk.vkCmdEndRenderPass(s->slots[s->slot].cb);
    else if (s->open == GLES_OPEN_RENDERING) gles2_rendering_end(s);
    s->open = GLES_OPEN_NONE;
}

bool gles_acquire(GlesSurface* s) {
    if (!gles_frame_begin(s)) return false;
    if (s->acquired) return true;
    GlesSlot* slot = &s->slots[s->slot];
    VkResult r = g.vk.vkAcquireNextImageKHR(g.device, s->swapchain, UINT64_MAX, slot->acquire, VK_NULL_HANDLE,
                                            &s->image);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        s->stale = true;
        return false;
    }
    s->acquired = true;
    s->touched = false;
    return true;
}

bool gles_pass_begin(GlesSurface* s) {
    if (s->pbuffer || !gles_acquire(s)) return false;
    if (s->open == GLES_OPEN_PASS) return true;
    gles_pass_end(s);
    GlesSlot* slot = &s->slots[s->slot];
    VkClearValue clear = {{{0, 0, 0, 1}}};
    VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s->touched ? g.pass_load : g.pass_clear,
                                  .framebuffer = s->framebuffers[s->image],
                                  .renderArea = {{0, 0}, s->extent},
                                  .clearValueCount = 1,
                                  .pClearValues = &clear};
    g.vk.vkCmdBeginRenderPass(slot->cb, &info, VK_SUBPASS_CONTENTS_INLINE);
    s->open = GLES_OPEN_PASS;
    s->touched = true;
    return true;
}

/* the frame's commands submitted: with the fence of the slot (end) or the flush fence */
static VkResult submit(GlesSurface* s, bool end, bool present) {
    GlesSlot* slot = &s->slots[s->slot];
    gles_pass_end(s);
    g.vk.vkEndCommandBuffer(slot->cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    bool wait = s->acquired && !s->pbuffer && !s->waited;
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .waitSemaphoreCount = wait ? 1 : 0,
                         .pWaitSemaphores = &slot->acquire,
                         .pWaitDstStageMask = &stage,
                         .commandBufferCount = 1,
                         .pCommandBuffers = &slot->cb,
                         .signalSemaphoreCount = present ? 1 : 0,
                         .pSignalSemaphores = &s->rendered[s->image]};
    s->waited |= wait;
    VkFence fence = end ? slot->fence : g.flush_fence;
    if (end) g.vk.vkResetFences(g.device, 1, &fence);
    VkResult r = g.vk.vkQueueSubmit(g.queue, 1, &info, fence);
    uint64_t serial = ++g.submitted;
    if (end) slot->serial = serial;
    if (r != VK_SUCCESS && end) {
        /* nothing signals the fence: the slot is free again once what went before is done */
        g.vk.vkDeviceWaitIdle(g.device);
        VkSubmitInfo none = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        g.vk.vkQueueSubmit(g.queue, 1, &none, fence);
    }
    return r;
}

bool gles_flush(GlesSurface* s) {
    if (!s->recording) return true;
    VkResult r = submit(s, false, false);
    if (r == VK_SUCCESS) {
        g.vk.vkWaitForFences(g.device, 1, &g.flush_fence, VK_TRUE, UINT64_MAX);
        g.vk.vkResetFences(g.device, 1, &g.flush_fence);
        g.completed = g.submitted;
        collect();
    } else {
        g.vk.vkDeviceWaitIdle(g.device);
    }
    GlesSlot* slot = &s->slots[s->slot];
    g.vk.vkResetCommandBuffer(slot->cb, 0);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    g.vk.vkBeginCommandBuffer(slot->cb, &begin);
    return r == VK_SUCCESS;
}

void gles_frame_end(GlesSurface* s) {
    if (!s->recording) return;
    submit(s, true, false);
    s->recording = false;
    s->slot = (s->slot + 1) % GLES_SLOTS;
}

bool gles_swap(GlesSurface* s) {
    pthread_mutex_lock(&g.lock);
    if (s && s->pbuffer) {
        gles_frame_end(s);
        pthread_mutex_unlock(&g.lock);
        return true;
    }
    /* a frame that drew nothing presents a cleared image */
    bool ok = s && gles_frame_begin(s) && (s->touched || gles_pass_begin(s));
    if (!ok) {
        /* the frame is dropped: its commands ended unsubmitted, the image (if acquired) not presented */
        if (s && s->recording) {
            gles_pass_end(s);
            g.vk.vkEndCommandBuffer(s->slots[s->slot].cb);
            s->recording = false;
        }
        pthread_mutex_unlock(&g.lock);
        return false;
    }
    VkResult r = submit(s, true, true);
    s->recording = false;
    if (r == VK_SUCCESS) {
        VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &s->rendered[s->image],
                                    .swapchainCount = 1,
                                    .pSwapchains = &s->swapchain,
                                    .pImageIndices = &s->image};
        r = hle_vulkan_present(g.queue, &present);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) s->stale = true;
    }
    s->slot = (s->slot + 1) % GLES_SLOTS;
    pthread_mutex_unlock(&g.lock);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR;
}
