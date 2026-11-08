/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libvulkan as a pass-through (docs/platforms/android-apps/native.md, "Vulkan: a pass-through"). The guest calls the host's Vulkan:
   each command it names (through dlsym or vkGet*ProcAddr) is an export that calls the host loader's command of that
   name, which the loader's vkGetInstanceProcAddr gives (it dispatches by the handle, so any instance or device
   works). A guest pointer is a host pointer and the layouts agree on a little-endian LP64 host, so the structures and
   their pNext chains pass as they are. What changes:
   - the instance's surface extensions: VK_KHR_android_surface becomes the host window's (SDL's), or
     VK_EXT_headless_surface without a display,
   - vkCreateAndroidSurfaceKHR makes a surface of the host window, which ScarletNX's own renderer then gives up,
   - the guest's allocation callbacks are left out: the driver allocates for itself, at create and at destroy alike,
   - the debug messengers the guest asks for are made without their callback,
   - the device the guest sees is a Mali-G710 (ARM's vendor and driver IDs), with the host's limits and features,
   - each present blends the renderer's windows over the guest's image first, when one shows (the layer) */
#define _GNU_SOURCE
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_vulkan.h"
#include "android/bridge/hle_vulkan_host.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <volk.h>
#include <SDL3/SDL_vulkan.h>
#if defined(_WIN32)
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
#include <unistd.h>
#endif
#include "android/bridge/compose_frag_spv.h"
#include "android/bridge/compose_vert_spv.h"

/* the device's name and IDs as the guest sees them */
const char hle_gpu_name[] = "Mali-G710";
enum { ARM_VENDOR_ID = 0x13b5, ARM_DRIVER_ID = 9 /* VK_DRIVER_ID_ARM_PROPRIETARY */ };

static const HleWindowHost* window_host;
void hle_vulkan_set_host(const HleWindowHost* host) {
    window_host = host;
}

typedef struct {
    char* name;
    void* host;          /* the loader's command, resolved at the first call */
    int allocator;       /* the parameter that is a VkAllocationCallbacks*, -1 for none */
    char kind;           /* hle_call's result kind */
    const char* params;  /* hle_call's parameters, NULL for integers and pointers alone */
} Command;

static struct {
    pthread_mutex_t lock;
    bool loader;          /* volkInitialize done */
    VkInstance instance;  /* the guest's last, to resolve its commands */
    Command** commands;
    size_t count, capacity;
} v = {.lock = PTHREAD_MUTEX_INITIALIZER};

static bool loader(void) {
    if (!v.loader) v.loader = volkInitialize() == VK_SUCCESS;
    return v.loader;
}

/* the host loader's command: by the guest's instance, or global (the commands before an instance). A global command
   is always the loader's own, by no instance: with one, the loader gives NULL for it (Vulkan 1.2.193 on), and an
   app makes a second instance while its first lives */
static void* host_command(const char* name) {
    if (!loader()) return NULL;
    static const char* const globals[] = {"vkCreateInstance", "vkEnumerateInstanceExtensionProperties",
                                          "vkEnumerateInstanceLayerProperties", "vkEnumerateInstanceVersion"};
    for (size_t i = 0; i < sizeof globals / sizeof *globals; ++i)
        if (!strcmp(globals[i], name)) return (void*)vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
    return (void*)vkGetInstanceProcAddr(v.instance, name);
}

/* The parameters of the commands that take a float, in order, for hle_call (Windows passes by position): from the
   registry's commands (vulkan_core.h, Vulkan-Headers 1.4) whose parameters include a float or a double by value. No
   command takes a structure by value, and none returns a floating value */
static const char* params_of(const char* name) {
    static const struct {
        const char *name, *params;
    } floating[] = {
        {"vkCmdSetLineWidth", "IF"},
        {"vkCmdSetDepthBias", "IFFF"},
        {"vkCmdSetDepthBounds", "IFF"},
        {"vkSetDeviceMemoryPriorityEXT", "IIF"},
        {"vkCmdSetExtraPrimitiveOverestimationSizeEXT", "IF"},
    };
    for (size_t i = 0; i < sizeof floating / sizeof *floating; ++i)
        if (!strcmp(floating[i].name, name)) return floating[i].params;
    return NULL;
}

/* The parameter of a command that is a VkAllocationCallbacks*, by the registry's shapes: create (parent, info,
   allocator, out), destroy (parent, object, allocator), the instance's and the device's own, and the few others */
static int allocator_of(const char* name) {
    static const struct {
        const char* name;
        int at;
    } odd[] = {{"vkCreateInstance", 1},
               {"vkDestroyInstance", 1},
               {"vkDestroyDevice", 1},
               {"vkCreateGraphicsPipelines", 4},
               {"vkCreateComputePipelines", 4},
               {"vkCreateRayTracingPipelinesNV", 4},
               {"vkCreateRayTracingPipelinesKHR", 5},
               {"vkCreateExecutionGraphPipelinesAMDX", 4},
               {"vkCreateSharedSwapchainsKHR", 3},
               {"vkCreateShadersEXT", 3},
               {"vkCreateDisplayModeKHR", 3}};
    for (size_t i = 0; i < sizeof odd / sizeof *odd; ++i)
        if (!strcmp(odd[i].name, name)) return odd[i].at;
    if (!strncmp(name, "vkCreate", 8) || !strncmp(name, "vkDestroy", 9) || !strcmp(name, "vkAllocateMemory") ||
        !strcmp(name, "vkFreeMemory") || !strncmp(name, "vkRegister", 10))
        return 2;
    return -1;
}

/* the guest's allocator left out, its callbacks being guest code: the driver allocates for itself, at the create
   and at the destroy alike */
static void drop_allocator(CPU* c, int at) {
    if (at >= 0 && at <= 7) c->x[at] = 0;
}

/* a command the guest calls: the host's, with the guest's arguments as they are */
static void command(CPU* c, void* data) {
    Command* cmd = data;
    void* fn = __atomic_load_n(&cmd->host, __ATOMIC_ACQUIRE);
    if (!fn) {
        pthread_mutex_lock(&v.lock);
        fn = host_command(cmd->name);
        __atomic_store_n(&cmd->host, fn, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&v.lock);
    }
    if (!fn) {
        fprintf(stderr, "scarletnx-android: Vulkan: %s is not in the host's Vulkan\n", cmd->name);
        X(0) = (uint64_t)(int64_t)VK_ERROR_EXTENSION_NOT_PRESENT;
        return;
    }
    drop_allocator(c, cmd->allocator);
    hle_call_wide(c, fn, cmd->kind, cmd->params);
}

/* ---- the commands that change ---- */

static void create_instance(CPU* c, void* data);
static void enumerate_instance_extensions(CPU* c, void* data);
static void create_android_surface(CPU* c, void* data);
static void physical_device_properties(CPU* c, void* data);
static void physical_device_properties2(CPU* c, void* data);
static void get_proc_addr(CPU* c, void* data);
static void debug_messenger(CPU* c, void* data);
static void debug_messenger_destroy(CPU* c, void* data);
static void surface_capabilities(CPU* c, void* data);
static void surface_capabilities2(CPU* c, void* data);
static void surface_support(CPU* c, void* data);
static void queue_present(CPU* c, void* data);
static void create_device(CPU* c, void* data);
static void get_device_queue(CPU* c, void* data);
static void get_device_queue2(CPU* c, void* data);
static void create_swapchain(CPU* c, void* data);
static void get_swapchain_images(CPU* c, void* data);
static void create_shader_module(CPU* c, void* data);
static void cmd_clear_attachments(CPU* c, void* data);
static void create_render_pass(CPU* c, void* data);
static void create_render_pass2(CPU* c, void* data);
static void destroy_swapchain(CPU* c, void* data);
static void destroy_device(CPU* c, void* data);

static const struct {
    const char* name;
    BridgeHandler handler;
} special[] = {
    {"vkGetInstanceProcAddr", get_proc_addr},
    {"vkGetDeviceProcAddr", get_proc_addr},
    {"vkCreateInstance", create_instance},
    {"vkEnumerateInstanceExtensionProperties", enumerate_instance_extensions},
    {"vkCreateAndroidSurfaceKHR", create_android_surface},
    {"vkGetPhysicalDeviceProperties", physical_device_properties},
    {"vkGetPhysicalDeviceProperties2", physical_device_properties2},
    {"vkGetPhysicalDeviceProperties2KHR", physical_device_properties2},
    {"vkCreateDebugUtilsMessengerEXT", debug_messenger},
    {"vkCreateDebugReportCallbackEXT", debug_messenger},
    {"vkDestroyDebugUtilsMessengerEXT", debug_messenger_destroy},
    {"vkDestroyDebugReportCallbackEXT", debug_messenger_destroy},
    {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", surface_capabilities},
    {"vkGetPhysicalDeviceSurfaceCapabilities2KHR", surface_capabilities2},
    {"vkGetPhysicalDeviceSurfaceSupportKHR", surface_support},
    {"vkQueuePresentKHR", queue_present},
    {"vkCreateDevice", create_device},
    {"vkGetDeviceQueue", get_device_queue},
    {"vkGetDeviceQueue2", get_device_queue2},
    {"vkCreateSwapchainKHR", create_swapchain},
    {"vkDestroySwapchainKHR", destroy_swapchain},
    {"vkDestroyDevice", destroy_device},
    {"vkGetSwapchainImagesKHR", get_swapchain_images},
    {"vkCreateShaderModule", create_shader_module},
    {"vkCmdClearAttachments", cmd_clear_attachments},
    {"vkCreateRenderPass", create_render_pass},
    {"vkCreateRenderPass2", create_render_pass2},
    {"vkCreateRenderPass2KHR", create_render_pass2},
};

uint64_t hle_vulkan_symbol(const char* name) {
    if (strncmp(name, "vk", 2)) return 0;
    for (size_t i = 0; i < sizeof special / sizeof *special; ++i)
        if (!strcmp(special[i].name, name)) {
            char* export = snx_format("libvulkan::%s", name);
            uint64_t a = bridge_export(export, special[i].handler, (void*)special[i].name);
            free(export);
            return a;
        }
    pthread_mutex_lock(&v.lock);
    Command* cmd = NULL;
    for (size_t i = 0; i < v.count && !cmd; ++i)
        if (!strcmp(v.commands[i]->name, name)) cmd = v.commands[i];
    if (!cmd) {
        if (v.count == v.capacity) {
            v.capacity = v.capacity ? v.capacity * 2 : 512;
            v.commands = snx_realloc(v.commands, v.capacity * sizeof *v.commands);
        }
        cmd = snx_alloc(sizeof *cmd);
        cmd->name = snx_format("%s", name);
        cmd->host = NULL;
        cmd->allocator = allocator_of(name);
        cmd->kind = 'I'; /* no Vulkan command returns a floating value */
        cmd->params = params_of(name);
        v.commands[v.count++] = cmd;
    }
    pthread_mutex_unlock(&v.lock);
    char* export = snx_format("libvulkan::%s", name);
    uint64_t a = bridge_export(export, command, cmd);
    free(export);
    return a;
}

/* vkGetInstanceProcAddr and vkGetDeviceProcAddr: the export of the name, NULL for a command the host lacks */
static void get_proc_addr(CPU* c, void* data) {
    (void)data;
    const char* name = STR(1);
    if (!name) {
        X(0) = 0;
        return;
    }
    bool known = false;
    for (size_t i = 0; i < sizeof special / sizeof *special && !known; ++i) known = !strcmp(special[i].name, name);
    pthread_mutex_lock(&v.lock);
    known = known || host_command(name) != NULL;
    pthread_mutex_unlock(&v.lock);
    X(0) = known ? hle_vulkan_symbol(name) : 0;
}

/* ---- the instance ---- */

static bool host_has(const char* extension) {
    uint32_t n = 0;
    if (!loader() || vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS) return false;
    VkExtensionProperties* list = calloc(n ? n : 1, sizeof *list);
    bool found = false;
    if (list && vkEnumerateInstanceExtensionProperties(NULL, &n, list) == VK_SUCCESS)
        for (uint32_t i = 0; i < n && !found; ++i) found = !strcmp(list[i].extensionName, extension);
    free(list);
    return found;
}

/* the host window's surface extensions, or the headless surface's: what VK_KHR_android_surface becomes */
static const char* const* window_extensions(uint32_t* count) {
    static const char* headless[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    void* window = window_host ? window_host->window() : NULL;
    if (window) {
        Uint32 n = 0;
        const char* const* list = SDL_Vulkan_GetInstanceExtensions(&n);
        if (list) {
            *count = n;
            return list;
        }
    }
    *count = 2;
    return headless;
}

VkResult hle_vulkan_create_instance(const VkInstanceCreateInfo* in, VkInstance* out) {
    if (!loader()) return VK_ERROR_INITIALIZATION_FAILED;
    VkInstanceCreateInfo info = *in;
    /* the extensions: the android surface becomes the host window's, the others stay when the host has them */
    const char** names = calloc(info.enabledExtensionCount + 8, sizeof *names);
    uint32_t n = 0;
    bool surface = false;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
        const char* e = info.ppEnabledExtensionNames[i];
        if (!strcmp(e, "VK_KHR_android_surface")) {
            surface = true;
            continue;
        }
        if (host_has(e)) names[n++] = e;
        else fprintf(stderr, "scarletnx-android: Vulkan: the instance extension %s is not the host's: left out\n", e);
    }
    if (surface) {
        uint32_t count;
        const char* const* list = window_extensions(&count);
        for (uint32_t i = 0; i < count; ++i) {
            bool have = false;
            for (uint32_t k = 0; k < n && !have; ++k) have = !strcmp(names[k], list[i]);
            if (!have) names[n++] = list[i];
        }
    }
    info.enabledExtensionCount = n;
    info.ppEnabledExtensionNames = names;
    /* the debug callbacks in the chain are the guest's code: left out, as the messengers are */
    VkBaseOutStructure* prev = NULL;
    for (VkBaseOutStructure* s = (VkBaseOutStructure*)(uintptr_t)info.pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT ||
            s->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) {
            if (prev) prev->pNext = s->pNext;
            else info.pNext = s->pNext;
        } else {
            prev = s;
        }
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&info, NULL, &instance);
    free(names);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&v.lock);
        v.instance = instance;
        /* the commands resolved for another instance resolve again for this one */
        for (size_t i = 0; i < v.count; ++i) __atomic_store_n(&v.commands[i]->host, NULL, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&v.lock);
        memcpy(out, &instance, sizeof instance);
    }
    return r;
}
static void create_instance(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)hle_vulkan_create_instance(PTR(0), PTR(2));
}

