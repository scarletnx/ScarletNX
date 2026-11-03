/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libvulkan as a pass-through (docs/android/NATIVE.md, "Vulkan: a pass-through"). The guest calls the host's Vulkan:
   each command it names (through dlsym or vkGet*ProcAddr) is an export that calls the host loader's command of that
   name, which the loader's vkGetInstanceProcAddr gives (it dispatches by the handle, so any instance or device
   works). A guest pointer is a host pointer and the layouts agree on a little-endian LP64 host, so the structures and
   their pNext chains pass as they are. What changes:
   - the instance's surface extensions: VK_KHR_android_surface becomes the host window's (SDL's), or
     VK_EXT_headless_surface without a display,
   - vkCreateAndroidSurfaceKHR makes a surface of the host window, which ScarletNX's own renderer then gives up,
   - the guest's allocation callbacks are left out: the driver allocates for itself, at create and at destroy alike,
   - the debug messengers the guest asks for are made without their callback,
   - the device the guest sees is a Mali-G710 (ARM's vendor and driver IDs), with the host's limits and features */
#define _GNU_SOURCE
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_vulkan.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <volk.h>
#include <SDL3/SDL_vulkan.h>

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

/* the host loader's command: by the guest's instance, or global (the commands before an instance) */
static void* host_command(const char* name) {
    if (!loader()) return NULL;
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
    {"vkQueuePresentKHR", queue_present},
    {"vkCreateDevice", create_device},
    {"vkGetDeviceQueue", get_device_queue},
    {"vkGetDeviceQueue2", get_device_queue2},
    {"vkCreateSwapchainKHR", create_swapchain},
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

static void create_instance(CPU* c, void* data) {
    (void)data;
    if (!loader()) {
        X(0) = (uint64_t)(int64_t)VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    VkInstanceCreateInfo info = *(const VkInstanceCreateInfo*)PTR(0);
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
        memcpy(PTR(2), &instance, sizeof instance);
    }
    X(0) = (uint64_t)(int64_t)r;
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
static void create_android_surface(CPU* c, void* data) {
    (void)data;
    VkInstance instance = (VkInstance)(uintptr_t)X(0);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    void* window = window_host ? window_host->window() : NULL;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (window) {
        /* ScarletNX's renderer gives the window up: a window has one swapchain at a time */
        window_host->yield();
        r = SDL_Vulkan_CreateSurface(window, instance, NULL, &surface) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    } else {
        PFN_vkCreateHeadlessSurfaceEXT make =
            (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT");
        VkHeadlessSurfaceCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        if (make) r = make(instance, &info, NULL, &surface);
    }
    if (r == VK_SUCCESS) memcpy(PTR(3), &surface, sizeof surface);
    X(0) = (uint64_t)(int64_t)r;
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
static void surface_capabilities(CPU* c, void* data) {
    (void)data;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        vkGetInstanceProcAddr(v.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkResult r = get ? get((VkPhysicalDevice)(uintptr_t)X(0), (VkSurfaceKHR)(uintptr_t)X(1), PTR(2))
                     : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (r == VK_SUCCESS) window_extent(PTR(2));
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

/* ---- --shot: the guest's presented images, saved as binary PPM (hle_vulkan.h) ----
   The pass-through records what a copy of a presented image needs: each device's physical device, each queue's
   device and family, each swapchain's device, format, extent and images (made with TRANSFER_SRC as well, when shots
   are asked for). At a listed present, a submission on the presenting queue waits for the present's semaphores,
   copies the image into a host-visible buffer and signals a semaphore of its own, which the present then waits for */

static struct {
    pthread_mutex_t lock;
    char path[1024];
    long frames[64];
    size_t frame_count;
    struct {
        VkDevice device;
        VkPhysicalDevice physical;
    } devices[8];
    size_t device_count;
    struct {
        VkQueue queue;
        VkDevice device;
        uint32_t family;
    } queues[32];
    size_t queue_count;
    struct {
        VkSwapchainKHR swapchain;
        VkDevice device;
        VkFormat format;
        VkExtent2D extent;
        VkImage images[16];
        uint32_t image_count;
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

static void create_device(CPU* c, void* data) {
    (void)data;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)vk("vkCreateDevice");
    VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)X(0);
    VkResult r = create ? create(physical, PTR(1), NULL, PTR(3)) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&shot.lock);
        if (shot.device_count < 8) {
            shot.devices[shot.device_count].device = *(VkDevice*)PTR(3);
            shot.devices[shot.device_count++].physical = physical;
        }
        pthread_mutex_unlock(&shot.lock);
    }
    X(0) = (uint64_t)(int64_t)r;
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
static void get_device_queue(CPU* c, void* data) {
    (void)data;
    PFN_vkGetDeviceQueue get = (PFN_vkGetDeviceQueue)vk("vkGetDeviceQueue");
    if (!get) return;
    get((VkDevice)(uintptr_t)X(0), (uint32_t)X(1), (uint32_t)X(2), PTR(3));
    record_queue((VkDevice)(uintptr_t)X(0), (uint32_t)X(1), *(VkQueue*)PTR(3));
}
static void get_device_queue2(CPU* c, void* data) {
    (void)data;
    PFN_vkGetDeviceQueue2 get = (PFN_vkGetDeviceQueue2)vk("vkGetDeviceQueue2");
    if (!get) return;
    const VkDeviceQueueInfo2* info = PTR(1);
    get((VkDevice)(uintptr_t)X(0), info, PTR(2));
    record_queue((VkDevice)(uintptr_t)X(0), info->queueFamilyIndex, *(VkQueue*)PTR(2));
}
static void create_swapchain(CPU* c, void* data) {
    (void)data;
    PFN_vkCreateSwapchainKHR create = (PFN_vkCreateSwapchainKHR)vk("vkCreateSwapchainKHR");
    if (!create) {
        X(0) = (uint64_t)(int64_t)VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    VkSwapchainCreateInfoKHR info = *(const VkSwapchainCreateInfoKHR*)PTR(1);
    if (shot.path[0]) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = create((VkDevice)(uintptr_t)X(0), &info, NULL, PTR(3));
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&shot.lock);
        size_t i = 0;
        while (i < shot.swapchain_count && shot.swapchains[i].swapchain != info.oldSwapchain) ++i;
        if (i == shot.swapchain_count && shot.swapchain_count < 8) ++shot.swapchain_count;
        if (i < shot.swapchain_count) {
            shot.swapchains[i].swapchain = *(VkSwapchainKHR*)PTR(3);
            shot.swapchains[i].device = (VkDevice)(uintptr_t)X(0);
            shot.swapchains[i].format = info.imageFormat;
            shot.swapchains[i].extent = info.imageExtent;
            shot.swapchains[i].image_count = 0;
        }
        pthread_mutex_unlock(&shot.lock);
    }
    X(0) = (uint64_t)(int64_t)r;
}
static void get_swapchain_images(CPU* c, void* data) {
    (void)data;
    PFN_vkGetSwapchainImagesKHR get = (PFN_vkGetSwapchainImagesKHR)vk("vkGetSwapchainImagesKHR");
    if (!get) {
        X(0) = (uint64_t)(int64_t)VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    VkSwapchainKHR swapchain = (VkSwapchainKHR)(uintptr_t)X(1);
    uint32_t* count = PTR(2);
    VkImage* images = PTR(3);
    VkResult r = get((VkDevice)(uintptr_t)X(0), swapchain, count, images);
    if (images && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
        pthread_mutex_lock(&shot.lock);
        for (size_t i = 0; i < shot.swapchain_count; ++i)
            if (shot.swapchains[i].swapchain == swapchain) {
                uint32_t n = *count < 16 ? *count : 16;
                memcpy(shot.swapchains[i].images, images, n * sizeof *images);
                shot.swapchains[i].image_count = n;
            }
        pthread_mutex_unlock(&shot.lock);
    }
    X(0) = (uint64_t)(int64_t)r;
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

/* the copy of the image info presents, submitted to wait for its semaphores: false when it can't be made, and the
   present goes as the guest made it */
static bool shot_begin(VkQueue queue, const VkPresentInfoKHR* info, long number, ShotJob* j) {
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
    uint32_t waits = info->waitSemaphoreCount < 16 ? info->waitSemaphoreCount : 16;
    for (uint32_t i = 0; i < waits; ++i) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, waits, info->pWaitSemaphores, stages, 1, &cmd, 1,
                           &j->done};
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
static void queue_present(CPU* c, void* data) {
    (void)data;
    PFN_vkQueuePresentKHR present = (PFN_vkQueuePresentKHR)vk("vkQueuePresentKHR");
    VkQueue queue = (VkQueue)(uintptr_t)X(0);
    const VkPresentInfoKHR* info = PTR(1);
    long number = (long)__atomic_add_fetch(&presents, 1, __ATOMIC_RELAXED);
    ShotJob job;
    bool took = shot.path[0] && present && info->swapchainCount && shot_due(number) &&
                shot_begin(queue, info, number, &job);
    VkPresentInfoKHR waited = *info;
    if (took) {
        waited.waitSemaphoreCount = 1;
        waited.pWaitSemaphores = &job.done;
    }
    VkResult r = present ? present(queue, took ? &waited : info) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (took) shot_finish(&job);
    X(0) = (uint64_t)(int64_t)r;
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
