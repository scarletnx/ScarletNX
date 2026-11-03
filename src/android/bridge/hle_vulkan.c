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
   - the device the guest sees is a Mali-G52 (ARM's vendor and driver IDs), with the host's limits and features */
#define _GNU_SOURCE
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_vulkan.h"
#include "common/utilities/common.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <volk.h>
#include <SDL3/SDL_vulkan.h>

/* the device's name and IDs as the guest sees them */
static const char DEVICE_NAME[] = "Mali-G52";
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
    hle_call_wide(c, fn, cmd->kind);
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

/* ---- the device the guest sees ---- */

static void masquerade(VkPhysicalDeviceProperties* p) {
    snprintf(p->deviceName, sizeof p->deviceName, "%s", DEVICE_NAME);
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
