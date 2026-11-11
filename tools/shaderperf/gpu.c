/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vulkan side of snx-shaderperf (bench.h): the device, the resources each binding gets, the pipelines, the timed
   submissions and the queries around them */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "bench.h"
#include "fullscreen_vert.h"
#include "vkload.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif

#define CHECK(call)                                                                                                    \
    do {                                                                                                               \
        VkResult r_ = (call);                                                                                          \
        if (r_ != VK_SUCCESS) {                                                                                        \
            fprintf(stderr, "snx-shaderperf: %s failed: %d (%s:%d)\n", #call, (int)r_, __FILE__, __LINE__);           \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

#define MAX_SETS 8
#define MAX_VERTEX_INPUTS 16

/* ---- time ---- */

static double now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
#endif
}

static void sleep_ms(double ms) {
    if (ms <= 0)
        return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000) * 1e6);
    nanosleep(&ts, NULL);
#endif
}

/* ---- small helpers ---- */

static bool ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

static bool icontains(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n)
            return true;
    }
    return n == 0;
}

static char* dupstr(const char* s) {
    size_t n = strlen(s);
    char* d = malloc(n + 1);
    if (!d) {
        fprintf(stderr, "snx-shaderperf: out of memory\n");
        exit(2);
    }
    memcpy(d, s, n + 1);
    return d;
}

static void* xcalloc(size_t n, size_t size) {
    void* p = calloc(n ? n : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "snx-shaderperf: out of memory\n");
        exit(2);
    }
    return p;
}

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 16, n = 0;
    uint8_t* buf = malloc(cap);
    while (buf) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap)
            break;
        uint8_t* nb = realloc(buf, cap * 2);
        if (!nb) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = nb;
        cap *= 2;
    }
    fclose(f);
    *size = n;
    return buf;
}

static bool write_file(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

static uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* "k=v,k=v" lists: a part without '=' continues the value before it, so values can hold commas */
typedef struct {
    char key[32];
    char* value;
} Kv;
typedef struct {
    Kv kv[32];
    size_t count;
    char* buf;
} KvList;

static void kv_parse(const char* spec, KvList* out) {
    memset(out, 0, sizeof *out);
    out->buf = dupstr(spec ? spec : "");
    char* p = out->buf;
    while (*p) {
        char* end = strchr(p, ',');
        if (end)
            *end = 0;
        char* eq = strchr(p, '=');
        if (eq && out->count < 32) {
            *eq = 0;
            snprintf(out->kv[out->count].key, sizeof out->kv[0].key, "%s", p);
            out->kv[out->count].value = eq + 1;
            out->count++;
        } else if (out->count) {
            /* rejoin: the value spans up to here */
            p[-1] = ',';
        } else if (*p && out->count < 32) {
            snprintf(out->kv[out->count].key, sizeof out->kv[0].key, "%s", p);
            out->kv[out->count].value = p + strlen(p);
            out->count++;
        }
        if (!end)
            break;
        p = end + 1;
    }
}

static const char* kv_get(const KvList* l, const char* key) {
    for (size_t i = 0; i < l->count; i++)
        if (ieq(l->kv[i].key, key))
            return l->kv[i].value;
    return NULL;
}

static void kv_free(KvList* l) { free(l->buf); }

static bool parse_dims(const char* s, uint32_t* w, uint32_t* h, uint32_t* d) {
    unsigned a = 0, b = 1, c = 1;
    int n = sscanf(s, "%ux%ux%u", &a, &b, &c);
    if (n < 1 || !a || !b || !c)
        return false;
    *w = a;
    *h = n >= 2 ? b : 1;
    *d = n >= 3 ? c : 1;
    return true;
}

/* ---- the device ---- */

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void* map;
} Buf;

typedef struct {
    uint32_t set, binding;
    VkDescriptorType type;
    uint32_t count;
    bool writable;
    char desc[256];
    /* buffers and texel buffers */
    Buf b;
    VkBufferView bview;
    /* images */
    VkImage img;
    VkDeviceMemory imem;
    VkImageView view;
    VkSampler sampler;
    const SpFormat* fmt;
    uint32_t w, h, d, layers, mips, samples;
    VkImageViewType vtype;
    VkImageLayout layout;
    VkImageAspectFlags aspect;
    /* the first contents, for uploading again before a --check run */
    uint8_t* init;
    size_t init_size;
    VkBufferImageCopy* regions;
    uint32_t region_count;
} Res;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    const SpFormat* fmt;
    VkFormat vk;
    VkImageAspectFlags aspect;
} Target;

typedef struct {
    VkDescriptorSetLayout sets[MAX_SETS];
    uint32_t set_count;
    VkPipelineLayout layout;
    VkDescriptorSet dsets[MAX_SETS];
    VkPipeline pipe;
    VkCommandBuffer cb;
    uint8_t push[256];
    uint32_t push_size;
    uint8_t spec_data[1024];
    VkSpecializationMapEntry spec_map[64];
    VkSpecializationInfo spec;
    VkShaderModule module;
} Variant;

/* the stages of a graphics pipeline */
enum { ST_VS, ST_GS, ST_FS, ST_COUNT };
static const VkShaderStageFlagBits k_stage_bit[ST_COUNT] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                                            VK_SHADER_STAGE_FRAGMENT_BIT};
static const char* const k_stage_opt[ST_COUNT] = {"--vs", "--gs", "--fs"};

typedef struct {
    SpBench* b;
    VkInstance inst;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memp;
    VkCommandPool pool;
    VkFence fence;
    VkQueryPool ts_pool, stat_pool;
    VkQueryPipelineStatisticFlags stat_flags;
    bool has_exec, has_perf, has_calib_khr, has_calib_ext, has_shader_clock, has_sm_builtins, has_host_reset;
    uint32_t ts_mask_bits;
    /* the measured stage's pipeline kind */
    VkShaderStageFlagBits stage;
    /* the companion stages of a graphics pipeline (--vs, --gs, --fs, ST_*): each one for every shader, or one for
       each (given once a shader) */
    SrModule comp[ST_COUNT][SP_MAX_SHADERS];
    uint32_t* comp_words[ST_COUNT][SP_MAX_SHADERS];
    size_t comp_size[ST_COUNT][SP_MAX_SHADERS];
    VkShaderModule comp_module[ST_COUNT][SP_MAX_SHADERS];
    size_t comp_count[ST_COUNT];
    /* the built-in vertex shader feeds the pipeline (no --vs); fullscreen: it draws one full-screen triangle, for a
       fragment shader */
    bool builtin_vs, fullscreen;
    VkShaderModule builtin_module;
    Res* res;
    size_t res_count;
    Target rts[SP_MAX_RTS];
    uint32_t rt_count;
    VkFormat rt_formats[SP_MAX_RTS];
    Target depth;
    bool have_depth;
    Buf vbufs[MAX_VERTEX_INPUTS];
    VkVertexInputAttributeDescription attrs[MAX_VERTEX_INPUTS];
    VkVertexInputBindingDescription vbinds[MAX_VERTEX_INPUTS];
    uint32_t attr_count;
    Buf ibuf;
    bool indexed;
    VkPrimitiveTopology topology;
    VkDescriptorPool dpool;
    Variant var[SP_MAX_SHADERS];
    uint32_t validation_errors;
} Gpu;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                               VkDebugUtilsMessageTypeFlagsEXT types,
                                               const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)types;
    Gpu* g = user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        g->validation_errors++;
    fprintf(stderr, "validation: %s\n", data->pMessage);
    return VK_FALSE;
}

static bool instance_has_ext(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties* p = xcalloc(n, sizeof *p);
    vkEnumerateInstanceExtensionProperties(NULL, &n, p);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++)
        found = strcmp(p[i].extensionName, name) == 0;
    free(p);
    return found;
}

static bool instance_has_layer(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties* p = xcalloc(n, sizeof *p);
    vkEnumerateInstanceLayerProperties(&n, p);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++)
        found = strcmp(p[i].layerName, name) == 0;
    free(p);
    return found;
}

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    char buf[256];
    snprintf(buf, sizeof buf, "%s=%s", name, value);
    _putenv(buf);
#else
    setenv(name, value, 1);
#endif
}

static bool make_instance(Gpu* g, const SpConfig* cfg) {
    char err[256];
    if (!cfg->driver_cache) {
        /* the drivers' own disk caches would turn every compile after the first into a lookup */
        set_env("__GL_SHADER_DISK_CACHE", "0");
        set_env("MESA_SHADER_CACHE_DISABLE", "true");
        set_env("MESA_DISK_CACHE_SINGLE_FILE", "false");
        set_env("RADV_DEBUG", getenv("RADV_DEBUG") ? getenv("RADV_DEBUG") : "nocache");
    }
    if (!sp_vk_load(cfg->loader, err, sizeof err)) {
        fprintf(stderr, "snx-shaderperf: %s\n", err);
        return false;
    }
    uint32_t api = VK_API_VERSION_1_1;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&api);
    if (api > VK_API_VERSION_1_4)
        api = VK_API_VERSION_1_4;
    const char* exts[4];
    uint32_t ext_count = 0;
    const char* layers[1];
    uint32_t layer_count = 0;
    bool debug = false;
    if (cfg->validate) {
        if (instance_has_layer("VK_LAYER_KHRONOS_validation")) {
            layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
            if (instance_has_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                exts[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                debug = true;
            }
        } else {
            sp_warn(g->b, "--validate: VK_LAYER_KHRONOS_validation is not installed");
        }
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "snx-shaderperf", 1, "ScarletNX", 1, api};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, layer_count, layers,
                                ext_count, exts};
    CHECK(vkCreateInstance(&ici, NULL, &g->inst));
    sp_vk_load_instance(g->inst);
    if (debug && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT mci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debug_cb;
        mci.pUserData = g;
        vkCreateDebugUtilsMessengerEXT(g->inst, &mci, NULL, &g->messenger);
    }
    return true;
}

static VkPhysicalDevice* physical_devices(VkInstance inst, uint32_t* count) {
    *count = 0;
    vkEnumeratePhysicalDevices(inst, count, NULL);
    VkPhysicalDevice* list = xcalloc(*count, sizeof *list);
    vkEnumeratePhysicalDevices(inst, count, list);
    return list;
}

static int type_rank(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 0;
    default: return 1;
    }
}

static bool pick_device(Gpu* g, const SpConfig* cfg) {
    uint32_t n;
    VkPhysicalDevice* list = physical_devices(g->inst, &n);
    if (!n) {
        fprintf(stderr, "snx-shaderperf: no Vulkan device\n");
        free(list);
        return false;
    }
    int best = -1, best_rank = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(list[i], &p);
        if (cfg->device) {
            char* end;
            unsigned long idx = strtoul(cfg->device, &end, 10);
            bool match = *end == 0 ? idx == i : icontains(p.deviceName, cfg->device);
            if (match && best < 0)
                best = (int)i;
            continue;
        }
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        if (type_rank(p.deviceType) > best_rank) {
            best_rank = type_rank(p.deviceType);
            best = (int)i;
        }
    }
    if (best < 0) {
        fprintf(stderr, "snx-shaderperf: no device matches \"%s\" (--list-devices)\n",
                cfg->device ? cfg->device : "Vulkan 1.3");
        free(list);
        return false;
    }
    g->phys = list[best];
    free(list);
    vkGetPhysicalDeviceProperties(g->phys, &g->props);
    vkGetPhysicalDeviceMemoryProperties(g->phys, &g->memp);
    if (g->props.apiVersion < VK_API_VERSION_1_3) {
        fprintf(stderr, "snx-shaderperf: %s has Vulkan %u.%u; the tool needs 1.3 (dynamic rendering)\n",
                g->props.deviceName, VK_API_VERSION_MAJOR(g->props.apiVersion),
                VK_API_VERSION_MINOR(g->props.apiVersion));
        return false;
    }
    return true;
}

static bool device_has_ext(VkPhysicalDevice phys, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    VkExtensionProperties* p = xcalloc(n, sizeof *p);
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, p);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++)
        found = strcmp(p[i].extensionName, name) == 0;
    free(p);
    return found;
}

