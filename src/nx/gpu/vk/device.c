/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan instance, device, queues and swapchain. The renderer is built on timeline semaphores (1.2), synchronization2
   and dynamic rendering (1.3): it runs on Vulkan 1.3, or on 1.2 with the extensions 1.3 took in (vk12_extensions). */
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/options.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/vertex_input.h"
#include "nx/gpu/vk/pipeline_parts.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/texture/astc_bc.h"
#include "nx/gpu/vk/extension_bans.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/spirv_emit.h"

int g_vk_preserve_infnan;
int g_vk_preserve_infnan64;
int g_vk_flush_denorm;
VkShaderStageFlags g_vk_subgroup_stages;

VkState g_vk;
/* reentrant on the thread that holds it: landing compute results writes guest memory, which can come back here */
static PMutex g_vklock;
static _Thread_local int t_vkdepth;
static u32 g_vk_owner; /* the thread holding it (plat_tid), 0 when none: a trap's fault on it can't be answered */
void vk_lock(void) {
    if (!t_vkdepth++) {
        if (!pm_trylock(&g_vklock)) { /* held: the GPU thread says so for the stall report */
            /* Only a contended acquisition gets a zone. */
            SNX_ZONE(z_wait, "vk lock wait");
            gpu_wait_note("vk_lock, which another thread holds", 0);
            pm_lock(&g_vklock);
            gpu_wait_note(NULL, 0);
        }
        __atomic_store_n(&g_vk_owner, plat_tid(), __ATOMIC_RELAXED);
    }
}
/* lets go of this thread's last hold, then signals the event slots its releases under it reached */
static void vk_let_go(void) {
    __atomic_store_n(&g_vk_owner, 0, __ATOMIC_RELAXED);
    pm_unlock(&g_vklock);
    vk_release_events();
}
void vk_unlock(void) {
    if (!--t_vkdepth) vk_let_go();
}
int vk_trylock(void) {
    if (!t_vkdepth && !pm_trylock(&g_vklock)) return 0;
    if (!t_vkdepth++) __atomic_store_n(&g_vk_owner, plat_tid(), __ATOMIC_RELAXED);
    return 1;
}
u32 vk_lock_owner(void) { return __atomic_load_n(&g_vk_owner, __ATOMIC_RELAXED); }
int vk_lock_held(void) { return t_vkdepth > 0; }

/* What held the memory at an out-of-memory, at the first ones: each heap as VMA counts it, and the descriptor pools,
   which MoltenVK backs with Metal memory that VMA's blocks do not hold (cmd.c). The heap's use less its blocks is
   what the driver and the other allocations of the process took. Any thread can fail, and VMA's statistics take its
   own lock */
/* set once the allocator exists, which lives to the end of the process: vk_memory_text's threads read g_vk.vma only
   after they see it, atomic */
static int g_vma_ready;
#define OOM_REPORTS 3
static void oom_report(VkResult r) {
    static u32 reports;
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    const VkPhysicalDeviceMemoryProperties* mp;
    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) return;
    if (!__atomic_load_n(&g_vma_ready, __ATOMIC_ACQUIRE) ||
        __atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) >= OOM_REPORTS)
        return;
    vmaGetHeapBudgets(g_vk.vma, b);
    vmaGetMemoryProperties(g_vk.vma, &mp);
    for (u32 h = 0; h < mp->memoryHeapCount; h++)
        LOG("vulkan: out of memory, heap %u%s: use %llu MiB, budget %llu MiB, VMA blocks %u (%llu MiB), allocations "
            "%u (%llu MiB), heap size %llu MiB\n",
            h, mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (device local)" : "",
            (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20), b[h].statistics.blockCount,
            (unsigned long long)(b[h].statistics.blockBytes >> 20), b[h].statistics.allocationCount,
            (unsigned long long)(b[h].statistics.allocationBytes >> 20),
            (unsigned long long)(mp->memoryHeaps[h].size >> 20));
    LOG("vulkan: out of memory with %u descriptor pools\n", vk_desc_pools());
}

/* Each heap and the descriptor pools in one part of a line, for a low memory warning from a thread that never calls
   Vulkan. VMA's functions are safe on any thread (its own locks), and the budget it fetches again now and then comes
   from vkGetPhysicalDeviceMemoryProperties2, which needs no external synchronization */
u64 vk_memory_blocks(void) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    const VkPhysicalDeviceMemoryProperties* mp;
    u64 sum = 0;
    if (!__atomic_load_n(&g_vma_ready, __ATOMIC_ACQUIRE)) return 0;
    vmaGetHeapBudgets(g_vk.vma, b);
    vmaGetMemoryProperties(g_vk.vma, &mp);
    for (u32 h = 0; h < mp->memoryHeapCount; h++) sum += b[h].statistics.blockBytes;
    return sum;
}
void vk_memory_text(char* out, size_t size) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    const VkPhysicalDeviceMemoryProperties* mp;
    size_t n = 0;
    if (!size) return;
    out[0] = 0;
    if (!__atomic_load_n(&g_vma_ready, __ATOMIC_ACQUIRE)) {
        snprintf(out, size, "no GPU heaps yet, %u descriptor pools", vk_desc_pools());
        return;
    }
    vmaGetHeapBudgets(g_vk.vma, b);
    vmaGetMemoryProperties(g_vk.vma, &mp);
    for (u32 h = 0; h < mp->memoryHeapCount && n < size; h++) {
        int w = snprintf(out + n, size - n, "GPU heap %u%s use %llu of %llu MiB (VMA blocks %llu MiB), ", h,
                         mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (device local)" : "",
                         (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20),
                         (unsigned long long)(b[h].statistics.blockBytes >> 20));
        if (w < 0) return;
        n += (size_t)w;
    }
    if (n < size) snprintf(out + n, size - n, "%u descriptor pools", vk_desc_pools());
}

int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    if (r >= 0) return 1;
    WARN("vulkan: %s failed: %s (%d) at %s:%d\n", what, snx_vk_result_str(r), (int)r, file, line);
    if (r == VK_ERROR_DEVICE_LOST) vk_checkpoint_dump("device lost");
    oom_report(r);
    return 0;
}

u32 g_vk_validation_messages;
/* a message names an image with binds in a batch that hasn't gone to the queue yet (memory.c): among its objects, or
   in its text alone, as the layers' message about a barrier names no image object */
static int validation_unissued(const VkDebugUtilsMessengerCallbackDataEXT* d) {
    for (u32 i = 0; i < d->objectCount; i++)
        if (d->pObjects[i].objectType == VK_OBJECT_TYPE_IMAGE &&
            vk_sparse_unissued((VkImage)(uintptr_t)d->pObjects[i].objectHandle))
            return 1;
    for (const char* at = strstr(d->pMessage, "VkImage 0x"); at; at = strstr(at + 10, "VkImage 0x"))
        if (vk_sparse_unissued((VkImage)(uintptr_t)strtoull(at + 10, NULL, 16))) return 1;
    return 0;
}
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT* d, void* user) {
    (void)type;
    (void)user;
    /* an image whose binds wait in a batch for the next submission (memory.c): the layers check its memory as each
       command is recorded, and the batch goes to the queue after that, ahead of the submission that runs the command */
    if (d->pMessage && strstr(d->pMessage, "vkQueueBindSparse") && validation_unissued(d)) {
        LOG_LIMIT(4, "vulkan: the validation layers find an image unbound as a command for it is recorded, its binds "
                     "going to the queue with the next submission (not counted)\n");
        return VK_FALSE;
    }
    /* The layers up to 1.4.363 add a stage's input and output clip distances against
       maxCombinedClipAndCullDistances, which limits each: a tessellation or geometry stage that passes eight planes on
       read 16 against 8. Their 99fd2cb576 ("layers: Fix clip cull") counts each on its own, with the CTS case
       dEQP-VK.clipping.user_defined.clip_cull_distance.vert_geom as its test. Only those stages have both, and each of
       ours declares eight at most, so the check stays where it applies */
    if (d->pMessageIdName && !strcmp(d->pMessageIdName, "VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710") &&
        d->pMessage && (strstr(d->pMessage, "TESSELLATION_") || strstr(d->pMessage, "GEOMETRY_BIT"))) {
        LOG_LIMIT(4, "vulkan: the validation layers add a tessellation or geometry stage's input and output clip "
                     "distances, which their later versions count on their own (not counted)\n");
        return VK_FALSE;
    }
    g_vk_validation_messages++;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        WARN("vulkan validation: %s\n", d->pMessage);
    else
        LOG("vulkan validation: %s\n", d->pMessage);
    return VK_FALSE;
}

