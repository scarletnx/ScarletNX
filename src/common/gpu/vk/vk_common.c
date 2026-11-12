/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* getpid in strict C99 */
#endif
#include "common/gpu/vk/vk_common.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* GetCurrentProcessId */
#else
#include <unistd.h> /* getpid */
#endif

const char* snx_vk_result_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
        default: return "VkResult";
    }
}

bool snx_vk_has_layer(const char* name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, NULL) != VK_SUCCESS || !n) return false;
    VkLayerProperties* l = calloc(n, sizeof *l);
    if (!l) return false;
    bool found = false;
    if (vkEnumerateInstanceLayerProperties(&n, l) >= 0)
        for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(l[i].layerName, name);
    free(l);
    return found;
}
bool snx_vk_has_instance_extension(const char* name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS || !n) return false;
    VkExtensionProperties* e = calloc(n, sizeof *e);
    if (!e) return false;
    bool found = false;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, e) >= 0)
        for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(e[i].extensionName, name);
    free(e);
    return found;
}

VkResult snx_vk_instance_create(const SnxVkInstanceInfo* info, SnxVkInstance* out) {
    *out = (SnxVkInstance){0};
    /* the window's, then debug utils and portability enumeration */
    const char** ext = calloc((size_t)info->extension_count + 3, sizeof *ext);
    if (!ext) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t next = 0;
    bool asked = false; /* the window system may ask for portability enumeration itself (SDL does on Apple systems) */
    for (uint32_t i = 0; i < info->extension_count; i++) {
        ext[next++] = info->extensions[i];
        asked |= !strcmp(info->extensions[i], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    const char* layers[1];
    uint32_t nlayers = 0;
    bool gpu_assisted = false;
    if (info->validation && snx_vk_has_layer("VK_LAYER_KHRONOS_validation")) {
        layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
        ext[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        if (info->gpu_assisted && snx_vk_has_instance_extension(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            ext[next++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME; /* GPU-assisted validation finds bad accesses */
            gpu_assisted = true;
        }
        out->validation = true;
    }
    out->portability = asked;
    if (info->portability && !asked && snx_vk_has_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        ext[next++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        out->portability = true;
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = info->app_name,
                             .pEngineName = "ScarletNX",
                             .apiVersion = info->api_version ? info->api_version : VK_API_VERSION_1_3};
    /* GPU-assisted validation (VK_EXT_validation_features) instruments the shaders to report out-of-bounds and
       uninitialized descriptor accesses, which the plain layer can't see: the way a GPU fault is pinned to an access.
       Only when asked: it also logs the settings it changes, and makes each run several times slower */
    VkValidationFeatureEnableEXT vf_enable = VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT;
    VkValidationFeaturesEXT vf = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                                  .enabledValidationFeatureCount = 1,
                                  .pEnabledValidationFeatures = &vf_enable};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .flags = out->portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0,
                               .pNext = gpu_assisted ? &vf : NULL,
                               .pApplicationInfo = &app,
                               .enabledLayerCount = nlayers,
                               .ppEnabledLayerNames = nlayers ? layers : NULL,
                               .enabledExtensionCount = next,
                               .ppEnabledExtensionNames = next ? ext : NULL};
    VkResult r = vkCreateInstance(&ci, NULL, &out->instance);
    free(ext);
    if (r != VK_SUCCESS) {
        *out = (SnxVkInstance){0};
        return r;
    }
    volkLoadInstance(out->instance);
    if (out->validation && info->debug) {
        VkDebugUtilsMessengerCreateInfoEXT di = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                                                 .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                                 .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                                                 .pfnUserCallback = info->debug};
        r = vkCreateDebugUtilsMessengerEXT(out->instance, &di, NULL, &out->messenger);
        if (r != VK_SUCCESS) out->messenger = VK_NULL_HANDLE; /* the instance stands without it */
    }
    return VK_SUCCESS;
}
void snx_vk_instance_free(SnxVkInstance* instance) {
    if (instance->messenger) vkDestroyDebugUtilsMessengerEXT(instance->instance, instance->messenger, NULL);
    if (instance->instance) vkDestroyInstance(instance->instance, NULL);
    *instance = (SnxVkInstance){0};
}

bool snx_vk_device_extensions(VkPhysicalDevice phys, SnxVkExtensions* out) {
    *out = (SnxVkExtensions){0};
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL) != VK_SUCCESS || !n) return false;
    out->list = calloc(n, sizeof *out->list);
    if (!out->list) return false;
    if (vkEnumerateDeviceExtensionProperties(phys, NULL, &n, out->list) < 0) {
        snx_vk_extensions_free(out);
        return false;
    }
    out->count = n;
    return true;
}
bool snx_vk_extensions_has(const SnxVkExtensions* extensions, const char* name) {
    for (uint32_t i = 0; i < extensions->count; ++i)
        if (!strcmp(extensions->list[i].extensionName, name)) return true;
    return false;
}
void snx_vk_extensions_free(SnxVkExtensions* extensions) {
    free(extensions->list);
    *extensions = (SnxVkExtensions){0};
}

VkQueueFamilyProperties* snx_vk_queue_families(VkPhysicalDevice phys, uint32_t* count) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, NULL);
    VkQueueFamilyProperties* q = n ? calloc(n, sizeof *q) : NULL;
    if (q) vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, q);
    *count = q ? n : 0;
    return q;
}
uint32_t snx_vk_queue_family(VkPhysicalDevice phys, VkQueueFlags flags, VkSurfaceKHR surface, uint32_t first) {
    uint32_t n = 0;
    VkQueueFamilyProperties* q = snx_vk_queue_families(phys, &n);
    uint32_t found = UINT32_MAX;
    for (uint32_t f = first; f < n && found == UINT32_MAX; ++f) {
        VkBool32 present = VK_TRUE; /* a query that fails is no support */
        if (surface && vkGetPhysicalDeviceSurfaceSupportKHR(phys, f, surface, &present) != VK_SUCCESS)
            present = VK_FALSE;
        if ((q[f].queueFlags & flags) == flags && present) found = f;
    }
    free(q);
    return found;
}