/* the host's instance extensions, with VK_KHR_android_surface, as a phone lists it */
static void enumerate_instance_extensions(CPU* c, void* data) {
    (void)data;
    uint32_t* count = PTR(1);
    VkExtensionProperties* out = PTR(2);
    uint32_t n = 0;
    if (!loader() || vkEnumerateInstanceExtensionProperties(STR(0), &n, NULL) != VK_SUCCESS) {
        X(0) = (uint64_t)(int64_t)VK_ERROR_LAYER_NOT_PRESENT;
        return;
    }
    VkExtensionProperties* list = calloc(n + 1, sizeof *list);
    vkEnumerateInstanceExtensionProperties(STR(0), &n, list);
    if (!X(0)) {
        snprintf(list[n].extensionName, sizeof list[n].extensionName, "VK_KHR_android_surface");
        list[n++].specVersion = 6;
    }
    VkResult r = VK_SUCCESS;
    if (!out) {
        *count = n;
    } else {
        uint32_t copy = *count < n ? *count : n;
        memcpy(out, list, copy * sizeof *out);
        *count = copy;
        if (copy < n) r = VK_INCOMPLETE;
    }
    free(list);
    X(0) = (uint64_t)(int64_t)r;
}

/* vkCreateAndroidSurfaceKHR(instance, info, allocator, surface): the host window's surface on the guest's instance,
   or a headless one without a display */
VkResult hle_vulkan_create_surface(VkInstance instance, VkSurfaceKHR* out) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    void* window = window_host ? window_host->window() : NULL;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    /* ScarletNX's renderer gives the window up (a window has one swapchain at a time), and its frames from then on
       go into the layer that the presents blend, with a window or without */
    if (window_host) window_host->yield();
    if (window) {
        r = SDL_Vulkan_CreateSurface(window, instance, NULL, &surface) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    } else {
        PFN_vkCreateHeadlessSurfaceEXT make =
            (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT");
        VkHeadlessSurfaceCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        if (make) r = make(instance, &info, NULL, &surface);
    }
    if (r == VK_SUCCESS) memcpy(out, &surface, sizeof surface);
    return r;
}
/* the headless surfaces of SurfaceTextures, the last few: presentable from any graphics queue, as Android's are */
enum { TEXTURE_SURFACES = 8 };
static VkSurfaceKHR texture_surfaces[TEXTURE_SURFACES];
static unsigned texture_next;
static bool texture_surface(VkSurfaceKHR surface) {
    bool found = false;
    pthread_mutex_lock(&v.lock);
    for (unsigned i = 0; i < TEXTURE_SURFACES && !found; ++i) found = surface && texture_surfaces[i] == surface;
    pthread_mutex_unlock(&v.lock);
    return found;
}
static void create_android_surface(CPU* c, void* data) {
    (void)data;
    /* VkAndroidSurfaceCreateInfoKHR: the window at 24. A SurfaceTexture's shows nowhere: a headless surface, and
       the renderer keeps the host window */
    uint64_t window = 0;
    if (X(1)) memcpy(&window, (const uint8_t*)(uintptr_t)X(1) + 24, sizeof window);
    if (hle_android_is_texture_window(window)) {
        VkInstance instance = (VkInstance)(uintptr_t)X(0);
        PFN_vkCreateHeadlessSurfaceEXT make =
            (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT");
        VkHeadlessSurfaceCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult r = make ? make(instance, &info, NULL, &surface) : VK_ERROR_INITIALIZATION_FAILED;
        if (r == VK_SUCCESS) {
            memcpy(PTR(3), &surface, sizeof surface);
            pthread_mutex_lock(&v.lock);
            texture_surfaces[texture_next++ % TEXTURE_SURFACES] = surface;
            pthread_mutex_unlock(&v.lock);
        }
        X(0) = (uint64_t)(int64_t)r;
        return;
    }
    X(0) = (uint64_t)(int64_t)hle_vulkan_create_surface((VkInstance)(uintptr_t)X(0), PTR(3));
}

/* An Android surface's current extent is its window's buffers, always: engines size the swapchain by it. A host
   surface that leaves the size to the swapchain (Wayland's, a headless one: 0xFFFFFFFF) gets the native window's, in
   the surface's bounds */
static void window_extent(VkSurfaceCapabilitiesKHR* caps) {
    if (caps->currentExtent.width != UINT32_MAX) return;
    int width = 0, height = 0;
    hle_android_window(&width, &height);
    if (width <= 0 || height <= 0) return;
    uint32_t w = (uint32_t)width, h = (uint32_t)height;
    if (w < caps->minImageExtent.width) w = caps->minImageExtent.width;
    if (w > caps->maxImageExtent.width) w = caps->maxImageExtent.width;
    if (h < caps->minImageExtent.height) h = caps->minImageExtent.height;
    if (h > caps->maxImageExtent.height) h = caps->maxImageExtent.height;
    caps->currentExtent = (VkExtent2D){w, h};
}
VkResult hle_vulkan_surface_capabilities(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                         VkSurfaceCapabilitiesKHR* caps) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        vkGetInstanceProcAddr(v.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkResult r = get ? get(physical, surface, caps) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (r == VK_SUCCESS) window_extent(caps);
    return r;
}
static void surface_capabilities(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)hle_vulkan_surface_capabilities((VkPhysicalDevice)(uintptr_t)X(0),
                                                              (VkSurfaceKHR)(uintptr_t)X(1), PTR(2));
}
/* vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, supported): the host's answer, but a SurfaceTexture's
   headless surface is presentable from a graphics family (a driver without headless presentation says no: the GPU's
   probe of an app then finds no present queue) */