static uint32_t pick_family(Gpu* g, bool compute_only) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &n, NULL);
    VkQueueFamilyProperties* q = xcalloc(n, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &n, q);
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < n && found == UINT32_MAX; i++) {
        if (!q[i].timestampValidBits)
            continue;
        if (compute_only) {
            if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                found = i;
        } else if ((q[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                   (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            found = i;
        }
    }
    if (found != UINT32_MAX)
        g->ts_mask_bits = q[found].timestampValidBits;
    free(q);
    return found;
}

/* the device with every core feature it has, but the robustness ones that cost time (unless --robust), and the
   extensions of the queries */
static bool make_device(Gpu* g, const SpConfig* cfg) {
    bool want_compute = cfg->compute_queue && g->stage == VK_SHADER_STAGE_COMPUTE_BIT;
    g->family = pick_family(g, want_compute);
    if (g->family == UINT32_MAX && want_compute) {
        sp_warn(g->b, "--compute-queue: the device has no compute-only queue family with timestamps; using the "
                      "universal one");
        g->family = pick_family(g, false);
    }
    if (g->family == UINT32_MAX) {
        fprintf(stderr, "snx-shaderperf: no queue family with graphics, compute and timestamps\n");
        return false;
    }
    const char* exts[48];
    uint32_t ext_count = 0;
    g->has_exec = device_has_ext(g->phys, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    g->has_perf = device_has_ext(g->phys, VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);
    g->has_calib_khr = device_has_ext(g->phys, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    g->has_calib_ext = !g->has_calib_khr && device_has_ext(g->phys, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    g->has_shader_clock = device_has_ext(g->phys, VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
    g->has_sm_builtins = device_has_ext(g->phys, VK_NV_SHADER_SM_BUILTINS_EXTENSION_NAME);
    if (g->has_exec)
        exts[ext_count++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    if (g->has_perf)
        exts[ext_count++] = VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME;
    if (g->has_calib_khr)
        exts[ext_count++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    if (g->has_calib_ext)
        exts[ext_count++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    if (g->has_shader_clock)
        exts[ext_count++] = VK_KHR_SHADER_CLOCK_EXTENSION_NAME;
    if (g->has_sm_builtins)
        exts[ext_count++] = VK_NV_SHADER_SM_BUILTINS_EXTENSION_NAME;
    for (size_t i = 0; i < cfg->extension_count && ext_count < 48; i++) {
        if (!device_has_ext(g->phys, cfg->extensions[i])) {
            fprintf(stderr, "snx-shaderperf: the device lacks --ext %s\n", cfg->extensions[i]);
            return false;
        }
        bool dup = false;
        for (uint32_t k = 0; k < ext_count; k++)
            dup |= strcmp(exts[k], cfg->extensions[i]) == 0;
        if (!dup)
            exts[ext_count++] = cfg->extensions[i];
    }
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features f14 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR fexec = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDevicePerformanceQueryFeaturesKHR fperf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR};
    VkPhysicalDeviceShaderClockFeaturesKHR fclock = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    VkPhysicalDeviceShaderSMBuiltinsFeaturesNV fsm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_FEATURES_NV};
    void** tail = &f2.pNext;
#define LINK(s)                                                                                                        \
    do {                                                                                                               \
        *tail = &(s);                                                                                                  \
        tail = &(s).pNext;                                                                                             \
    } while (0)
    LINK(f11);
    LINK(f12);
    LINK(f13);
    if (g->props.apiVersion >= VK_API_VERSION_1_4)
        LINK(f14);
    if (g->has_exec)
        LINK(fexec);
    if (g->has_perf)
        LINK(fperf);
    if (g->has_shader_clock)
        LINK(fclock);
    if (g->has_sm_builtins)
        LINK(fsm);
#undef LINK
    vkGetPhysicalDeviceFeatures2(g->phys, &f2);
    if (!cfg->robust) {
        f2.features.robustBufferAccess = VK_FALSE;
        f13.robustImageAccess = VK_FALSE;
    }
    /* the performance query's own costs are not wanted without --counters */
    fperf.performanceCounterMultipleQueryPools = VK_FALSE;
    if (!cfg->counters)
        fperf.performanceCounterQueryPools = VK_FALSE;
    g->has_perf = g->has_perf && fperf.performanceCounterQueryPools;
    g->has_exec = g->has_exec && fexec.pipelineExecutableInfo;
    g->has_host_reset = f12.hostQueryReset;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g->family, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ext_count;
    dci.ppEnabledExtensionNames = exts;
    CHECK(vkCreateDevice(g->phys, &dci, NULL, &g->dev));
    sp_vk_load_device(g->dev);
    vkGetDeviceQueue(g->dev, g->family, 0, &g->queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g->family};
    CHECK(vkCreateCommandPool(g->dev, &pci, NULL, &g->pool));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(g->dev, &fci, NULL, &g->fence));
    if (f2.features.pipelineStatisticsQuery && cfg->pipeline_stats) {
        g->stat_flags = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                        VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                        VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                        VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                        VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
        if (g->family == UINT32_MAX)
            g->stat_flags = 0;
    } else if (cfg->pipeline_stats) {
        sp_warn(g->b, "the device has no pipelineStatisticsQuery");
    }
    return true;
}

/* ---- memory, buffers, one-time commands ---- */

static uint32_t memory_type(Gpu* g, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g->memp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g->memp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static bool alloc(Gpu* g, VkMemoryRequirements req, bool host, VkDeviceMemory* out) {
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = memory_type(g, req.memoryTypeBits, want);
    if (type == UINT32_MAX && !host)
        type = memory_type(g, req.memoryTypeBits, 0);
    if (type == UINT32_MAX) {
        fprintf(stderr, "snx-shaderperf: no memory type for %s memory\n", host ? "host" : "device");
        return false;
    }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    CHECK(vkAllocateMemory(g->dev, &mai, NULL, out));
    return true;
}

static bool buf_make(Gpu* g, VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buf* out) {
    memset(out, 0, sizeof *out);
    out->size = size ? size : 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = out->size;
    bci.usage = usage;
    CHECK(vkCreateBuffer(g->dev, &bci, NULL, &out->buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->dev, out->buf, &req);
    if (!alloc(g, req, host, &out->mem))
        return false;
    CHECK(vkBindBufferMemory(g->dev, out->buf, out->mem, 0));
    if (host)
        CHECK(vkMapMemory(g->dev, out->mem, 0, VK_WHOLE_SIZE, 0, &out->map));
    return true;
}

static void buf_free(Gpu* g, Buf* b) {
    if (b->buf)
        vkDestroyBuffer(g->dev, b->buf, NULL);
    if (b->mem)
        vkFreeMemory(g->dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

static VkCommandBuffer once_begin(Gpu* g) {
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g->pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g->dev, &cai, &cb) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static bool submit_wait(Gpu* g, VkCommandBuffer cb, const void* pnext) {
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = pnext;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    CHECK(vkResetFences(g->dev, 1, &g->fence));
    CHECK(vkQueueSubmit(g->queue, 1, &si, g->fence));
    CHECK(vkWaitForFences(g->dev, 1, &g->fence, VK_TRUE, UINT64_MAX));
    return true;
}

static bool once_end(Gpu* g, VkCommandBuffer cb) {
    if (!cb)
        return false;
    vkEndCommandBuffer(cb);
    bool ok = submit_wait(g, cb, NULL);
    vkFreeCommandBuffers(g->dev, g->pool, 1, &cb);
    return ok;
}

static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL,
                         0, NULL);
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
                          VkImageLayout to) {
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = from;
    ib.newLayout = to;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange = (VkImageSubresourceRange){aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &ib);
}

static bool buffer_upload(Gpu* g, Buf* dst, const void* data, size_t size) {
    Buf st;
    if (!buf_make(g, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &st))
        return false;
    memcpy(st.map, data, size);
    VkCommandBuffer cb = once_begin(g);
    VkBufferCopy region = {0, 0, size};
    vkCmdCopyBuffer(cb, st.buf, dst->buf, 1, &region);
    barrier_all(cb);
    bool ok = once_end(g, cb);
    buf_free(g, &st);
    return ok;
}

static bool buffer_read(Gpu* g, Buf* src, uint8_t* out, size_t size) {
    Buf st;
    if (!buf_make(g, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &st))
        return false;
    VkCommandBuffer cb = once_begin(g);
    barrier_all(cb);
    VkBufferCopy region = {0, 0, size};
    vkCmdCopyBuffer(cb, src->buf, st.buf, 1, &region);
    barrier_all(cb);
    bool ok = once_end(g, cb);
    if (ok)
        memcpy(out, st.map, size);
    buf_free(g, &st);
    return ok;
}

/* ---- resources ---- */

static uint32_t full_mips(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w > h ? w : h;
    m = m > d ? m : d;
    uint32_t n = 1;
    while (m > 1) {
        m >>= 1;
        n++;
    }
    return n;
}

static uint32_t mip_dim(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}

static VkImageAspectFlags aspect_of(const SpFormat* f) {
    if (f->kind != SP_KIND_DEPTH)
        return VK_IMAGE_ASPECT_COLOR_BIT;
    if (f->vk == VK_FORMAT_S8_UINT)
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT;
}

static const SpFormat* default_image_format(const SrBinding* sb) {
    if (sb->depth)
        return sp_format_vk(VK_FORMAT_D32_SFLOAT);
    if (sb->image_format) {
        const SpFormat* f = sp_format_vk(sr_image_format_vk(sb->image_format));
        if (f)
            return f;
    }
    switch (sb->sampled_scalar) {
    case SR_SINT: return sp_format_vk(VK_FORMAT_R8G8B8A8_SINT);
    case SR_UINT: return sp_format_vk(VK_FORMAT_R8G8B8A8_UINT);
    default: return sp_format_vk(VK_FORMAT_R8G8B8A8_UNORM);
    }
}

static bool parse_view(const char* s, VkImageViewType* out) {
    static const struct {
        const char* name;
        VkImageViewType type;
    } k[] = {{"1d", VK_IMAGE_VIEW_TYPE_1D},     {"2d", VK_IMAGE_VIEW_TYPE_2D},
             {"3d", VK_IMAGE_VIEW_TYPE_3D},     {"cube", VK_IMAGE_VIEW_TYPE_CUBE},
             {"1darray", VK_IMAGE_VIEW_TYPE_1D_ARRAY}, {"2darray", VK_IMAGE_VIEW_TYPE_2D_ARRAY},
             {"cubearray", VK_IMAGE_VIEW_TYPE_CUBE_ARRAY}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (ieq(s, k[i].name)) {
            *out = k[i].type;
            return true;
        }
    return false;
}

static VkImageViewType view_of(const SrBinding* sb) {
    switch (sb->dim) {
    case 0: return sb->arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 2: return VK_IMAGE_VIEW_TYPE_3D;
    case 3: return sb->arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return sb->arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

static void image_adjust(SpImage* img, double scale, double bias) {
    if (scale == 1 && bias == 0)
        return;
    for (size_t i = 0; i < (size_t)img->w * img->h * 4; i++)
        img->rgba[i] = (float)(img->rgba[i] * scale + bias);
}

static bool parse_compare(const char* s, VkCompareOp* out) {
    static const char* const names[] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};
    for (int i = 0; i < 8; i++)
        if (ieq(s, names[i])) {
            *out = (VkCompareOp)i;
            return true;
        }
    return false;
}

static bool parse_wrap(const char* s, VkSamplerAddressMode* out) {
    if (ieq(s, "repeat"))
        *out = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    else if (ieq(s, "mirror"))
        *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    else if (ieq(s, "clamp"))
        *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    else if (ieq(s, "border"))
        *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    else if (ieq(s, "mirror_clamp"))
        *out = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    else
        return false;
    return true;
}

/* the sampler of r (its image's format known for a combined image sampler: a format without linear filtering gets
   nearest, a depth image a comparison, unless the keys say otherwise) */
static bool make_sampler(Gpu* g, Res* r, const KvList* l, const SrBinding* sb) {
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const char* filter = kv_get(l, "filter");
    const char* mip = kv_get(l, "mip");
    const char* wrap = kv_get(l, "wrap");
    const char* aniso = kv_get(l, "aniso");
    const char* compare = kv_get(l, "compare");
    const char* border = kv_get(l, "border");
    bool linear_ok = true;
    if (r->fmt) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g->phys, r->fmt->vk, &fp);
        linear_ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    }
    bool nearest = filter ? ieq(filter, "nearest") : !linear_ok;
    if (!nearest && !linear_ok)
        sp_warn(g->b, "binding %u.%u: %s has no linear filtering on this device", r->set, r->binding, r->fmt->name);
    sci.magFilter = sci.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.mipmapMode = (mip ? ieq(mip, "nearest") : !linear_ok) ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                              : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (!compare && sb && sb->depth && r->fmt)
        compare = "lequal";
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (wrap && !parse_wrap(wrap, &sci.addressModeU)) {
        fprintf(stderr, "snx-shaderperf: wrap=%s: repeat, mirror, clamp, border or mirror_clamp\n", wrap);
        return false;
    }
    sci.addressModeV = sci.addressModeW = sci.addressModeU;
    sci.mipLodBias = kv_get(l, "lod_bias") ? (float)atof(kv_get(l, "lod_bias")) : 0.0f;
    if (aniso && atof(aniso) > 1) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = (float)atof(aniso);
        if (sci.maxAnisotropy > g->props.limits.maxSamplerAnisotropy)
            sci.maxAnisotropy = g->props.limits.maxSamplerAnisotropy;
    }
    if (compare) {
        sci.compareEnable = VK_TRUE;
        if (!parse_compare(compare, &sci.compareOp)) {
            fprintf(stderr, "snx-shaderperf: compare=%s: never less equal lequal greater notequal gequal always\n",
                    compare);
            return false;
        }
    }
    sci.minLod = kv_get(l, "min_lod") ? (float)atof(kv_get(l, "min_lod")) : 0.0f;
    sci.maxLod = kv_get(l, "max_lod") ? (float)atof(kv_get(l, "max_lod")) : VK_LOD_CLAMP_NONE;
    sci.borderColor = border && ieq(border, "white")   ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                      : border && ieq(border, "black") ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                       : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.unnormalizedCoordinates = kv_get(l, "unnormalized") && atoi(kv_get(l, "unnormalized")) ? VK_TRUE : VK_FALSE;
    if (sci.unnormalizedCoordinates) {
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.maxLod = 0;
        sci.anisotropyEnable = VK_FALSE;
    }
    CHECK(vkCreateSampler(g->dev, &sci, NULL, &r->sampler));
    size_t n = strlen(r->desc);
    snprintf(r->desc + n, sizeof r->desc - n, "%ssampler %s/%s%s%s%s%s", n ? " + " : "",
             sci.magFilter == VK_FILTER_NEAREST ? "nearest" : "linear",
             sci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST ? "nearest" : "linear", wrap ? " wrap " : "",
             wrap ? wrap : "", compare ? " compare " : "", compare ? compare : "");
    return true;
}

/* the bytes of each level of an image, all its layers, level after level */
static size_t image_bytes(const Res* r, uint32_t level) {
    return sp_format_level_bytes(r->fmt, mip_dim(r->w, level), mip_dim(r->h, level), mip_dim(r->d, level)) *
           r->layers;
}

static bool image_upload(Gpu* g, Res* r) {
    VkCommandBuffer cb;
    if (r->samples > 1 || !r->init) {
        /* no copies into multisampled images: a clear */
        cb = once_begin(g);
        image_barrier(cb, r->img, r->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange range = {r->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (r->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue zero = {{0}};
            vkCmdClearColorImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        } else {
            VkClearDepthStencilValue zero = {0, 0};
            vkCmdClearDepthStencilImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        }
        image_barrier(cb, r->img, r->aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, r->layout);
        return once_end(g, cb);
    }
    Buf st;
    if (!buf_make(g, r->init_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &st))
        return false;
    memcpy(st.map, r->init, r->init_size);
    cb = once_begin(g);
    image_barrier(cb, r->img, r->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, st.buf, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, r->region_count, r->regions);
    image_barrier(cb, r->img, r->aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, r->layout);
    bool ok = once_end(g, cb);
    buf_free(g, &st);
    return ok;
}

/* the contents of level 0 to the last of an image, as image_bytes lays them out */
static bool image_read(Gpu* g, VkImage img, VkImageLayout layout, const VkBufferImageCopy* regions, uint32_t count,
                       VkImageAspectFlags aspect, uint8_t* out, size_t size) {
    Buf st;
    if (!buf_make(g, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &st))
        return false;
    VkCommandBuffer cb = once_begin(g);
    image_barrier(cb, img, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buf, count, regions);
    image_barrier(cb, img, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    bool ok = once_end(g, cb);
    if (ok)
        memcpy(out, st.map, size);
    buf_free(g, &st);
    return ok;
}

/* fills the image's first contents from raw=, file= or pattern= */
static bool image_contents(Gpu* g, Res* r, const KvList* l, bool mips_given) {
    const SpConfig* cfg = &g->b->cfg;
    const char* raw = kv_get(l, "raw");
    const char* file = kv_get(l, "file");
    const char* pattern = kv_get(l, "pattern");
    double scale = kv_get(l, "scale") ? atof(kv_get(l, "scale")) : 1.0;
    double bias = kv_get(l, "bias") ? atof(kv_get(l, "bias")) : 0.0;
    size_t total = 0;
    for (uint32_t m = 0; m < r->mips; m++)
        total += image_bytes(r, m);
    r->init_size = total;
    r->init = xcalloc(total, 1);
    r->regions = xcalloc(r->mips, sizeof *r->regions);
    r->region_count = r->mips;
    size_t off = 0;
    for (uint32_t m = 0; m < r->mips; m++) {
        VkBufferImageCopy* c = &r->regions[m];
        c->bufferOffset = off;
        c->imageSubresource = (VkImageSubresourceLayers){r->aspect, m, 0, r->layers};
        c->imageExtent = (VkExtent3D){mip_dim(r->w, m), mip_dim(r->h, m), mip_dim(r->d, m)};
        off += image_bytes(r, m);
    }
    size_t level0_one = sp_format_level_bytes(r->fmt, r->w, r->h, r->d);
    if (raw) {
        size_t n;
        uint8_t* data = read_file(raw, &n);
        if (!data) {
            fprintf(stderr, "snx-shaderperf: cannot read raw=%s\n", raw);
            return false;
        }
        if (n == total) {
            memcpy(r->init, data, total);
        } else if (n == level0_one || n == level0_one * r->layers) {
            if (r->mips > 1 && mips_given) {
                fprintf(stderr,
                        "snx-shaderperf: raw=%s has %zu bytes, level 0 alone; mips=%u needs %zu bytes (every level, "
                        "each level's layers together, level 0 first)\n",
                        raw, n, r->mips, total);
                free(data);
                return false;
            }
            r->mips = 1;
            r->region_count = 1;
            r->init_size = level0_one * r->layers;
            for (uint32_t k = 0; k < r->layers; k++)
                memcpy(r->init + level0_one * k, data + (n == level0_one ? 0 : level0_one * k), level0_one);
        } else {
            fprintf(stderr,
                    "snx-shaderperf: raw=%s has %zu bytes; a %ux%ux%u %s image wants %zu (level 0, one layer), %zu "
                    "(level 0, %u layers) or %zu (%u levels)\n",
                    raw, n, r->w, r->h, r->d, r->fmt->name, level0_one, level0_one * r->layers, r->layers, total,
                    r->mips);
            free(data);
            return false;
        }
        free(data);
        snprintf(r->desc + strlen(r->desc), sizeof r->desc - strlen(r->desc), " raw %s", raw);
        return true;
    }
    bool block = r->fmt->kind == SP_KIND_BC || r->fmt->kind == SP_KIND_ETC2 || r->fmt->kind == SP_KIND_ASTC;
    if (block) {
        if (file) {
            fprintf(stderr, "snx-shaderperf: %s is a block format: give its blocks with raw=, file= transcodes "
                            "only to plain formats\n",
                    r->fmt->name);
            return false;
        }
        if (pattern && !ieq(pattern, "noise") && !ieq(pattern, "zero")) {
            fprintf(stderr, "snx-shaderperf: a block format takes pattern=noise (random blocks) or zero\n");
            return false;
        }
        uint64_t st = cfg->seed ^ ((uint64_t)r->set << 32 | r->binding);
        if (!pattern || ieq(pattern, "noise"))
            for (size_t i = 0; i < total; i++)
                r->init[i] = (uint8_t)sp_rand(&st);
        sp_warn(g->b, "binding %u.%u: random %s blocks; give real data with raw= for timings like a game's", r->set,
                r->binding, r->fmt->name);
        snprintf(r->desc + strlen(r->desc), sizeof r->desc - strlen(r->desc), " random blocks");
        return true;
    }
    SpImage src = {0};
    char err[256];
    if (file) {
        if (!sp_image_load(file, &src, err, sizeof err)) {
            fprintf(stderr, "snx-shaderperf: %s\n", err);
            return false;
        }
    } else {
        uint64_t seed = cfg->seed ^ ((uint64_t)r->set << 32 | r->binding);
        if (!sp_image_pattern(pattern ? pattern : "noise", r->w, r->h * r->d, seed, &src)) {
            fprintf(stderr, "snx-shaderperf: pattern=%s: noise, gradient, checker, solid:R,G,B,A or zero\n", pattern);
            return false;
        }
    }
    image_adjust(&src, scale, bias);
    for (uint32_t m = 0; m < r->mips; m++) {
        uint32_t mw = mip_dim(r->w, m), mh = mip_dim(r->h, m) * mip_dim(r->d, m);
        SpImage lv = {0};
        if (src.w == mw && src.h == mh) {
            lv = src;
        } else if (!sp_image_resize(&src, mw, mh, &lv)) {
            fprintf(stderr, "snx-shaderperf: out of memory resizing\n");
            sp_image_free(&src);
            return false;
        }
        size_t one = sp_format_level_bytes(r->fmt, mip_dim(r->w, m), mip_dim(r->h, m), mip_dim(r->d, m));
        uint8_t* dst = r->init + r->regions[m].bufferOffset;
        bool ok = sp_encode(r->fmt, &lv, dst);
        if (lv.rgba != src.rgba)
            sp_image_free(&lv);
        if (!ok) {
            fprintf(stderr, "snx-shaderperf: cannot encode to %s\n", r->fmt->name);
            sp_image_free(&src);
            return false;
        }
        for (uint32_t k = 1; k < r->layers; k++)
            memcpy(dst + one * k, dst, one);
    }
    sp_image_free(&src);
    snprintf(r->desc + strlen(r->desc), sizeof r->desc - strlen(r->desc), " %s%s", file ? "file " : "pattern ",
             file ? file : pattern ? pattern : "noise");
    return true;
}

static bool make_image(Gpu* g, Res* r, const SrBinding* sb, const KvList* l) {
    const SpConfig* cfg = &g->b->cfg;
    bool storage = r->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const char* fs = kv_get(l, "format");
    r->fmt = fs ? sp_format_find(fs) : default_image_format(sb);
    if (!r->fmt) {
        fprintf(stderr, "snx-shaderperf: binding %u.%u: unknown format \"%s\" (--list-formats)\n", r->set, r->binding,
                fs ? fs : "?");
        return false;
    }
    r->vtype = view_of(sb);
    if (kv_get(l, "view") && !parse_view(kv_get(l, "view"), &r->vtype)) {
        fprintf(stderr, "snx-shaderperf: view=%s: 1d 2d 3d cube 1darray 2darray cubearray\n", kv_get(l, "view"));
        return false;
    }
    bool cube = r->vtype == VK_IMAGE_VIEW_TYPE_CUBE || r->vtype == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    bool is3d = r->vtype == VK_IMAGE_VIEW_TYPE_3D;
    bool is1d = r->vtype == VK_IMAGE_VIEW_TYPE_1D || r->vtype == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    if (storage) {
        r->w = cfg->width;
        r->h = cfg->height;
        r->d = is3d ? cfg->depth : 1;
    } else {
        r->w = r->h = is3d ? 128 : cube ? 512 : 1024;
        r->d = is3d ? 128 : 1;
    }
    if (is1d)
        r->h = 1;
    const char* size = kv_get(l, "size");
    if (size && !parse_dims(size, &r->w, &r->h, &r->d)) {
        fprintf(stderr, "snx-shaderperf: size=%s: WxH or WxHxD\n", size);
        return false;
    }
    if (!is3d)
        r->d = 1;
    r->layers = r->vtype == VK_IMAGE_VIEW_TYPE_CUBE         ? 6
                : r->vtype == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 12
                : r->vtype == VK_IMAGE_VIEW_TYPE_2D_ARRAY || r->vtype == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 4
                                                                                                    : 1;
    if (kv_get(l, "layers"))
        r->layers = (uint32_t)atoi(kv_get(l, "layers"));
    if (!r->layers || (cube && r->layers % 6)) {
        fprintf(stderr, "snx-shaderperf: binding %u.%u: %u layers%s\n", r->set, r->binding, r->layers,
                cube ? ", a cube needs a multiple of 6" : "");
        return false;
    }
    r->samples = sb->ms ? 4 : 1;
    if (kv_get(l, "samples"))
        r->samples = (uint32_t)atoi(kv_get(l, "samples"));
    const char* mips = kv_get(l, "mips");
    uint32_t full = full_mips(r->w, r->h, r->d);
    r->mips = storage || r->samples > 1 || kv_get(l, "raw") ? 1 : full;
    if (mips)
        r->mips = ieq(mips, "full") ? full : (uint32_t)atoi(mips);
    if (!r->mips || r->mips > full)
        r->mips = full;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    if (kv_get(l, "tiling") && ieq(kv_get(l, "tiling"), "linear"))
        tiling = VK_IMAGE_TILING_LINEAR;
    r->aspect = aspect_of(r->fmt);
    r->layout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (kv_get(l, "layout") && ieq(kv_get(l, "layout"), "general"))
        r->layout = VK_IMAGE_LAYOUT_GENERAL;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              (storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageType itype = is3d ? VK_IMAGE_TYPE_3D : is1d ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    VkImageCreateFlags flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    VkImageFormatProperties ifp;
    VkResult q = vkGetPhysicalDeviceImageFormatProperties(g->phys, r->fmt->vk, itype, tiling, usage, flags, &ifp);
    if (q != VK_SUCCESS) {
        fprintf(stderr,
                "snx-shaderperf: binding %u.%u: the device can't make a %s %s image of %s with %s tiling (%d); "
                "--list-formats shows what it supports\n",
                r->set, r->binding, is3d ? "3D" : is1d ? "1D" : "2D", storage ? "storage" : "sampled", r->fmt->name,
                tiling == VK_IMAGE_TILING_LINEAR ? "linear" : "optimal", (int)q);
        return false;
    }
    if (!(ifp.sampleCounts & r->samples)) {
        fprintf(stderr, "snx-shaderperf: binding %u.%u: %s has no %u-sample images\n", r->set, r->binding,
                r->fmt->name, r->samples);
        return false;
    }
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = flags;
    ici.imageType = itype;
    ici.format = r->fmt->vk;
    ici.extent = (VkExtent3D){r->w, r->h, r->d};
    ici.mipLevels = r->mips;
    ici.arrayLayers = r->layers;
    ici.samples = (VkSampleCountFlagBits)r->samples;
    ici.tiling = tiling;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(g->dev, &ici, NULL, &r->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g->dev, r->img, &req);
    if (!alloc(g, req, false, &r->imem))
        return false;
    CHECK(vkBindImageMemory(g->dev, r->img, r->imem, 0));
    char dims[48];
    if (is3d)
        snprintf(dims, sizeof dims, "%ux%ux%u", r->w, r->h, r->d);
    else
        snprintf(dims, sizeof dims, "%ux%u", r->w, r->h);
    snprintf(r->desc, sizeof r->desc, "%s %s, %u layers, %u mips, %s%s%s", r->fmt->name, dims, r->layers, r->mips,
             tiling == VK_IMAGE_TILING_LINEAR ? "linear" : "optimal",
             r->layout == VK_IMAGE_LAYOUT_GENERAL ? " general" : "", r->samples > 1 ? ", multisampled" : "");
    if (r->samples == 1 && !image_contents(g, r, l, mips != NULL))
        return false;
    if (!image_upload(g, r))
        return false;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = r->img;
    vci.viewType = r->vtype;
    vci.format = r->fmt->vk;
    vci.subresourceRange = (VkImageSubresourceRange){r->aspect, 0, r->mips, 0, r->layers};
    if (r->vtype == VK_IMAGE_VIEW_TYPE_1D || r->vtype == VK_IMAGE_VIEW_TYPE_2D || r->vtype == VK_IMAGE_VIEW_TYPE_3D)
        vci.subresourceRange.layerCount = 1;
    if (r->vtype == VK_IMAGE_VIEW_TYPE_CUBE)
        vci.subresourceRange.layerCount = 6;
    if (storage)
        vci.subresourceRange.levelCount = 1;
    CHECK(vkCreateImageView(g->dev, &vci, NULL, &r->view));
    return true;
}

/* fills size bytes of a buffer's first contents from fill= value= file= */
static bool buffer_contents(Gpu* g, Res* r, const KvList* l, size_t size) {
    r->init_size = size;
    r->init = xcalloc(size, 1);
    const char* file = kv_get(l, "file");
    const char* fill = kv_get(l, "fill");
    const char* value = kv_get(l, "value");
    uint64_t st = g->b->cfg.seed ^ ((uint64_t)r->set << 32 | r->binding) ^ 0xb0ffull;
    if (file) {
        size_t n;
        uint8_t* data = read_file(file, &n);
        if (!data) {
            fprintf(stderr, "snx-shaderperf: cannot read file=%s\n", file);
            return false;
        }
        memcpy(r->init, data, n < size ? n : size);
        free(data);
        snprintf(r->desc + strlen(r->desc), sizeof r->desc - strlen(r->desc), " file %s", file);
        return true;
    }
    if (value) {
        /* type:v,v,... repeated over the buffer */
        uint8_t pat[1024];
        size_t plen = 0;
        char type[8] = "f32";
        const char* vals = value;
        const char* colon = strchr(value, ':');
        if (colon && colon - value < (int)sizeof type) {
            memcpy(type, value, (size_t)(colon - value));
            type[colon - value] = 0;
            vals = colon + 1;
        }
        char* copy = dupstr(vals);
        for (char* t = strtok(copy, ","); t && plen + 8 <= sizeof pat; t = strtok(NULL, ",")) {
            if (ieq(type, "f32")) {
                float f = (float)atof(t);
                memcpy(pat + plen, &f, 4);
                plen += 4;
            } else if (ieq(type, "f64")) {
                double d = atof(t);
                memcpy(pat + plen, &d, 8);
                plen += 8;
            } else if (ieq(type, "u32") || ieq(type, "i32")) {
                uint32_t u = (uint32_t)strtoll(t, NULL, 0);
                memcpy(pat + plen, &u, 4);
                plen += 4;
            } else if (ieq(type, "u16") || ieq(type, "i16")) {
                uint16_t u = (uint16_t)strtol(t, NULL, 0);
                memcpy(pat + plen, &u, 2);
                plen += 2;
            } else if (ieq(type, "u8") || ieq(type, "i8")) {
                pat[plen++] = (uint8_t)strtol(t, NULL, 0);
            } else if (ieq(type, "u64") || ieq(type, "i64")) {
                uint64_t u = (uint64_t)strtoull(t, NULL, 0);
                memcpy(pat + plen, &u, 8);
                plen += 8;
            } else {
                fprintf(stderr, "snx-shaderperf: value=%s: types f32 f64 u8 i8 u16 i16 u32 i32 u64 i64\n", value);
                free(copy);
                return false;
            }
        }
        free(copy);
        for (size_t i = 0; plen && i < size; i++)
            r->init[i] = pat[i % plen];
        snprintf(r->desc + strlen(r->desc), sizeof r->desc - strlen(r->desc), " value %s", value);
        return true;
    }
    if (!fill || ieq(fill, "zero")) {
        snprintf(r->desc + strlen(r->desc), sizeof r->desc - strlen(r->desc), ", fill zero");
        return true;
    }
    if (ieq(fill, "noise")) { /* floats in 0..1 */
        for (size_t i = 0; i + 4 <= size; i += 4) {
            float f = (float)((double)(sp_rand(&st) >> 40) / (double)(1 << 24));
            memcpy(r->init + i, &f, 4);
        }
    } else if (ieq(fill, "bytes")) {
        for (size_t i = 0; i < size; i++)
            r->init[i] = (uint8_t)sp_rand(&st);
    } else if (ieq(fill, "seq")) { /* u32 0, 1, 2... */
        for (size_t i = 0; i + 4 <= size; i += 4) {
            uint32_t u = (uint32_t)(i / 4);
            memcpy(r->init + i, &u, 4);
        }
    } else if (ieq(fill, "ones")) {
        for (size_t i = 0; i + 4 <= size; i += 4) {
            float f = 1.0f;
            memcpy(r->init + i, &f, 4);
        }
    } else {
        fprintf(stderr, "snx-shaderperf: fill=%s: zero, noise (floats 0..1), bytes, seq (u32 index), ones (1.0f)\n",
                fill);
        return false;
    }
    snprintf(r->desc + strlen(r->desc), sizeof r->desc - strlen(r->desc), ", fill %s", fill);
    return true;
}

static bool make_buffer(Gpu* g, Res* r, const SrBinding* sb, const KvList* l) {
    const SpConfig* cfg = &g->b->cfg;
    bool texel = r->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || r->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    bool uniform = r->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uint64_t size = 0;
    const char* ssize = kv_get(l, "size");
    const char* elements = kv_get(l, "elements");
    if (texel) {
        const char* fs = kv_get(l, "format");
        r->fmt = fs ? sp_format_find(fs) : NULL;
        if (!r->fmt && !fs)
            r->fmt = sb->image_format ? sp_format_vk(sr_image_format_vk(sb->image_format))
                     : sb->sampled_scalar == SR_UINT ? sp_format_vk(VK_FORMAT_R32_UINT)
                     : sb->sampled_scalar == SR_SINT ? sp_format_vk(VK_FORMAT_R32_SINT)
                                                     : sp_format_vk(VK_FORMAT_R32G32B32A32_SFLOAT);
        if (!r->fmt || r->fmt->bw != 1) {
            fprintf(stderr, "snx-shaderperf: binding %u.%u: format \"%s\" for a texel buffer\n", r->set, r->binding,
                    fs ? fs : "?");
            return false;
        }
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g->phys, r->fmt->vk, &fp);
        VkFormatFeatureFlags need = r->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                        ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT
                                        : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
        if (!(fp.bufferFeatures & need)) {
            fprintf(stderr, "snx-shaderperf: binding %u.%u: the device has no %s texel buffers of %s\n", r->set,
                    r->binding, r->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ? "storage" : "uniform",
                    r->fmt->name);
            return false;
        }
        uint64_t n = cfg->default_buffer / r->fmt->bytes;
        if (elements)
            n = strtoull(elements, NULL, 0);
        if (n > g->props.limits.maxTexelBufferElements)
            n = g->props.limits.maxTexelBufferElements;
        size = n * r->fmt->bytes;
    } else if (elements && sb->runtime_stride) {
        size = sb->block_size + strtoull(elements, NULL, 0) * sb->runtime_stride;
    } else if (uniform) {
        size = sb->block_size;
    } else {
        size = sb->runtime_stride ? cfg->default_buffer : sb->block_size;
        if (size < sb->block_size)
            size = sb->block_size;
    }
    if (ssize && !sp_parse_size(ssize, &size)) {
        fprintf(stderr, "snx-shaderperf: size=%s\n", ssize);
        return false;
    }
    if (!size)
        size = 16;
    size = (size + 15) & ~(uint64_t)15;
    if (kv_get(l, "file") && !ssize && !elements) {
        size_t n = 0;
        uint8_t* d = read_file(kv_get(l, "file"), &n);
        free(d);
        if (n)
            size = ((uint64_t)n + 15) & ~(uint64_t)15;
    }
    uint64_t limit = uniform ? g->props.limits.maxUniformBufferRange : g->props.limits.maxStorageBufferRange;
    if (!texel && size > limit) {
        sp_warn(g->b, "binding %u.%u: %llu bytes pass the device's range of %llu; the descriptor covers the range",
                r->set, r->binding, (unsigned long long)size, (unsigned long long)limit);
    }
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    switch (r->type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT; break;
    default: usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT; break;
    }
    if (!buf_make(g, size, usage, false, &r->b))
        return false;
    if (texel)
        snprintf(r->desc, sizeof r->desc, "%s x%llu (%llu bytes)", r->fmt->name,
                 (unsigned long long)(size / r->fmt->bytes), (unsigned long long)size);
    else
        snprintf(r->desc, sizeof r->desc, "%llu bytes", (unsigned long long)size);
    if (!buffer_contents(g, r, l, (size_t)size))
        return false;
    if (!buffer_upload(g, &r->b, r->init, r->init_size))
        return false;
    if (texel) {
        VkBufferViewCreateInfo bvci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        bvci.buffer = r->b.buf;
        bvci.format = r->fmt->vk;
        bvci.range = VK_WHOLE_SIZE;
        CHECK(vkCreateBufferView(g->dev, &bvci, NULL, &r->bview));
    }
    return true;
}

/* the --bind argument of a binding: "S.B" or the variable's name */
static const char* bind_spec(const SpConfig* cfg, const SrBinding* sb) {
    char key[32];
    snprintf(key, sizeof key, "%u.%u", sb->set, sb->binding);
    for (size_t i = 0; i < cfg->bind_count; i++)
        if (strcmp(cfg->binds[i].target, key) == 0 || (sb->name[0] && strcmp(cfg->binds[i].target, sb->name) == 0))
            return cfg->binds[i].value;
    return NULL;
}

static Res* find_res(Gpu* g, uint32_t set, uint32_t binding) {
    for (size_t i = 0; i < g->res_count; i++)
        if (g->res[i].set == set && g->res[i].binding == binding)
            return &g->res[i];
    return NULL;
}

static bool add_res(Gpu* g, const SrBinding* sb) {
    const SpConfig* cfg = &g->b->cfg;
    Res* have = find_res(g, sb->set, sb->binding);
    if (have) {
        if (have->type != sb->type) {
            fprintf(stderr, "snx-shaderperf: binding %u.%u is a %s in one shader and a %s in another\n", sb->set,
                    sb->binding, sr_descriptor_name(have->type), sr_descriptor_name(sb->type));
            return false;
        }
        have->writable |= sb->writable;
        return true;
    }
    if (sb->set >= MAX_SETS) {
        fprintf(stderr, "snx-shaderperf: set %u: the tool handles sets 0 to %d\n", sb->set, MAX_SETS - 1);
        return false;
    }
    if (sb->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT || sb->type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        fprintf(stderr, "snx-shaderperf: binding %u.%u: %s descriptors are not supported\n", sb->set, sb->binding,
                sr_descriptor_name(sb->type));
        return false;
    }
    Res* r = &g->res[g->res_count++];
    memset(r, 0, sizeof *r);
    r->set = sb->set;
    r->binding = sb->binding;
    r->type = sb->type;
    r->writable = sb->writable;
    KvList l;
    kv_parse(bind_spec(cfg, sb), &l);
    r->count = sb->count ? sb->count : (kv_get(&l, "count") ? (uint32_t)atoi(kv_get(&l, "count")) : cfg->runtime_count);
    if (!r->count)
        r->count = 1;
    bool ok = true;
    switch (r->type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
        ok = make_sampler(g, r, &l, NULL);
        break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        ok = make_image(g, r, sb, &l) && make_sampler(g, r, &l, sb);
        break;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        ok = make_image(g, r, sb, &l);
        break;
    default:
        ok = make_buffer(g, r, sb, &l);
        break;
    }
    kv_free(&l);
    if (ok)
        sp_note_resource(g->b, "%u.%u %s%s%s%s: %s%s", r->set, r->binding, sr_descriptor_name(r->type),
                         sb->name[0] ? " " : "", sb->name, r->count > 1 ? " (array)" : "", r->desc,
                         r->writable ? " [written]" : "");
    return ok;
}

static void res_free(Gpu* g, Res* r) {
    if (r->bview)
        vkDestroyBufferView(g->dev, r->bview, NULL);
    buf_free(g, &r->b);
    if (r->view)
        vkDestroyImageView(g->dev, r->view, NULL);
    if (r->img)
        vkDestroyImage(g->dev, r->img, NULL);
    if (r->imem)
        vkFreeMemory(g->dev, r->imem, NULL);
    if (r->sampler)
        vkDestroySampler(g->dev, r->sampler, NULL);
    free(r->init);
    free(r->regions);
}

/* ---- shaders ---- */

/* "path#entry": the path, and the entry (NULL when not given), in buf */
static const char* split_entry(const char* spec, char* buf, size_t size, const char** entry) {
    snprintf(buf, size, "%s", spec);
    char* hash = strrchr(buf, '#');
    *entry = NULL;
    if (hash) {
        *hash = 0;
        *entry = hash + 1;
    }
    return buf;
}

static uint32_t* load_spirv(const char* path, size_t* size) {
    uint8_t* data = read_file(path, size);
    if (!data) {
        fprintf(stderr, "snx-shaderperf: cannot read %s\n", path);
        return NULL;
    }
    if (*size % 4) {
        fprintf(stderr, "snx-shaderperf: %s: %zu bytes is not whole SPIR-V words\n", path, *size);
        free(data);
        return NULL;
    }
    uint32_t* words = xcalloc(*size / 4, 4);
    memcpy(words, data, *size);
    free(data);
    return words;
}

/* the companion of stage s that goes with shader v */
static size_t ci(const Gpu* g, int s, size_t v) { return g->comp_count[s] > 1 ? v : 0; }

/* the module of stage s in shader v's pipeline: the measured shader, a companion, or NULL for none (or the built-in
   vertex shader) */
static const SrModule* stage_mod(const Gpu* g, int s, size_t v) {
    if (g->stage == k_stage_bit[s])
        return &g->b->shaders[v].mod;
    return g->comp_count[s] ? &g->comp[s][ci(g, s, v)] : NULL;
}

/* the modules of shader v's pipeline, the measured one first: their count */
static size_t pipe_mods(const Gpu* g, size_t v, const SrModule* out[ST_COUNT]) {
    size_t n = 0;
    out[n++] = &g->b->shaders[v].mod;
    for (int s = 0; s < ST_COUNT; s++)
        if (g->stage != k_stage_bit[s] && g->comp_count[s])
            out[n++] = &g->comp[s][ci(g, s, v)];
    return n;
}

static bool load_comp(Gpu* g, int s, const char* spec) {
    char path[1024], err[256];
    const char* entry;
    split_entry(spec, path, sizeof path, &entry);
    size_t k = g->comp_count[s]++;
    g->comp_words[s][k] = load_spirv(path, &g->comp_size[s][k]);
    if (!g->comp_words[s][k])
        return false;
    if (!sr_reflect(g->comp_words[s][k], g->comp_size[s][k], entry, &g->comp[s][k], err, sizeof err)) {
        fprintf(stderr, "snx-shaderperf: %s: %s\n", path, err);
        return false;
    }
    if (g->comp[s][k].entry.stage != k_stage_bit[s]) {
        fprintf(stderr, "snx-shaderperf: %s %s is a %s shader\n", k_stage_opt[s], path,
                sr_stage_name(g->comp[s][k].entry.stage));
        return false;
    }
    return true;
}

static VkFormat attr_format(const SrVarying* v) {
    static const VkFormat f32[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                   VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat f16[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT,
                                   VK_FORMAT_R16G16B16A16_SFLOAT};
    static const VkFormat f64[] = {VK_FORMAT_R64_SFLOAT, VK_FORMAT_R64G64_SFLOAT, VK_FORMAT_R64G64B64_SFLOAT,
                                   VK_FORMAT_R64G64B64A64_SFLOAT};
    static const VkFormat s32[] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT,
                                   VK_FORMAT_R32G32B32A32_SINT};
    static const VkFormat u32[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                                   VK_FORMAT_R32G32B32A32_UINT};
    static const VkFormat s16[] = {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT,
                                   VK_FORMAT_R16G16B16A16_SINT};
    static const VkFormat u16[] = {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT,
                                   VK_FORMAT_R16G16B16A16_UINT};
    static const VkFormat s8[] = {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT,
                                  VK_FORMAT_R8G8B8A8_SINT};
    static const VkFormat u8[] = {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT,
                                  VK_FORMAT_R8G8B8A8_UINT};
    int n = (v->vec ? v->vec : 1) - 1;
    if (n > 3)
        n = 3;
    switch (v->scalar) {
    case SR_FLOAT: return v->width == 16 ? f16[n] : v->width == 64 ? f64[n] : f32[n];
    case SR_SINT: return v->width == 16 ? s16[n] : v->width == 8 ? s8[n] : s32[n];
    case SR_UINT: return v->width == 16 ? u16[n] : v->width == 8 ? u8[n] : u32[n];
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkFormat rt_default(const SrVarying* v) {
    static const VkFormat s32[] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32A32_SINT,
                                   VK_FORMAT_R32G32B32A32_SINT};
    static const VkFormat u32[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT,
                                   VK_FORMAT_R32G32B32A32_UINT};
    int n = (v->vec ? v->vec : 1) - 1;
    if (n > 3)
        n = 3;
    if (v->scalar == SR_SINT)
        return s32[n];
    if (v->scalar == SR_UINT)
        return u32[n];
    return VK_FORMAT_R8G8B8A8_UNORM;
}

static bool make_target(Gpu* g, Target* t, const SpFormat* f, bool depth) {
    const SpConfig* cfg = &g->b->cfg;
    t->fmt = f;
    t->vk = f->vk;
    t->aspect = depth ? (f->vk == VK_FORMAT_D24_UNORM_S8_UINT || f->vk == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                                 f->vk == VK_FORMAT_D16_UNORM_S8_UINT
                             ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                             : VK_IMAGE_ASPECT_DEPTH_BIT)
                      : VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g->phys, f->vk, &fp);
    VkFormatFeatureFlags need = depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (!(fp.optimalTilingFeatures & need)) {
        fprintf(stderr, "snx-shaderperf: the device can't render to %s\n", f->name);
        return false;
    }
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = f->vk;
    ici.extent = (VkExtent3D){cfg->width, cfg->height, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = (VkSampleCountFlagBits)cfg->samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CHECK(vkCreateImage(g->dev, &ici, NULL, &t->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g->dev, t->img, &req);
    if (!alloc(g, req, false, &t->mem))
        return false;
    CHECK(vkBindImageMemory(g->dev, t->img, t->mem, 0));
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t->img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = f->vk;
    vci.subresourceRange = (VkImageSubresourceRange){t->aspect, 0, 1, 0, 1};
    CHECK(vkCreateImageView(g->dev, &vci, NULL, &t->view));
    VkCommandBuffer cb = once_begin(g);
    image_barrier(cb, t->img, t->aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                  depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    return once_end(g, cb);
}

static void target_free(Gpu* g, Target* t) {
    if (t->view)
        vkDestroyImageView(g->dev, t->view, NULL);
    if (t->img)
        vkDestroyImage(g->dev, t->img, NULL);
    if (t->mem)
        vkFreeMemory(g->dev, t->mem, NULL);
    memset(t, 0, sizeof *t);
}


static bool parse_topology(const char* s, VkPrimitiveTopology* out) {
    static const struct {
        const char* name;
        VkPrimitiveTopology t;
    } k[] = {{"points", VK_PRIMITIVE_TOPOLOGY_POINT_LIST},       {"lines", VK_PRIMITIVE_TOPOLOGY_LINE_LIST},
             {"linestrip", VK_PRIMITIVE_TOPOLOGY_LINE_STRIP},    {"triangles", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
             {"strip", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP},     {"fan", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (ieq(s, k[i].name)) {
            *out = k[i].t;
            return true;
        }
    return false;
}

/* render targets, vertex buffers and the index buffer of a graphics pipeline */
static bool make_graphics(Gpu* g) {
    SpConfig* cfg = &g->b->cfg;
    const SrModule* fs = stage_mod(g, ST_FS, 0);
    const SrModule* vs = stage_mod(g, ST_VS, 0);
    if (!vs) {
        /* the built-in vertex shader feeds the next stage */
        const SrModule* next = stage_mod(g, ST_GS, 0) ? stage_mod(g, ST_GS, 0) : fs;
        g->builtin_vs = true;
        g->fullscreen = g->stage == VK_SHADER_STAGE_FRAGMENT_BIT;
        for (size_t i = 0; i < next->input_count; i++) {
            const SrVarying* v = &next->inputs[i];
            if (v->builtin)
                continue;
            if (v->scalar != SR_FLOAT || v->width != 32 || v->location + v->locations > 8 || v->vec > 4) {
                fprintf(stderr,
                        "snx-shaderperf: the %s shader's input %s at location %u is %s%u x%u; the built-in "
                        "vertex shader gives float vec4 at locations 0 to 7 only: give one with --vs (--print-vs "
                        "writes a matching GLSL one)\n",
                        sr_stage_name(next->entry.stage), v->name[0] ? v->name : "?", v->location,
                        sr_scalar_name(v->scalar), v->width, v->vec);
                return false;
            }
        }
    }
    if (!parse_topology(cfg->topology, &g->topology)) {
        fprintf(stderr, "snx-shaderperf: --topology %s: points lines linestrip triangles strip fan\n", cfg->topology);
        return false;
    }
    if (!cfg->vertices)
        cfg->vertices = g->stage == VK_SHADER_STAGE_VERTEX_BIT ? 3u * 65536u : 3u;
    if (fs) {
        for (size_t i = 0; i < fs->output_count; i++) {
            const SrVarying* v = &fs->outputs[i];
            if (v->builtin)
                continue;
            for (uint32_t k = 0; k < v->locations && v->location + k < SP_MAX_RTS; k++) {
                uint32_t loc = v->location + k;
                if (g->rt_formats[loc])
                    continue;
                VkFormat vk = rt_default(v);
                if (cfg->rt_formats[loc]) {
                    const SpFormat* f = sp_format_find(cfg->rt_formats[loc]);
                    if (!f) {
                        fprintf(stderr, "snx-shaderperf: --rt %u=%s: unknown format\n", loc, cfg->rt_formats[loc]);
                        return false;
                    }
                    vk = f->vk;
                }
                g->rt_formats[loc] = vk;
                if (loc + 1 > g->rt_count)
                    g->rt_count = loc + 1;
            }
        }
        for (uint32_t loc = 0; loc < g->rt_count; loc++) {
            if (!g->rt_formats[loc])
                continue;
            if (!make_target(g, &g->rts[loc], sp_format_vk(g->rt_formats[loc]), false))
                return false;
            sp_note_resource(g->b, "rt%u: %s %ux%u%s", loc, g->rts[loc].fmt->name, cfg->width, cfg->height,
                             cfg->samples > 1 ? " multisampled" : "");
        }
    }
    if (cfg->depth_format) {
        const SpFormat* f = sp_format_find(cfg->depth_format);
        if (!f || f->kind != SP_KIND_DEPTH) {
            fprintf(stderr, "snx-shaderperf: --depth %s: a depth format (d16_unorm, d32_sfloat...)\n",
                    cfg->depth_format);
            return false;
        }
        if (!make_target(g, &g->depth, f, true))
            return false;
        g->have_depth = true;
        sp_note_resource(g->b, "depth: %s %ux%u%s", f->name, cfg->width, cfg->height,
                         cfg->samples > 1 ? " multisampled" : "");
    }
    /* vertex inputs, from noise in each attribute's format */
    if (vs && !g->builtin_vs) {
        for (size_t i = 0; i < vs->input_count; i++) {
            const SrVarying* v = &vs->inputs[i];
            if (v->builtin)
                continue;
            for (uint32_t k = 0; k < v->locations; k++) {
                if (g->attr_count >= MAX_VERTEX_INPUTS) {
                    fprintf(stderr, "snx-shaderperf: more than %d vertex input locations\n", MAX_VERTEX_INPUTS);
                    return false;
                }
                VkFormat vk = attr_format(v);
                const SpFormat* f = sp_format_vk(vk);
                if (!f) {
                    fprintf(stderr, "snx-shaderperf: vertex input %s: no attribute format for %s%u x%u\n", v->name,
                            sr_scalar_name(v->scalar), v->width, v->vec);
                    return false;
                }
                uint32_t a = g->attr_count++;
                g->attrs[a] = (VkVertexInputAttributeDescription){v->location + k, a, vk, 0};
                g->vbinds[a] = (VkVertexInputBindingDescription){a, f->bytes, VK_VERTEX_INPUT_RATE_VERTEX};
                SpImage img = {0};
                const char* fill = cfg->vertex_fill ? cfg->vertex_fill : "noise";
                if (!sp_image_pattern(fill, cfg->vertices, 1, cfg->seed + a, &img)) {
                    fprintf(stderr, "snx-shaderperf: --vertex-fill %s: noise, gradient, checker, solid:..., zero\n",
                            fill);
                    return false;
                }
                if (v->scalar != SR_FLOAT)
                    image_adjust(&img, 255.0, 0.0);
                size_t bytes = (size_t)f->bytes * cfg->vertices;
                uint8_t* data = xcalloc(bytes, 1);
                bool ok = sp_encode(f, &img, data);
                sp_image_free(&img);
                ok = ok && buf_make(g, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    false, &g->vbufs[a]) &&
                     buffer_upload(g, &g->vbufs[a], data, bytes);
                free(data);
                if (!ok)
                    return false;
                sp_note_resource(g->b, "vertex input %u (%s): %s x%u %s", v->location + k, v->name, f->name,
                                 cfg->vertices, fill);
            }
        }
    }
    if (cfg->index && !ieq(cfg->index, "none")) {
        bool random = ieq(cfg->index, "random");
        if (!random && !ieq(cfg->index, "seq")) {
            fprintf(stderr, "snx-shaderperf: --index %s: none, seq or random\n", cfg->index);
            return false;
        }
        size_t bytes = (size_t)cfg->vertices * 4;
        uint32_t* idx = xcalloc(cfg->vertices, 4);
        uint64_t st = cfg->seed ^ 0x1dull;
        for (uint32_t i = 0; i < cfg->vertices; i++)
            idx[i] = random ? (uint32_t)(sp_rand(&st) % cfg->vertices) : i;
        bool ok = buf_make(g, bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
                           &g->ibuf) &&
                  buffer_upload(g, &g->ibuf, idx, bytes);
        free(idx);
        if (!ok)
            return false;
        g->indexed = true;
        sp_note_resource(g->b, "index buffer: %u u32 indices, %s", cfg->vertices, random ? "random" : "sequential");
    }
    return true;
}

/* ---- per-shader state ---- */

static uint32_t push_size_of(Gpu* g, size_t v) {
    const SrModule* mods[ST_COUNT];
    size_t n = pipe_mods(g, v, mods);
    uint32_t s = 0;
    for (size_t k = 0; k < n; k++)
        if (mods[k]->push_size > s)
            s = mods[k]->push_size;
    return (s + 3) & ~3u;
}

/* parses "[type:]v,v,..." into bytes at out for a member (type from it when not given). The bytes written */
static int encode_values(const char* value, const SrMember* m, uint8_t* out, size_t room) {
    char type[8] = "";
    const char* vals = value;
    const char* colon = strchr(value, ':');
    if (colon && colon - value < (int)sizeof type && isalpha((unsigned char)value[0])) {
        memcpy(type, value, (size_t)(colon - value));
        type[colon - value] = 0;
        vals = colon + 1;
    }
    if (!type[0]) {
        if (m && m->scalar == SR_FLOAT)
            snprintf(type, sizeof type, "f%u", m->width ? m->width : 32);
        else if (m && m->scalar == SR_SINT)
            snprintf(type, sizeof type, "i%u", m->width ? m->width : 32);
        else if (m && (m->scalar == SR_UINT || m->scalar == SR_BOOL))
            snprintf(type, sizeof type, "u%u", m->width ? m->width : 32);
        else
            snprintf(type, sizeof type, "%s", strchr(vals, '.') ? "f32" : "u32");
    }
    char* copy = dupstr(vals);
    size_t n = 0;
    bool hex = ieq(type, "hex");
    for (char* t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        if (hex) {
            for (const char* p = t; p[0] && p[1]; p += 2) {
                unsigned byte;
                if (n >= room || sscanf(p, "%2x", &byte) != 1)
                    goto bad;
                out[n++] = (uint8_t)byte;
            }
            continue;
        }
        int bits = atoi(type + 1);
        size_t bytes = (size_t)bits / 8;
        if (!bytes || n + bytes > room || !strchr("fiu", tolower((unsigned char)type[0])))
            goto bad;
        if (tolower((unsigned char)type[0]) == 'f') {
            double d = atof(t);
            if (bits == 32) {
                float f = (float)d;
                memcpy(out + n, &f, 4);
            } else if (bits == 64) {
                memcpy(out + n, &d, 8);
            } else if (bits == 16) {
                /* half: through the format table's encoder */
                SpImage img = {1, 1, NULL};
                float px[4] = {(float)d, 0, 0, 1};
                img.rgba = px;
                sp_encode(sp_format_vk(VK_FORMAT_R16_SFLOAT), &img, out + n);
            } else {
                goto bad;
            }
        } else {
            uint64_t u = tolower((unsigned char)type[0]) == 'i' ? (uint64_t)strtoll(t, NULL, 0) : strtoull(t, NULL, 0);
            for (size_t k = 0; k < bytes; k++)
                out[n + k] = (uint8_t)(u >> (8 * k));
        }
        n += bytes;
    }
    free(copy);
    return (int)n;
bad:
    free(copy);
    fprintf(stderr, "snx-shaderperf: push value \"%s\": types f16 f32 f64 i8..i64 u8..u64 hex, within the block\n",
            value);
    return -1;
}

static bool build_push(Gpu* g, size_t v) {
    const SpConfig* cfg = &g->b->cfg;
    Variant* var = &g->var[v];
    const SrModule* m = &g->b->shaders[v].mod;
    var->push_size = push_size_of(g, v);
    if (var->push_size > sizeof var->push || var->push_size > g->props.limits.maxPushConstantsSize) {
        fprintf(stderr, "snx-shaderperf: %u bytes of push constants pass the device's %u\n", var->push_size,
                g->props.limits.maxPushConstantsSize);
        return false;
    }
    memset(var->push, 0, sizeof var->push);
    if (cfg->push_file) {
        size_t n;
        uint8_t* data = read_file(cfg->push_file, &n);
        if (!data) {
            fprintf(stderr, "snx-shaderperf: cannot read --push-file %s\n", cfg->push_file);
            return false;
        }
        memcpy(var->push, data, n < var->push_size ? n : var->push_size);
        free(data);
    }
    for (size_t i = 0; i < cfg->push_count; i++) {
        const char* target = cfg->pushes[i].target;
        const SrMember* mem = NULL;
        uint32_t offset;
        char* end;
        unsigned long off = strtoul(target, &end, 0);
        if (*end == 0) {
            offset = (uint32_t)off;
            for (size_t k = 0; k < m->push_count; k++)
                if (m->push[k].offset == offset)
                    mem = &m->push[k];
        } else {
            for (size_t k = 0; k < m->push_count && !mem; k++)
                if (strcmp(m->push[k].name, target) == 0)
                    mem = &m->push[k];
            const SrModule* mods[ST_COUNT];
            size_t nm = pipe_mods(g, v, mods);
            for (size_t j = 1; j < nm; j++)
                for (size_t k = 0; k < mods[j]->push_count && !mem; k++)
                    if (strcmp(mods[j]->push[k].name, target) == 0)
                        mem = &mods[j]->push[k];
            if (!mem) {
                fprintf(stderr, "snx-shaderperf: --push %s: no such member (--reflect lists them)\n", target);
                return false;
            }
            offset = mem->offset;
        }
        if (offset >= var->push_size) {
            fprintf(stderr, "snx-shaderperf: --push %s: offset %u is past the %u-byte block\n", target, offset,
                    var->push_size);
            return false;
        }
        if (encode_values(cfg->pushes[i].value, mem, var->push + offset, var->push_size - offset) < 0)
            return false;
    }
    return true;
}

static bool build_spec(Gpu* g, size_t v) {
    const SpConfig* cfg = &g->b->cfg;
    Variant* var = &g->var[v];
    const SrModule* m = &g->b->shaders[v].mod;
    size_t used = 0;
    var->spec = (VkSpecializationInfo){0};
    for (size_t i = 0; i < cfg->spec_count; i++) {
        const char* target = cfg->specs[i].target;
        const SrSpec* s = NULL;
        char* end;
        unsigned long id = strtoul(target, &end, 0);
        for (size_t k = 0; k < m->spec_count && !s; k++)
            if ((*end == 0 && m->specs[k].spec_id == id) || (*end != 0 && strcmp(m->specs[k].name, target) == 0))
                s = &m->specs[k];
        if (!s) {
            if (g->b->shader_count > 1)
                continue; /* another shader's constant */
            fprintf(stderr, "snx-shaderperf: --spec %s: no such specialization constant (--reflect lists them)\n",
                    target);
            return false;
        }
        const char* val = cfg->specs[i].value;
        uint32_t size = s->scalar == SR_BOOL ? 4 : s->width / 8;
        if (used + size > sizeof var->spec_data || var->spec.mapEntryCount >= 64)
            return false;
        uint8_t* dst = var->spec_data + used;
        if (s->scalar == SR_BOOL) {
            uint32_t b = ieq(val, "true") || atoi(val) != 0;
            memcpy(dst, &b, 4);
        } else {
            SrMember mm = {{0}, 0, size, s->scalar, s->width, 1, 1, 0, 0};
            if (encode_values(val, &mm, dst, size) < 0)
                return false;
        }
        var->spec_map[var->spec.mapEntryCount++] = (VkSpecializationMapEntry){s->spec_id, (uint32_t)used, size};
        used += size;
    }
    var->spec.dataSize = used;
    var->spec.pData = var->spec_data;
    var->spec.pMapEntries = var->spec_map;
    /* the workgroup size, with its specialization */
    SpShader* sh = &g->b->shaders[v];
    for (int a = 0; a < 3; a++) {
        if (m->local_spec[a] == UINT32_MAX)
            continue;
        for (uint32_t k = 0; k < var->spec.mapEntryCount; k++)
            if (var->spec_map[k].constantID == m->local_spec[a])
                memcpy(&sh->mod.local[a], var->spec_data + var->spec_map[k].offset, 4);
    }
    return true;
}

/* set layouts, descriptor sets and the pipeline layout of a shader with the companion stage */
static bool make_layout(Gpu* g, size_t v) {
    Variant* var = &g->var[v];
    const SrModule* mods[ST_COUNT];
    size_t nm = pipe_mods(g, v, mods);
    uint32_t max_set = 0;
    bool any = false;
    for (size_t k = 0; k < nm; k++)
        for (size_t i = 0; i < mods[k]->binding_count; i++) {
            any = true;
            if (mods[k]->bindings[i].set > max_set)
                max_set = mods[k]->bindings[i].set;
        }
    var->set_count = any ? max_set + 1 : 0;
    for (uint32_t s = 0; s < var->set_count; s++) {
        VkDescriptorSetLayoutBinding lb[64];
        uint32_t n = 0;
        for (size_t k = 0; k < nm; k++)
            for (size_t i = 0; i < mods[k]->binding_count; i++) {
                const SrBinding* sb = &mods[k]->bindings[i];
                if (sb->set != s)
                    continue;
                bool dup = false;
                for (uint32_t j = 0; j < n; j++)
                    dup |= lb[j].binding == sb->binding;
                if (dup || n >= 64)
                    continue;
                Res* r = find_res(g, s, sb->binding);
                lb[n++] = (VkDescriptorSetLayoutBinding){sb->binding, sb->type, r->count, VK_SHADER_STAGE_ALL, NULL};
            }
        VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, n, lb};
        CHECK(vkCreateDescriptorSetLayout(g->dev, &lci, NULL, &var->sets[s]));
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g->dpool, 1,
                                           &var->sets[s]};
        CHECK(vkAllocateDescriptorSets(g->dev, &dai, &var->dsets[s]));
        for (uint32_t j = 0; j < n; j++) {
            Res* r = find_res(g, s, lb[j].binding);
            VkDescriptorImageInfo* ii = xcalloc(r->count, sizeof *ii);
            VkDescriptorBufferInfo* bi = xcalloc(r->count, sizeof *bi);
            VkBufferView* bv = xcalloc(r->count, sizeof *bv);
            for (uint32_t e = 0; e < r->count; e++) {
                ii[e] = (VkDescriptorImageInfo){r->sampler, r->view, r->layout};
                VkDeviceSize range = r->b.size;
                VkDeviceSize limit = r->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         ? g->props.limits.maxUniformBufferRange
                                         : g->props.limits.maxStorageBufferRange;
                if (range > limit)
                    range = limit;
                bi[e] = (VkDescriptorBufferInfo){r->b.buf, 0, range};
                bv[e] = r->bview;
            }
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = var->dsets[s];
            w.dstBinding = lb[j].binding;
            w.descriptorCount = r->count;
            w.descriptorType = r->type;
            w.pImageInfo = ii;
            w.pBufferInfo = bi;
            w.pTexelBufferView = bv;
            vkUpdateDescriptorSets(g->dev, 1, &w, 0, NULL);
            free(ii);
            free(bi);
            free(bv);
        }
    }
    VkPushConstantRange pr = {VK_SHADER_STAGE_ALL, 0, var->push_size};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = var->set_count;
    plci.pSetLayouts = var->sets;
    plci.pushConstantRangeCount = var->push_size ? 1 : 0;
    plci.pPushConstantRanges = &pr;
    CHECK(vkCreatePipelineLayout(g->dev, &plci, NULL, &var->layout));
    return true;
}

static bool make_pool(Gpu* g) {
    VkDescriptorPoolSize sizes[16];
    uint32_t n = 0, sets = 0;
    for (size_t v = 0; v < g->b->shader_count; v++) {
        const SrModule* mods[ST_COUNT];
        size_t nm = pipe_mods(g, v, mods);
        for (size_t k = 0; k < nm; k++)
            for (size_t i = 0; i < mods[k]->binding_count; i++) {
                const SrBinding* sb = &mods[k]->bindings[i];
                Res* r = find_res(g, sb->set, sb->binding);
                uint32_t j = 0;
                while (j < n && sizes[j].type != sb->type)
                    j++;
                if (j == n && n < 16)
                    sizes[n++] = (VkDescriptorPoolSize){sb->type, 0};
                if (j < 16)
                    sizes[j].descriptorCount += r->count;
            }
        sets += MAX_SETS;
    }
    if (!n)
        sizes[n++] = (VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, sets, n, sizes};
    CHECK(vkCreateDescriptorPool(g->dev, &dpci, NULL, &g->dpool));
    return true;
}

static VkShaderModule make_module(Gpu* g, const uint32_t* words, size_t size) {
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, words};
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(g->dev, &smci, NULL, &m);
    if (r != VK_SUCCESS)
        fprintf(stderr, "snx-shaderperf: vkCreateShaderModule failed: %d\n", (int)r);
    return m;
}

static bool create_pipeline(Gpu* g, size_t v, VkPipelineCreateFlags flags, VkPipeline* out) {
    const SpConfig* cfg = &g->b->cfg;
    Variant* var = &g->var[v];
    SpShader* sh = &g->b->shaders[v];
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, cfg->subgroup_size};
    VkPipelineShaderStageCreateInfo stages[ST_COUNT] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = g->stage;
    stages[0].module = var->module;
    stages[0].pName = sh->mod.entry.name;
    stages[0].pSpecializationInfo = var->spec.mapEntryCount ? &var->spec : NULL;
    if (cfg->subgroup_size)
        stages[0].pNext = &rss;
    if (cfg->full_subgroups)
        stages[0].flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    if (g->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
        VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.flags = flags;
        cpci.stage = stages[0];
        cpci.layout = var->layout;
        CHECK(vkCreateComputePipelines(g->dev, VK_NULL_HANDLE, 1, &cpci, NULL, out));
        return true;
    }
    uint32_t stage_count = 1;
    for (int s = 0; s < ST_COUNT; s++) {
        if (g->stage == k_stage_bit[s])
            continue;
        VkShaderModule m = VK_NULL_HANDLE;
        const char* name = "main";
        if (g->comp_count[s]) {
            m = g->comp_module[s][ci(g, s, v)];
            name = g->comp[s][ci(g, s, v)].entry.name;
        } else if (s == ST_VS && g->builtin_vs) {
            m = g->builtin_module;
        }
        if (!m)
            continue;
        stages[stage_count] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[stage_count].stage = k_stage_bit[s];
        stages[stage_count].module = m;
        stages[stage_count].pName = name;
        stage_count++;
    }
    bool has_fs = stage_mod(g, ST_FS, v) != NULL;
    VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vis.vertexBindingDescriptionCount = g->attr_count;
    vis.pVertexBindingDescriptions = g->vbinds;
    vis.vertexAttributeDescriptionCount = g->attr_count;
    vis.pVertexAttributeDescriptions = g->attrs;
    VkPipelineInputAssemblyStateCreateInfo ias = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ias.topology = g->fullscreen ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : g->topology;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.rasterizerDiscardEnable = !has_fs;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = (VkSampleCountFlagBits)cfg->samples;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = g->have_depth && cfg->depth_test;
    ds.depthWriteEnable = g->have_depth && cfg->depth_write;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba[SP_MAX_RTS];
    for (uint32_t i = 0; i < SP_MAX_RTS; i++) {
        cba[i] = (VkPipelineColorBlendAttachmentState){0};
        cba[i].colorWriteMask = 0xf;
        VkFormatProperties fp = {0};
        if (i < g->rt_count && g->rt_formats[i])
            vkGetPhysicalDeviceFormatProperties(g->phys, g->rt_formats[i], &fp);
        bool can_blend = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
        if (cfg->blend && !ieq(cfg->blend, "none") && can_blend) {
            cba[i].blendEnable = VK_TRUE;
            bool add = ieq(cfg->blend, "add");
            cba[i].srcColorBlendFactor = add ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
            cba[i].dstColorBlendFactor = add ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].dstAlphaBlendFactor = add ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
    }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = g->rt_count;
    cbs.pAttachments = cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dys = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pri.colorAttachmentCount = g->rt_count;
    pri.pColorAttachmentFormats = g->rt_formats;
    if (g->have_depth) {
        pri.depthAttachmentFormat = g->depth.vk;
        if (g->depth.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
            pri.stencilAttachmentFormat = g->depth.vk;
    }
    VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.pNext = &pri;
    gpci.flags = flags;
    gpci.stageCount = stage_count;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cbs;
    gpci.pDynamicState = &dys;
    gpci.layout = var->layout;
    CHECK(vkCreateGraphicsPipelines(g->dev, VK_NULL_HANDLE, 1, &gpci, NULL, out));
    return true;
}

/* ---- recording ---- */

static void record_body(Gpu* g, size_t v, VkCommandBuffer cb, uint32_t reps, bool clear) {
    const SpConfig* cfg = &g->b->cfg;
    Variant* var = &g->var[v];
    SpShader* sh = &g->b->shaders[v];
    bool compute = g->stage == VK_SHADER_STAGE_COMPUTE_BIT;
    VkPipelineBindPoint bp = compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline(cb, bp, var->pipe);
    if (var->set_count)
        vkCmdBindDescriptorSets(cb, bp, var->layout, 0, var->set_count, var->dsets, 0, NULL);
    if (var->push_size)
        vkCmdPushConstants(cb, var->layout, VK_SHADER_STAGE_ALL, 0, var->push_size, var->push);
    if (compute) {
        for (uint32_t r = 0; r < reps; r++) {
            vkCmdDispatch(cb, sh->dispatch[0], sh->dispatch[1], sh->dispatch[2]);
            if (!cfg->overlap && r + 1 < reps)
                barrier_all(cb);
        }
        return;
    }
    VkViewport vp = {0, 0, (float)cfg->width, (float)cfg->height, 0, 1};
    VkRect2D sc = {{0, 0}, {cfg->width, cfg->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    if (g->attr_count) {
        VkBuffer bufs[MAX_VERTEX_INPUTS];
        VkDeviceSize offs[MAX_VERTEX_INPUTS] = {0};
        for (uint32_t i = 0; i < g->attr_count; i++)
            bufs[i] = g->vbufs[i].buf;
        vkCmdBindVertexBuffers(cb, 0, g->attr_count, bufs, offs);
    }
    if (g->indexed)
        vkCmdBindIndexBuffer(cb, g->ibuf.buf, 0, VK_INDEX_TYPE_UINT32);
    VkRenderingAttachmentInfo cols[SP_MAX_RTS];
    VkAttachmentLoadOp load = clear || cfg->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    for (uint32_t i = 0; i < g->rt_count; i++) {
        cols[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        cols[i].imageView = g->rts[i].view;
        cols[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        cols[i].loadOp = load;
        cols[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo dep = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    dep.imageView = g->depth.view;
    dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    dep.loadOp = clear || cfg->clear || cfg->depth_test ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    dep.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = sc;
    ri.layerCount = 1;
    ri.colorAttachmentCount = g->rt_count;
    ri.pColorAttachments = cols;
    ri.pDepthAttachment = g->have_depth ? &dep : NULL;
    ri.pStencilAttachment = g->have_depth && (g->depth.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &dep : NULL;
    uint32_t instances = g->fullscreen ? 1 : cfg->instances;
    uint32_t verts = g->fullscreen ? 3 : cfg->vertices;
    bool one_pass = cfg->overlap;
    if (one_pass)
        vkCmdBeginRendering(cb, &ri);
    for (uint32_t r = 0; r < reps; r++) {
        if (!one_pass)
            vkCmdBeginRendering(cb, &ri);
        if (g->indexed)
            vkCmdDrawIndexed(cb, verts, instances, 0, 0, 0);
        else
            vkCmdDraw(cb, verts, instances, 0, 0);
        if (!one_pass) {
            vkCmdEndRendering(cb);
            if (r + 1 < reps)
                barrier_all(cb);
        }
    }
    if (one_pass)
        vkCmdEndRendering(cb);
}

static bool record_timed(Gpu* g, size_t v, uint32_t reps) {
    Variant* var = &g->var[v];
    if (!var->cb) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g->pool,
                                           VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        CHECK(vkAllocateCommandBuffers(g->dev, &cai, &var->cb));
    } else {
        CHECK(vkResetCommandBuffer(var->cb, 0));
    }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(var->cb, &bi));
    uint32_t q = (uint32_t)v;
    vkCmdResetQueryPool(var->cb, g->ts_pool, 2 * q, 2);
    if (g->stat_pool)
        vkCmdResetQueryPool(var->cb, g->stat_pool, q, 1);
    barrier_all(var->cb);
    vkCmdWriteTimestamp(var->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g->ts_pool, 2 * q);
    if (g->stat_pool)
        vkCmdBeginQuery(var->cb, g->stat_pool, q, 0);
    record_body(g, v, var->cb, reps, false);
    if (g->stat_pool)
        vkCmdEndQuery(var->cb, g->stat_pool, q);
    vkCmdWriteTimestamp(var->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->ts_pool, 2 * q + 1);
    CHECK(vkEndCommandBuffer(var->cb));
    return true;
}

/* ---- timing ---- */

static bool calibrated(Gpu* g, uint64_t* gpu) {
    VkCalibratedTimestampInfoKHR info = {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, NULL,
                                         VK_TIME_DOMAIN_DEVICE_KHR};
    uint64_t dev = 0;
    if (g->has_calib_khr && vkGetCalibratedTimestampsKHR)
        return vkGetCalibratedTimestampsKHR(g->dev, 1, &info, gpu, &dev) == VK_SUCCESS;
    if (g->has_calib_ext && vkGetCalibratedTimestampsEXT)
        return vkGetCalibratedTimestampsEXT(g->dev, 1, &info, gpu, &dev) == VK_SUCCESS;
    return false;
}

static void push_sample(SpShader* sh, double per_rep, double total, double wall, double latency) {
    if (sh->sample_count == sh->sample_cap) {
        size_t cap = sh->sample_cap ? sh->sample_cap * 2 : 256;
        sh->samples = realloc(sh->samples, cap * sizeof(double));
        sh->totals = realloc(sh->totals, cap * sizeof(double));
        sh->wall = realloc(sh->wall, cap * sizeof(double));
        sh->latency = realloc(sh->latency, cap * sizeof(double));
        if (!sh->samples || !sh->totals || !sh->wall || !sh->latency) {
            fprintf(stderr, "snx-shaderperf: out of memory\n");
            exit(2);
        }
        sh->sample_cap = cap;
    }
    sh->samples[sh->sample_count] = per_rep;
    sh->totals[sh->sample_count] = total;
    sh->wall[sh->sample_count] = wall;
    sh->latency[sh->sample_count] = latency;
    sh->sample_count++;
}

/* one submission of shader v: its ns per repetition in *per_rep; kept as a sample when keep */
static bool sample(Gpu* g, size_t v, uint32_t reps, bool keep, double* per_rep) {
    const SpConfig* cfg = &g->b->cfg;
    SpShader* sh = &g->b->shaders[v];
    uint64_t at_submit = 0;
    bool have_cal = cfg->calibrate && calibrated(g, &at_submit);
    double t0 = now_ns();
    if (!submit_wait(g, g->var[v].cb, NULL))
        return false;
    double t1 = now_ns();
    uint64_t ts[2];
    CHECK(vkGetQueryPoolResults(g->dev, g->ts_pool, 2 * (uint32_t)v, 2, sizeof ts, ts, 8,
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    uint64_t mask = g->ts_mask_bits >= 64 ? ~0ull : (1ull << g->ts_mask_bits) - 1;
    double total = (double)((ts[1] - ts[0]) & mask) * g->props.limits.timestampPeriod;
    *per_rep = total / reps;
    if (g->stat_pool) {
        uint64_t st[16];
        uint32_t n = 0;
        for (uint32_t bit = 0; bit < 16; bit++)
            n += (g->stat_flags >> bit) & 1;
        if (vkGetQueryPoolResults(g->dev, g->stat_pool, (uint32_t)v, 1, sizeof st, st, sizeof st,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            static const char* const names[] = {"ia_vertices", "ia_primitives", "vs_invocations", "gs_invocations",
                                                "gs_primitives", "clip_invocations", "clip_primitives",
                                                "fs_invocations", "tcs_patches", "tes_invocations", "cs_invocations"};
            uint32_t k = 0;
            for (uint32_t bit = 0; bit < 11 && k < n; bit++) {
                if (!((g->stat_flags >> bit) & 1))
                    continue;
                sh->stat_names[k] = names[bit];
                sh->pipeline_stats[k] = st[k] / reps;
                k++;
            }
            sh->pipeline_stat_count = k;
        }
    }
    if (keep) {
        double lat = NAN;
        if (have_cal)
            lat = (double)((ts[0] - at_submit) & mask) * g->props.limits.timestampPeriod;
        push_sample(sh, *per_rep, total, t1 - t0, lat);
    }
    return true;
}

/* the relative half-width of the 95 % interval of shader v's median */
static double median_rse(const SpBench* b, size_t v) {
    const SpShader* sh = &b->shaders[v];
    if (sh->sample_count < 4)
        return INFINITY;
    double* x = xcalloc(sh->sample_count, sizeof *x);
    memcpy(x, sh->samples, sh->sample_count * sizeof *x);
    SpStats s;
    sp_stats(x, sh->sample_count, b->cfg.outliers, &s);
    free(x);
    return s.median > 0 ? (s.median_hi - s.median_lo) / 2 / s.median : INFINITY;
}

static bool measure(Gpu* g, const size_t* idx, size_t n, uint64_t* rng) {
    const SpConfig* cfg = &g->b->cfg;
    double start = now_ns();
    size_t order[SP_MAX_SHADERS];
    for (uint32_t round = 0;;) {
        memcpy(order, idx, n * sizeof *order);
        if (cfg->order == SP_ORDER_SHUFFLE)
            for (size_t i = n; i > 1; i--) {
                size_t j = (size_t)(sp_rand(rng) % i);
                size_t t = order[i - 1];
                order[i - 1] = order[j];
                order[j] = t;
            }
        for (size_t i = 0; i < n; i++) {
            double t;
            if (!sample(g, order[i], g->b->reps, true, &t))
                return false;
            sleep_ms(cfg->sleep_ms);
        }
        round++;
        double el = (now_ns() - start) / 1e6;
        if (round >= cfg->max_runs || (cfg->max_time_ms > 0 && el >= cfg->max_time_ms))
            break;
        if (cfg->target_rse > 0) {
            if (round < cfg->min_runs)
                continue;
            bool done = true;
            for (size_t i = 0; i < n && done; i++)
                done = median_rse(g->b, idx[i]) <= cfg->target_rse;
            if (done)
                break;
            continue;
        }
        if (round >= cfg->runs && el >= cfg->min_time_ms)
            break;
    }
    return true;
}

static bool run_timing(Gpu* g) {
    SpBench* b = g->b;
    const SpConfig* cfg = &b->cfg;
    uint64_t rng = cfg->seed ^ 0x0dd5ull;
    b->reps = cfg->reps_auto ? 1 : cfg->reps;
    for (size_t v = 0; v < b->shader_count; v++)
        if (!record_timed(g, v, b->reps))
            return false;
    /* warm up: clocks rise, caches fill, the driver settles */
    double start = now_ns();
    for (uint32_t i = 0; i < cfg->warmup || (now_ns() - start) / 1e6 < cfg->warmup_ms; i++)
        for (size_t v = 0; v < b->shader_count; v++) {
            double t;
            if (!sample(g, v, b->reps, false, &t))
                return false;
        }
    if (cfg->reps_auto) {
        /* the repetitions for a submission of sample_ms, from the slowest shader's median of 5 */
        double slowest = 0;
        for (size_t v = 0; v < b->shader_count; v++) {
            double t[5];
            for (int i = 0; i < 5; i++)
                if (!sample(g, v, 1, false, &t[i]))
                    return false;
            for (int i = 1; i < 5; i++)
                for (int j = i; j > 0 && t[j - 1] > t[j]; j--) {
                    double x = t[j];
                    t[j] = t[j - 1];
                    t[j - 1] = x;
                }
            if (t[2] > slowest)
                slowest = t[2];
        }
        double want = cfg->sample_ms * 1e6 / (slowest > 1 ? slowest : 1);
        b->reps = want < 1 ? 1 : want > 100000 ? 100000 : (uint32_t)ceil(want);
        for (size_t v = 0; v < b->shader_count; v++)
            if (!record_timed(g, v, b->reps))
                return false;
        for (uint32_t i = 0; i < 3; i++)
            for (size_t v = 0; v < b->shader_count; v++) {
                double t;
                if (!sample(g, v, b->reps, false, &t))
                    return false;
            }
    }
    size_t all[SP_MAX_SHADERS];
    for (size_t v = 0; v < b->shader_count; v++)
        all[v] = v;
    if (cfg->order == SP_ORDER_SEQUENTIAL) {
        for (size_t v = 0; v < b->shader_count; v++)
            if (!measure(g, &all[v], 1, &rng))
                return false;
    } else if (!measure(g, all, b->shader_count, &rng)) {
        return false;
    }
    for (size_t v = 0; v < b->shader_count; v++) {
        SpShader* sh = &b->shaders[v];
        size_t n = sh->sample_count;
        double* x = xcalloc(n, sizeof *x);
        memcpy(x, sh->samples, n * sizeof *x);
        sp_stats(x, n, cfg->outliers, &sh->time);
        memcpy(x, sh->totals, n * sizeof *x);
        sp_stats(x, n, cfg->outliers, &sh->total);
        memcpy(x, sh->wall, n * sizeof *x);
        sp_stats(x, n, cfg->outliers, &sh->wall_stats);
        size_t k = 0;
        for (size_t i = 0; i < n; i++)
            if (!isnan(sh->latency[i]))
                x[k++] = sh->latency[i];
        sp_stats(x, k, cfg->outliers, &sh->latency_stats);
        free(x);
    }
    return true;
}

/* ---- pipeline executable properties ---- */

static void sanitize(char* s) {
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.')
            *s = '_';
}

static bool exec_stats(Gpu* g, size_t v) {
    const SpConfig* cfg = &g->b->cfg;
    SpShader* sh = &g->b->shaders[v];
    VkPipelineCreateFlags flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    if (cfg->exec_ir_dir)
        flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    VkPipeline pipe;
    if (!create_pipeline(g, v, flags, &pipe))
        return false;
    VkPipelineInfoKHR pi = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, NULL, pipe};
    uint32_t n = 0;
    vkGetPipelineExecutablePropertiesKHR(g->dev, &pi, &n, NULL);
    VkPipelineExecutablePropertiesKHR* props = xcalloc(n, sizeof *props);
    for (uint32_t i = 0; i < n; i++)
        props[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    vkGetPipelineExecutablePropertiesKHR(g->dev, &pi, &n, props);
    sh->execs = xcalloc(n, sizeof *sh->execs);
    sh->exec_count = n;
    for (uint32_t i = 0; i < n; i++) {
        SpExec* e = &sh->execs[i];
        snprintf(e->name, sizeof e->name, "%s", props[i].name);
        snprintf(e->desc, sizeof e->desc, "%s", props[i].description);
        e->subgroup_size = props[i].subgroupSize;
        e->stages = props[i].stages;
        VkPipelineExecutableInfoKHR ei = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, NULL, pipe, i};
        uint32_t sn = 0;
        vkGetPipelineExecutableStatisticsKHR(g->dev, &ei, &sn, NULL);
        VkPipelineExecutableStatisticKHR* st = xcalloc(sn, sizeof *st);
        for (uint32_t k = 0; k < sn; k++)
            st[k].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        vkGetPipelineExecutableStatisticsKHR(g->dev, &ei, &sn, st);
        e->stats = xcalloc(sn, sizeof *e->stats);
        e->stat_count = sn;
        for (uint32_t k = 0; k < sn; k++) {
            SpExecStat* s = &e->stats[k];
            snprintf(s->name, sizeof s->name, "%s", st[k].name);
            snprintf(s->desc, sizeof s->desc, "%s", st[k].description);
            switch (st[k].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                snprintf(s->value, sizeof s->value, "%s", st[k].value.b32 ? "true" : "false");
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                snprintf(s->value, sizeof s->value, "%lld", (long long)st[k].value.i64);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                snprintf(s->value, sizeof s->value, "%llu", (unsigned long long)st[k].value.u64);
                break;
            default:
                snprintf(s->value, sizeof s->value, "%.17g", st[k].value.f64);
                break;
            }
        }
        free(st);
        if (cfg->exec_ir_dir) {
            uint32_t rn = 0;
            vkGetPipelineExecutableInternalRepresentationsKHR(g->dev, &ei, &rn, NULL);
            VkPipelineExecutableInternalRepresentationKHR* ir = xcalloc(rn, sizeof *ir);
            for (uint32_t k = 0; k < rn; k++)
                ir[k].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
            vkGetPipelineExecutableInternalRepresentationsKHR(g->dev, &ei, &rn, ir);
            for (uint32_t k = 0; k < rn; k++)
                ir[k].pData = xcalloc(ir[k].dataSize + 1, 1);
            vkGetPipelineExecutableInternalRepresentationsKHR(g->dev, &ei, &rn, ir);
            for (uint32_t k = 0; k < rn; k++) {
                char name[512], part[256];
                snprintf(part, sizeof part, "%s.%u.%s", sh->label, i, ir[k].name);
                sanitize(part);
                snprintf(name, sizeof name, "%s/%s.%s", cfg->exec_ir_dir, part, ir[k].isText ? "txt" : "bin");
                size_t len = ir[k].dataSize;
                if (ir[k].isText && len && ((char*)ir[k].pData)[len - 1] == 0)
                    len--;
                if (!write_file(name, ir[k].pData, len))
                    sp_warn(g->b, "cannot write %s", name);
                free(ir[k].pData);
            }
            if (!rn)
                sp_warn(g->b, "%s: the driver gives no internal representations", sh->label);
            free(ir);
        }
    }
    free(props);
    vkDestroyPipeline(g->dev, pipe, NULL);
    return true;
}

/* ---- performance counters (VK_KHR_performance_query) ---- */

static double counter_value(const VkPerformanceCounterResultKHR* r, VkPerformanceCounterStorageKHR storage) {
    switch (storage) {
    case VK_PERFORMANCE_COUNTER_STORAGE_INT32_KHR: return r->int32;
    case VK_PERFORMANCE_COUNTER_STORAGE_INT64_KHR: return (double)r->int64;
    case VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR: return r->uint32;
    case VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR: return (double)r->uint64;
    case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR: return r->float32;
    default: return r->float64;
    }
}

static const char* unit_name(VkPerformanceCounterUnitKHR u) {
    static const char* const names[] = {"generic", "percentage", "ns", "bytes", "bytes/s", "kelvin", "watts",
                                        "volts", "amps", "hertz", "cycles"};
    return (unsigned)u < sizeof names / sizeof names[0] ? names[u] : "?";
}

static VkPerformanceCounterKHR* query_counters(Gpu* g, uint32_t* n, VkPerformanceCounterDescriptionKHR** descs) {
    *n = 0;
    vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(g->phys, g->family, n, NULL, NULL);
    VkPerformanceCounterKHR* c = xcalloc(*n, sizeof *c);
    *descs = xcalloc(*n, sizeof **descs);
    for (uint32_t i = 0; i < *n; i++) {
        c[i].sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR;
        (*descs)[i].sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR;
    }
    vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(g->phys, g->family, n, c, *descs);
    return c;
}

static bool run_counters(Gpu* g) {
    SpBench* b = g->b;
    const SpConfig* cfg = &b->cfg;
    if (!g->has_perf) {
        sp_warn(b, "--counters: the device has no VK_KHR_performance_query (NVIDIA's driver exposes none; its "
                   "counters need Nsight Perf), so no counters were read");
        return true;
    }
    uint32_t n;
    VkPerformanceCounterDescriptionKHR* descs;
    VkPerformanceCounterKHR* all = query_counters(g, &n, &descs);
    uint32_t* pick = xcalloc(n, sizeof *pick);
    uint32_t count = 0;
    char* list = dupstr(cfg->counters);
    for (char* t = strtok(list, ","); t; t = strtok(NULL, ",")) {
        for (uint32_t i = 0; i < n; i++) {
            bool match = ieq(t, "all") || ieq(t, descs[i].name) || icontains(descs[i].name, t);
            bool dup = false;
            for (uint32_t k = 0; k < count; k++)
                dup |= pick[k] == i;
            if (match && !dup)
                pick[count++] = i;
        }
    }
    free(list);
    if (!count) {
        sp_warn(b, "--counters %s: no counter matches (--list-counters)", cfg->counters);
        free(pick);
        free(all);
        free(descs);
        return true;
    }
    VkQueryPoolPerformanceCreateInfoKHR pci = {VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR, NULL,
                                               g->family, count, pick};
    uint32_t passes = 0;
    vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR(g->phys, &pci, &passes);
    VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, &pci, 0,
                                  VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR, 1, 0};
    VkQueryPool pool;
    CHECK(vkCreateQueryPool(g->dev, &qpci, NULL, &pool));
    VkAcquireProfilingLockInfoKHR li = {VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR, NULL, 0, UINT64_MAX};
    if (vkAcquireProfilingLockKHR(g->dev, &li) != VK_SUCCESS) {
        sp_warn(b, "--counters: the profiling lock was refused");
        vkDestroyQueryPool(g->dev, pool, NULL);
        free(pick);
        free(all);
        free(descs);
        return true;
    }
    VkPerformanceCounterResultKHR* res = xcalloc(count, sizeof *res);
    double* values = xcalloc((size_t)count * cfg->counter_runs, sizeof *values);
    bool ok = true;
    for (size_t v = 0; v < b->shader_count && ok; v++) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g->pool,
                                           VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cb;
        if (vkAllocateCommandBuffers(g->dev, &cai, &cb) != VK_SUCCESS)
            break;
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        barrier_all(cb);
        vkCmdBeginQuery(cb, pool, 0, 0);
        record_body(g, v, cb, b->reps, false);
        vkCmdEndQuery(cb, pool, 0);
        vkEndCommandBuffer(cb);
        for (uint32_t run = 0; run < cfg->counter_runs && ok; run++) {
            if (g->has_host_reset) {
                vkResetQueryPool(g->dev, pool, 0, 1);
            } else {
                VkCommandBuffer rc = once_begin(g);
                vkCmdResetQueryPool(rc, pool, 0, 1);
                ok = once_end(g, rc);
            }
            for (uint32_t p = 0; p < passes && ok; p++) {
                VkPerformanceQuerySubmitInfoKHR psi = {VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR, NULL, p};
                ok = submit_wait(g, cb, &psi);
            }
            if (ok && vkGetQueryPoolResults(g->dev, pool, 0, 1, count * sizeof *res, res, sizeof *res * count,
                                            VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
                ok = false;
            for (uint32_t k = 0; ok && k < count; k++)
                values[(size_t)k * cfg->counter_runs + run] = counter_value(&res[k], all[pick[k]].storage);
        }
        vkFreeCommandBuffers(g->dev, g->pool, 1, &cb);
        SpShader* sh = &b->shaders[v];
        sh->counters = xcalloc(count, sizeof *sh->counters);
        sh->counter_count = ok ? count : 0;
        for (uint32_t k = 0; ok && k < count; k++) {
            SpCounter* c = &sh->counters[k];
            snprintf(c->name, sizeof c->name, "%s", descs[pick[k]].name);
            snprintf(c->category, sizeof c->category, "%s", descs[pick[k]].category);
            snprintf(c->unit, sizeof c->unit, "%s", unit_name(all[pick[k]].unit));
            sp_stats(values + (size_t)k * cfg->counter_runs, cfg->counter_runs, SP_OUTLIERS_NONE, &c->stats);
        }
    }
    if (!ok)
        sp_warn(b, "--counters: a counter pass failed");
    vkReleaseProfilingLockKHR(g->dev);
    vkDestroyQueryPool(g->dev, pool, NULL);
    free(values);
    free(res);
    free(pick);
    free(all);
    free(descs);
    return true;
}

/* ---- --check and --dump ---- */

static bool decodable_path(const char* path) {
    size_t n = strlen(path);
    return n > 4 && (ieq(path + n - 4, ".ppm") || ieq(path + n - 4, ".pfm") || ieq(path + n - 4, ".pgm"));
}

/* path with "-v" before its extension when there are several shaders */
static void dump_path(const SpBench* b, size_t v, const char* path, char* out, size_t size) {
    if (b->shader_count == 1) {
        snprintf(out, size, "%s", path);
        return;
    }
    const char* dot = strrchr(path, '.');
    const char* slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash))
        snprintf(out, size, "%s-%zu", path, v);
    else
        snprintf(out, size, "%.*s-%zu%s", (int)(dot - path), path, v, dot);
}

static bool save_image(Gpu* g, const char* path, const SpFormat* f, const uint8_t* data, size_t size, uint32_t w,
                       uint32_t h) {
    if (!decodable_path(path))
        return write_file(path, data, size);
    SpImage img;
    char err[256];
    if (!sp_decode(f, data, w, h, &img)) {
        sp_warn(g->b, "%s: %s can't be decoded; writing raw bytes instead", path, f->name);
        return write_file(path, data, size);
    }
    bool ok = sp_image_save(path, &img, err, sizeof err);
    if (!ok)
        sp_warn(g->b, "%s", err);
    sp_image_free(&img);
    return ok;
}

static const char* dump_for(const SpConfig* cfg, const char* target) {
    for (size_t i = 0; i < cfg->dump_count; i++)
        if (strcmp(cfg->dumps[i].target, target) == 0)
            return cfg->dumps[i].value;
    return NULL;
}

static void add_hash(SpShader* sh, const char* target, const uint8_t* data, size_t size) {
    if (sh->hash_count >= SP_MAX_ARGS)
        return;
    SpHash* h = &sh->hashes[sh->hash_count++];
    snprintf(h->target, sizeof h->target, "%s", target);
    h->hash = fnv1a(data, size);
    h->bytes = size;
}

/* reads a target or resource back: hashes it, and dumps it when asked */
static bool read_back(Gpu* g, size_t v, const char* target, const char* named, VkImage img, VkImageLayout layout,
                      VkImageAspectFlags aspect, const SpFormat* f, const VkBufferImageCopy* regions, uint32_t count,
                      uint32_t w, uint32_t h, Buf* buf) {
    SpBench* b = g->b;
    size_t size = 0;
    if (buf) {
        size = buf->size;
    } else {
        for (uint32_t i = 0; i < count; i++) {
            const VkBufferImageCopy* c = &regions[i];
            size_t end = c->bufferOffset + sp_format_level_bytes(f, c->imageExtent.width, c->imageExtent.height,
                                                                 c->imageExtent.depth) *
                                               c->imageSubresource.layerCount;
            if (end > size)
                size = end;
        }
    }
    uint8_t* data = xcalloc(size, 1);
    bool ok = buf ? buffer_read(g, buf, data, size) : image_read(g, img, layout, regions, count, aspect, data, size);
    if (ok) {
        add_hash(&b->shaders[v], target, data, size);
        const char* path = dump_for(&b->cfg, target);
        if (!path && named)
            path = dump_for(&b->cfg, named);
        if (path) {
            char out[1024];
            dump_path(b, v, path, out, sizeof out);
            bool saved = buf ? write_file(out, data, size) : save_image(g, out, f, data, size, w, h);
            if (!saved)
                sp_warn(b, "cannot write %s", out);
        }
    }
    free(data);
    return ok;
}

static bool run_check(Gpu* g) {
    SpBench* b = g->b;
    for (size_t v = 0; v < b->shader_count; v++) {
        /* the first contents again, and the targets cleared by the pass */
        for (size_t i = 0; i < g->res_count; i++) {
            Res* r = &g->res[i];
            if (!r->writable)
                continue;
            if (r->img ? !image_upload(g, r) : !buffer_upload(g, &r->b, r->init, r->init_size))
                return false;
        }
        VkCommandBuffer cb = once_begin(g);
        barrier_all(cb);
        record_body(g, v, cb, 1, true);
        barrier_all(cb);
        if (!once_end(g, cb))
            return false;
        for (size_t i = 0; i < g->res_count; i++) {
            Res* r = &g->res[i];
            char target[32];
            snprintf(target, sizeof target, "%u.%u", r->set, r->binding);
            if (!r->writable && !dump_for(&b->cfg, target))
                continue;
            if (r->img && r->samples > 1) {
                sp_warn(b, "%s is multisampled: not read back", target);
                continue;
            }
            if (r->img) {
                VkBufferImageCopy region = {0};
                region.imageSubresource = (VkImageSubresourceLayers){r->aspect, 0, 0, r->layers};
                region.imageExtent = (VkExtent3D){r->w, r->h, r->d};
                if (!read_back(g, v, target, NULL, r->img, r->layout, r->aspect, r->fmt, &region, 1, r->w,
                               r->h * r->d, NULL))
                    return false;
            } else if (!read_back(g, v, target, NULL, VK_NULL_HANDLE, 0, 0, NULL, NULL, 0, 0, 0, &r->b)) {
                return false;
            }
        }
        const SpConfig* cfg = &b->cfg;
        for (uint32_t i = 0; i < g->rt_count; i++) {
            if (!g->rts[i].img)
                continue;
            char target[16];
            snprintf(target, sizeof target, "rt%u", i);
            if (cfg->samples > 1) {
                sp_warn(b, "%s is multisampled: not read back", target);
                continue;
            }
            VkBufferImageCopy region = {0};
            region.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = (VkExtent3D){cfg->width, cfg->height, 1};
            if (!read_back(g, v, target, NULL, g->rts[i].img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT, g->rts[i].fmt, &region, 1, cfg->width, cfg->height, NULL))
                return false;
        }
        if (g->have_depth && cfg->samples == 1 && cfg->depth_write) {
            VkBufferImageCopy region = {0};
            region.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            region.imageExtent = (VkExtent3D){cfg->width, cfg->height, 1};
            if (!read_back(g, v, "depth", NULL, g->depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           g->depth.aspect, g->depth.fmt, &region, 1, cfg->width, cfg->height, NULL))
                return false;
        }
    }
    return true;
}

/* ---- device information ---- */

static void smi(const SpBench* b, char* out, size_t size) {
    out[0] = 0;
    if (!b->cfg.smi || b->dev.vendor != 0x10de)
        return;
    FILE* p = popen("nvidia-smi --query-gpu=clocks.sm,clocks.mem,clocks.max.sm,temperature.gpu,pstate,power.draw,"
                    "utilization.gpu --format=csv,noheader 2>/dev/null",
                    "r");
    if (!p)
        return;
    if (!fgets(out, (int)size, p))
        out[0] = 0;
    pclose(p);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = 0;
}

static const char* domain_name(VkTimeDomainKHR d) {
    switch (d) {
    case VK_TIME_DOMAIN_DEVICE_KHR: return "device";
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR: return "clock_monotonic";
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR: return "clock_monotonic_raw";
    case VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR: return "qpc";
    default: return "?";
    }
}

static void fill_device_info(Gpu* g) {
    SpDeviceInfo* d = &g->b->dev;
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &drv};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
    vkGetPhysicalDeviceProperties2(g->phys, &p2);
    snprintf(d->name, sizeof d->name, "%s", g->props.deviceName);
    snprintf(d->driver, sizeof d->driver, "%s", drv.driverName);
    snprintf(d->driver_info, sizeof d->driver_info, "%s", drv.driverInfo);
    uint32_t a = g->props.apiVersion, dv = g->props.driverVersion;
    snprintf(d->api, sizeof d->api, "%u.%u.%u", VK_API_VERSION_MAJOR(a), VK_API_VERSION_MINOR(a),
             VK_API_VERSION_PATCH(a));
    if (g->props.vendorID == 0x10de)
        snprintf(d->driver_version, sizeof d->driver_version, "%u.%u.%u", dv >> 22, (dv >> 14) & 0xff, (dv >> 6) & 0xff);
    else
        snprintf(d->driver_version, sizeof d->driver_version, "%u.%u.%u", VK_API_VERSION_MAJOR(dv),
                 VK_API_VERSION_MINOR(dv), VK_API_VERSION_PATCH(dv));
    d->vendor = g->props.vendorID;
    d->device_id = g->props.deviceID;
    d->type = g->props.deviceType;
    d->timestamp_period = g->props.limits.timestampPeriod;
    d->timestamp_bits = g->ts_mask_bits;
    d->subgroup_size = p11.subgroupSize;
    d->perf_query = g->has_perf;
    d->exec_props = g->has_exec;
    d->calibrated = g->has_calib_khr || g->has_calib_ext;
    d->stats_query = g->stat_pool != VK_NULL_HANDLE || g->stat_flags;
    d->time_domains[0] = 0;
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR get = vkGetPhysicalDeviceCalibrateableTimeDomainsKHR
                                                                 ? vkGetPhysicalDeviceCalibrateableTimeDomainsKHR
                                                                 : vkGetPhysicalDeviceCalibrateableTimeDomainsEXT;
    if (d->calibrated && get) {
        uint32_t n = 0;
        get(g->phys, &n, NULL);
        VkTimeDomainKHR* dom = xcalloc(n, sizeof *dom);
        get(g->phys, &n, dom);
        for (uint32_t i = 0; i < n; i++) {
            size_t len = strlen(d->time_domains);
            snprintf(d->time_domains + len, sizeof d->time_domains - len, "%s%s", i ? "," : "", domain_name(dom[i]));
        }
        free(dom);
    }
}

static void gpu_free(Gpu* g) {
    if (!g->dev) {
        if (g->inst) {
            if (g->messenger)
                vkDestroyDebugUtilsMessengerEXT(g->inst, g->messenger, NULL);
            vkDestroyInstance(g->inst, NULL);
        }
        return;
    }
    vkDeviceWaitIdle(g->dev);
    for (size_t v = 0; v < SP_MAX_SHADERS; v++) {
        Variant* var = &g->var[v];
        if (var->pipe)
            vkDestroyPipeline(g->dev, var->pipe, NULL);
        if (var->layout)
            vkDestroyPipelineLayout(g->dev, var->layout, NULL);
        for (uint32_t s = 0; s < var->set_count; s++)
            if (var->sets[s])
                vkDestroyDescriptorSetLayout(g->dev, var->sets[s], NULL);
        if (var->module)
            vkDestroyShaderModule(g->dev, var->module, NULL);
    }
    for (int s = 0; s < ST_COUNT; s++)
        for (size_t k = 0; k < SP_MAX_SHADERS; k++)
            if (g->comp_module[s][k])
                vkDestroyShaderModule(g->dev, g->comp_module[s][k], NULL);
    if (g->builtin_module)
        vkDestroyShaderModule(g->dev, g->builtin_module, NULL);
    if (g->dpool)
        vkDestroyDescriptorPool(g->dev, g->dpool, NULL);
    for (size_t i = 0; i < g->res_count; i++)
        res_free(g, &g->res[i]);
    free(g->res);
    for (uint32_t i = 0; i < SP_MAX_RTS; i++)
        target_free(g, &g->rts[i]);
    target_free(g, &g->depth);
    for (uint32_t i = 0; i < MAX_VERTEX_INPUTS; i++)
        buf_free(g, &g->vbufs[i]);
    buf_free(g, &g->ibuf);
    if (g->ts_pool)
        vkDestroyQueryPool(g->dev, g->ts_pool, NULL);
    if (g->stat_pool)
        vkDestroyQueryPool(g->dev, g->stat_pool, NULL);
    if (g->fence)
        vkDestroyFence(g->dev, g->fence, NULL);
    if (g->pool)
        vkDestroyCommandPool(g->dev, g->pool, NULL);
    vkDestroyDevice(g->dev, NULL);
    if (g->messenger)
        vkDestroyDebugUtilsMessengerEXT(g->inst, g->messenger, NULL);
    vkDestroyInstance(g->inst, NULL);
    for (int s = 0; s < ST_COUNT; s++)
        for (size_t k = 0; k < g->comp_count[s]; k++) {
            sr_free(&g->comp[s][k]);
            free(g->comp_words[s][k]);
        }
}

/* a device without the benchmark: for the listings */
static bool open_device(Gpu* g, const SpConfig* cfg) {
    g->stage = VK_SHADER_STAGE_COMPUTE_BIT;
    if (!make_instance(g, cfg) || !pick_device(g, cfg) || !make_device(g, cfg))
        return false;
    fill_device_info(g);
    return true;
}

int sp_gpu_list_devices(const SpConfig* cfg) {
    SpBench b;
    memset(&b, 0, sizeof b);
    Gpu g;
    memset(&g, 0, sizeof g);
    g.b = &b;
    if (!make_instance(&g, cfg))
        return 1;
    uint32_t n;
    VkPhysicalDevice* list = physical_devices(g.inst, &n);
    static const char* const types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(list[i], &p);
        printf("%u: %s (%s, Vulkan %u.%u.%u, vendor 0x%04x device 0x%04x)\n", i, p.deviceName,
               p.deviceType <= 4 ? types[p.deviceType] : "?", VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.vendorID, p.deviceID);
    }
    free(list);
    gpu_free(&g);
    return 0;
}

int sp_gpu_info(SpBench* b) {
    Gpu g;
    memset(&g, 0, sizeof g);
    g.b = b;
    if (!open_device(&g, &b->cfg)) {
        gpu_free(&g);
        return 1;
    }
    const SpDeviceInfo* d = &b->dev;
    const VkPhysicalDeviceLimits* l = &g.props.limits;
    printf("{\n  \"name\": \"%s\",\n  \"vendor\": %u,\n  \"device_id\": %u,\n  \"api\": \"%s\",\n", d->name, d->vendor,
           d->device_id, d->api);
    printf("  \"driver\": \"%s\",\n  \"driver_info\": \"%s\",\n  \"driver_version\": \"%s\",\n", d->driver,
           d->driver_info, d->driver_version);
    printf("  \"timestamp_period_ns\": %g,\n  \"timestamp_valid_bits\": %u,\n  \"subgroup_size\": %u,\n",
           d->timestamp_period, d->timestamp_bits, d->subgroup_size);
    printf("  \"queries\": {\"pipeline_executable_properties\": %s, \"performance_query\": %s, "
           "\"calibrated_timestamps\": %s, \"time_domains\": \"%s\"},\n",
           d->exec_props ? "true" : "false", d->perf_query ? "true" : "false", d->calibrated ? "true" : "false",
           d->time_domains);
    printf("  \"limits\": {\"max_compute_workgroup_invocations\": %u, \"max_compute_workgroup_size\": [%u, %u, %u], "
           "\"max_compute_workgroup_count\": [%u, %u, %u], \"max_compute_shared_memory\": %u, "
           "\"max_push_constants\": %u, \"max_uniform_buffer_range\": %u, \"max_storage_buffer_range\": %u, "
           "\"max_texel_buffer_elements\": %u, \"max_image_dimension_2d\": %u, \"max_sampler_anisotropy\": %g, "
           "\"max_color_attachments\": %u, \"framebuffer_color_sample_counts\": %u}\n}\n",
           l->maxComputeWorkGroupInvocations, l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1],
           l->maxComputeWorkGroupSize[2], l->maxComputeWorkGroupCount[0], l->maxComputeWorkGroupCount[1],
           l->maxComputeWorkGroupCount[2], l->maxComputeSharedMemorySize, l->maxPushConstantsSize,
           l->maxUniformBufferRange, l->maxStorageBufferRange, l->maxTexelBufferElements, l->maxImageDimension2D,
           l->maxSamplerAnisotropy, l->maxColorAttachments, l->framebufferColorSampleCounts);
    gpu_free(&g);
    return 0;
}

int sp_gpu_list_counters(SpBench* b) {
    Gpu g;
    memset(&g, 0, sizeof g);
    g.b = b;
    b->cfg.counters = "all";
    if (!open_device(&g, &b->cfg)) {
        gpu_free(&g);
        return 1;
    }
    if (!g.has_perf) {
        printf("%s has no VK_KHR_performance_query: no counters (timestamps, pipeline statistics and pipeline "
               "executable statistics remain)\n",
               g.props.deviceName);
        gpu_free(&g);
        return 0;
    }
    uint32_t n;
    VkPerformanceCounterDescriptionKHR* descs;
    VkPerformanceCounterKHR* all = query_counters(&g, &n, &descs);
    for (uint32_t i = 0; i < n; i++)
        printf("%s\t%s\t%s\t%s\n", descs[i].name, unit_name(all[i].unit), descs[i].category, descs[i].description);
    free(all);
    free(descs);
    gpu_free(&g);
    return 0;
}

int sp_gpu_list_formats(SpBench* b) {
    Gpu g;
    memset(&g, 0, sizeof g);
    g.b = b;
    if (!open_device(&g, &b->cfg)) {
        gpu_free(&g);
        return 1;
    }
    size_t n;
    const SpFormat* f = sp_formats(&n);
    static const char* const kinds[] = {"plain", "packed", "depth", "bc", "etc2", "astc"};
    printf("%-26s %-6s %-5s %-6s %s\n", "format", "kind", "block", "bytes", "device support");
    for (size_t i = 0; i < n; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.phys, f[i].vk, &fp);
        VkFormatFeatureFlags o = fp.optimalTilingFeatures, bf = fp.bufferFeatures;
        char block[16];
        snprintf(block, sizeof block, "%ux%u", f[i].bw, f[i].bh);
        printf("%-26s %-6s %-5s %-6u%s%s%s%s%s%s%s%s\n", f[i].name, f[i].kind < 6 ? kinds[f[i].kind] : "?", block,
               f[i].bytes, (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? " sampled" : "",
               (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? " linear" : "",
               (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? " storage" : "",
               (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? " color_attachment" : "",
               (o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? " depth_attachment" : "",
               (bf & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) ? " uniform_texel" : "",
               (bf & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) ? " storage_texel" : "",
               (bf & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? " vertex" : "");
    }
    gpu_free(&g);
    return 0;
}

/* ---- the benchmark ---- */

static bool load_shaders(SpBench* b) {
    const SpConfig* cfg = &b->cfg;
    b->shader_count = cfg->shader_count;
    for (size_t i = 0; i < cfg->shader_count; i++) {
        SpShader* sh = &b->shaders[i];
        char path[1024], err[256];
        const char* entry;
        split_entry(cfg->shaders[i], path, sizeof path, &entry);
        if (!entry)
            entry = cfg->entry;
        sh->path = cfg->shaders[i];
        const char* base = strrchr(cfg->shaders[i], '/');
        sh->label = base ? base + 1 : cfg->shaders[i];
        sh->words = load_spirv(path, &sh->size);
        if (!sh->words)
            return false;
        if (!sr_reflect(sh->words, sh->size, entry, &sh->mod, err, sizeof err)) {
            fprintf(stderr, "snx-shaderperf: %s: %s\n", path, err);
            return false;
        }
        if (sh->mod.entry.stage != b->shaders[0].mod.entry.stage) {
            fprintf(stderr, "snx-shaderperf: %s is a %s shader and %s a %s one: compare shaders of one stage\n",
                    sh->label, sr_stage_name(sh->mod.entry.stage), b->shaders[0].label,
                    sr_stage_name(b->shaders[0].mod.entry.stage));
            return false;
        }
    }
    return true;
}

bool sp_gpu_run(SpBench* b) {
    SpConfig* cfg = &b->cfg;
    double t_start = now_ns();
    if (!load_shaders(b))
        return false;
    Gpu* g = xcalloc(1, sizeof *g);
    g->b = b;
    bool ok = false;
    g->stage = b->shaders[0].mod.entry.stage;
    if (g->stage != VK_SHADER_STAGE_COMPUTE_BIT && g->stage != VK_SHADER_STAGE_FRAGMENT_BIT &&
        g->stage != VK_SHADER_STAGE_VERTEX_BIT) {
        fprintf(stderr, "snx-shaderperf: %s shaders are not supported: compute, fragment and vertex are\n",
                sr_stage_name(g->stage));
        goto out;
    }
    {
        /* the companion stages: each given once for all the shaders, or once for each */
        const char* const* lists[ST_COUNT] = {cfg->vs, cfg->gs, cfg->fs};
        size_t counts[ST_COUNT] = {cfg->vs_count, cfg->gs_count, cfg->fs_count};
        for (int s = 0; s < ST_COUNT; s++) {
            if (!counts[s])
                continue;
            if (g->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
                sp_warn(b, "%s is for graphics shaders: ignored", k_stage_opt[s]);
                continue;
            }
            if (g->stage == k_stage_bit[s]) {
                sp_warn(b, "%s: the measured shaders are that stage: ignored", k_stage_opt[s]);
                continue;
            }
            if (counts[s] != 1 && counts[s] != b->shader_count) {
                fprintf(stderr, "snx-shaderperf: %zu %s for %zu shaders: give one for all, or one each\n", counts[s],
                        k_stage_opt[s], b->shader_count);
                goto out;
            }
            for (size_t k = 0; k < counts[s]; k++)
                if (!load_comp(g, s, lists[s][k]))
                    goto out;
        }
    }
    if (!make_instance(g, cfg) || !pick_device(g, cfg) || !make_device(g, cfg))
        goto out;
    fill_device_info(g);
    smi(b, b->smi_before, sizeof b->smi_before);
    {
        VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP,
                                     2 * SP_MAX_SHADERS, 0};
        if (vkCreateQueryPool(g->dev, &qci, NULL, &g->ts_pool) != VK_SUCCESS)
            goto out;
        if (g->stat_flags) {
            qci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            qci.queryCount = SP_MAX_SHADERS;
            qci.pipelineStatistics = g->stat_flags;
            if (vkCreateQueryPool(g->dev, &qci, NULL, &g->stat_pool) != VK_SUCCESS)
                goto out;
        }
    }
    {
        size_t total = 0;
        for (int s = 0; s < ST_COUNT; s++)
            for (size_t k = 0; k < g->comp_count[s]; k++)
                total += g->comp[s][k].binding_count;
        for (size_t v = 0; v < b->shader_count; v++)
            total += b->shaders[v].mod.binding_count;
        g->res = xcalloc(total, sizeof *g->res);
        for (size_t v = 0; v < b->shader_count; v++)
            for (size_t i = 0; i < b->shaders[v].mod.binding_count; i++)
                if (!add_res(g, &b->shaders[v].mod.bindings[i]))
                    goto out;
        for (int s = 0; s < ST_COUNT; s++)
            for (size_t k = 0; k < g->comp_count[s]; k++)
                for (size_t i = 0; i < g->comp[s][k].binding_count; i++)
                    if (!add_res(g, &g->comp[s][k].bindings[i]))
                        goto out;
        for (size_t i = 0; i < cfg->bind_count; i++) {
            bool used = false;
            for (size_t k = 0; k < g->res_count && !used; k++) {
                char key[32];
                snprintf(key, sizeof key, "%u.%u", g->res[k].set, g->res[k].binding);
                used = strcmp(key, cfg->binds[i].target) == 0;
            }
            for (size_t v = 0; v < b->shader_count && !used; v++)
                for (size_t k = 0; k < b->shaders[v].mod.binding_count && !used; k++)
                    used = strcmp(b->shaders[v].mod.bindings[k].name, cfg->binds[i].target) == 0;
            if (!used)
                sp_warn(b, "--bind %s: no shader has that binding", cfg->binds[i].target);
        }
    }
    if (g->stage != VK_SHADER_STAGE_COMPUTE_BIT && !make_graphics(g))
        goto out;
    if (!make_pool(g))
        goto out;
    if (g->builtin_vs && !(g->builtin_module = make_module(g, k_fullscreen_vert, sizeof k_fullscreen_vert)))
        goto out;
    for (int s = 0; s < ST_COUNT; s++)
        for (size_t k = 0; k < g->comp_count[s]; k++)
            if (!(g->comp_module[s][k] = make_module(g, g->comp_words[s][k], g->comp_size[s][k])))
                goto out;
    for (size_t v = 0; v < b->shader_count; v++) {
        SpShader* sh = &b->shaders[v];
        Variant* var = &g->var[v];
        if (!build_push(g, v) || !build_spec(g, v) || !make_layout(g, v))
            goto out;
        var->module = make_module(g, sh->words, sh->size);
        if (!var->module)
            goto out;
        uint32_t runs = cfg->compile_runs ? cfg->compile_runs : 1;
        sh->compile = xcalloc(runs, sizeof *sh->compile);
        for (uint32_t i = 0; i < runs; i++) {
            if (var->pipe)
                vkDestroyPipeline(g->dev, var->pipe, NULL);
            var->pipe = VK_NULL_HANDLE;
            double t0 = now_ns();
            if (!create_pipeline(g, v, 0, &var->pipe))
                goto out;
            sh->compile[i] = (now_ns() - t0) / 1e6;
        }
        double* x = xcalloc(runs, sizeof *x);
        memcpy(x, sh->compile, runs * sizeof *x);
        sp_stats(x, runs, SP_OUTLIERS_NONE, &sh->compile_stats);
        free(x);
        if (cfg->exec_stats && g->has_exec && !exec_stats(g, v))
            goto out;
        if (g->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
            const uint32_t* local = sh->mod.local;
            uint32_t grid[3] = {cfg->width, cfg->height, cfg->depth};
            for (int a = 0; a < 3; a++) {
                sh->dispatch[a] = cfg->have_dispatch ? cfg->dispatch[a] : (grid[a] + local[a] - 1) / local[a];
                if (sh->dispatch[a] > g->props.limits.maxComputeWorkGroupCount[a]) {
                    fprintf(stderr, "snx-shaderperf: %s: %u workgroups on axis %d pass the device's %u\n", sh->label,
                            sh->dispatch[a], a, g->props.limits.maxComputeWorkGroupCount[a]);
                    goto out;
                }
            }
            sh->invocations = (uint64_t)sh->dispatch[0] * sh->dispatch[1] * sh->dispatch[2] * local[0] * local[1] *
                              local[2];
        } else if (g->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            sh->invocations = (uint64_t)cfg->width * cfg->height * (g->fullscreen ? 1 : cfg->instances);
        } else {
            sh->invocations = (uint64_t)cfg->vertices * cfg->instances;
        }
    }
    if (cfg->exec_stats && !g->has_exec)
        sp_warn(b, "the device has no VK_KHR_pipeline_executable_properties: no register or instruction counts");
    if (!run_timing(g))
        goto out;
    if (cfg->counters && !run_counters(g))
        goto out;
    if ((cfg->check || cfg->dump_count) && !run_check(g))
        goto out;
    smi(b, b->smi_after, sizeof b->smi_after);
    if (g->validation_errors)
        sp_warn(b, "the validation layer reported %u errors", g->validation_errors);
    ok = true;
out:
    gpu_free(g);
    free(g);
    b->elapsed_ms = (now_ns() - t_start) / 1e6;
    return ok;
}