/* the instance's physical devices, *count of them (calloc'd, NULL for none) */
static VkPhysicalDevice* physical_devices(VkInstance instance, uint32_t* count) {
    uint32_t n = 0;
    *count = 0;
    if (vkEnumeratePhysicalDevices(instance, &n, NULL) != VK_SUCCESS || !n) return NULL;
    VkPhysicalDevice* devices = calloc(n, sizeof *devices);
    if (devices && vkEnumeratePhysicalDevices(instance, &n, devices) >= 0) {
        *count = n;
        return devices;
    }
    free(devices);
    return NULL;
}
bool snx_vk_first_device(VkInstance instance, uint32_t api_version, VkQueueFlags flags, VkSurfaceKHR surface,
                         SnxVkMake make, void* user) {
    uint32_t count = 0;
    VkPhysicalDevice* devices = physical_devices(instance, &count);
    bool made = false;
    for (uint32_t i = 0; i < count && !made; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.apiVersion < api_version) continue;
        for (uint32_t f = snx_vk_queue_family(devices[i], flags, surface, 0); f != UINT32_MAX && !made;
             f = snx_vk_queue_family(devices[i], flags, surface, f + 1))
            made = make(devices[i], f, user);
    }
    free(devices);
    return made;
}
VkPhysicalDevice snx_vk_best_device(VkInstance instance, uint32_t api_version, SnxVkScore score, void* user) {
    uint32_t count = 0;
    VkPhysicalDevice* devices = physical_devices(instance, &count);
    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_score = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.apiVersion < api_version) continue;
        int s = score(devices[i], &p, user);
        if (s > best_score) {
            best_score = s;
            best = devices[i];
        }
    }
    free(devices);
    return best;
}

VkResult snx_vk_device_create(VkPhysicalDevice phys, uint32_t family, const void* features,
                              const char* const* extensions, uint32_t extension_count, VkDevice* out) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = family,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = features,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi,
                             .enabledExtensionCount = extension_count,
                             .ppEnabledExtensionNames = extensions};
    return vkCreateDevice(phys, &di, NULL, out);
}

void* snx_vk_chain(void* const* links, size_t count) {
    void* head = NULL;
    /* from the last one back, each pointing at the one after it: pNext is written as bytes at its offset, which every
       Vulkan structure shares with VkBaseOutStructure, so no structure is accessed as another type */
    for (size_t i = count; i-- > 0;) {
        if (!links[i]) continue;
        memcpy((unsigned char*)links[i] + offsetof(VkBaseOutStructure, pNext), &head, sizeof head);
        head = links[i];
    }
    return head;
}