static void surface_support(CPU* c, void* data) {
    (void)data;
    VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)X(0);
    uint32_t family = (uint32_t)X(1);
    VkSurfaceKHR surface = (VkSurfaceKHR)(uintptr_t)X(2);
    VkBool32* supported = PTR(3);
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR get =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)vkGetInstanceProcAddr(v.instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    VkResult r = get ? get(physical, family, surface, supported) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (texture_surface(surface) && (r != VK_SUCCESS || !*supported)) {
        PFN_vkGetPhysicalDeviceQueueFamilyProperties families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            vkGetInstanceProcAddr(v.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t n = 0;
        if (families) families(physical, &n, NULL);
        VkQueueFamilyProperties* list = n ? calloc(n, sizeof *list) : NULL;
        if (list) families(physical, &n, list);
        *supported = list && family < n && (list[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? VK_TRUE : VK_FALSE;
        free(list);
        r = VK_SUCCESS;
    }
    X(0) = (uint64_t)(int64_t)r;
}
static void surface_capabilities2(CPU* c, void* data) {
    (void)data;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR get = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
        vkGetInstanceProcAddr(v.instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    VkResult r = get ? get((VkPhysicalDevice)(uintptr_t)X(0), PTR(1), PTR(2)) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (r == VK_SUCCESS) window_extent(&((VkSurfaceCapabilities2KHR*)PTR(2))->surfaceCapabilities);
    X(0) = (uint64_t)(int64_t)r;
}

/* ---- the devices, queues and swapchains the guest made, for --shot and the layer ----
   The pass-through records what a copy of a presented image or a blend over it needs: each device's physical device,
   each queue's device and family, each swapchain's device, format, extent and images (made with TRANSFER_SRC as
   well, when shots are asked for, and COLOR_ATTACHMENT always). At a listed present, a submission on the presenting
   queue waits for the present's semaphores, copies the image into a host-visible buffer and signals a semaphore of
   its own, which the present then waits for */

enum { SWAP_IMAGES = 16 };
static struct {
    pthread_mutex_t lock;
    char path[1024];
    long frames[64];
    size_t frame_count;
    struct {
        VkDevice device;
        VkPhysicalDevice physical;
        bool importable; /* HLE_LAYER_EXTENSION enabled on it: the layer can be imported */
    } devices[8];
    size_t device_count;
    struct {
        VkQueue queue;
        VkDevice device;
        uint32_t family;
    } queues[32];
    size_t queue_count;
    struct {
        VkSwapchainKHR swapchain; /* VK_NULL_HANDLE: a free entry */
        VkDevice device;
        VkFormat format;
        VkExtent2D extent;
        VkImage images[SWAP_IMAGES];
        uint32_t image_count;
        /* the layer's blend into each image (compose_begin): its view and framebuffer, and the semaphore the
           present waits for, which stays with the entry until the device goes */
        VkImageView views[SWAP_IMAGES];
        VkFramebuffer framebuffers[SWAP_IMAGES];
        VkSemaphore composed[SWAP_IMAGES];
    } swapchains[8];
    size_t swapchain_count;
} shot = {.lock = PTHREAD_MUTEX_INITIALIZER};

void hle_vulkan_set_shot(const char* path, const char* frames) {
    if (!path || !path[0]) return;
    snprintf(shot.path, sizeof shot.path, "%s", path);
    if (!frames || !frames[0]) frames = "60";
    for (char* end; *frames && shot.frame_count < 64; frames = *end ? end + 1 : end) {
        long f = strtol(frames, &end, 10);
        if (end == frames) break;
        shot.frames[shot.frame_count++] = f;
    }
}

static void* vk(const char* name) {
    return (void*)vkGetInstanceProcAddr(v.instance, name);
}
/* a command of the guest's instance, as its PFN type */
#define VKF(name) ((PFN_##name)vk(#name))

/* whether the physical device lists a device extension */
static bool device_has(VkPhysicalDevice physical, const char* name) {
    PFN_vkEnumerateDeviceExtensionProperties list = VKF(vkEnumerateDeviceExtensionProperties);
    uint32_t n = 0;
    if (!list || list(physical, NULL, &n, NULL) != VK_SUCCESS) return false;
    VkExtensionProperties* all = calloc(n ? n : 1, sizeof *all);
    bool found = false;
    if (all && list(physical, NULL, &n, all) == VK_SUCCESS)
        for (uint32_t i = 0; i < n && !found; ++i) found = !strcmp(all[i].extensionName, name);
    free(all);
    return found;
}

/* vkCreateDevice: the guest's device, with the layer's import extension (HLE_LAYER_EXTENSION) added when the host's
   device has it, which changes nothing else of the device */
VkResult hle_vulkan_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* in, VkDevice* out) {
    PFN_vkCreateDevice create = VKF(vkCreateDevice);
    VkDeviceCreateInfo info = *in;
    const char** names = calloc(info.enabledExtensionCount + 1, sizeof *names);
    bool importable = false;
    if (names && HLE_LAYER_MEMORY) {
        for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
            names[i] = info.ppEnabledExtensionNames[i];
            importable |= !strcmp(names[i], HLE_LAYER_EXTENSION);
        }
        if (!importable && device_has(physical, HLE_LAYER_EXTENSION)) {
            names[info.enabledExtensionCount++] = HLE_LAYER_EXTENSION;
            info.ppEnabledExtensionNames = names;
            importable = true;
        }
    }
    VkResult r = create ? create(physical, &info, NULL, out) : VK_ERROR_INITIALIZATION_FAILED;
    free(names);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&shot.lock);
        if (shot.device_count < 8) {
            shot.devices[shot.device_count].device = *out;
            shot.devices[shot.device_count].importable = importable;
            shot.devices[shot.device_count++].physical = physical;
        }
        pthread_mutex_unlock(&shot.lock);
    }
    return r;
}
static void create_device(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)hle_vulkan_create_device((VkPhysicalDevice)(uintptr_t)X(0), PTR(1), PTR(3));
}
static void record_queue(VkDevice device, uint32_t family, VkQueue queue) {
    pthread_mutex_lock(&shot.lock);
    bool known = false;
    for (size_t i = 0; i < shot.queue_count && !known; ++i) known = shot.queues[i].queue == queue;
    if (!known && shot.queue_count < 32) {
        shot.queues[shot.queue_count].queue = queue;
        shot.queues[shot.queue_count].device = device;
        shot.queues[shot.queue_count++].family = family;
    }
    pthread_mutex_unlock(&shot.lock);
}
void hle_vulkan_get_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue* out) {
    PFN_vkGetDeviceQueue get = VKF(vkGetDeviceQueue);
    if (!get) return;
    get(device, family, index, out);
    record_queue(device, family, *out);
}
static void get_device_queue(CPU* c, void* data) {
    (void)data;
    hle_vulkan_get_queue((VkDevice)(uintptr_t)X(0), (uint32_t)X(1), (uint32_t)X(2), PTR(3));
}
static void get_device_queue2(CPU* c, void* data) {
    (void)data;
    PFN_vkGetDeviceQueue2 get = VKF(vkGetDeviceQueue2);
    if (!get) return;
    const VkDeviceQueueInfo2* info = PTR(1);
    get((VkDevice)(uintptr_t)X(0), info, PTR(2));
    record_queue((VkDevice)(uintptr_t)X(0), info->queueFamilyIndex, *(VkQueue*)PTR(2));
}

