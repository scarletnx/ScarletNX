/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-shaderperf's Vulkan loader: the loader library opened at run time and the functions the tool calls, one pointer
   each, so that the tool links nothing of Vulkan. gpu.c alone includes it. The pointers are static: global ones of
   Vulkan's names would be exported where the program exports its symbols (sanitizer builds), and the loader would
   bind its own lookups to them */
#ifndef SNX_SHADERPERF_VKLOAD_H
#define SNX_SHADERPERF_VKLOAD_H
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <stdbool.h>
#include <vulkan/vulkan_core.h>

#define SP_VK_GLOBAL(X)                                                                                                \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)              \
    X(vkEnumerateInstanceVersion)

#define SP_VK_INSTANCE(X)                                                                                              \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                              \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties)    \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)                                    \
    X(vkGetPhysicalDeviceImageFormatProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)            \
    X(vkGetDeviceProcAddr) X(vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR)                        \
    X(vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR) X(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)     \
    X(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT) X(vkCreateDebugUtilsMessengerEXT)                              \
    X(vkDestroyDebugUtilsMessengerEXT)

#define SP_VK_DEVICE(X)                                                                                                \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle)                   \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer)         \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage)                        \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)                  \
    X(vkCreateBufferView) X(vkDestroyBufferView) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule)      \
    X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)                          \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkCreateGraphicsPipelines)    \
    X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkCreateDescriptorPool)                \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool)          \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer)              \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)              \
    X(vkResetFences) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkResetQueryPool)         \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdDraw)              \
    X(vkCmdDrawIndexed) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdPipelineBarrier)                    \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdFillBuffer)                        \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdBeginRendering)         \
    X(vkCmdEndRendering) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdClearColorImage)                              \
    X(vkCmdClearDepthStencilImage) X(vkGetPipelineExecutablePropertiesKHR) X(vkGetPipelineExecutableStatisticsKHR)   \
    X(vkGetPipelineExecutableInternalRepresentationsKHR) X(vkAcquireProfilingLockKHR) X(vkReleaseProfilingLockKHR)   \
    X(vkGetCalibratedTimestampsKHR) X(vkGetCalibratedTimestampsEXT)

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define SP_VK_DEFINE(name) static PFN_##name name;
SP_VK_GLOBAL(SP_VK_DEFINE)
SP_VK_INSTANCE(SP_VK_DEFINE)
SP_VK_DEVICE(SP_VK_DEFINE)
#undef SP_VK_DEFINE

static PFN_vkGetInstanceProcAddr g_gipa;

/* a function pointer from the library's symbol: through a union, as ISO C has no conversion from void* */
static PFN_vkGetInstanceProcAddr symbol(void* lib) {
#ifdef _WIN32
    FARPROC p = GetProcAddress((HMODULE)lib, "vkGetInstanceProcAddr");
    PFN_vkGetInstanceProcAddr f;
    memcpy(&f, &p, sizeof f);
    return f;
#else
    void* p = dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkGetInstanceProcAddr f;
    memcpy(&f, &p, sizeof f);
    return f;
#endif
}

static bool sp_vk_load(const char* path, char* err, size_t err_size) {
#ifdef _WIN32
    static const char* const names[] = {"vulkan-1.dll"};
#elif defined(__APPLE__)
    static const char* const names[] = {"libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib"};
#else
    static const char* const names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    void* lib = NULL;
    const char* tried = path;
    for (size_t i = 0; !lib && i < (path ? 1 : sizeof names / sizeof names[0]); i++) {
        tried = path ? path : names[i];
#ifdef _WIN32
        lib = (void*)LoadLibraryA(tried);
#else
        lib = dlopen(tried, RTLD_NOW | RTLD_LOCAL);
#endif
    }
    if (!lib) {
        snprintf(err, err_size, "cannot open the Vulkan loader (%s)", tried);
        return false;
    }
    g_gipa = symbol(lib);
    if (!g_gipa) {
        snprintf(err, err_size, "%s has no vkGetInstanceProcAddr", tried);
        return false;
    }
#define SP_VK_GET(name) name = (PFN_##name)g_gipa(VK_NULL_HANDLE, #name);
    SP_VK_GLOBAL(SP_VK_GET)
#undef SP_VK_GET
    if (!vkCreateInstance) {
        snprintf(err, err_size, "the Vulkan loader gives no vkCreateInstance");
        return false;
    }
    return true;
}

static void sp_vk_load_instance(VkInstance instance) {
#define SP_VK_GET(name) name = (PFN_##name)g_gipa(instance, #name);
    SP_VK_INSTANCE(SP_VK_GET)
#undef SP_VK_GET
}

static void sp_vk_load_device(VkDevice device) {
#define SP_VK_GET(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name);
    SP_VK_DEVICE(SP_VK_GET)
#undef SP_VK_GET
}

#endif