uint32_t snx_vk_memory_type(const VkPhysicalDeviceMemoryProperties* memory, uint32_t bits,
                            VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i)
        if ((bits >> i & 1) && (memory->memoryTypes[i].propertyFlags & properties) == properties) return i;
    return UINT32_MAX;
}
SnxVkMemoryModel snx_vk_memory_model(VkPhysicalDeviceType type, const VkPhysicalDeviceMemoryProperties* memory) {
    /* all heaps, the biggest device-local one, the biggest the CPU maps */
    uint64_t total = 0, local = 0, visible = 0;
    for (uint32_t h = 0; h < memory->memoryHeapCount; ++h) {
        total += memory->memoryHeaps[h].size;
        if ((memory->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && memory->memoryHeaps[h].size > local)
            local = memory->memoryHeaps[h].size;
    }
    for (uint32_t t = 0; t < memory->memoryTypeCount; ++t) {
        VkMemoryPropertyFlags f = memory->memoryTypes[t].propertyFlags;
        uint64_t size = memory->memoryHeaps[memory->memoryTypes[t].heapIndex].size;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && size > visible)
            visible = size;
    }
    if (type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || type == VK_PHYSICAL_DEVICE_TYPE_CPU) return SNX_VK_UMA;
    /* not a discrete GPU with resizable BAR and more memory of its own than the system's */
    if (type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && local && visible == local && local > total / 2)
        return SNX_VK_UMA;
    if (local && visible >= local / 2) return SNX_VK_REBAR;
    return SNX_VK_SMALL_BAR;
}
int snx_vk_pipeline_workers(int setting, int cpus) {
    int n = cpus - 2;
    if (setting > 0) return setting;
    /* A first run's burst of new pipelines goes to the workers side by side, and the frame waits for them (or skips
       their draws), so the game's threads have little to do meanwhile, and an idle worker costs nothing. A quarter of
       8 CPUs (two workers) froze one game's first scene for 4.6 s with its caches empty, where six took 1.7 s and
       eight 1.5 s: past six the CPUs the game's own threads use take back what more would give */
    if (n < 2) n = 2;
    if (n > 12) n = 12;
    return n;
}
/* Two workers at normal priority, the two every machine had before Auto counted the CPUs, keep the pipelines the
   frame waits for while the game plays at that pace. The rest take the CPU time the game's threads leave: all of it
   in a burst, where the frame waits and the game idles, and what the system gives after the game's threads while it
   plays */
int snx_vk_pipeline_low_workers(int workers) { return workers > 2 ? workers - 2 : 0; }

VkPipelineCache snx_vk_pipeline_cache_new(VkDevice dev, const VkPhysicalDeviceProperties* props, const void* data,
                                          size_t size, size_t* taken, char* why, size_t n) {
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    char found[256];
    *taken = 0;
    if (n) *why = 0;
    if (data && snx_vk_pipeline_cache_check(data, size, props, found, sizeof found))
        snprintf(why, n, "it %s, so the pipeline cache starts empty", found);
    else if (data) {
        ci.initialDataSize = size;
        ci.pInitialData = data;
    }
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkResult r = vkCreatePipelineCache(dev, &ci, NULL, &cache);
    if (r != VK_SUCCESS && ci.initialDataSize) {
        snprintf(why, n, "the driver refused its %zu bytes (%s), so the pipeline cache starts empty", size,
                 snx_vk_result_str(r));
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        r = vkCreatePipelineCache(dev, &ci, NULL, &cache);
    }
    if (r != VK_SUCCESS) {
        size_t at = n ? strlen(why) : 0;
        snprintf(why + at, n - at,
                 "%sthe device refuses even an empty pipeline cache (%s): pipelines are made without one",
                 at ? "; " : "", snx_vk_result_str(r));
        return VK_NULL_HANDLE;
    }
    *taken = ci.initialDataSize;
    return cache;
}
VkPipelineCache snx_vk_pipeline_cache_load(VkDevice dev, const VkPhysicalDeviceProperties* props, const char* path,
                                           size_t* loaded, char* why, size_t n) {
    PFileMap data = {0};
    char os[256] = "";
    int there = path && snx_path_exists(path);
    if (there && plat_file_map(&data, path)) snx_os_error(os, sizeof os);
    /* an empty file is checked too: what a run cut short leaves on a file system that lost the data */
    const void* bytes = !there || os[0] ? NULL : data.data ? (const void*)data.data : "";
    VkPipelineCache cache = snx_vk_pipeline_cache_new(dev, props, bytes, (size_t)data.size, loaded, why, n);
    if (os[0] && cache) snprintf(why, n, "it cannot be read (%s), so the pipeline cache starts empty", os);
    plat_file_unmap(&data); /* the cache copied it, and a mapped file can't be replaced on Windows */
    return cache;
}
int snx_vk_pipeline_cache_save(VkDevice dev, VkPipelineCache cache, const char* path, size_t* saved) {
    *saved = 0;
    size_t n = 0;
    if (!cache || vkGetPipelineCacheData(dev, cache, &n, NULL) != VK_SUCCESS || !n) return -1;
    void* data = malloc(n);
#if defined(_WIN32)
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    char* tmp = snx_format("%s.%lu.tmp", path, pid);
    int failed = !data || !tmp || vkGetPipelineCacheData(dev, cache, &n, data) != VK_SUCCESS;
    if (!failed) {
        FILE* f = fopen(tmp, "wb");
        failed = !f || fwrite(data, 1, n, f) != n;
        if (f && fclose(f)) failed = 1;
        /* the old file stays whole until this */
        if (!failed && plat_replace_file(tmp, path)) failed = 1;
        if (failed && f) remove(tmp);
    }
    if (!failed) *saved = n;
    free(tmp);
    free(data);
    return failed ? -1 : 0;
}