/* ---- the layer: the renderer's windows blended over each present (hle_vulkan.h's HleLayer) ----
   The objects are the guest's device's, made at the first blend: the layer's images imported from the renderer's
   memory, a sampler, a pipeline for each swapchain format, and a few slots of a command buffer and a fence. A slot
   reads one layer image until its fence is signaled (hle_vulkan_layer_reading). compose_lock orders the objects,
   the slots and their fences against the renderer's thread, and is taken before shot.lock and the renderer's layer
   lock. The entries of shot.swapchains are written with both held */

enum { COMPOSE_SLOTS = 4, COMPOSE_TARGETS = 4 };
typedef struct {
    uint64_t generation; /* the renderer's image it imports, 0 for none */
    VkImage image;
    VkDeviceMemory memory;
    VkImageView views[2];    /* as the renderer's format, and as its sRGB form (for an sRGB swapchain) */
    VkDescriptorSet sets[2]; /* the views' */
} ComposeLayer;
static pthread_mutex_t compose_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    VkDevice device; /* the device the objects are on, VK_NULL_HANDLE before the first blend */
    bool failed;     /* that device can't blend the layer: not tried again */
    uint32_t family;
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkCommandPool commands;
    VkShaderModule vert, frag;
    ComposeLayer layers[HLE_LAYER_IMAGES];
    struct {
        VkFormat format;
        VkRenderPass pass;
        VkPipeline pipeline;
    } targets[COMPOSE_TARGETS];
    unsigned target_count;
    struct {
        VkCommandBuffer cb;
        VkFence fence;
        int layer; /* the layer image it reads, -1 for none */
    } slots[COMPOSE_SLOTS];
    unsigned next;
} compose;

static bool srgb_format(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

/* the slots' reads done so far: their fences signaled. All of them, waiting, when all is set */
static void compose_retire(bool all) {
    for (unsigned i = 0; i < COMPOSE_SLOTS; ++i) {
        if (compose.slots[i].layer < 0 || !compose.slots[i].fence) continue;
        VkFence f = compose.slots[i].fence;
        if (all) VKF(vkWaitForFences)(compose.device, 1, &f, VK_TRUE, UINT64_MAX);
        if (VKF(vkGetFenceStatus)(compose.device, f) == VK_SUCCESS) compose.slots[i].layer = -1;
    }
}

bool hle_vulkan_layer_reading(unsigned index) {
    pthread_mutex_lock(&compose_lock);
    bool reading = false;
    if (compose.device) {
        compose_retire(false);
        for (unsigned i = 0; i < COMPOSE_SLOTS; ++i) reading |= compose.slots[i].layer == (int)index;
    }
    pthread_mutex_unlock(&compose_lock);
    return reading;
}

static void compose_free_layer(unsigned i) {
    VkDevice d = compose.device;
    for (unsigned k = 0; k < 2; ++k)
        if (compose.layers[i].views[k]) VKF(vkDestroyImageView)(d, compose.layers[i].views[k], NULL);
    if (compose.layers[i].image) VKF(vkDestroyImage)(d, compose.layers[i].image, NULL);
    if (compose.layers[i].memory) VKF(vkFreeMemory)(d, compose.layers[i].memory, NULL);
    ComposeLayer cleared = {.sets = {compose.layers[i].sets[0], compose.layers[i].sets[1]}};
    compose.layers[i] = cleared;
}

/* a swapchain entry's views and framebuffers gone (its swapchain replaced or destroyed), once no blend uses them */
static void compose_forget_swapchain(size_t entry) {
    if (!compose.device || shot.swapchains[entry].device != compose.device) return;
    compose_retire(true);
    for (uint32_t i = 0; i < SWAP_IMAGES; ++i) {
        if (shot.swapchains[entry].framebuffers[i])
            VKF(vkDestroyFramebuffer)(compose.device, shot.swapchains[entry].framebuffers[i], NULL);
        if (shot.swapchains[entry].views[i])
            VKF(vkDestroyImageView)(compose.device, shot.swapchains[entry].views[i], NULL);
        shot.swapchains[entry].framebuffers[i] = VK_NULL_HANDLE;
        shot.swapchains[entry].views[i] = VK_NULL_HANDLE;
    }
}

/* everything of the blend on its device gone: the device is about to be destroyed, and idle */
static void compose_free(void) {
    VkDevice d = compose.device;
    if (!d) return;
    for (size_t e = 0; e < shot.swapchain_count; ++e) {
        if (shot.swapchains[e].device != d) continue;
        compose_forget_swapchain(e);
        for (uint32_t i = 0; i < SWAP_IMAGES; ++i)
            if (shot.swapchains[e].composed[i]) VKF(vkDestroySemaphore)(d, shot.swapchains[e].composed[i], NULL);
        memset(shot.swapchains[e].composed, 0, sizeof shot.swapchains[e].composed);
    }
    for (unsigned i = 0; i < HLE_LAYER_IMAGES; ++i) compose_free_layer(i);
    for (unsigned i = 0; i < compose.target_count; ++i) {
        if (compose.targets[i].pipeline) VKF(vkDestroyPipeline)(d, compose.targets[i].pipeline, NULL);
        if (compose.targets[i].pass) VKF(vkDestroyRenderPass)(d, compose.targets[i].pass, NULL);
    }
    for (unsigned i = 0; i < COMPOSE_SLOTS; ++i)
        if (compose.slots[i].fence) VKF(vkDestroyFence)(d, compose.slots[i].fence, NULL);
    if (compose.commands) VKF(vkDestroyCommandPool)(d, compose.commands, NULL);
    if (compose.pool) VKF(vkDestroyDescriptorPool)(d, compose.pool, NULL);
    if (compose.layout) VKF(vkDestroyPipelineLayout)(d, compose.layout, NULL);
    if (compose.set_layout) VKF(vkDestroyDescriptorSetLayout)(d, compose.set_layout, NULL);
    if (compose.sampler) VKF(vkDestroySampler)(d, compose.sampler, NULL);
    if (compose.vert) VKF(vkDestroyShaderModule)(d, compose.vert, NULL);
    if (compose.frag) VKF(vkDestroyShaderModule)(d, compose.frag, NULL);
    memset(&compose, 0, sizeof compose);
    for (unsigned i = 0; i < COMPOSE_SLOTS; ++i) compose.slots[i].layer = -1;
}

static VkShaderModule compose_shader(const u32* words, u64 size) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)size, words};
    VkShaderModule m = VK_NULL_HANDLE;
    VKF(vkCreateShaderModule)(compose.device, &ci, NULL, &m);
    return m;
}

/* The objects of the blend on device, whose physical device must be the renderer's (an opaque handle imports only
   there): false when the device can't blend the layer */
static bool compose_setup(VkDevice device, VkPhysicalDevice physical, uint32_t family, const HleLayer* layer) {
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    PFN_vkGetPhysicalDeviceProperties2 props = VKF(vkGetPhysicalDeviceProperties2);
    if (!props) return false;
    props(physical, &p);
    if (p.properties.apiVersion < VK_API_VERSION_1_1 || memcmp(id.deviceUUID, layer->device_uuid, 16) ||
        memcmp(id.driverUUID, layer->driver_uuid, 16)) {
        fprintf(stderr, "scarletnx-android: Vulkan: the app's device is not the renderer's: no windows over it\n");
        return false;
    }
    compose.device = device;
    compose.family = family;
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                            VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo sli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1,
                                           .pBindings = &binding};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * HLE_LAYER_IMAGES};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 2 * HLE_LAYER_IMAGES,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &size};
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = family};
    bool ok = VKF(vkCreateSampler)(device, &sci, NULL, &compose.sampler) == VK_SUCCESS &&
              VKF(vkCreateDescriptorSetLayout)(device, &sli, NULL, &compose.set_layout) == VK_SUCCESS &&
              VKF(vkCreateDescriptorPool)(device, &dpi, NULL, &compose.pool) == VK_SUCCESS &&
              VKF(vkCreateCommandPool)(device, &cpi, NULL, &compose.commands) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &compose.set_layout};
    ok = ok && VKF(vkCreatePipelineLayout)(device, &li, NULL, &compose.layout) == VK_SUCCESS;
    for (unsigned i = 0; i < HLE_LAYER_IMAGES && ok; ++i) {
        VkDescriptorSetLayout layouts[2] = {compose.set_layout, compose.set_layout};
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = compose.pool,
                                          .descriptorSetCount = 2,
                                          .pSetLayouts = layouts};
        ok = VKF(vkAllocateDescriptorSets)(device, &ai, compose.layers[i].sets) == VK_SUCCESS;
    }
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (unsigned i = 0; i < COMPOSE_SLOTS && ok; ++i) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = compose.commands,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        compose.slots[i].layer = -1;
        ok = VKF(vkAllocateCommandBuffers)(device, &ai, &compose.slots[i].cb) == VK_SUCCESS &&
             VKF(vkCreateFence)(device, &fci, NULL, &compose.slots[i].fence) == VK_SUCCESS;
    }
    if (ok) {
        compose.vert = compose_shader(android_compose_vert_spv, android_compose_vert_spv_size);
        compose.frag = compose_shader(android_compose_frag_spv, android_compose_frag_spv_size);
        ok = compose.vert && compose.frag;
    }
    if (!ok) fprintf(stderr, "scarletnx-android: Vulkan: the blend of the windows can't be made on the app's device\n");
    return ok;
}