/* VkPhysicalDevicePortabilitySubsetFeaturesKHR, which vulkan_beta.h holds back */
#define PORTABILITY_FEATURES_STYPE ((VkStructureType)1000163000)
typedef struct {
    VkStructureType sType;
    void* pNext;
    VkBool32 constantAlphaColorBlendFactors, events, imageViewFormatReinterpretation, imageViewFormatSwizzle,
        imageView2DOn3DImage, multisampleArrayImage, mutableComparisonSamplers, pointPolygons, samplerMipLodBias,
        separateStencilMaskRef, shaderSampleRateInterpolationFunctions, tessellationIsolines, tessellationPointMode,
        triangleFans, vertexAttributeAccessBeyondStride;
} PortabilityFeatures;
/* MoltenVK (macOS, iOS) is a portability driver: the loader lists it only for an instance that asks for those */
static int create_instance(void) {
    u32 nbe = 0;
    const char* const* be = g_opt_headless ? NULL : backend_vk_extensions(&nbe);
    /* the window's, and the surface's present timing capabilities (VK_EXT_present_timing) are asked through
       VK_KHR_get_surface_capabilities2 */
    const char* ext[17];
    u32 next = 0;
    for (u32 i = 0; i < nbe && next < 16; i++) ext[next++] = be[i];
    if (nbe > 16) WARN("vulkan: the window backend wants %u instance extensions, only 16 are taken\n", nbe);
    if (!g_opt_headless && snx_vk_has_instance_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
        ext[next++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        g_vk.has_surface_caps2 = 1;
    }
    SnxVkInstanceInfo info = {.app_name = "ScarletNX",
                              .extensions = ext,
                              .extension_count = next,
                              .api_version = g_vk.api_cap,
                              .validation = g_settings.vulkan_validation_layer != 0,
                              .gpu_assisted = g_settings.vulkan_validation_layer == 2,
                              .portability = true,
                              .debug = debug_cb};
    SnxVkInstance made;
    if (!VK_OK(snx_vk_instance_create(&info, &made))) return 0;
    if (info.validation && !made.validation)
        WARN("vulkan: validation was asked for (vk_validation), but "
             "VK_LAYER_KHRONOS_validation is not installed\n");
    g_vk.instance = made.instance;
    g_vk.debug = made.messenger;
    return 1;
}

/* the queue family that decodes H.264 with Vulkan Video, other than the graphics and compute ones; ~0u when none */
static u32 video_decode_family(void) {
    u32 n = 0, found = ~0u;
    vkGetPhysicalDeviceQueueFamilyProperties2(g_vk.phys, &n, NULL);
    VkQueueFamilyProperties2* q = calloc(n ? n : 1, sizeof *q);
    VkQueueFamilyVideoPropertiesKHR* v = calloc(n ? n : 1, sizeof *v);
    if (!q || !v) fatal("vulkan: out of memory for the queue families");
    for (u32 i = 0; i < n; i++) {
        v[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        q[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        q[i].pNext = &v[i];
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(g_vk.phys, &n, q);
    for (u32 i = 0; i < n && found == ~0u; i++)
        if ((q[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) &&
            (v[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) && i != g_vk.gfx_family &&
            i != g_vk.compute_family)
            found = i;
    free(q);
    free(v);
    return found;
}

/* What the renderer takes from Vulkan 1.3, as a 1.2 device has it: these extensions, their features (create_device)
   and their commands under the core names (vk12_commands), and SPIR-V 1.5. A read-only depth target's store op NONE
   comes from VK_KHR_load_store_op_none or the EXT it was promoted from (vk12_store_none) */
static const char* const vk12_extensions[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                              VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                                              VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
                                              VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
                                              VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
                                              VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME,
                                              VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME,
                                              VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME};
#define VK12_EXTENSIONS (sizeof vk12_extensions / sizeof *vk12_extensions)
static const char* vk12_store_none(const SnxVkExtensions* e) {
    return snx_vk_extensions_has(e, VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME) ? VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME
           : snx_vk_extensions_has(e, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME)
               ? VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME
               : NULL;
}
/* how many of those e lacks, their names in out (size bytes, may be 0), comma separated */
u32 vk_1_2_missing(const SnxVkExtensions* e, char* out, size_t size) {
    u32 n = 0;
    size_t at = 0;
    if (size) out[0] = 0;
    for (u32 i = 0; i <= VK12_EXTENSIONS; i++) {
        const char* name = i < VK12_EXTENSIONS ? vk12_extensions[i] : VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME;
        if (i < VK12_EXTENSIONS ? snx_vk_extensions_has(e, name) : vk12_store_none(e) != NULL) continue;
        if (at < size) at += (size_t)snprintf(out + at, size - at, "%s%s", n ? ", " : "", name);
        n++;
    }
    return n;
}
int vk_api_setting(const char* text, u32* cap, char* why, size_t n) {
    if (!strcmp(text, "auto") || !strcmp(text, "1.2")) {
        *cap = text[0] == 'a' ? 0 : VK_API_VERSION_1_2;
        return 1;
    }
    snprintf(why, n, "vk_api=%s is not auto or 1.2", text);
    return 0;
}

/* the version the renderer runs a device of api at: the device's, up to 1.3 (or api_cap) */
static u32 vk_api_for(u32 api) {
    u32 most = g_vk.api_cap ? g_vk.api_cap : VK_API_VERSION_1_3;
    return api < most ? api : most;
}

/* Vulkan 1.3, or 1.2 with vk12_extensions, and a graphics queue. Only windowed runs need WSI and presentation support.
   *gfx_queues: how many queues the graphics family has */
static int device_score(VkPhysicalDevice pd, const VkPhysicalDeviceProperties* p, u32* gfx, u32* compute,
                        u32* gfx_queues) {
    SnxVkExtensions e;
    u32 api = vk_api_for(p->apiVersion);
    int listed = snx_vk_device_extensions(pd, &e);
    int swapchain = listed && snx_vk_extensions_has(&e, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    int lacks = api < VK_API_VERSION_1_3 && (!listed || vk_1_2_missing(&e, NULL, 0));
    snx_vk_extensions_free(&e);
    if ((!g_opt_headless && !swapchain) || api < VK_API_VERSION_1_2 || lacks) return -1;
    u32 nq = 0;
    VkQueueFamilyProperties* q = snx_vk_queue_families(pd, &nq);
    /* ASTC uploads use compute */
    *gfx = snx_vk_queue_family(pd, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                               g_opt_headless ? VK_NULL_HANDLE : g_vk.surface, 0);
    *compute = ~0u;
    /* a compute family without graphics runs alongside the graphics queue */
    for (u32 i = 0; i < nq; i++)
        if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && *compute == ~0u)
            *compute = i;
    if (*compute == ~0u) *compute = *gfx;
    *gfx_queues = *gfx < nq ? q[*gfx].queueCount : 0;
    free(q);
    if (*gfx == ~0u) return -1;
    /* of two devices of a kind, the one with 1.3 */
    return (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 6
            : p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 4
                                                                      : 2) +
           (api >= VK_API_VERSION_1_3);
}
/* device_score as snx_vk_best_device rates a device */
static int device_rating(VkPhysicalDevice pd, const VkPhysicalDeviceProperties* p, void* user) {
    u32 gf, cf, gq;
    (void)user;
    return device_score(pd, p, &gf, &cf, &gq);
}

/* g_vk.start_failure for no device found: each device with its Vulkan version, which a driver under 1.3 holds back
   (a GPU driver package may have a newer one: gpu_driver), and what a 1.2 one lacks of vk12_extensions */
static void start_failure_devices(void) {
    VkPhysicalDevice devices[8];
    u32 n = 8;
    size_t at = 0;
    if (vkEnumeratePhysicalDevices(g_vk.instance, &n, devices) < 0) n = 0;
    for (u32 i = 0; i < n && at < sizeof g_vk.start_failure; ++i) {
        VkPhysicalDeviceProperties p;
        char lacks[512] = "";
        vkGetPhysicalDeviceProperties(devices[i], &p);
        u32 api = vk_api_for(p.apiVersion);
        if (api >= VK_API_VERSION_1_2 && api < VK_API_VERSION_1_3) {
            SnxVkExtensions e;
            char names[400];
            if (!snx_vk_device_extensions(devices[i], &e))
                snprintf(lacks, sizeof lacks, " (its extensions could not be listed)");
            else if (vk_1_2_missing(&e, names, sizeof names))
                snprintf(lacks, sizeof lacks, " without %s", names);
            snx_vk_extensions_free(&e);
        }
        WARN("vulkan: %s has Vulkan %u.%u.%u%s, driver %08x\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), lacks, p.driverVersion);
        at += (size_t)snprintf(g_vk.start_failure + at, sizeof g_vk.start_failure - at, "%s%s has Vulkan %u.%u.%u%s",
                               i ? "; " : "", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), lacks);
    }
#ifdef __ANDROID__
    const char* hint = n ? ". A GPU driver package may have it (Settings > App > GPU Drivers)" : "";
#else
    const char* hint = "";
#endif
    if (at < sizeof g_vk.start_failure)
        snprintf(g_vk.start_failure + at, sizeof g_vk.start_failure - at,
                 "%sScarletNX needs Vulkan 1.3, or 1.2 with the extensions 1.3 took in, and a %s queue%s",
                 n ? ", and " : "No Vulkan device: ", g_opt_headless ? "graphics" : "graphics and presentation", hint);
}

/* whether the device multiplies 16x16x16 f16 A and B into an f32 accumulator in subgroup scope in compute, as
   g_shader_hmma_coop asks (VK_KHR_cooperative_matrix: RADV from GFX11) */
static int coop_f16_f32(VkPhysicalDevice phys) {
    VkPhysicalDeviceCooperativeMatrixPropertiesKHR cp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cp};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    if (!(cp.cooperativeMatrixSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
        !vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
        return 0;
    u32 n = 0;
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(phys, &n, NULL);
    VkCooperativeMatrixPropertiesKHR props[64];
    if (n > 64) n = 64;
    for (u32 i = 0; i < n; i++)
        props[i] = (VkCooperativeMatrixPropertiesKHR){.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR};
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(phys, &n, props);
    for (u32 i = 0; i < n; i++)
        if (props[i].MSize == 16 && props[i].NSize == 16 && props[i].KSize == 16 &&
            props[i].AType == VK_COMPONENT_TYPE_FLOAT16_KHR && props[i].BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
            props[i].CType == VK_COMPONENT_TYPE_FLOAT32_KHR && props[i].ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR &&
            !props[i].saturatingAccumulation && props[i].scope == VK_SCOPE_SUBGROUP_KHR)
            return 1;
    return 0;
}

/* An Ounce's mesh draws (render.c): VK_EXT_mesh_shader's mesh stage, and its task stage when the device has one. The
   programs' lanes are a warp's (sm86.h), so both stages need a required subgroup size of 32 in full subgroups
   (subgroupSizeControl, computeFullSubgroups, requiredSubgroupSizeStages). Without them, or with the experimental
   mesh_draws setting off, the draws are skipped, and the log says why once */
static void mesh_choose(int has_ext, const VkPhysicalDeviceMeshShaderFeaturesEXT* f,
                        const VkPhysicalDeviceVulkan13Features* f13) {
    VkPhysicalDeviceMeshShaderPropertiesEXT mp = {.sType =
                                                      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
                                              .pNext = has_ext ? &mp : NULL};
    VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
    if (g_vk.api >= VK_API_VERSION_1_3) vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
    const VkShaderStageFlags both = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT;
    const char* why = !g_settings.mesh_draws                                    ? "mesh_draws off (experimental)"
                      : !has_ext                                                ? "no VK_EXT_mesh_shader"
                      : !f->meshShader                                          ? "no meshShader"
                      : !f13->subgroupSizeControl || !f13->computeFullSubgroups ? "no subgroup size control"
                      : p13.minSubgroupSize > 32 || p13.maxSubgroupSize < 32    ? "no subgroups of 32"
                      : (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_MESH_BIT_EXT) == 0
                          ? "no required subgroup size in the mesh stage"
                          : NULL;
    g_vk.has_mesh = why == NULL;
    g_vk.has_task = g_vk.has_mesh && f->taskShader && (p13.requiredSubgroupSizeStages & both) == both;
    g_vk.mesh_stages = g_vk.has_mesh ? VK_SHADER_STAGE_MESH_BIT_EXT | (g_vk.has_task ? both : 0) : 0;
    u32 groups = g_vk.has_task ? mp.maxTaskWorkGroupCount[0] : mp.maxMeshWorkGroupCount[0];
    g_vk.mesh_groups_max = g_vk.has_mesh ? groups : 0;
    g_shader_mesh_tasks_max = g_vk.has_task ? (mp.maxMeshWorkGroupCount[0] < mp.maxMeshWorkGroupTotalCount
                                                   ? mp.maxMeshWorkGroupCount[0]
                                                   : mp.maxMeshWorkGroupTotalCount)
                                            : 0;
    g_vk.mesh_output_bytes_max = mp.maxMeshOutputMemorySize;
    g_vk.mesh_vertex_granule = mp.meshOutputPerVertexGranularity;
    g_vk.mesh_primitive_granule = mp.meshOutputPerPrimitiveGranularity;
    if (why)
        LOG("vulkan: mesh draws are skipped: %s%s\n", g_settings.mesh_draws ? "this GPU has " : "", why);
    else
        LOG("vulkan: mesh draws use VK_EXT_mesh_shader%s\n",
            g_vk.has_task ? " with task shaders" : ", without task shaders");
}
/* How the ASTC textures the GPU decodes are held, where it samples BCn but not ASTC (astc_mode, gpu/texture/astc_bc.h):
   Auto takes BC7 or Exact by the budget of the heap images go to as the device starts, which the line says with its
   numbers. vk_mem_use's budget is the one the caches keep within: the driver's, or vk_budget_mb where that is lower */
static void astc_choose(void) {
    static const char exact[] = "decoded to RGBA8, exact, in four times BC7's memory",
                      bc7[] = "decoded to RGBA8 at once and encoded again as BC7 between frames, a quarter of RGBA8's "
                              "memory";
    u64 usage = 0, budget = 0;
    if (g_vk.feat.textureCompressionASTC_LDR) return; /* sampled as the game's blocks */
    if (g_settings.astc_mode == 1) {
        LOG("vulkan: astc_mode=1 (Exact): ASTC textures are %s\n", exact);
        return;
    }
    if (!g_vk.feat.textureCompressionBC) return; /* RGBA8, which the warning above says */
    vk_mem_use(&usage, &budget);
    g_vk.astc_auto_bc7 = g_shader_sm86 || snx_astc_auto_bc7(budget);
    g_vk.astc_at_load = g_settings.astc_mode == 3 || (g_settings.astc_mode == 0 && g_shader_sm86);
    if (g_settings.astc_mode == 0 && g_shader_sm86)
        LOG("vulkan: astc_mode=0 (Auto): an Ounce program: ASTC textures are encoded as BC7 as they load, a quarter of "
            "RGBA8's memory (BC7 at load)\n");
    else if (g_settings.astc_mode == 0)
        LOG("vulkan: astc_mode=0 (Auto): heap %d's budget is %llu MB as the device starts, %s %llu MB: ASTC textures "
            "are %s (%s)\n",
            vk_mem_heap(), (unsigned long long)(budget >> 20), g_vk.astc_auto_bc7 ? "under" : "at least",
            SNX_ASTC_EXACT_MB, g_vk.astc_auto_bc7 ? bc7 : exact, g_vk.astc_auto_bc7 ? "BC7" : "Exact");
    else if (g_settings.astc_mode == 3)
        LOG("vulkan: astc_mode=3 (BC7 at load): ASTC textures are encoded as BC7 as they load, a quarter of RGBA8's "
            "memory\n");
    else
        LOG("vulkan: astc_mode=2 (BC7): ASTC textures are %s\n", bc7);
}

/* a line of the extension bans (extension_bans.h) for the log */
static void ban_say(const char* line, int warn) {
    if (warn)
        WARN("%s\n", line);
    else
        LOG("%s\n", line);
}

/* the stages whose programs the translator gives subgroup operations, and what VOTE and SHFL do in the others */
static void log_subgroups(u32 size, VkSubgroupFeatureFlags operations, VkShaderStageFlags stages) {
    static const struct {
        VkShaderStageFlags bit;
        const char* name;
    } names[] = {{VK_SHADER_STAGE_VERTEX_BIT, "vertex"},
                 {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "control"},
                 {VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "evaluation"},
                 {VK_SHADER_STAGE_GEOMETRY_BIT, "geometry"},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, "fragment"},
                 {VK_SHADER_STAGE_COMPUTE_BIT, "compute"}};
    char with[80] = "", without[80] = "";
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char* list = (g_vk_subgroup_stages & names[i].bit) ? with : without;
        size_t n = strlen(list);
        snprintf(list + n, 80 - n, "%s%s", n ? ", " : "", names[i].name);
    }
    LOG("vulkan: subgroups of %u lanes, operations %#x, stages %#x: VOTE and SHFL use them in %s programs%s\n", size,
        (unsigned)operations, (unsigned)stages, with[0] ? with : "no",
        g_vk_subgroup_stages || !stages ? "" : ", as they need 32 lanes or more, ballot, shuffle and arithmetic");
    if (without[0])
        LOG("vulkan: %s programs have no subgroup: a vertex, evaluation or geometry one among them that votes or "
            "shuffles runs each invocation as lane 0 alone, and a control, fragment or compute one is not translated\n",
            without);
}

/* the commands of Vulkan 1.3 the renderer calls, as a 1.2 device has them: by the names of its extensions, the same
   calls (the core names stay unloaded there). 0, with *lacks the first one the driver has not */
static int vk12_commands(const char** lacks) {
#define VK12_COMMAND(name, by)                                                                                         \
    name = name##by;                                                                                                   \
    if (!name && !*lacks) *lacks = #name #by
    *lacks = NULL;
    VK12_COMMAND(vkCmdBeginRendering, KHR);
    VK12_COMMAND(vkCmdEndRendering, KHR);
    VK12_COMMAND(vkCmdPipelineBarrier2, KHR);
    VK12_COMMAND(vkCmdWriteTimestamp2, KHR);
    VK12_COMMAND(vkQueueSubmit2, KHR);
    VK12_COMMAND(vkCmdBindVertexBuffers2, EXT);
    VK12_COMMAND(vkCmdSetCullMode, EXT);
    VK12_COMMAND(vkCmdSetFrontFace, EXT);
    VK12_COMMAND(vkCmdSetPrimitiveTopology, EXT);
    VK12_COMMAND(vkCmdSetViewportWithCount, EXT);
    VK12_COMMAND(vkCmdSetScissorWithCount, EXT);
    VK12_COMMAND(vkCmdSetDepthTestEnable, EXT);
    VK12_COMMAND(vkCmdSetDepthWriteEnable, EXT);
    VK12_COMMAND(vkCmdSetDepthCompareOp, EXT);
    VK12_COMMAND(vkCmdSetDepthBoundsTestEnable, EXT);
    VK12_COMMAND(vkCmdSetStencilTestEnable, EXT);
    VK12_COMMAND(vkCmdSetStencilOp, EXT);
    VK12_COMMAND(vkCmdSetDepthBiasEnable, EXT);
    VK12_COMMAND(vkCmdSetPrimitiveRestartEnable, EXT);
    VK12_COMMAND(vkCmdSetRasterizerDiscardEnable, EXT);
    VK12_COMMAND(vkGetDeviceImageMemoryRequirements, KHR);
    VK12_COMMAND(vkGetDeviceImageSparseMemoryRequirements, KHR);
#undef VK12_COMMAND
    return !*lacks;
}

static int create_device(void) {
    g_vk.phys = snx_vk_best_device(g_vk.instance, VK_API_VERSION_1_2, device_rating, NULL);
    if (!g_vk.phys) {
        WARN("vulkan: no Vulkan 1.3 device, or 1.2 with the extensions 1.3 took in, with a suitable %s queue\n",
             g_opt_headless ? "graphics" : "presentation");
        start_failure_devices();
        return 0;
    }
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    g_vk.api = vk_api_for(g_vk.props.apiVersion);
    const int core13 = g_vk.api >= VK_API_VERSION_1_3;
    /* SPIR-V 1.6 is Vulkan 1.3's: a 1.2 device takes 1.5, which declares demotion as its extension (the translator
       does either way) */
    spv_emit_set_version(core13 ? 0x00010600u : 0x00010500u);
    u32 gfx_queues = 0;
    device_score(g_vk.phys, &g_vk.props, &g_vk.gfx_family, &g_vk.compute_family, &gfx_queues);
    VkPhysicalDeviceSubgroupProperties subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                              .pNext = &subgroup};
    VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
    {
        /* the drivers known to break an extension or feature, read before any is turned on: a table the build
           carries that does not read is the build's fault, and which bans hold would be unknown */
        char err[1024];
        if (!vk_bans_load(snx_vk_extension_bans_text, err, sizeof err)) fatal("vulkan: %s", err);
        /* the bans a run goes over to test a driver again; a key the table has not stops the game, as the player
           meant a ban that would otherwise still hold */
        if (!vk_bans_ignore(g_settings.vulkan_ban_ignore, err, sizeof err)) fatal("settings: %s", err);
        VkBanDevice bd = {g_vk.props.deviceName,    p12.driverName,      p12.driverInfo, (uint32_t)p12.driverID,
                          g_vk.props.driverVersion, g_vk.props.deviceID, vk_ban_system()};
        vk_bans_device(&bd);
        vk_bans_ignored(ban_say);
    }
    g_vk_subgroup_stages =
        subgroup.subgroupSize >= 32 &&
                (subgroup.supportedOperations & (VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                                 VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)) ==
                    (VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                     VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)
            ? subgroup.supportedStages
            : 0;
    g_shader_quad = (g_vk_subgroup_stages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                    (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
    /* compute has quad operations wherever the bit is and the stage has subgroup operations at all (the quad limit,
       quadOperationsInAllStages, is about the other stages) */
    g_shader_quad_compute = (g_vk_subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                            (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
    log_subgroups(subgroup.subgroupSize, subgroup.supportedOperations, subgroup.supportedStages);
    g_vk_preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32 != 0;
    /* a binding's stride as dynamic state, core in Vulkan 1.3, unless a driver is banned from it (MoltenVK's fails every
       pipeline that lists it): pipeline.c puts the stride in the pipeline then */
    g_vk.has_dynamic_stride = !vk_ban("VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE", 1, ban_say);
    g_vk_preserve_infnan64 = p12.shaderSignedZeroInfNanPreserveFloat64 != 0;
    g_vk.has_minmax_swizzle = p12.filterMinmaxImageComponentMapping != 0;
    /* 32 bit alone: a device whose denorm modes can't differ per width would need 16 and 64 bit declared too. A driver
       banned from the mode (Qualcomm's miscompiles with it) has its FTZ done per instruction, as on a device without
       it */
    int flush_denorm = p12.shaderDenormFlushToZeroFloat32 &&
                       p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    g_vk_flush_denorm = flush_denorm && !vk_ban("shaderDenormFlushToZeroFloat32", flush_denorm, ban_say);
    /* without it, vk_pipeline_init measures whether the device flushes by itself (vk_ftz_native) */
    if (!g_vk_preserve_infnan) WARN("vulkan: FP32 signed-zero/Inf/NaN preservation unavailable on this GPU\n");

    /* compute gets a queue of its own: of a compute-only family, or else a second one of the graphics family */
    int second = g_vk.compute_family == g_vk.gfx_family && gfx_queues >= 2;
    float prio[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci[3] = {{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = g_vk.gfx_family,
                                       .queueCount = second ? 2 : 1,
                                       .pQueuePriorities = prio},
                                      {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = g_vk.compute_family,
                                       .queueCount = 1,
                                       .pQueuePriorities = prio}};
    /* the features drawing uses, where the device has them */
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &have);
    VkPhysicalDeviceFeatures* want = &g_vk.feat;
#define WANT(f) want->f = have.f
    /* constant buffers are bound with the bytes the shader reads, not the 64K it declares: reads past them stay in
       bounds (their values are undefined) */
    WANT(robustBufferAccess);
    WANT(imageCubeArray);
    WANT(independentBlend);
    WANT(dualSrcBlend);
    WANT(logicOp);
    WANT(depthClamp);
    WANT(depthBiasClamp);
    WANT(fillModeNonSolid);
    WANT(depthBounds);
    WANT(wideLines);
    WANT(largePoints);
    WANT(multiViewport);
    WANT(samplerAnisotropy);
    WANT(occlusionQueryPrecise); /* samples-passed counts (query.c) are exact */
    WANT(sampleRateShading);
    WANT(alphaToOne); /* multisampled draws asking for it (pipeline.c) */
    /* BCn, ASTC and ETC2 sampled as they are (format.c) where the settings ask for it and the device has them, else
       decoded on the GPU as they are uploaded; ASTC decoded so is encoded again as BC7 where the GPU samples BCn,
       unless astc_mode asks for exact RGBA8 (astc_choose says which, once the budget is known). With exact RGBA8 only
       a texture with holes in its mapping is BC7 (texture.c). Thus BCn always where the device has it, which
       native_bc decides the sampling of */
    WANT(textureCompressionBC);
    g_vk.astc_bc7_could = have.textureCompressionBC && !have.textureCompressionASTC_LDR;
    if (g_settings.native_astc) WANT(textureCompressionASTC_LDR);
    if (g_settings.native_etc2) WANT(textureCompressionETC2);
    if (g_settings.native_bc && !want->textureCompressionBC)
        WARN("vulkan: the GPU can't sample BCn textures: they are decoded on the GPU as they are loaded\n");
    if (g_settings.native_astc && !want->textureCompressionASTC_LDR)
        WARN("vulkan: native ASTC textures were asked for, but the GPU can't sample ASTC: they are decoded on the GPU "
             "as they are loaded (astc_mode=%d)\n",
             g_settings.astc_mode);
    if (!want->textureCompressionASTC_LDR && !want->textureCompressionBC)
        WARN("vulkan: the GPU samples neither ASTC nor BCn: ASTC textures are decoded to RGBA8, four times BC7's "
             "memory\n");
    if (g_settings.native_etc2 && !want->textureCompressionETC2)
        WARN("vulkan: native ETC2 textures were asked for, but the GPU can't sample ETC2: they are decoded\n");
    WANT(vertexPipelineStoresAndAtomics);
    /* indirect draws (render.c's draw_indirect): several in one call, and a base instance from the GPU's arguments */
    WANT(multiDrawIndirect);
    WANT(drawIndirectFirstInstance);
    WANT(fragmentStoresAndAtomics);
    WANT(shaderImageGatherExtended);
    WANT(shaderStorageImageExtendedFormats);
    WANT(shaderStorageImageWriteWithoutFormat);
    WANT(shaderStorageImageReadWithoutFormat);
    WANT(shaderStorageImageMultisample);
    WANT(shaderResourceMinLod);
    g_shader_resource_min_lod = want->shaderResourceMinLod != 0;
    WANT(shaderClipDistance);
    WANT(shaderCullDistance);
    WANT(shaderFloat64);
    WANT(shaderInt64);
    WANT(shaderInt16);
    WANT(geometryShader);
    WANT(tessellationShader);
    WANT(shaderTessellationAndGeometryPointSize);
    /* images bigger than one allocation may be are bound in pieces, on the graphics queue, and textures that guest
       memory maps only in part get sparse residency there (memory.c) */
    int sparse_queue = 0;
    {
        u32 nq = 0;
        VkQueueFamilyProperties* q = snx_vk_queue_families(g_vk.phys, &nq);
        sparse_queue = g_vk.gfx_family < nq && (q[g_vk.gfx_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
        g_vk.gfx_timestamp_bits = g_vk.gfx_family < nq ? q[g_vk.gfx_family].timestampValidBits : 0;
        free(q);
    }
    if (sparse_queue) {
        WANT(sparseBinding);
        /* textures with holes in their mapping given memory only where it is mapped (memory.c): 3D volumes in bricks,
           and other textures in the device's sparse blocks */
        WANT(sparseResidencyImage2D);
        WANT(sparseResidencyImage3D);
        /* the mirror of guest memory that the programs logging their writes read and write in place (mirror.c) */
        WANT(sparseResidencyBuffer);
    }
#undef WANT
    /* points out of tessellation and geometry programs take the size those write, as the vertex program's do */
    g_shader_stage_point_size = want->shaderTessellationAndGeometryPointSize != 0;
    g_shader_geometry = want->geometryShader != 0;
    g_shader_tessellation = want->tessellationShader != 0;
    const char** ext = g_vk.dev_ext;
    int has_pipeline_robustness = 0, has_robustness2 = 0;
    u32 next = 0;
    int has_shader_clock = 0, has_feedback_dynamic = 0, has_cond = 0, has_provoking = 0, video_exts = 0;
    int has_list_restart = 0;
    int has_portability = 0, has_reconvergence = 0;
    int has_calibrated = 0, has_present_id = 0, has_present_wait = 0, has_present_v2 = 0, has_calibrated_khr = 0;
    int has_google_timing = 0;
    int has_host_import = 0, has_ahb = 0, has_placed = 0, has_map2 = 0, has_dmabuf = 0;
    int has_descriptor_buffer = 0, has_descriptor_heap = 0, has_extended_flags = 0, has_maintenance5 = 0;
    int has_maintenance8 = 0;
    int has_vertex_input = 0;
    int has_gpl = 0, has_eds2 = 0, has_eds3 = 0; /* the extensions of pipeline libraries and their dynamic state */
    int has_depth_clip_control = 0;              /* and of the depth range as one of those states */
    int has_mesh = 0;
    int has_mixed_dot = 0; /* VK_VALVE_shader_mixed_float_dot_product: HMMA's pairs of f16 products in one instruction */
    int has_coop = 0;      /* VK_KHR_cooperative_matrix: HMMA on the matrix unit (g_shader_hmma_coop) */
    int has_fault = 0;     /* VK_EXT_device_fault: what the GPU reports of a fault, at a lost device */
    const char* index8_ext = NULL; /* VK_KHR_index_type_uint8, or the EXT it was promoted from */
    const char* store_none_ext = NULL; /* a 1.2 device's store op NONE (vk12_store_none) */
    if (!g_opt_headless) ext[next++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    {
        SnxVkExtensions e;
        snx_vk_device_extensions(g_vk.phys, &e);
        /* the extensions a driver is banned from leave the list before anything below reads it */
        vk_bans_filter(&e, ban_say);
        store_none_ext = vk12_store_none(&e);
        /* again on this list, which the bans took out of (each said above): a 1.2 device banned from one of them is
           refused by its name, as one without it */
        char lacks12[400];
        if (!core13 && vk_1_2_missing(&e, lacks12, sizeof lacks12)) {
            WARN("vulkan: %s has Vulkan 1.2 without %s\n", g_vk.props.deviceName, lacks12);
            snprintf(g_vk.start_failure, sizeof g_vk.start_failure,
                     "%s has Vulkan 1.2 without %s. ScarletNX needs Vulkan 1.3, or 1.2 with the extensions 1.3 took in",
                     g_vk.props.deviceName, lacks12);
            snx_vk_extensions_free(&e);
            return 0;
        }
        { /* every device here has maintenance4 (core in 1.3, required of 1.2 above) */
            VkPhysicalDeviceMaintenance4Properties m4 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
            VkPhysicalDeviceProperties2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m4};
            vkGetPhysicalDeviceProperties2(g_vk.phys, &q);
            g_vk.max_buffer_size = m4.maxBufferSize;
        }
        g_vk.has_divisor = snx_vk_extensions_has(&e, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        has_vertex_input = snx_vk_extensions_has(&e, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
        has_gpl = snx_vk_extensions_has(&e, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                  snx_vk_extensions_has(&e, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        has_eds2 = snx_vk_extensions_has(&e, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
        has_eds3 = snx_vk_extensions_has(&e, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        has_depth_clip_control = snx_vk_extensions_has(&e, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
        g_vk.has_xfb = snx_vk_extensions_has(&e, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
        g_vk.has_mixed_samples = snx_vk_extensions_has(&e, VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);
        g_vk.has_nv_checkpoints = snx_vk_extensions_has(&e, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        has_fault = snx_vk_extensions_has(&e, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        g_vk.has_coverage_to_color = snx_vk_extensions_has(&e, VK_NV_FRAGMENT_COVERAGE_TO_COLOR_EXTENSION_NAME);
        g_vk.has_post_depth_coverage = snx_vk_extensions_has(&e, VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME);
        g_vk.sample_locations.available = snx_vk_extensions_has(&e, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME);
        g_vk.advanced_blend_caps.available = snx_vk_extensions_has(&e, VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME);
        g_vk.has_fragment_interlock = snx_vk_extensions_has(&e, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
        g_vk.has_border_color = snx_vk_extensions_has(&e, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        has_shader_clock = snx_vk_extensions_has(&e, VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
        has_descriptor_buffer = snx_vk_extensions_has(&e, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        has_descriptor_heap = snx_vk_extensions_has(&e, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        has_extended_flags = snx_vk_extensions_has(&e, VK_KHR_EXTENDED_FLAGS_EXTENSION_NAME);
        has_maintenance5 = snx_vk_extensions_has(&e, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        has_maintenance8 = snx_vk_extensions_has(&e, VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
        has_reconvergence = snx_vk_extensions_has(&e, VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME);
        g_vk.has_feedback = snx_vk_extensions_has(&e, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
        has_feedback_dynamic = snx_vk_extensions_has(&e, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
        g_vk.has_local_read = snx_vk_extensions_has(&e, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
        has_cond = snx_vk_extensions_has(&e, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
        has_provoking = snx_vk_extensions_has(&e, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
        has_mesh = snx_vk_extensions_has(&e, VK_EXT_MESH_SHADER_EXTENSION_NAME);
        has_mixed_dot = snx_vk_extensions_has(&e, VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME);
        has_coop = snx_vk_extensions_has(&e, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        has_list_restart = snx_vk_extensions_has(&e, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
        g_vk.has_conservative = snx_vk_extensions_has(&e, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
        g_vk.has_depth_range_unrestricted = snx_vk_extensions_has(&e, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
        has_pipeline_robustness = snx_vk_extensions_has(&e, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
        has_robustness2 = snx_vk_extensions_has(&e, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        /* the driver's budget for each heap, the other processes' use taken out, for VMA: without it VMA took
           80% of a heap's size as its budget, blind to what the desktop and other programs hold */
        g_vk.has_budget = snx_vk_extensions_has(&e, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        /* the GPU's clock next to the client's, for the profiler's GPU track (gpuprof.c's calibration) */
        has_calibrated = snx_vk_extensions_has(&e, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        /* a portability driver's subset must be enabled when it has one (MoltenVK) */
        has_portability = snx_vk_extensions_has(&e, "VK_KHR_portability_subset");
        if (has_portability) ext[next++] = "VK_KHR_portability_subset";
        video_exts = snx_vk_extensions_has(&e, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) +
                     snx_vk_extensions_has(&e, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) +
                     snx_vk_extensions_has(&e, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
        if (snx_vk_extensions_has(&e, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME))
            index8_ext = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME;
        else if (snx_vk_extensions_has(&e, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME))
            index8_ext = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME;
        has_present_id = !g_opt_headless && snx_vk_extensions_has(&e, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        has_present_wait = !g_opt_headless && snx_vk_extensions_has(&e, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        if (!g_opt_headless && g_vk.has_surface_caps2)
            has_present_v2 = snx_vk_extensions_has(&e, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) +
                             snx_vk_extensions_has(&e, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME) +
                             snx_vk_extensions_has(&e, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
        has_calibrated_khr = snx_vk_extensions_has(&e, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        has_google_timing = !g_opt_headless && snx_vk_extensions_has(&e, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        has_host_import = snx_vk_extensions_has(&e, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        has_placed = snx_vk_extensions_has(&e, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
        has_map2 = snx_vk_extensions_has(&e, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
        has_dmabuf = snx_vk_extensions_has(&e, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                     snx_vk_extensions_has(&e, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
#if defined(__ANDROID__) /* every driver there takes AHardwareBuffers, which guest memory can move into (uma.c) */
        has_ahb = snx_vk_extensions_has(&e, "VK_ANDROID_external_memory_android_hardware_buffer");
#endif
        snx_vk_extensions_free(&e);
    }
    /* guest memory the GPU reads in place (uma.c) */
    int uma_with = vk_uma_choose(has_host_import, has_ahb, has_placed && has_map2);
    if (uma_with == VK_UMA_WITH_HOST) ext[next++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (uma_with == VK_UMA_WITH_AHB) ext[next++] = "VK_ANDROID_external_memory_android_hardware_buffer";
    if (g_vk.has_budget) ext[next++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    if (g_vk.has_depth_range_unrestricted) ext[next++] = VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME;
    if (g_vk.has_mixed_samples) ext[next++] = VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME;
    /* the hanging draw's or dispatch's marker (record.c's vk_checkpoint_dump), and checkpoints.c's */
    if (g_vk.has_nv_checkpoints) ext[next++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    if (g_vk.has_coverage_to_color) ext[next++] = VK_NV_FRAGMENT_COVERAGE_TO_COLOR_EXTENSION_NAME;
    if (g_vk.has_post_depth_coverage) ext[next++] = VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME;
    if (g_vk.sample_locations.available) {
        ext[next++] = VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME;
        VkPhysicalDeviceSampleLocationsPropertiesEXT locations = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                  .pNext = &locations};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &properties);
        g_vk.sample_locations.counts = locations.sampleLocationSampleCounts;
        g_vk.sample_locations.subpixel_bits = locations.sampleLocationSubPixelBits;
        g_vk.sample_locations.variable = locations.variableSampleLocations;
        g_vk.sample_locations.coordinate_min = locations.sampleLocationCoordinateRange[0];
        g_vk.sample_locations.coordinate_max = locations.sampleLocationCoordinateRange[1];
        for (u32 i = 0; i < 5; i++) {
            VkMultisamplePropertiesEXT multisample = {.sType = VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT};
            vkGetPhysicalDeviceMultisamplePropertiesEXT(g_vk.phys, (VkSampleCountFlagBits)(1u << i), &multisample);
            g_vk.sample_locations.grid[i] = multisample.maxSampleLocationGridSize;
        }
    }
    /* the extension only with a host time domain among the calibrateable ones: CLOCK_MONOTONIC_RAW is the clock
       the Tracy client keeps its own time in, so the two domains' pair maps the GPU's timestamps onto it */
    if (has_calibrated) {
        u32 nd = 0;
        if (VK_OK(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(g_vk.phys, &nd, NULL)) && nd) {
            VkTimeDomainEXT* dom = malloc(sizeof *dom * nd);
            if (dom) {
                if (VK_OK(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(g_vk.phys, &nd, dom)))
                    for (u32 i = 0; i < nd; i++) {
                        if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT) g_vk.has_calibrated_host = 1;
                        if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) g_vk.has_calibrated_mono = 1;
                    }
                free(dom);
            }
        }
        if (g_vk.has_calibrated_host) ext[next++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }
    /* FFmpeg's Vulkan decoder loads the 1.3 commands by their core names */
    g_vk.video_family = g_settings.vulkan_video_decode && video_exts == 3 && core13 ? video_decode_family() : ~0u;
    if (g_vk.video_family != ~0u) {
        ext[next++] = VK_KHR_VIDEO_QUEUE_EXTENSION_NAME;
        ext[next++] = VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME;
        ext[next++] = VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME;
        LOG("vulkan: H.264 movies decode with Vulkan Video, on queue family %u (experimental)\n", g_vk.video_family);
    } else if (g_settings.vulkan_video_decode && !core13)
        WARN("vulkan: Vulkan Video decode was asked for, but FFmpeg's Vulkan decoder needs Vulkan 1.3 and this device "
             "runs 1.2: FFmpeg decodes movies on the CPU\n");
    else if (g_settings.vulkan_video_decode)
        WARN("vulkan: Vulkan Video decode was asked for, but the device has no H.264 decode queue: FFmpeg decodes "
             "movies on the CPU\n");
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fdiv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fxfb = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT qadvanced = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT qinterlock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceShaderClockFeaturesKHR qclock = {.sType =
                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR qreconv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR};
    VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE qmixdot = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR qcoop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDevicePipelineRobustnessFeaturesEXT qpipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT qrobust = {.sType =
                                                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features q12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceMeshShaderFeaturesEXT qmesh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceVulkan11Features q11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan13Features q13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    /* a 1.2 device's: those of vk12_extensions, which the 1.3 struct holds on a 1.3 one (and EDS's are core there) */
    VkPhysicalDeviceSynchronization2Features qsync2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures qrendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceMaintenance4Features qmaintenance4 = {.sType =
                                                              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures qdemote = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT qeds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT qeds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceFaultFeaturesEXT qfault = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT qdesc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT qheap = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceExtendedFlagsFeaturesKHR qflags = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_FLAGS_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR qmaintenance5 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceMaintenance8FeaturesKHR qmaintenance8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT qloop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT qloop_dyn = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR qlocal = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT qcond = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT qprov = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT qlist_restart = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR qindex8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT qvertex_input = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT qgpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    /* qeds2 above: a 1.2 device's, and the pipeline libraries' patch control points (has_eds2) */
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT qeds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT qclip_control = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    /* with the subset enabled, each of its features is off until the device asks for it: a view's swizzle, for one */
    PortabilityFeatures qport = {.sType = PORTABILITY_FEATURES_STYPE};
    VkPhysicalDevicePresentIdFeaturesKHR qpid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR qpwait = {.sType =
                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentId2FeaturesKHR qpid2 = {.sType =
                                                       VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    VkPhysicalDevicePresentWait2FeaturesKHR qpwait2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR};
    VkPhysicalDevicePresentTimingFeaturesEXT qtiming = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    int present_wait = has_present_id && has_present_wait;
    has_present_v2 = has_present_v2 == 3 && has_calibrated_khr;
    int robustness = has_pipeline_robustness && has_robustness2;
    void* const query[] = {&f2,
                           has_descriptor_buffer ? &qdesc : NULL,
                           has_descriptor_heap ? &qheap : NULL,
                           has_extended_flags ? &qflags : NULL,
                           has_maintenance5 ? &qmaintenance5 : NULL,
                           has_maintenance8 ? &qmaintenance8 : NULL,
                           &q11,
                           &q12,
                           core13 ? &q13 : NULL,
                           has_mesh ? &qmesh : NULL,
                           core13 ? NULL : &qsync2,
                           core13 ? NULL : &qrendering,
                           core13 ? NULL : &qmaintenance4,
                           core13 ? NULL : &qdemote,
                           core13 ? NULL : &qeds,
                           /* a 1.2 device's, and the patch control points of pipeline libraries' dynamic state */
                           !core13 || has_eds2 ? &qeds2 : NULL,
                           g_vk.has_divisor ? &fdiv : NULL,
                           g_vk.has_border_color ? &fbc : NULL,
                           g_vk.has_xfb ? &fxfb : NULL,
                           g_vk.advanced_blend_caps.available ? &qadvanced : NULL,
                           g_vk.has_fragment_interlock ? &qinterlock : NULL,
                           g_vk.has_feedback ? &qloop : NULL,
                           has_feedback_dynamic ? &qloop_dyn : NULL,
                           g_vk.has_local_read ? &qlocal : NULL,
                           has_cond ? &qcond : NULL,
                           has_provoking ? &qprov : NULL,
                           has_list_restart ? &qlist_restart : NULL,
                           index8_ext ? &qindex8 : NULL,
                           has_vertex_input ? &qvertex_input : NULL,
                           has_gpl ? &qgpl : NULL,
                           has_eds3 ? &qeds3 : NULL,
                           has_depth_clip_control ? &qclip_control : NULL,
                           has_portability ? &qport : NULL,
                           has_reconvergence ? &qreconv : NULL,
                           robustness ? &qpipeline : NULL,
                           robustness ? &qrobust : NULL,
                           present_wait ? &qpid : NULL,
                           present_wait ? &qpwait : NULL,
                           has_present_v2 ? &qpid2 : NULL,
                           has_present_v2 ? &qpwait2 : NULL,
                           has_present_v2 ? &qtiming : NULL,
                           has_shader_clock ? &qclock : NULL,
                           has_mixed_dot ? &qmixdot : NULL,
                           has_coop ? &qcoop : NULL,
                           has_fault ? &qfault : NULL};
    snx_vk_chain(query, sizeof query / sizeof *query);
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    /* without it the shaders do their FP16 work in 32 bits */
    if (vk_ban("shaderFloat16", q12.shaderFloat16, ban_say)) q12.shaderFloat16 = VK_FALSE;
    if (!core13) {
        q13.synchronization2 = qsync2.synchronization2;
        q13.dynamicRendering = qrendering.dynamicRendering;
        q13.maintenance4 = qmaintenance4.maintenance4;
        q13.shaderDemoteToHelperInvocation = qdemote.shaderDemoteToHelperInvocation;
        static const char* const names[] = {"synchronization2",     "dynamicRendering",
                                            "maintenance4",         "shaderDemoteToHelperInvocation",
                                            "extendedDynamicState", "extendedDynamicState2"};
        const VkBool32 have12[] = {q13.synchronization2,      q13.dynamicRendering,
                                   q13.maintenance4,          q13.shaderDemoteToHelperInvocation,
                                   qeds.extendedDynamicState, qeds2.extendedDynamicState2};
        char lacks[256] = "";
        size_t at = 0;
        for (u32 i = 0; i < sizeof names / sizeof *names; i++)
            if (!have12[i] && at < sizeof lacks)
                at += (size_t)snprintf(lacks + at, sizeof lacks - at, "%s%s", at ? ", " : "", names[i]);
        if (at) {
            WARN("vulkan: %s has Vulkan 1.2 and the extensions 1.3 took in, but not their features %s\n",
                 g_vk.props.deviceName, lacks);
            snprintf(
                g_vk.start_failure, sizeof g_vk.start_failure,
                "%s has Vulkan 1.2 without the features %s. ScarletNX needs Vulkan 1.3, or 1.2 with the extensions "
                "1.3 took in and their features",
                g_vk.props.deviceName, lacks);
            return 0;
        }
    }
    g_vk.has_present_wait = has_present_id && has_present_wait && qpid.presentId && qpwait.presentWait;
    g_vk.has_present_timing = g_vk.has_present_wait && has_present_v2 && qpid2.presentId2 && qpwait2.presentWait2 &&
                              qtiming.presentTiming && qtiming.presentAtAbsoluteTime;
    g_vk.has_display_timing = has_google_timing && !g_vk.has_present_wait;
    g_vk.has_vertex_robustness2 = qpipeline.pipelineRobustness && qrobust.robustBufferAccess2;
    g_vk.has_cond_render = has_cond && qcond.conditionalRendering;
    g_vk.has_fault = has_fault && qfault.deviceFault;
    g_vk.has_provoking_last = has_provoking && qprov.provokingVertexLast;
    g_vk.has_list_restart = has_list_restart && qlist_restart.primitiveTopologyListRestart;
    g_vk.has_patch_list_restart = g_vk.has_list_restart && qlist_restart.primitiveTopologyPatchListRestart;
    if (g_vk.has_list_restart)
        LOG("vulkan: the restart index restarts lists as it does strips%s (VK_EXT_primitive_topology_list_restart)\n",
            g_vk.has_patch_list_restart ? ", and patch lists" : ", not patch lists");
    else
        LOG("vulkan: the restart index restarts strips and fans only: it is a vertex in a list, as the device has %s\n",
            has_list_restart ? "VK_EXT_primitive_topology_list_restart without its list feature"
                             : "no VK_EXT_primitive_topology_list_restart");
    g_vk.has_index8 = index8_ext && qindex8.indexTypeUint8;
    mesh_choose(has_mesh, &qmesh, &q13);
    if (core13) { /* the subgroup sizes are Vulkan 1.3's, as mesh_choose reads them */
        VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
        g_vk.has_cs_warp32 = q13.subgroupSizeControl && q13.computeFullSubgroups && p13.minSubgroupSize <= 32 &&
                             p13.maxSubgroupSize >= 32 &&
                             (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    }
    {
        /* each draw's vertex layout as dynamic state, or a pipeline per layout: the setting, then the device and the
           drivers known to draw it wrong (vertex_input.h). A value the setting doesn't take stops the game here, as
           a player who wrote vertex_input_dynamic_state=of would otherwise play with what they meant to turn off */
        int mode = vk_vi_mode(g_settings.vertex_input_dynamic_state), warn;
        char line[2048], ban[1200];
        uint32_t ban_line = vk_ban_find(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
        if (ban_line) vk_ban_why(ban_line, ban, sizeof ban);
        if (mode < 0)
            fatal("settings: vertex_input_dynamic_state=%s is not auto, on or off",
                  g_settings.vertex_input_dynamic_state);
#if defined(__ANDROID__)
        int android = 1;
#else
        int android = 0;
#endif
        VkViDevice vd = {g_vk.props.deviceName,
                         p12.driverName,
                         p12.driverInfo,
                         (u32)p12.driverID,
                         g_vk.props.driverVersion,
                         has_vertex_input ? (qvertex_input.vertexInputDynamicState ? 2 : 1) : 0,
                         android,
                         ban_line ? ban : NULL};
        g_vk.vertex_input_dynamic = vk_vi_choose(mode, &vd, line, sizeof line, &warn);
        if (warn)
            WARN("%s\n", line);
        else
            LOG("%s\n", line);
    }
    /* the dynamic state and the barriers within a pass only serve feedback loops */
    g_vk.has_feedback = g_vk.has_feedback && qloop.attachmentFeedbackLoopLayout;
    g_vk.has_feedback_dynamic =
        g_vk.has_feedback && has_feedback_dynamic && qloop_dyn.attachmentFeedbackLoopDynamicState;
    g_vk.has_local_read = g_vk.has_feedback && g_vk.has_local_read && qlocal.dynamicRenderingLocalRead;
    g_shader_maximal_reconvergence = has_reconvergence && qreconv.shaderMaximalReconvergence;
    g_shader_clock_scopes =
        has_shader_clock ? (qclock.shaderSubgroupClock ? 1 : 0) | (qclock.shaderDeviceClock ? 2 : 0) : 0;
    g_vk.has_divisor = g_vk.has_divisor && fdiv.vertexAttributeInstanceRateDivisor;
    {
        /* pipelines from libraries (pipeline_parts.h), and the pre-rasterization state they take as dynamic state */
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        if (has_gpl) {
            VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp};
            vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        }
        VkGplDevice gd = {.setting = g_settings.pipeline_libraries,
                          .has_ext = has_gpl,
                          .library = qgpl.graphicsPipelineLibrary,
                          .fast_link = gp.graphicsPipelineLibraryFastLinking,
                          .independent_interpolation = gp.graphicsPipelineLibraryIndependentInterpolationDecoration,
                          .polygon = qeds3.extendedDynamicState3PolygonMode,
                          .depth_clamp = qeds3.extendedDynamicState3DepthClampEnable,
                          .provoking = qeds3.extendedDynamicState3ProvokingVertexMode,
                          .conservative_mode = qeds3.extendedDynamicState3ConservativeRasterizationMode,
                          .conservative_size = qeds3.extendedDynamicState3ExtraPrimitiveOverestimationSize,
                          .patch = qeds2.extendedDynamicState2PatchControlPoints,
                          .depth_range = qeds3.extendedDynamicState3DepthClipNegativeOneToOne,
                          .depth_clip_control = qclip_control.depthClipControl,
                          .has_provoking_last = g_vk.has_provoking_last,
                          .has_conservative = g_vk.has_conservative};
        char line[400];
        vk_gpl_bans(&gd, ban_say);
        g_vk.has_gpl = vk_gpl_choose(&gd, &g_vk.dyn_pr, line, sizeof line);
        LOG("%s\n", line);
        /* a ban that holds took the extension out of the list (vk_bans_filter): its line says why */
        uint32_t ban_line = vk_ban_find(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        if (ban_line) {
            char ban[1200];
            vk_ban_why(ban_line, ban, sizeof ban);
            LOG("%s\n", ban);
        }
        vk_depth_range_line(&gd, g_vk.dyn_pr, line, sizeof line);
        LOG("%s\n", line);
    }
    g_vk.has_xfb = g_vk.has_xfb && fxfb.transformFeedback;
    g_vk.has_geometry_streams = g_vk.has_xfb && fxfb.geometryStreams;
    g_shader_geometry_streams = g_vk.has_geometry_streams;
    if (g_vk.has_geometry_streams) {
        VkPhysicalDeviceTransformFeedbackPropertiesEXT properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &properties};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &query);
        g_vk.geometry_streams_lines_triangles = properties.transformFeedbackStreamsLinesTriangles;
    }
    g_shader_geometry_streams_lines_triangles = g_vk.geometry_streams_lines_triangles;
    g_vk.has_sample_interlock = g_vk.has_fragment_interlock && qinterlock.fragmentShaderSampleInterlock;
    /* without the extension (MoltenVK) a capture's vertex program stores its words itself, which vertex stores allow */
    g_shader_xfb_stores = !g_vk.has_xfb && want->vertexPipelineStoresAndAtomics;
    if (g_shader_xfb_stores)
        LOG("vulkan: no VK_EXT_transform_feedback: vertex programs store what transform feedback captures\n");
    g_vk.has_fragment_interlock = g_vk.has_fragment_interlock && qinterlock.fragmentShaderPixelInterlock;
    if (g_vk.advanced_blend_caps.available) {
        VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT advanced = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                  .pNext = &advanced};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &properties);
        g_vk.advanced_blend_caps.coherent = qadvanced.advancedBlendCoherentOperations;
        g_vk.advanced_blend_caps.max_attachments = advanced.advancedBlendMaxColorAttachments;
        g_vk.advanced_blend_caps.nonpremultiplied_source = advanced.advancedBlendNonPremultipliedSrcColor;
        g_vk.advanced_blend_caps.correlated_overlap = advanced.advancedBlendCorrelatedOverlap;
        g_vk.advanced_blend_caps.all_operations = advanced.advancedBlendAllOperations;
    }
    g_vk.has_border_color = g_vk.has_border_color && fbc.customBorderColors && fbc.customBorderColorWithoutFormat;
    if (g_vk.has_border_color) { /* how many samplers may have a custom border color: as few as 32 */
        VkPhysicalDeviceCustomBorderColorPropertiesEXT cbp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 cp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cbp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &cp2);
        g_vk.border_color_max = cbp.maxCustomBorderColorSamplers;
    }
    if (has_descriptor_buffer && qdesc.descriptorBuffer && q12.bufferDeviceAddress) {
        g_vk.descriptor_props = (VkPhysicalDeviceDescriptorBufferPropertiesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 desc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                            .pNext = &g_vk.descriptor_props};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &desc);
        g_vk.has_descriptor_buffers =
            g_vk.descriptor_props.combinedImageSamplerDescriptorSingleArray &&
            g_vk.descriptor_props.maxSamplerDescriptorBufferRange >= (64ull << 20) &&
            g_vk.descriptor_props.maxResourceDescriptorBufferRange >= (64ull << 20) &&
            g_vk.descriptor_props.samplerDescriptorBufferAddressSpaceSize >= (VK_FRAMES * (64ull << 20)) &&
            g_vk.descriptor_props.resourceDescriptorBufferAddressSpaceSize >= (VK_FRAMES * (64ull << 20)) &&
            g_vk.descriptor_props.descriptorBufferAddressSpaceSize >= (VK_FRAMES * (64ull << 20));
    }
    /* Automatic by where the device's memory is (vk_descriptors_auto) */
    int descriptors = vk_descriptors_auto(g_settings.descriptor_buffers, g_vk.mem_model == VK_MEM_UMA);
    if (descriptors != g_settings.descriptor_buffers)
        LOG("vulkan: vk_descriptors_mode=%d (Automatic) is %d, %s, on this GPU, which %s\n",
            g_settings.descriptor_buffers, descriptors, descriptors ? "descriptor heaps" : "descriptor sets",
            g_vk.mem_model == VK_MEM_UMA ? "shares the system's memory" : "has memory of its own");
    g_vk.has_descriptor_buffers = g_vk.has_descriptor_buffers && descriptors;
    if (g_vk.has_descriptor_buffers && !vk_descriptors_memory(g_vk.phys)) {
        WARN("vulkan: descriptor buffers need host-visible, coherent and cached memory, which this device has not: "
             "graphics descriptors use sets\n");
        g_vk.has_descriptor_buffers = 0;
    }
    const char* heaps_missing = vk_descriptor_heap_missing(
        has_descriptor_heap, has_descriptor_heap && qheap.descriptorHeap, q12.bufferDeviceAddress,
        (has_extended_flags && qflags.extendedFlags) || (has_maintenance5 && qmaintenance5.maintenance5));
    g_vk.has_descriptor_heap_support = !heaps_missing && descriptors >= 2;
    if (heaps_missing && descriptors >= 2)
        LOG("vulkan: vk_descriptors_mode=%d asks for descriptor heaps, which this device has not (%s): graphics "
            "descriptors use %s\n",
            g_settings.descriptor_buffers, heaps_missing, g_vk.has_descriptor_buffers ? "buffers" : "sets");
    g_vk.has_descriptor_heaps = g_vk.has_descriptor_heap_support;
    if (g_vk.has_descriptor_heap_support) {
        g_vk.descriptor_heap_props = (VkPhysicalDeviceDescriptorHeapPropertiesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                             .pNext = &g_vk.descriptor_heap_props};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
    }
    /* FFmpeg's decoder makes views of its two plane pictures */
    g_shader_dynamic_texture_offsets = has_maintenance8 && qmaintenance8.maintenance8 && want->shaderImageGatherExtended;
    LOG("vulkan: dynamic texture offsets use %s\n",
        g_shader_dynamic_texture_offsets ? "native maintenance8 sampling" : "portable dispatch");
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .shaderDrawParameters = q11.shaderDrawParameters,
                                            .samplerYcbcrConversion =
                                                g_vk.video_family != ~0u && q11.samplerYcbcrConversion};
    g_vk.has_shader_draw_parameters = q11.shaderDrawParameters != 0;
    g_shader_draw_parameters = g_vk.has_shader_draw_parameters;
    g_vk.has_draw_indirect_count = q12.drawIndirectCount != 0;
    /* Enable mirror clamp and vertex shader Layer output when the device supports them. */
    g_vk.has_mirror_clamp = q12.samplerMirrorClampToEdge;
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &f11,
                                            .samplerMirrorClampToEdge = q12.samplerMirrorClampToEdge,
                                            .samplerFilterMinmax = q12.samplerFilterMinmax,
                                            .timelineSemaphore = VK_TRUE,
                                            .bufferDeviceAddress = g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support,
                                            .shaderFloat16 = q12.shaderFloat16,
                                            .shaderOutputViewportIndex = q12.shaderOutputViewportIndex,
                                            .shaderOutputLayer = q12.shaderOutputLayer,
                                            .drawIndirectCount = q12.drawIndirectCount,
                                            .shaderSampledImageArrayNonUniformIndexing =
                                                q12.shaderSampledImageArrayNonUniformIndexing};
    /* shaders pick an indexed texture per invocation from an array (SNX_VKB_TEXARRAY) */
    g_shader_texture_arrays = q12.shaderSampledImageArrayNonUniformIndexing != 0;
    g_shader_float16 = q12.shaderFloat16 != 0;
    /* its f16 vectors are Float16's, so shaderFloat16 too */
    g_shader_mixed_dot = has_mixed_dot && qmixdot.shaderMixedFloatDotProductFloat16AccFloat32 && g_shader_float16;
    /* its f16 matrices are Float16's, its modules the Vulkan memory model's, and its matrices span a subgroup of 32 */
    g_shader_hmma_coop = has_coop && qcoop.cooperativeMatrix && g_shader_float16 && q12.vulkanMemoryModel &&
                                 q12.vulkanMemoryModelDeviceScope && g_vk.has_cs_warp32 && coop_f16_f32(g_vk.phys)
                             ? (int)g_vk.props.limits.maxComputeSharedMemorySize
                             : 0;
    f12.vulkanMemoryModel = f12.vulkanMemoryModelDeviceScope = g_shader_hmma_coop != 0;
    g_shader_half_rounding = 0;
    if (q12.shaderFloat16 && p12.shaderRoundingModeRTEFloat16 && p12.shaderDenormPreserveFloat16 &&
        p12.shaderSignedZeroInfNanPreserveFloat16 &&
        p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE &&
        p12.roundingModeIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE) {
        int linked = p12.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY ||
                     p12.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY;
        if (!linked || (have.shaderFloat64 && p12.shaderRoundingModeRTEFloat64 && p12.shaderDenormPreserveFloat64 &&
                        p12.shaderSignedZeroInfNanPreserveFloat64))
            g_shader_half_rounding = linked ? 3 : 1;
    }
    g_shader_output_layer = q12.shaderOutputLayer != 0; /* ShaderLayer: a vertex or tessellation program's layer */
    g_shader_viewport_index = have.multiViewport ? 2 | (q12.shaderOutputViewportIndex ? 1 : 0) : 0;
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = &f12,
                                            .synchronization2 = VK_TRUE,
                                            .dynamicRendering = VK_TRUE,
                                            .maintenance4 = q13.maintenance4,
                                            /* KIL demotes the lane; S2R queries its helper status */
                                            .shaderDemoteToHelperInvocation = VK_TRUE,
                                            /* the task and mesh stages' subgroups of 32, whole (mesh_choose), and
                                               those of the compute programs that use other lanes' values */
                                            .subgroupSizeControl = g_vk.has_mesh || g_vk.has_cs_warp32,
                                            .computeFullSubgroups = g_vk.has_mesh || g_vk.has_cs_warp32};
    void* chain = &f13;
    /* a 1.2 device: those features as its extensions' (vk12_extensions), the 1.3 struct left out */
    VkPhysicalDeviceSynchronization2Features fsync2 = {.sType =
                                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
                                                       .pNext = &f12,
                                                       .synchronization2 = VK_TRUE};
    VkPhysicalDeviceDynamicRenderingFeatures frendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = &fsync2,
        .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceMaintenance4Features fmaintenance4 = {.sType =
                                                              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,
                                                          .pNext = &frendering,
                                                          .maintenance4 = VK_TRUE};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures fdemote = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
        .pNext = &fmaintenance4,
        .shaderDemoteToHelperInvocation = VK_TRUE};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
        .pNext = &fdemote,
        .extendedDynamicState = VK_TRUE};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT feds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,
        .pNext = &feds,
        .extendedDynamicState2 = VK_TRUE};
    if (!core13) {
        chain = &feds2;
        for (u32 i = 0; i < VK12_EXTENSIONS; i++) ext[next++] = vk12_extensions[i];
        ext[next++] = store_none_ext;
    }
    VkPhysicalDeviceMaintenance8FeaturesKHR fmaintenance8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR, .maintenance8 = VK_TRUE};
    if (g_shader_dynamic_texture_offsets) {
        fmaintenance8.pNext = chain;
        chain = &fmaintenance8;
        ext[next++] = VK_KHR_MAINTENANCE_8_EXTENSION_NAME;
    }
    VkPhysicalDeviceDescriptorBufferFeaturesEXT fdesc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
        .pNext = chain,
        .descriptorBuffer = VK_TRUE};
    if (g_vk.has_descriptor_buffers) {
        chain = &fdesc;
        ext[next++] = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME;
    }
    VkPhysicalDeviceDescriptorHeapFeaturesEXT fheap = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT, .descriptorHeap = VK_TRUE};
    VkPhysicalDeviceExtendedFlagsFeaturesKHR fflags = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_FLAGS_FEATURES_KHR, .extendedFlags = VK_TRUE};
    VkPhysicalDeviceMaintenance5FeaturesKHR fmaintenance5 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .maintenance5 = VK_TRUE};
    if (g_vk.has_descriptor_heap_support) {
        if (has_maintenance5 && qmaintenance5.maintenance5) {
            fmaintenance5.pNext = chain;
            chain = &fmaintenance5;
            ext[next++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
        } else {
            fflags.pNext = chain;
            chain = &fflags;
            ext[next++] = VK_KHR_EXTENDED_FLAGS_EXTENSION_NAME;
        }
        fheap.pNext = chain;
        chain = &fheap;
        ext[next++] = VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME;
    }
    if (g_vk.has_divisor) {
        fdiv = (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
            .pNext = chain,
            .vertexAttributeInstanceRateDivisor = VK_TRUE};
        chain = &fdiv;
        ext[next++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    }
    if (g_vk.has_border_color) {
        fbc = (VkPhysicalDeviceCustomBorderColorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
            .pNext = chain,
            .customBorderColors = VK_TRUE,
            .customBorderColorWithoutFormat = VK_TRUE};
        chain = &fbc;
        ext[next++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    }
    if (g_vk.has_xfb) {
        fxfb = (VkPhysicalDeviceTransformFeedbackFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
            .pNext = chain,
            .transformFeedback = VK_TRUE,
            .geometryStreams = g_vk.has_geometry_streams};
        chain = &fxfb;
        ext[next++] = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    }
    /* the heap as GPU memory where it is (uma.c's placed maps) */
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT fplaced = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
        .pNext = chain,
        .memoryMapPlaced = VK_TRUE,
        .memoryUnmapReserve = VK_TRUE};
    if (vk_uma_can_place()) {
        chain = &fplaced;
        ext[next++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
        ext[next++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
        vk_uma_dmabuf(has_dmabuf); /* a view of it maps the same memory again, through a dma-buf */
        if (vk_uma_can_view()) {
            ext[next++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
            ext[next++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
        }
    }
    VkPhysicalDeviceShaderClockFeaturesKHR fclock = {.sType =
                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
                                                     .pNext = chain,
                                                     .shaderSubgroupClock = !!(g_shader_clock_scopes & 1),
                                                     .shaderDeviceClock = !!(g_shader_clock_scopes & 2)};
    if (g_shader_clock_scopes) {
        chain = &fclock;
        ext[next++] = VK_KHR_SHADER_CLOCK_EXTENSION_NAME;
    }
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR freconv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR,
        .pNext = chain,
        .shaderMaximalReconvergence = VK_TRUE};
    if (g_shader_maximal_reconvergence) {
        chain = &freconv;
        ext[next++] = VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME;
    }
    VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE fmixdot = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE,
        .pNext = chain,
        .shaderMixedFloatDotProductFloat16AccFloat32 = VK_TRUE};
    if (g_shader_mixed_dot) {
        chain = &fmixdot;
        ext[next++] = VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME;
    }
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR fcoop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
        .pNext = chain,
        .cooperativeMatrix = VK_TRUE};
    if (g_shader_hmma_coop) {
        chain = &fcoop;
        ext[next++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
    }
    /* a lost device's fault, which vk_checkpoint_dump logs */
    VkPhysicalDeviceFaultFeaturesEXT ffault = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .pNext = chain, .deviceFault = VK_TRUE};
    if (g_vk.has_fault) {
        chain = &ffault;
        ext[next++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    }
    PortabilityFeatures fport = qport;
    fport.pNext = chain;
    if (has_portability) chain = &fport;
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT floop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,
        .pNext = chain,
        .attachmentFeedbackLoopLayout = VK_TRUE};
    if (g_vk.has_feedback) {
        chain = &floop;
        ext[next++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
    }
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT floop_dyn = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT,
        .pNext = chain,
        .attachmentFeedbackLoopDynamicState = VK_TRUE};
    if (g_vk.has_feedback_dynamic) {
        chain = &floop_dyn;
        ext[next++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME;
    }
    /* a barrier within a vkCmdBeginRendering pass needs this feature (VUID-vkCmdPipelineBarrier2-None-09553) */
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flocal = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR,
        .pNext = chain,
        .dynamicRenderingLocalRead = VK_TRUE};
    if (g_vk.has_local_read) {
        chain = &flocal;
        ext[next++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
    }
    VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT fadvanced = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT,
        .pNext = chain,
        .advancedBlendCoherentOperations = g_vk.advanced_blend_caps.coherent};
    if (g_vk.advanced_blend_caps.available) {
        chain = &fadvanced;
        ext[next++] = VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME;
    }
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT finterlock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
        .pNext = chain,
        .fragmentShaderPixelInterlock = VK_TRUE,
        .fragmentShaderSampleInterlock = g_vk.has_sample_interlock};
    if (g_vk.has_fragment_interlock) {
        chain = &finterlock;
        ext[next++] = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME;
    }
    /* the render enable's comparison of an occlusion query's count, made on the GPU (gpu/vk/query.c) */
    VkPhysicalDeviceConditionalRenderingFeaturesEXT fcond = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
        .pNext = chain,
        .conditionalRendering = VK_TRUE};
    if (g_vk.has_cond_render) {
        chain = &fcond;
        ext[next++] = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;
    }
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT flist_restart = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT,
        .pNext = chain,
        .primitiveTopologyListRestart = VK_TRUE,
        .primitiveTopologyPatchListRestart = (VkBool32)g_vk.has_patch_list_restart};
    if (g_vk.has_list_restart) {
        chain = &flist_restart;
        ext[next++] = VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME;
    }
    VkPhysicalDeviceProvokingVertexFeaturesEXT fprov = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
        .pNext = chain,
        .provokingVertexLast = VK_TRUE};
    if (g_vk.has_provoking_last) {
        chain = &fprov;
        ext[next++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
    }
    /* conservative rasterization (the 3D engine's CONSERVATIVE_RASTER, NVN's SetConservativeRasterEnable), overestimated
       by the dilation the game asks for up to the device's most: the extension has no features to enable */
    if (g_vk.has_conservative) {
        VkPhysicalDeviceConservativeRasterizationPropertiesEXT cons = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cons};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        g_vk.conservative_extra = cons.maxExtraPrimitiveOverestimationSize;
        ext[next++] = VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME;
    }
    /* the 3D engine's 8-bit indices, read as they are (render.c); without it they are widened on the GPU */
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvertex_input = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
        .pNext = chain,
        .vertexInputDynamicState = VK_TRUE};
    if (g_vk.vertex_input_dynamic) {
        chain = &fvertex_input;
        ext[next++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    }
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
        .pNext = chain,
        .graphicsPipelineLibrary = VK_TRUE};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT feds2_patch = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,
        .extendedDynamicState2PatchControlPoints = VK_TRUE};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
        .extendedDynamicState3PolygonMode = (g_vk.dyn_pr & VK_DYN_PR_POLYGON) != 0,
        .extendedDynamicState3DepthClampEnable = (g_vk.dyn_pr & VK_DYN_PR_DEPTH_CLAMP) != 0,
        .extendedDynamicState3ProvokingVertexMode = (g_vk.dyn_pr & VK_DYN_PR_PROVOKING) != 0,
        .extendedDynamicState3ConservativeRasterizationMode = (g_vk.dyn_pr & VK_DYN_PR_CONSERVATIVE) != 0,
        .extendedDynamicState3ExtraPrimitiveOverestimationSize = (g_vk.dyn_pr & VK_DYN_PR_CONSERVATIVE) != 0,
        .extendedDynamicState3DepthClipNegativeOneToOne = (g_vk.dyn_pr & VK_DYN_PR_DEPTH_RANGE) != 0};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fclip_control = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, .depthClipControl = VK_TRUE};
    if (g_vk.has_gpl) {
        chain = &fgpl;
        ext[next++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[next++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        if ((g_vk.dyn_pr & VK_DYN_PR_PATCH) && !core13) /* a 1.2 device has EDS2 in its chain (vk12_extensions) */
            feds2.extendedDynamicState2PatchControlPoints = VK_TRUE;
        else if (g_vk.dyn_pr & VK_DYN_PR_PATCH) {
            feds2_patch.pNext = chain;
            chain = &feds2_patch;
            ext[next++] = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME;
        }
        if (g_vk.dyn_pr & ~VK_DYN_PR_PATCH) {
            feds3.pNext = chain;
            chain = &feds3;
            ext[next++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
        }
        if (g_vk.dyn_pr & VK_DYN_PR_DEPTH_RANGE) {
            fclip_control.pNext = chain;
            chain = &fclip_control;
            ext[next++] = VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME;
        }
    }
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR findex8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR,
        .pNext = chain,
        .indexTypeUint8 = VK_TRUE};
    if (g_vk.has_index8) {
        chain = &findex8;
        ext[next++] = index8_ext;
    }
    /* when each present is displayed (display.c), which the game's pace goes by */
    VkPhysicalDevicePresentIdFeaturesKHR fpid = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = chain, .presentId = VK_TRUE};
    VkPhysicalDevicePresentWaitFeaturesKHR fpwait = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, .pNext = &fpid, .presentWait = VK_TRUE};
    if (g_vk.has_present_wait) {
        chain = &fpwait;
        ext[next++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
        ext[next++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
    }
    /* a game frame held until the refresh it is due at (display.c), where the surface can */
    VkPhysicalDevicePresentId2FeaturesKHR fpid2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, .pNext = chain, .presentId2 = VK_TRUE};
    VkPhysicalDevicePresentWait2FeaturesKHR fpwait2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR,
        .pNext = &fpid2,
        .presentWait2 = VK_TRUE};
    VkPhysicalDevicePresentTimingFeaturesEXT ftiming = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
        .pNext = &fpwait2,
        .presentTiming = VK_TRUE,
        .presentAtAbsoluteTime = VK_TRUE};
    if (g_vk.has_present_timing) {
        chain = &ftiming;
        ext[next++] = VK_KHR_PRESENT_ID_2_EXTENSION_NAME;
        ext[next++] = VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME;
        ext[next++] = VK_EXT_PRESENT_TIMING_EXTENSION_NAME;
        ext[next++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME; /* present timing's requirement */
    }
    if (g_vk.has_display_timing) ext[next++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME; /* when presents are shown */
    VkPhysicalDevicePipelineRobustnessFeaturesEXT fpipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT,
        .pNext = chain,
        .pipelineRobustness = VK_TRUE};
    if (g_vk.has_vertex_robustness2) {
        chain = &fpipeline;
        ext[next++] = VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME;
        ext[next++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    }
    VkPhysicalDeviceMeshShaderFeaturesEXT fmesh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
                                                   .pNext = chain,
                                                   .meshShader = VK_TRUE,
                                                   .taskShader = (VkBool32)g_vk.has_task};
    if (g_vk.has_mesh) {
        chain = &fmesh;
        ext[next++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
    }
    /* vk_checkpoints=1: checkpoints.c's markers too, on VK_NV_device_diagnostic_checkpoints, which the device enables
       above where it has it (has_nv_checkpoints) */
    if (g_settings.gpu_checkpoints) {
        g_vk.has_checkpoints = g_vk.has_nv_checkpoints;
        if (!g_vk.has_checkpoints)
            WARN("vulkan: vk_checkpoints: this driver has no VK_NV_device_diagnostic_checkpoints\n");
    }
    u32 nq = g_vk.compute_family == g_vk.gfx_family ? 1 : 2;
    if (g_vk.video_family != ~0u)
        qci[nq++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = g_vk.video_family,
                                              .queueCount = 1,
                                              .pQueuePriorities = prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = chain,
                              .queueCreateInfoCount = nq,
                              .pQueueCreateInfos = qci,
                              .enabledExtensionCount = next,
                              .ppEnabledExtensionNames = ext,
                              .pEnabledFeatures = &g_vk.feat};
    if (!VK_OK(vkCreateDevice(g_vk.phys, &dci, NULL, &g_vk.dev))) return 0;
    g_vk.gfx_queues = second ? 2 : 1;
    g_vk.dev_ext_n = next;
    g_vk.feat11 = f11;
    g_vk.feat12 = f12;
    g_vk.feat13 = f13;
    g_vk.feat11.pNext = &g_vk.feat12;
    g_vk.feat12.pNext = core13 ? &g_vk.feat13 : NULL;
    g_vk.feat13.pNext = NULL;
    g_vk.feat2 = (VkPhysicalDeviceFeatures2){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &g_vk.feat11, .features = g_vk.feat};
    volkLoadDevice(g_vk.dev);
    const char* lacks_command = NULL;
    if (!core13 && !vk12_commands(&lacks_command)) {
        WARN("vulkan: %s enabled the extensions 1.3 took in, but has no %s\n", g_vk.props.deviceName, lacks_command);
        snprintf(g_vk.start_failure, sizeof g_vk.start_failure,
                 "%s has Vulkan 1.2 and the extensions 1.3 took in, but its driver has no %s", g_vk.props.deviceName,
                 lacks_command);
        vkDestroyDevice(g_vk.dev, NULL);
        g_vk.dev = VK_NULL_HANDLE;
        return 0;
    }
    if (g_vk.has_checkpoints) vk_ck_init();
    pm_lock_announce(&g_vklock, "vulkan"); /* the device's calls, every thread's */
    vkGetDeviceQueue(g_vk.dev, g_vk.gfx_family, 0, &g_vk.gfx);
    vkGetDeviceQueue(g_vk.dev, g_vk.compute_family, second ? 1 : 0, &g_vk.compute);
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
                             .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    vk_mem_init(sparse_queue);
    /* VMA's blocks are 256 MiB at most already, and kept within a smaller cap */
    VmaAllocatorCreateInfo vai = {.physicalDevice = g_vk.phys,
                                  .device = g_vk.dev,
                                  .instance = g_vk.instance,
                                  .preferredLargeHeapBlockSize = g_vk.alloc_cap < (256ull << 20) ? g_vk.alloc_cap : 0,
                                  .vulkanApiVersion = core13 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2,
                                  .pVulkanFunctions = &vf,
                                  .flags = g_vk.has_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0};
    if (g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support)
        vai.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (!VK_OK(vmaCreateAllocator(&vai, &g_vk.vma))) return 0;
    __atomic_store_n(&g_vma_ready, 1, __ATOMIC_RELEASE);
    if (g_vk.has_descriptor_heaps && !vk_descriptor_heaps_init(&g_vk.descriptor_heap_props)) {
        g_vk.has_descriptor_heaps = 0;
        g_vk.has_descriptor_heap_support = 0;
    }
    LOG("vulkan: graphics descriptors use %s%s\n", g_vk.has_descriptor_heaps ? "heaps" :
        g_vk.has_descriptor_buffers ? "buffers" : "sets",
        g_vk.has_descriptor_heaps ? (g_vk.has_descriptor_buffers ? " with buffer fallback" : " with set fallback") : "");
    vk_uma_init();
    astc_choose();
    {
        /* which driver runs, by its own name: the system's, or a package's (gpu_driver) */
        VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &pp);
        g_vk.driver_id = (u32)dp.driverID;
        snprintf(g_vk.driver_name, sizeof g_vk.driver_name, "%s", dp.driverName);
        LOG("vulkan: the driver that runs: %s, %s (driver ID %d)\n", dp.driverName, dp.driverInfo, (int)dp.driverID);
    }
    if (g_vk.api_cap && g_vk.props.apiVersion >= VK_API_VERSION_1_3)
        LOG("vulkan: vk_api=1.2 runs the Vulkan 1.2 way on this %u.%u device\n",
            VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion));
    else if (g_vk.api_cap)
        LOG("vulkan: vk_api=1.2 on a Vulkan %u.%u device, which runs the 1.2 way as auto would\n",
            VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion));
    if (core13)
        LOG("vulkan: the Vulkan 1.3 way: synchronization2, dynamic rendering and the extended dynamic state are core\n");
    else {
        char names[512];
        size_t at = 0;
        for (u32 i = 0; i < VK12_EXTENSIONS && at < sizeof names; i++)
            at += (size_t)snprintf(names + at, sizeof names - at, "%s, ", vk12_extensions[i]);
        LOG("vulkan: the Vulkan 1.2 way: what 1.3 has from %s%s, shaders in SPIR-V 1.5, no Vulkan Video decode\n",
            names, store_none_ext);
    }
    LOG("vulkan: %s, Vulkan %u.%u.%u, driver %08x, graphics queue family %u, compute %u%s\n", g_vk.props.deviceName,
        VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
        VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.props.driverVersion, g_vk.gfx_family, g_vk.compute_family,
        g_vk.compute_family != g_vk.gfx_family ? " (separate family)"
        : second                               ? " (a second queue)"
                                               : " (the graphics queue)");
    if (g_vk.has_feedback)
        LOG("vulkan: draws sample the color targets they render to in place (attachment feedback loops, %s, %s)\n",
            g_vk.has_feedback_dynamic ? "enabled by dynamic state" : "enabled per pipeline",
            g_vk.has_local_read ? "ordered by barriers within the pass" : "ordered by beginning the pass again");
    else
        LOG("vulkan: draws that sample a color target they render to read a copy of it (no attachment feedback "
            "loops)\n");
    vk_bans_unclaimed(ban_say); /* a ban on something nothing here asked about, said rather than dropped */
    return 1;
}

/* the swapchain destroyed. Its images' semaphores: a present may still wait on one after the device is idle (the
   presentation engine isn't covered by vkDeviceWaitIdle), so they go a swapchain later, when those presents are long
   done */
static void swapchain_retire(void) { /* sc_lock held */
    static VkSemaphore retired[VK_SC_MAX];
    static u32 nretired;
    for (u32 i = 0; i < nretired; i++) vkDestroySemaphore(g_vk.dev, retired[i], NULL);
    nretired = 0;
    for (u32 i = 0; i < g_vk.sc_count; i++) retired[nretired++] = g_vk.sc_done[i];
    vk_ui_swapchain_gone();
    vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    g_vk.sc_clock = VK_CLOCK_NONE;
    g_vk.sc_hold = 0;
    g_vk.sc_count = 0;
}
/* vkDeviceWaitIdle, which takes every queue: those of the decoder's (FFmpeg's) are excluded by vk_lock, held here */
static void device_wait_idle(void) {
    vk_queue_lock(g_vk.gfx);
    if (g_vk.compute != g_vk.gfx) vk_queue_lock(g_vk.compute);
    vkDeviceWaitIdle(g_vk.dev);
    if (g_vk.compute != g_vk.gfx) vk_queue_unlock(g_vk.compute);
    vk_queue_unlock(g_vk.gfx);
}

/* the window's surface made anew, its swapchain to follow: Android destroys a window's surface while the app is in
   the background, and the Vulkan surface on it is lost for good */
int vk_surface_recreate(void) {
    vk_rec_sync(); /* the device wait covers only what was submitted */
    device_wait_idle();
    pm_lock(&g_vk.sc_lock);
    if (g_vk.swapchain) swapchain_retire();
    pm_unlock(&g_vk.sc_lock);
    vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        VkBool32 present = VK_FALSE; /* the queue chosen for the old surface, asked again for the new one */
        if (vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.gfx_family, g_vk.surface, &present) == VK_SUCCESS &&
            present)
            return 1;
        WARN_LIMIT(4, "vulkan: the window's new surface can't be presented to from the graphics queue\n");
        return 0;
    }
    WARN_LIMIT(4, "vulkan: the window's surface could not be made again\n");
    return 0;
}

int vk_window_release(void) {
    int waited = 0;
    /* a thread that holds the renderer and doesn't let go (stuck on a lost device) must not keep the error unseen */
    while (!vk_trylock()) {
        if (++waited == 200) return 0;
        plat_sleep_ns(10000000);
    }
    vk_rec_sync();
    device_wait_idle(); /* a lost device answers at once */
    pm_lock(&g_vk.sc_lock);
    if (g_vk.swapchain) swapchain_retire();
    pm_unlock(&g_vk.sc_lock);
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    /* every hold of this thread's at once: it won't return to the callers that took them */
    t_vkdepth = 0;
    vk_let_go();
    return 1;
}

int vk_swapchain_create(int srgb) {
    if (g_opt_headless) return 0; /* no WSI calls, even on a resize/recreate path */
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (cr != VK_SUCCESS) { /* asked again on each present while it fails: said a few times */
        WARN_LIMIT(4, "vulkan: the window's surface could not be queried: %s (%d)\n", snx_vk_result_str(cr), (int)cr);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) g_vk.sc_lost = 1;
        return 0;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        WARN("vulkan: the window's surface can't be drawn to by copies, which presenting needs\n");
        return 0;
    }
    int ww, wh; /* what presents compare against, whatever the surface makes of it */
    backend_window_size(&ww, &wh);
    /* Wayland: the swapchain decides, so the window's size */
    VkExtent2D ext = snx_vk_swap_extent(&caps, (VkExtent2D){ww > 0 ? (u32)ww : 0, wh > 0 ? (u32)wh : 0});
    if (!ext.width || !ext.height) return 0;

    /* 8-bit, in the encoding of the frames presented: a blit from an sRGB target to a UNORM image decodes the bytes,
       where the console's display shows them as they are */
    static const VkFormat srgb_formats[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    static const VkFormat unorm_formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    VkSurfaceFormatKHR fmt;
    if (!(srgb && snx_vk_surface_format(g_vk.phys, g_vk.surface, srgb_formats, 2, &fmt)))
        snx_vk_surface_format(g_vk.phys, g_vk.surface, unorm_formats, 2, &fmt);
    if (srgb && fmt.format != VK_FORMAT_B8G8R8A8_SRGB && fmt.format != VK_FORMAT_R8G8B8A8_SRGB) {
        LOG_LIMIT(1, "vulkan: the window has no sRGB format: frames in sRGB show darker\n");
    }

    /* vsync=1: FIFO, which every device has. Otherwise mailbox, else immediate, else FIFO all the same. Read once: the
       main thread's .ini reload writes it */
    int vsync = __atomic_load_n(&g_settings.vsync, __ATOMIC_RELAXED);
    u32 nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, NULL);
    VkPresentModeKHR* ms = calloc(nm ? nm : 1, sizeof *ms);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, ms);
    VkPresentModeKHR mode = snx_vk_present_mode(ms, nm, vsync);
    free(ms);

    /* an image for each frame in flight beyond what the driver keeps: a frame's present waits in the recording ring
       (vk_rec_present) while the next frame acquires, which takes no wait, so with one image fewer it found none.
       Where the game's pace waits for the display (frame_pacing, FIFO with a display clock), a frame is released only
       once the one before is displayed, so frames don't queue for the display: one image more than the driver keeps
       is enough, and more would only let frames that came early (a menu's, fps_limit's) queue */
    int clock = g_vk.has_present_wait || g_vk.has_display_timing;
    u32 extra = clock && mode == VK_PRESENT_MODE_FIFO_KHR && __atomic_load_n(&g_settings.frame_pacing, __ATOMIC_RELAXED)
                    ? 1
                    : VK_FRAMES;
    u32 count = snx_vk_swap_image_count(&caps, extra, VK_SC_MAX);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) /* the lowest one it has */
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    /* frames are drawn upright: a display held other than its own way (a phone held wide) has its compositor turn
       them, where its current transform would say they come turned already */
    VkSurfaceTransformFlagBitsKHR transform = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  : caps.currentTransform;
    VkSwapchainKHR old = g_vk.swapchain;
    if (old) {
        vk_rec_sync();
        device_wait_idle();
    }
    vk_rec_present_result(); /* the old swapchain's presents, all made (a retire drained the ring), say nothing of it */
    /* frames held until a time (display.c), under FIFO where the surface can: present ids and waits 2 with it */
    int timing = 0;
    VkPresentStageFlagsEXT stages = 0;
    if (g_vk.has_present_timing && mode == VK_PRESENT_MODE_FIFO_KHR) {
        VkPresentTimingSurfaceCapabilitiesEXT ct = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
        VkSurfaceCapabilitiesPresentId2KHR cid = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR,
                                                  .pNext = &ct};
        VkSurfaceCapabilitiesPresentWait2KHR cwait = {
            .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR, .pNext = &cid};
        VkSurfaceCapabilities2KHR c2 = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &cwait};
        VkPhysicalDeviceSurfaceInfo2KHR si2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                               .surface = g_vk.surface};
        timing = vkGetPhysicalDeviceSurfaceCapabilities2KHR(g_vk.phys, &si2, &c2) == VK_SUCCESS &&
                 ct.presentTimingSupported && ct.presentAtAbsoluteTimeSupported && cid.presentId2Supported &&
                 cwait.presentWait2Supported;
        stages = ct.presentStageQueries;
    }
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .flags = timing ? VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR |
                                                         VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR |
                                                         VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT
                                                   : 0,
                                   .surface = g_vk.surface,
                                   .minImageCount = count,
                                   .imageFormat = fmt.format,
                                   .imageColorSpace = fmt.colorSpace,
                                   .imageExtent = ext,
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = transform,
                                   .compositeAlpha = alpha,
                                   .presentMode = mode,
                                   .clipped = VK_TRUE,
                                   .oldSwapchain = old};
    /* old is retired by the create, made or not: on any failure it goes too, and the next present starts anew */
    VkSwapchainKHR sc;
    VkSemaphore done[VK_SC_MAX];
    u32 nimg = 0, ndone = 0;
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    pm_lock(&g_vk.sc_lock); /* until the new swapchain is g_vk's: the old one is retired from here */
    cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &sc);
    if (cr != VK_SUCCESS) { /* tried again on each present while it fails: said a few times */
        WARN_LIMIT(4, "vulkan: the swapchain could not be made: %s (%d)\n", snx_vk_result_str(cr), (int)cr);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) g_vk.sc_lost = 1;
        if (old) swapchain_retire();
        pm_unlock(&g_vk.sc_lock);
        return 0;
    }
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &nimg, NULL);
    if (nimg > VK_SC_MAX) WARN("vulkan: the swapchain has %u images, more than the %d taken\n", nimg, VK_SC_MAX);
    while (nimg <= VK_SC_MAX && ndone < nimg && VK_OK(vkCreateSemaphore(g_vk.dev, &si, NULL, &done[ndone]))) ndone++;
    if (nimg > VK_SC_MAX || ndone < nimg) {
        while (ndone) vkDestroySemaphore(g_vk.dev, done[--ndone], NULL);
        vkDestroySwapchainKHR(g_vk.dev, sc, NULL);
        if (old) swapchain_retire();
        pm_unlock(&g_vk.sc_lock);
        return 0;
    }
    if (old) swapchain_retire();
    g_vk.swapchain = sc;
    /* the times presents are held until: the system's CLOCK_MONOTONIC (plat_monotonic_ns) if the swapchain has it,
       else a present stage's own clock, calibrated against CLOCK_MONOTONIC at each present (display.c), the stage
       the first pixel goes out at if it can be. VK_GOOGLE_display_timing's are CLOCK_MONOTONIC's, under FIFO */
    g_vk.sc_clock = timing                    ? VK_CLOCK_WAIT2
                    : g_vk.has_present_wait   ? VK_CLOCK_WAIT
                    : g_vk.has_display_timing ? VK_CLOCK_GOOGLE
                                              : VK_CLOCK_NONE;
    g_vk.sc_hold = g_vk.sc_clock == VK_CLOCK_GOOGLE && mode == VK_PRESENT_MODE_FIFO_KHR && plat_monotonic_ns();
    g_vk.sc_time_stage = 0;
    if (timing && plat_monotonic_ns()) {
        VkTimeDomainKHR dom[16];
        u64 ids[16], counter = 0;
        VkSwapchainTimeDomainPropertiesEXT tp = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
                                                 .timeDomainCount = 16,
                                                 .pTimeDomains = dom,
                                                 .pTimeDomainIds = ids};
        VkResult tr = vkGetSwapchainTimeDomainPropertiesEXT(g_vk.dev, sc, &tp, &counter);
        for (u32 i = 0; (tr == VK_SUCCESS || tr == VK_INCOMPLETE) && i < tp.timeDomainCount && i < 16; i++)
            if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) {
                g_vk.sc_hold = 1;
                g_vk.sc_time_domain = ids[i];
                g_vk.sc_time_stage = 0;
            } else if (dom[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT && !g_vk.sc_hold && stages &&
                       g_vk.has_calibrated_mono) {
                g_vk.sc_hold = 1;
                g_vk.sc_time_domain = ids[i];
                g_vk.sc_time_stage = stages & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT
                                         ? VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT
                                         : stages & -stages;
            }
    }
    pm_unlock(&g_vk.sc_lock);
    vk_display_fifo(mode == VK_PRESENT_MODE_FIFO_KHR);
    g_vk.sc_extent = ext;
    g_vk.sc_win_w = ww;
    g_vk.sc_win_h = wh;
    g_vk.sc_srgb = fmt.format == VK_FORMAT_B8G8R8A8_SRGB || fmt.format == VK_FORMAT_R8G8B8A8_SRGB;
    g_vk.sc_format = fmt.format;
    g_vk.sc_turned = transform != caps.currentTransform;
    g_vk.sc_vsync = vsync;
    g_vk.sc_count = nimg;
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &g_vk.sc_count, g_vk.sc_images);
    memcpy(g_vk.sc_done, done, sizeof done[0] * nimg);
    LOG("vulkan: swapchain %ux%u, %u images%s%s, %s\n", ext.width, ext.height, g_vk.sc_count,
        g_vk.sc_srgb ? ", sRGB" : "",
        g_vk.sc_hold    ? ", presents timed and held until they are due"
        : g_vk.sc_clock ? ", presents timed"
                        : "",
        mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
        : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                : "fifo");
    return 1;
}

/* a start that failed once the window had its surface: the surface goes. On Android it holds the window, which takes
   one producer at a time, and the error's own screen (backend/sdl3.c's fatal_screen) could not draw in it: the system's
   plain box said the error instead */
static int start_failed(void) {
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    return 0;
}

const char* vk_start_failure(void) { return g_vk.start_failure; }

int vk_init(void) {
    char why[64];
    u32 cap;
    /* the instance is made for the version the setting allows, so it is read before. A value it takes not stops the
       game, as a player who wrote vk_api=1.3 would otherwise play the way they meant to leave */
    if (!vk_api_setting(g_settings.vulkan_api, &cap, why, sizeof why)) fatal("settings: %s", why);
    if (cap) g_vk.api_cap = cap;
    pm_init(&g_vklock);
    pm_init(&g_vk.sc_lock);
    if (g_opt_headless) {
        /* Load libvulkan/vulkan-1 directly, or the driver the program's folders carry (an Apple app's MoltenVK): no SDL
           video driver or headless-surface extension is needed. */
        void* own = backend_vk_driver();
        if (own)
            volkInitializeCustom((PFN_vkGetInstanceProcAddr)own);
        else if (!VK_OK(volkInitialize()))
            return 0;
    } else {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)backend_vk_loader();
        if (!gipa) {
            WARN("vulkan: no Vulkan loader (is a Vulkan driver installed?)\n");
            return 0;
        }
        volkInitializeCustom(gipa);
    }
    vk_renderdoc_load(); /* its capture layer is enabled when the instance is created */
    if (!create_instance()) return 0;
    backend_vk_check(); /* the instance opened the driver: the one asked for, or the run ends */
    if (!g_opt_headless && !backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        WARN("vulkan: the window backend could not create a surface\n");
        return 0;
    }
    if (!create_device()) return start_failed();
    return g_opt_headless || vk_swapchain_create(0) ? 1 : start_failed();
}