VkResult snx_vk_shader_module(VkDevice dev, const uint32_t* words, size_t size, VkShaderModule* out) {
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = words};
    return vkCreateShaderModule(dev, &info, NULL, out);
}
static VkResult module_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, VkShaderModule module,
                                const VkSpecializationInfo* spec, uint32_t subgroup, bool full, VkPipeline* out) {
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo size = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
        .requiredSubgroupSize = subgroup};
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .pNext = subgroup ? &size : NULL,
                  .flags = full ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main",
                  .pSpecializationInfo = spec},
        .layout = layout};
    return vkCreateComputePipelines(dev, cache, 1, &info, NULL, out);
}
VkResult snx_vk_module_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, VkShaderModule module,
                                const VkSpecializationInfo* spec, VkPipeline* out) {
    return module_pipeline(dev, cache, layout, module, spec, 0, false, out);
}
VkResult snx_vk_compute_pipeline_subgroups(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout,
                                           const uint32_t* words, size_t size, uint32_t subgroup, bool full,
                                           VkPipeline* out) {
    VkShaderModule module;
    VkResult r = snx_vk_shader_module(dev, words, size, &module);
    if (r != VK_SUCCESS) return r;
    r = module_pipeline(dev, cache, layout, module, NULL, subgroup, full, out);
    vkDestroyShaderModule(dev, module, NULL);
    return r;
}
VkResult snx_vk_compute_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, const uint32_t* words,
                                 size_t size, VkPipeline* out) {
    return snx_vk_compute_pipeline_subgroups(dev, cache, layout, words, size, 0, false, out);
}
VkResult snx_vk_compute_layouts(VkDevice dev, uint32_t buffers, uint32_t images, uint32_t push,
                                VkDescriptorSetLayout* set, VkPipelineLayout* layout) {
    VkDescriptorSetLayoutBinding bindings[SNX_VK_COMPUTE_BINDINGS];
    *set = VK_NULL_HANDLE;
    *layout = VK_NULL_HANDLE;
    if (buffers > SNX_VK_COMPUTE_BINDINGS || images > SNX_VK_COMPUTE_BINDINGS - buffers)
        return VK_ERROR_TOO_MANY_OBJECTS;
    for (uint32_t i = 0; i < buffers + images; ++i)
        bindings[i] = (VkDescriptorSetLayoutBinding){.binding = i,
                                                     .descriptorType = i < buffers ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                     .descriptorCount = 1,
                                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = buffers + images,
                                          .pBindings = bindings};
    VkResult r = vkCreateDescriptorSetLayout(dev, &si, NULL, set);
    if (r != VK_SUCCESS) {
        *set = VK_NULL_HANDLE;
        return r;
    }
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = set,
                                     .pushConstantRangeCount = push ? 1 : 0,
                                     .pPushConstantRanges = push ? &range : NULL};
    r = vkCreatePipelineLayout(dev, &li, NULL, layout);
    if (r != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(dev, *set, NULL);
        *set = VK_NULL_HANDLE;
        *layout = VK_NULL_HANDLE;
    }
    return r;
}
void snx_vk_storage_buffers(VkDevice dev, VkDescriptorSet set, const VkDescriptorBufferInfo* infos, uint32_t count) {
    VkWriteDescriptorSet writes[16];
    for (uint32_t first = 0; first < count;) { /* sixteen at a time: no program here binds more, and none is refused */
        uint32_t n = count - first < 16 ? count - first : 16;
        for (uint32_t i = 0; i < n; ++i)
            writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                               .dstSet = set,
                                               .dstBinding = first + i,
                                               .descriptorCount = 1,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                               .pBufferInfo = &infos[first + i]};
        vkUpdateDescriptorSets(dev, n, writes, 0, NULL);
        first += n;
    }
}