/* the renderer's layer image imported on the guest's device, when it is not that generation's yet */
static bool compose_import(VkPhysicalDevice physical, const HleLayer* layer) {
    unsigned i = layer->index;
    if (compose.layers[i].generation == layer->generation) return true;
    /* the older image's blends done first */
    compose_retire(true);
    compose_free_layer(i);
    VkDevice d = compose.device;
    VkExternalMemoryImageCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                                .handleTypes = HLE_LAYER_MEMORY};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .pNext = &external,
                            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = (VkFormat)layer->format,
                            .extent = {layer->width, layer->height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (VKF(vkCreateImage)(d, &ii, NULL, &compose.layers[i].image) != VK_SUCCESS) return false;
    VkMemoryRequirements need;
    VKF(vkGetImageMemoryRequirements)(d, compose.layers[i].image, &need);
    VkPhysicalDeviceMemoryProperties props;
    VKF(vkGetPhysicalDeviceMemoryProperties)(physical, &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < props.memoryTypeCount && type == UINT32_MAX; ++t)
        if ((need.memoryTypeBits >> t & 1) &&
            (props.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = t;
    intptr_t handle = type == UINT32_MAX ? -1 : window_host->layer_handle(i, layer->generation);
    if (handle == -1) return false;
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                               .image = compose.layers[i].image};
#if defined(_WIN32)
    VkImportMemoryWin32HandleInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                                               .pNext = &dedicated,
                                               .handleType = HLE_LAYER_MEMORY,
                                               .handle = (HANDLE)handle};
#else
    VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                      .pNext = &dedicated,
                                      .handleType = HLE_LAYER_MEMORY,
                                      .fd = (int)handle};
#endif
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &import,
                               .allocationSize = layer->size,
                               .memoryTypeIndex = type};
    VkResult r = VKF(vkAllocateMemory)(d, &ai, NULL, &compose.layers[i].memory);
#if defined(_WIN32)
    CloseHandle((HANDLE)handle); /* an import of a Win32 handle doesn't own it */
#else
    if (r != VK_SUCCESS) close((int)handle); /* an import of an fd owns it once it succeeds */
#endif
    if (r != VK_SUCCESS || VKF(vkBindImageMemory)(d, compose.layers[i].image, compose.layers[i].memory, 0)) {
        compose_free_layer(i);
        return false;
    }
    VkFormat formats[2] = {(VkFormat)layer->format, (VkFormat)layer->format == VK_FORMAT_B8G8R8A8_UNORM
                                                         ? VK_FORMAT_B8G8R8A8_SRGB
                                                         : VK_FORMAT_R8G8B8A8_SRGB};
    for (unsigned k = 0; k < 2; ++k) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = compose.layers[i].image,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = formats[k],
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (VKF(vkCreateImageView)(d, &vi, NULL, &compose.layers[i].views[k]) != VK_SUCCESS) {
            compose_free_layer(i);
            return false;
        }
        VkDescriptorImageInfo image = {compose.sampler, compose.layers[i].views[k], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = compose.layers[i].sets[k],
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &image};
        VKF(vkUpdateDescriptorSets)(d, 1, &w, 0, NULL);
    }
    compose.layers[i].generation = layer->generation;
    return true;
}

/* the render pass and pipeline that blend into a swapchain image of format: it keeps what the image holds
   (LOAD_OP_LOAD), and leaves it in PRESENT_SRC. NULL when they can't be made */
static unsigned compose_target(VkFormat format) {
    for (unsigned i = 0; i < compose.target_count; ++i)
        if (compose.targets[i].format == format) return i;
    if (compose.target_count == COMPOSE_TARGETS) return UINT32_MAX;
    VkDevice d = compose.device;
    VkAttachmentDescription attachment = {0,
                                          format,
                                          VK_SAMPLE_COUNT_1_BIT,
                                          VK_ATTACHMENT_LOAD_OP_LOAD,
                                          VK_ATTACHMENT_STORE_OP_STORE,
                                          VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    /* after the present's semaphores (waited at COLOR_ATTACHMENT_OUTPUT), and before the present */
    VkSubpassDependency deps[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, 0}};
    VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .attachmentCount = 1,
                                  .pAttachments = &attachment,
                                  .subpassCount = 1,
                                  .pSubpasses = &subpass,
                                  .dependencyCount = 2,
                                  .pDependencies = deps};
    VkRenderPass pass = VK_NULL_HANDLE;
    if (VKF(vkCreateRenderPass)(d, &rpi, NULL, &pass) != VK_SUCCESS) return UINT32_MAX;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = compose.vert,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = compose.frag,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    /* premultiplied: the layer's color, and the present's where the layer lets it through */
    VkPipelineColorBlendAttachmentState blend = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blending = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = 1,
                                                    .pAttachments = &blend};
    VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamics = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                 .dynamicStateCount = 2,
                                                 .pDynamicStates = dynamic};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &input,
                                       .pInputAssemblyState = &assembly,
                                       .pViewportState = &viewport,
                                       .pRasterizationState = &raster,
                                       .pMultisampleState = &multisample,
                                       .pColorBlendState = &blending,
                                       .pDynamicState = &dynamics,
                                       .layout = compose.layout,
                                       .renderPass = pass};
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (VKF(vkCreateGraphicsPipelines)(d, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) != VK_SUCCESS) {
        VKF(vkDestroyRenderPass)(d, pass, NULL);
        return UINT32_MAX;
    }
    unsigned t = compose.target_count++;
    compose.targets[t].format = format;
    compose.targets[t].pass = pass;
    compose.targets[t].pipeline = pipeline;
    return t;
}

/* The layer blended over the image info presents, when a window of the renderer shows over the guest's surface: a
   submission on the presenting queue that waits for the present's semaphores and signals *done, which the present
   waits for instead. False when nothing is blended, and the present goes as the guest made it */
static bool compose_begin(VkQueue queue, const VkPresentInfoKHR* info, VkSemaphore* done) {
    if (!window_host || !window_host->layer || info->swapchainCount != 1 || !HLE_LAYER_MEMORY) return false;
    pthread_mutex_lock(&compose_lock);
    HleLayer layer;
    bool ok = window_host->layer(&layer);
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0;
    bool importable = false;
    size_t entry = SIZE_MAX;
    uint32_t index = info->pImageIndices[0];
    if (ok) {
        pthread_mutex_lock(&shot.lock);
        for (size_t i = 0; i < shot.queue_count; ++i)
            if (shot.queues[i].queue == queue) device = shot.queues[i].device, family = shot.queues[i].family;
        for (size_t i = 0; i < shot.device_count; ++i)
            if (shot.devices[i].device == device)
                physical = shot.devices[i].physical, importable = shot.devices[i].importable;
        for (size_t i = 0; i < shot.swapchain_count; ++i)
            if (shot.swapchains[i].swapchain == info->pSwapchains[0] && index < shot.swapchains[i].image_count)
                entry = i;
        pthread_mutex_unlock(&shot.lock);
        ok = device && physical && importable && entry != SIZE_MAX &&
             !(compose.failed && compose.device == device) && (!compose.device || compose.device == device);
    }
    if (ok && !compose.device && !compose_setup(device, physical, family, &layer)) {
        compose_free();
        compose.device = device;
        compose.failed = true;
        ok = false;
    }
    ok = ok && family == compose.family && compose_import(physical, &layer);
    unsigned target = ok ? compose_target(shot.swapchains[entry].format) : UINT32_MAX;
    ok = target != UINT32_MAX;
    /* the image's view, framebuffer and semaphore */
    VkExtent2D extent = ok ? shot.swapchains[entry].extent : (VkExtent2D){0, 0};
    if (ok && !shot.swapchains[entry].framebuffers[index]) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = shot.swapchains[entry].images[index],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = shot.swapchains[entry].format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        ok = VKF(vkCreateImageView)(device, &vi, NULL, &shot.swapchains[entry].views[index]) == VK_SUCCESS;
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                      .renderPass = compose.targets[target].pass,
                                      .attachmentCount = 1,
                                      .pAttachments = &shot.swapchains[entry].views[index],
                                      .width = extent.width,
                                      .height = extent.height,
                                      .layers = 1};
        ok = ok && VKF(vkCreateFramebuffer)(device, &fi, NULL, &shot.swapchains[entry].framebuffers[index]) ==
                       VK_SUCCESS;
    }
    if (ok && !shot.swapchains[entry].composed[index]) {
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ok = VKF(vkCreateSemaphore)(device, &si, NULL, &shot.swapchains[entry].composed[index]) == VK_SUCCESS;
    }
    if (!ok) {
        pthread_mutex_unlock(&compose_lock);
        return false;
    }
    /* a slot: its last blend done (some presents ago) */
    unsigned s = compose.next;
    compose.next = (s + 1) % COMPOSE_SLOTS;
    VKF(vkWaitForFences)(device, 1, &compose.slots[s].fence, VK_TRUE, UINT64_MAX);
    VKF(vkResetFences)(device, 1, &compose.slots[s].fence);
    compose.slots[s].layer = -1;
    VkCommandBuffer cb = compose.slots[s].cb;
    VKF(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKF(vkBeginCommandBuffer)(cb, &begin);
    /* the layer image from the renderer's device (its release: hwui/render.c's layer_publish), and back after */
    VkImageMemoryBarrier acquire = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask = 0,
                                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
                                    .dstQueueFamilyIndex = family,
                                    .image = compose.layers[layer.index].image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    PFN_vkCmdPipelineBarrier barrier = VKF(vkCmdPipelineBarrier);
    barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
            &acquire);
    VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = compose.targets[target].pass,
                                .framebuffer = shot.swapchains[entry].framebuffers[index],
                                .renderArea = {{0, 0}, extent}};
    VKF(vkCmdBeginRenderPass)(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VKF(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compose.targets[target].pipeline);
    VkViewport vp = {0, 0, (float)extent.width, (float)extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, extent};
    VKF(vkCmdSetViewport)(cb, 0, 1, &vp);
    VKF(vkCmdSetScissor)(cb, 0, 1, &sc);
    VkDescriptorSet set = compose.layers[layer.index].sets[srgb_format(shot.swapchains[entry].format)];
    VKF(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compose.layout, 0, 1, &set, 0, NULL);
    VKF(vkCmdDraw)(cb, 3, 1, 0, 0);
    VKF(vkCmdEndRenderPass)(cb);
    VkImageMemoryBarrier release = acquire;
    release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    release.dstAccessMask = 0;
    release.srcQueueFamilyIndex = family;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
            &release);
    VKF(vkEndCommandBuffer)(cb);
    VkPipelineStageFlags stages[16];
    uint32_t waits = info->waitSemaphoreCount < 16 ? info->waitSemaphoreCount : 16;
    for (uint32_t i = 0; i < waits; ++i) stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, waits, info->pWaitSemaphores, stages, 1, &cb, 1,
                           &shot.swapchains[entry].composed[index]};
    ok = VKF(vkQueueSubmit)(queue, 1, &submit, compose.slots[s].fence) == VK_SUCCESS;
    if (ok) {
        compose.slots[s].layer = (int)layer.index;
        *done = shot.swapchains[entry].composed[index];
    } else {
        /* the fence unsignaled with nothing to signal it: made again, signaled */
        VKF(vkDestroyFence)(device, compose.slots[s].fence, NULL);
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        if (VKF(vkCreateFence)(device, &fci, NULL, &compose.slots[s].fence) != VK_SUCCESS) compose.failed = true;
    }
    pthread_mutex_unlock(&compose_lock);
    return ok;
}

VkResult hle_vulkan_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* in, VkSwapchainKHR* out) {
    PFN_vkCreateSwapchainKHR create = VKF(vkCreateSwapchainKHR);
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR info = *in;
    if (shot.path[0]) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; /* the layer's blend; every surface supports it */
    VkResult r = create(device, &info, NULL, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&compose_lock);
        pthread_mutex_lock(&shot.lock);
        /* the entry of the swapchain it replaces, or a free one */
        size_t i = 0;
        while (i < shot.swapchain_count && shot.swapchains[i].swapchain != info.oldSwapchain) ++i;
        if (i == shot.swapchain_count || !info.oldSwapchain)
            for (i = 0; i < shot.swapchain_count && shot.swapchains[i].swapchain; ++i) {}
        if (i == shot.swapchain_count && shot.swapchain_count < 8) ++shot.swapchain_count;
        if (i < shot.swapchain_count) {
            compose_forget_swapchain(i);
            if (shot.swapchains[i].device != device)
                memset(shot.swapchains[i].composed, 0, sizeof shot.swapchains[i].composed);
            shot.swapchains[i].swapchain = *out;
            shot.swapchains[i].device = device;
            shot.swapchains[i].format = info.imageFormat;
            shot.swapchains[i].extent = info.imageExtent;
            shot.swapchains[i].image_count = 0;
        }
        pthread_mutex_unlock(&shot.lock);
        pthread_mutex_unlock(&compose_lock);
    }
    return r;
}
static void create_swapchain(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)hle_vulkan_create_swapchain((VkDevice)(uintptr_t)X(0), PTR(1), PTR(3));
}
/* vkDestroySwapchainKHR(device, swapchain, allocator): its entry free again, its blend's objects gone first */
void hle_vulkan_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain) {
    PFN_vkDestroySwapchainKHR destroy = VKF(vkDestroySwapchainKHR);
    pthread_mutex_lock(&compose_lock);
    pthread_mutex_lock(&shot.lock);
    for (size_t i = 0; i < shot.swapchain_count && swapchain; ++i)
        if (shot.swapchains[i].swapchain == swapchain) {
            compose_forget_swapchain(i);
            shot.swapchains[i].swapchain = VK_NULL_HANDLE;
            shot.swapchains[i].image_count = 0;
        }
    pthread_mutex_unlock(&shot.lock);
    pthread_mutex_unlock(&compose_lock);
    if (destroy) destroy(device, swapchain, NULL);
}
static void destroy_swapchain(CPU* c, void* data) {
    (void)data;
    hle_vulkan_destroy_swapchain((VkDevice)(uintptr_t)X(0), (VkSwapchainKHR)(uintptr_t)X(1));
}
/* vkDestroyDevice(device, allocator): the blend's objects on it gone first, and its entries */
void hle_vulkan_destroy_device(VkDevice device) {
    PFN_vkDestroyDevice destroy = VKF(vkDestroyDevice);
    pthread_mutex_lock(&compose_lock);
    pthread_mutex_lock(&shot.lock);
    if (device && compose.device == device) {
        VKF(vkDeviceWaitIdle)(device);
        compose_free();
    }
    for (size_t i = 0; i < shot.swapchain_count; ++i)
        if (shot.swapchains[i].device == device) memset(&shot.swapchains[i], 0, sizeof shot.swapchains[i]);
    for (size_t i = 0; i < shot.device_count; ++i)
        if (shot.devices[i].device == device) shot.devices[i] = shot.devices[--shot.device_count];
    for (size_t i = 0; i < shot.queue_count;)
        if (shot.queues[i].device == device) shot.queues[i] = shot.queues[--shot.queue_count];
        else ++i;
    pthread_mutex_unlock(&shot.lock);
    pthread_mutex_unlock(&compose_lock);
    if (destroy) destroy(device, NULL);
}
void hle_vulkan_destroy_instance(VkInstance instance) {
    if (!instance) return;
    /* volk loads no instance's commands here: each is the instance's own (vk) */
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    pthread_mutex_lock(&v.lock);
    if (v.instance == instance) {
        v.instance = VK_NULL_HANDLE;
        for (size_t i = 0; i < v.count; ++i) __atomic_store_n(&v.commands[i]->host, NULL, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&v.lock);
    if (destroy) destroy(instance, NULL);
}
static void destroy_device(CPU* c, void* data) {
    (void)data;
    hle_vulkan_destroy_device((VkDevice)(uintptr_t)X(0));
}
VkResult hle_vulkan_swapchain_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) {
    PFN_vkGetSwapchainImagesKHR get = (PFN_vkGetSwapchainImagesKHR)vk("vkGetSwapchainImagesKHR");
    if (!get) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = get(device, swapchain, count, images);
    if (images && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
        pthread_mutex_lock(&compose_lock);
        pthread_mutex_lock(&shot.lock);
        for (size_t i = 0; i < shot.swapchain_count; ++i)
            if (shot.swapchains[i].swapchain == swapchain) {
                uint32_t n = *count < SWAP_IMAGES ? *count : SWAP_IMAGES;
                memcpy(shot.swapchains[i].images, images, n * sizeof *images);
                shot.swapchains[i].image_count = n;
            }
        pthread_mutex_unlock(&shot.lock);
        pthread_mutex_unlock(&compose_lock);
    }
    return r;
}
static void get_swapchain_images(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)hle_vulkan_swapchain_images((VkDevice)(uintptr_t)X(0), (VkSwapchainKHR)(uintptr_t)X(1),
                                                          PTR(2), PTR(3));
}

typedef struct {
    VkDevice device;
    VkQueue queue;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkCommandPool pool;
    VkSemaphore done;
    VkFence fence;
    VkExtent2D extent;
    VkFormat format;
    long number;
} ShotJob;

static bool shot_due(long number) {
    for (size_t i = 0; i < shot.frame_count; ++i)
        if (shot.frames[i] == number) return true;
    return false;
}

static void shot_free(ShotJob* j) {
    if (j->fence) ((PFN_vkDestroyFence)vk("vkDestroyFence"))(j->device, j->fence, NULL);
    if (j->done) ((PFN_vkDestroySemaphore)vk("vkDestroySemaphore"))(j->device, j->done, NULL);
    if (j->pool) ((PFN_vkDestroyCommandPool)vk("vkDestroyCommandPool"))(j->device, j->pool, NULL);
    if (j->buffer) ((PFN_vkDestroyBuffer)vk("vkDestroyBuffer"))(j->device, j->buffer, NULL);
    if (j->memory) ((PFN_vkFreeMemory)vk("vkFreeMemory"))(j->device, j->memory, NULL);
}

/* the copy of the image info presents, submitted to wait for the semaphores waits (the present's, or the layer's
   blend's): false when it can't be made, and the present waits for those as it would */
static bool shot_begin(VkQueue queue, const VkPresentInfoKHR* info, uint32_t wait_count, const VkSemaphore* waits,
                       long number, ShotJob* j) {
    *j = (ShotJob){0};
    j->queue = queue;
    j->number = number;
    VkImage image = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0;
    pthread_mutex_lock(&shot.lock);
    for (size_t i = 0; i < shot.queue_count; ++i)
        if (shot.queues[i].queue == queue) {
            j->device = shot.queues[i].device;
            family = shot.queues[i].family;
        }
    for (size_t i = 0; i < shot.device_count; ++i)
        if (shot.devices[i].device == j->device) physical = shot.devices[i].physical;
    for (size_t i = 0; i < shot.swapchain_count; ++i)
        if (shot.swapchains[i].swapchain == info->pSwapchains[0] &&
            info->pImageIndices[0] < shot.swapchains[i].image_count) {
            image = shot.swapchains[i].images[info->pImageIndices[0]];
            j->extent = shot.swapchains[i].extent;
            j->format = shot.swapchains[i].format;
        }
    pthread_mutex_unlock(&shot.lock);
    if (!j->device || !physical || !image) return false;
    VkDeviceSize size = (VkDeviceSize)j->extent.width * j->extent.height * 4;
    VkBufferCreateInfo b = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (((PFN_vkCreateBuffer)vk("vkCreateBuffer"))(j->device, &b, NULL, &j->buffer)) return false;
    VkMemoryRequirements need;
    ((PFN_vkGetBufferMemoryRequirements)vk("vkGetBufferMemoryRequirements"))(j->device, j->buffer, &need);
    VkPhysicalDeviceMemoryProperties props;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)vk("vkGetPhysicalDeviceMemoryProperties"))(physical, &props);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < props.memoryTypeCount && type == UINT32_MAX; ++t)
        if ((need.memoryTypeBits >> t & 1) && (props.memoryTypes[t].propertyFlags & want) == want) type = t;
    VkMemoryAllocateInfo m = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, need.size, type};
    if (type == UINT32_MAX || ((PFN_vkAllocateMemory)vk("vkAllocateMemory"))(j->device, &m, NULL, &j->memory) ||
        ((PFN_vkBindBufferMemory)vk("vkBindBufferMemory"))(j->device, j->buffer, j->memory, 0)) {
        shot_free(j);
        return false;
    }
    VkCommandPoolCreateInfo p = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo a = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE,
                                     VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    if (((PFN_vkCreateCommandPool)vk("vkCreateCommandPool"))(j->device, &p, NULL, &j->pool) ||
        (a.commandPool = j->pool, ((PFN_vkAllocateCommandBuffers)vk("vkAllocateCommandBuffers"))(j->device, &a, &cmd)) ||
        ((PFN_vkCreateSemaphore)vk("vkCreateSemaphore"))(j->device, &si, NULL, &j->done) ||
        ((PFN_vkCreateFence)vk("vkCreateFence"))(j->device, &fi, NULL, &j->fence)) {
        shot_free(j);
        return false;
    }
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    ((PFN_vkBeginCommandBuffer)vk("vkBeginCommandBuffer"))(cmd, &begin);
    VkImageMemoryBarrier to = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                               image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)vk("vkCmdPipelineBarrier");
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to);
    VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                {j->extent.width, j->extent.height, 1}};
    ((PFN_vkCmdCopyImageToBuffer)vk("vkCmdCopyImageToBuffer"))(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                                 j->buffer, 1, &region);
    VkImageMemoryBarrier back = to;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    ((PFN_vkEndCommandBuffer)vk("vkEndCommandBuffer"))(cmd);
    VkPipelineStageFlags stages[16];
    if (wait_count > 16) wait_count = 16;
    for (uint32_t i = 0; i < wait_count; ++i) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, wait_count, waits, stages, 1, &cmd, 1, &j->done};
    if (((PFN_vkQueueSubmit)vk("vkQueueSubmit"))(queue, 1, &submit, j->fence)) {
        shot_free(j);
        return false;
    }
    return true;
}

/* after the present: the copy's bytes into the file, and the job's objects freed */
static void shot_finish(ShotJob* j) {
    ((PFN_vkWaitForFences)vk("vkWaitForFences"))(j->device, 1, &j->fence, VK_TRUE, UINT64_MAX);
    ((PFN_vkQueueWaitIdle)vk("vkQueueWaitIdle"))(j->queue); /* the present's wait for the semaphore, before it goes */
    bool bgra = j->format == VK_FORMAT_B8G8R8A8_UNORM || j->format == VK_FORMAT_B8G8R8A8_SRGB;
    bool rgba = j->format == VK_FORMAT_R8G8B8A8_UNORM || j->format == VK_FORMAT_R8G8B8A8_SRGB;
    void* mapped = NULL;
    char name[1100];
    snprintf(name, sizeof name, shot.path, j->number);
    FILE* f = (bgra || rgba) ? fopen(name, "wb") : NULL;
    if (f && !((PFN_vkMapMemory)vk("vkMapMemory"))(j->device, j->memory, 0, VK_WHOLE_SIZE, 0, &mapped)) {
        fprintf(f, "P6\n%u %u\n255\n", j->extent.width, j->extent.height);
        const unsigned char* p = mapped;
        size_t pixels = (size_t)j->extent.width * j->extent.height;
        for (size_t i = 0; i < pixels; ++i, p += 4) {
            unsigned char rgb[3] = {p[bgra ? 2 : 0], p[1], p[bgra ? 0 : 2]};
            fwrite(rgb, 1, 3, f);
        }
        ((PFN_vkUnmapMemory)vk("vkUnmapMemory"))(j->device, j->memory);
        fprintf(stderr, "scarletnx-android: Vulkan present %ld saved in %s\n", j->number, name);
    } else {
        fprintf(stderr, "scarletnx-android: Vulkan present %ld not saved (format %d)\n", j->number, (int)j->format);
    }
    if (f) fclose(f);
    shot_free(j);
}

/* vkQueuePresentKHR, counted for --sample's lines (hle_vulkan_presents), and saved at the --shot presents */
static uint64_t presents;
uint64_t hle_vulkan_presents(void) {
    return __atomic_load_n(&presents, __ATOMIC_RELAXED);
}
/* At a present: the renderer's windows blended over the image first when one shows (compose_begin), then the shot's
   copy of what goes to the screen, each after the semaphores of what came before it */
VkResult hle_vulkan_present(VkQueue queue, const VkPresentInfoKHR* info) {
    static bool blending; /* the last present's, to say when it changes */
    PFN_vkQueuePresentKHR present = VKF(vkQueuePresentKHR);
    long number = (long)__atomic_add_fetch(&presents, 1, __ATOMIC_RELAXED);
    VkPresentInfoKHR waited = *info;
    VkSemaphore composed = VK_NULL_HANDLE;
    bool blend = present && compose_begin(queue, info, &composed);
    if (blend) {
        waited.waitSemaphoreCount = 1;
        waited.pWaitSemaphores = &composed;
    }
    if (blend != __atomic_exchange_n(&blending, blend, __ATOMIC_RELAXED))
        fprintf(stderr, "scarletnx-android: Vulkan present %ld: %s\n", number,
                blend ? "the app's windows blended over it" : "the app's own, no window over it");
    ShotJob job;
    bool took = shot.path[0] && present && info->swapchainCount && shot_due(number) &&
                shot_begin(queue, info, waited.waitSemaphoreCount, waited.pWaitSemaphores, number, &job);
    if (took) {
        waited.waitSemaphoreCount = 1;
        waited.pWaitSemaphores = &job.done;
    }
    VkResult r = present ? present(queue, &waited) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (took) shot_finish(&job);
    return r;
}
static void queue_present(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(int64_t)hle_vulkan_present((VkQueue)(uintptr_t)X(0), PTR(1));
}

/* ---- what a phone's driver accepts and the spec doesn't ----
   Engines tested on Mali and Adreno drivers make calls those drivers take, which a host driver may refuse or
   mishandle: each is made into the valid call with the effect the phone's driver gives it */

static void* device_command(const char* name) {
    return (void*)vkGetInstanceProcAddr(v.instance, name);
}

/* a repair, said the first few times */
static void repaired(const char* what) {
    static unsigned said;
    if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 8) fprintf(stderr, "scarletnx-android: Vulkan: %s\n", what);
}

/* SPIR-V that reads or writes a Buffer image without the capability it needs (ImageBuffer for a storage image,
   SampledBuffer for a sampled one), which the phone's compiler leaves alone. Both are core Vulkan capabilities, which
   every device supports, so they are added at the start of the instructions. NULL when nothing is missing; else a
   copy the caller frees, its size in *words */
static uint32_t* spirv_capabilities(const uint32_t* code, size_t size, size_t* words) {
    enum { OP_CAPABILITY = 17, OP_TYPE_IMAGE = 25, CAP_SAMPLED_BUFFER = 46, CAP_IMAGE_BUFFER = 47, DIM_BUFFER = 5 };
    size_t n = size / 4;
    if (n < 5 || snx_ld_le32(code) != 0x07230203) return NULL;
    bool has[2] = {false, false}, needs[2] = {false, false};
    for (size_t at = 5; at < n;) {
        uint32_t word = snx_ld_le32(code + at), count = word >> 16, op = word & 0xffff;
        if (!count || at + count > n) return NULL;
        if (op == OP_CAPABILITY && count >= 2) {
            uint32_t cap = snx_ld_le32(code + at + 1);
            if (cap == CAP_SAMPLED_BUFFER) has[0] = true;
            if (cap == CAP_IMAGE_BUFFER) has[1] = true;
        } else if (op == OP_TYPE_IMAGE && count >= 9 && snx_ld_le32(code + at + 3) == DIM_BUFFER) {
            uint32_t sampled = snx_ld_le32(code + at + 7);
            if (sampled == 1) needs[0] = true;
            else if (sampled == 2) needs[1] = true;
        }
        at += count;
    }
    unsigned add = (needs[0] && !has[0]) + (needs[1] && !has[1]);
    if (!add) return NULL;
    uint32_t* out = snx_alloc((n + 2 * add) * 4);
    memcpy(out, code, 5 * 4);
    size_t at = 5;
    for (unsigned k = 0; k < 2; ++k)
        if (needs[k] && !has[k]) {
            snx_st_le32(out + at++, (2u << 16) | OP_CAPABILITY);
            snx_st_le32(out + at++, k ? CAP_IMAGE_BUFFER : CAP_SAMPLED_BUFFER);
        }
    memcpy(out + at, code + 5, (n - 5) * 4);
    *words = n + 2 * add;
    return out;
}
static void create_shader_module(CPU* c, void* data) {
    (void)data;
    PFN_vkCreateShaderModule create = (PFN_vkCreateShaderModule)device_command("vkCreateShaderModule");
    if (!create) {
        X(0) = (uint64_t)(int64_t)VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    VkShaderModuleCreateInfo info = *(const VkShaderModuleCreateInfo*)PTR(1);
    size_t words = 0;
    uint32_t* fixed = info.pCode ? spirv_capabilities(info.pCode, info.codeSize, &words) : NULL;
    if (fixed) {
        repaired("a shader used a Buffer image without its capability (ImageBuffer, SampledBuffer): added");
        info.pCode = fixed;
        info.codeSize = words * 4;
    }
    VkResult r = create((VkDevice)(uintptr_t)X(0), &info, NULL, PTR(3));
    free(fixed);
    X(0) = (uint64_t)(int64_t)r;
}

/* vkCmdClearAttachments with a rectangle of no width or height, which clears nothing on a phone: only the others
   are given, and none is no call */
static void cmd_clear_attachments(CPU* c, void* data) {
    (void)data;
    PFN_vkCmdClearAttachments clear = (PFN_vkCmdClearAttachments)device_command("vkCmdClearAttachments");
    if (!clear) return;
    uint32_t count = (uint32_t)X(3);
    const VkClearRect* rects = PTR(4);
    VkClearRect* kept = count ? snx_alloc(count * sizeof *kept) : NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (rects[i].rect.extent.width && rects[i].rect.extent.height && rects[i].layerCount) kept[n++] = rects[i];
    if (n < count) repaired("a clear of an empty rectangle: left out");
    if (n) clear((VkCommandBuffer)(uintptr_t)X(0), (uint32_t)X(1), PTR(2), n, kept);
    free(kept);
}

static bool depth_format(VkFormat f) {
    return f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT;
}
/* A subpass whose depth and stencil attachment has a color format, which a phone's driver renders with no depth:
   the subpass gets no depth attachment, and the attachment stays in the pass for the framebuffer */
static void create_render_pass(CPU* c, void* data) {
    (void)data;
    PFN_vkCreateRenderPass create = (PFN_vkCreateRenderPass)device_command("vkCreateRenderPass");
    if (!create) {
        X(0) = (uint64_t)(int64_t)VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    VkRenderPassCreateInfo info = *(const VkRenderPassCreateInfo*)PTR(1);
    VkSubpassDescription* subpasses = NULL;
    for (uint32_t i = 0; i < info.subpassCount; ++i) {
        const VkAttachmentReference* d = info.pSubpasses[i].pDepthStencilAttachment;
        if (!d || d->attachment == VK_ATTACHMENT_UNUSED || d->attachment >= info.attachmentCount ||
            depth_format(info.pAttachments[d->attachment].format))
            continue;
        if (!subpasses) {
            subpasses = snx_alloc(info.subpassCount * sizeof *subpasses);
            memcpy(subpasses, info.pSubpasses, info.subpassCount * sizeof *subpasses);
        }
        char what[160];
        snprintf(what, sizeof what, "a render pass's depth attachment %u has color format %d: no depth there",
                 d->attachment, (int)info.pAttachments[d->attachment].format);
        repaired(what);
        subpasses[i].pDepthStencilAttachment = NULL;
    }
    if (subpasses) info.pSubpasses = subpasses;
    VkResult r = create((VkDevice)(uintptr_t)X(0), &info, NULL, PTR(3));
    free(subpasses);
    X(0) = (uint64_t)(int64_t)r;
}
static void create_render_pass2(CPU* c, void* data) {
    (void)data;
    PFN_vkCreateRenderPass2 create = (PFN_vkCreateRenderPass2)device_command("vkCreateRenderPass2");
    if (!create) create = (PFN_vkCreateRenderPass2)device_command("vkCreateRenderPass2KHR");
    if (!create) {
        X(0) = (uint64_t)(int64_t)VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    VkRenderPassCreateInfo2 info = *(const VkRenderPassCreateInfo2*)PTR(1);
    VkSubpassDescription2* subpasses = NULL;
    for (uint32_t i = 0; i < info.subpassCount; ++i) {
        const VkAttachmentReference2* d = info.pSubpasses[i].pDepthStencilAttachment;
        if (!d || d->attachment == VK_ATTACHMENT_UNUSED || d->attachment >= info.attachmentCount ||
            depth_format(info.pAttachments[d->attachment].format))
            continue;
        if (!subpasses) {
            subpasses = snx_alloc(info.subpassCount * sizeof *subpasses);
            memcpy(subpasses, info.pSubpasses, info.subpassCount * sizeof *subpasses);
        }
        repaired("a render pass's depth attachment has a color format: no depth there");
        subpasses[i].pDepthStencilAttachment = NULL;
    }
    if (subpasses) info.pSubpasses = subpasses;
    VkResult r = create((VkDevice)(uintptr_t)X(0), &info, NULL, PTR(3));
    free(subpasses);
    X(0) = (uint64_t)(int64_t)r;
}

/* ---- the device the guest sees ---- */

static void masquerade(VkPhysicalDeviceProperties* p) {
    snprintf(p->deviceName, sizeof p->deviceName, "%s", hle_gpu_name);
    p->vendorID = ARM_VENDOR_ID;
}
static void physical_device_properties(CPU* c, void* data) {
    (void)data;
    PFN_vkGetPhysicalDeviceProperties get =
        (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(v.instance, "vkGetPhysicalDeviceProperties");
    if (!get) return;
    get((VkPhysicalDevice)(uintptr_t)X(0), PTR(1));
    masquerade(PTR(1));
}
static void physical_device_properties2(CPU* c, void* data) {
    (void)data;
    VkPhysicalDeviceProperties2* p = PTR(1);
    PFN_vkGetPhysicalDeviceProperties2 get =
        (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(v.instance, "vkGetPhysicalDeviceProperties2");
    if (!get) get = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(v.instance,
                                                                             "vkGetPhysicalDeviceProperties2KHR");
    if (!get) return;
    get((VkPhysicalDevice)(uintptr_t)X(0), p);
    masquerade(&p->properties);
    for (VkBaseOutStructure* s = p->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties* d = (VkPhysicalDeviceDriverProperties*)s;
            d->driverID = ARM_DRIVER_ID;
            snprintf(d->driverName, sizeof d->driverName, "ARM proprietary driver");
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
            VkPhysicalDeviceVulkan12Properties* d = (VkPhysicalDeviceVulkan12Properties*)s;
            d->driverID = ARM_DRIVER_ID;
            snprintf(d->driverName, sizeof d->driverName, "ARM proprietary driver");
        }
    }
}

static void debug_messenger_destroy(CPU* c, void* data) {
    (void)c;
    (void)data;
}

/* a debug messenger without its callback (the guest's code): a handle the destroy takes back */
static void debug_messenger(CPU* c, void* data) {
    (void)data;
    static uint64_t next = 0x5c00000000000001ull;
    uint64_t handle = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
    memcpy(PTR(3), &handle, sizeof handle);
    X(0) = VK_SUCCESS;
}
