/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Recommended Settings' test of the GPU (probe.h), run by the launcher as scarletnx --probe, with no game and no
   window: the device a game would run on (the best rated with a graphics queue, as the renderer picks with no window),
   the block formats it samples, the budget of the heap its images go to (memory.c's heap_find), and how many new
   pipelines a second its driver makes on a few numbers of threads at once, the work pipeline_threads' workers do. It
   renders nothing and keeps no pipeline: each takes its own constant, so no cache the driver keeps can answer for it,
   and the drivers' disk caches are off for the run */
#include "nx/gpu/vk/probe.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/common.h"
#include "nx/gpu/vk/probe_spv.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

#define PROBE_PIPELINES 48 /* the pipelines a step makes, on one number of threads */
#define PROBE_ROUNDS    3  /* the steps of each number of threads, taken in turns: the median rate is kept */
#define PROBE_KS        3  /* the numbers of threads tried at most */

/* one step's pipelines, made by the threads that take them in turn */
typedef struct {
    VkDevice dev;
    VkShaderModule module;
    VkPipelineLayout layout;
    PMutex lock;
    PCond done;
    u32 seed;    /* the next pipeline's constant */
    int left;    /* pipelines still to make */
    int running; /* threads still taking them */
    VkResult failed;
} ProbeStep;

static void probe_worker(void* arg) {
    ProbeStep* s = arg;
    pm_lock(&s->lock);
    while (s->left > 0 && s->failed == VK_SUCCESS) {
        u32 seed = s->seed++;
        VkSpecializationMapEntry entry = {0, 0, sizeof seed};
        VkSpecializationInfo spec = {1, &entry, sizeof seed, &seed};
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = s->module,
                                                    .pName = "main",
                                                    .pSpecializationInfo = &spec},
                                          .layout = s->layout};
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult r;
        s->left--;
        pm_unlock(&s->lock);
        r = vkCreateComputePipelines(s->dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline);
        if (pipeline) vkDestroyPipeline(s->dev, pipeline, NULL);
        pm_lock(&s->lock);
        if (r != VK_SUCCESS && s->failed == VK_SUCCESS) s->failed = r;
    }
    s->running--;
    pc_signal(&s->done);
    pm_unlock(&s->lock);
}

/* the new pipelines a second threads threads make, PROBE_PIPELINES of them; 0 when one could not be made, its result
   in s->failed */
static double probe_rate(ProbeStep* s, int threads) {
    double start = snx_seconds(), took;
    int i;
    pm_lock(&s->lock);
    s->left = PROBE_PIPELINES;
    s->running = 0;
    for (i = 0; i < threads; i++) {
        s->running++;
        if (plat_thread(probe_worker, s, (size_t)1 << 20)) s->running--;
    }
    if (!s->running) s->failed = VK_ERROR_UNKNOWN;
    while (s->running) pc_wait(&s->done, &s->lock, -1);
    pm_unlock(&s->lock);
    took = snx_seconds() - start;
    return s->failed == VK_SUCCESS && took > 0 ? PROBE_PIPELINES / took : 0;
}

static int probe_cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* rated as the renderer rates a device with no window: discrete, integrated, then the rest; one without a graphics
   queue is left out */
static int probe_score(VkPhysicalDevice phys, const VkPhysicalDeviceProperties* p, void* user) {
    (void)user;
    if (snx_vk_queue_family(phys, VK_QUEUE_GRAPHICS_BIT, VK_NULL_HANDLE, 0) == UINT32_MAX) return -1;
    return p->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
           : p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                     : 1;
}

static void probe_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* the pipelines' step: each number of threads PROBE_ROUNDS times, in turns */
static void probe_pipelines(FILE* out, VkDevice dev, VkShaderModule module, VkPipelineLayout layout) {
    ProbeStep s = {.dev = dev, .module = module, .layout = layout, .seed = 1, .failed = VK_SUCCESS};
    double rates[PROBE_KS][PROBE_ROUNDS];
    int ids[256], ks[PROBE_KS], nk = 0, i, r, cpus = plat_cpus(ids, 256), logical, candidates[PROBE_KS];
    logical = cpus > 0 ? cpus : (int)snx_cpu_count();
    /* the processor's cores, Auto's workers, and all the CPUs but two where Auto stops at twelve short of that */
    candidates[0] = (int)snx_cpu_cores();
    candidates[1] = vk_pipeline_worker_count(0, cpus);
    candidates[2] = logical - 2 > 12 ? logical - 2 : 0;
    for (i = 0; i < PROBE_KS; i++) {
        int k, seen = 0;
        if (candidates[i] < 1) continue;
        for (k = 0; k < nk; k++) seen |= ks[k] == candidates[i];
        if (!seen) ks[nk++] = candidates[i];
    }
    pm_init(&s.lock);
    pc_init(&s.done);
    fprintf(out, "probe: pipelines\npipelines_auto=%d\n", candidates[1]);
    fflush(out);
    for (r = 0; r < PROBE_ROUNDS && s.failed == VK_SUCCESS; r++)
        for (i = 0; i < nk && s.failed == VK_SUCCESS; i++) rates[i][r] = probe_rate(&s, ks[i]);
    if (s.failed != VK_SUCCESS) {
        fprintf(out, "pipelines_error=vkCreateComputePipelines: %s\n", snx_vk_result_str(s.failed));
        return;
    }
    fputs("pipelines=", out);
    for (i = 0; i < nk; i++) {
        qsort(rates[i], PROBE_ROUNDS, sizeof rates[i][0], probe_cmp);
        fprintf(out, "%s%d:%.0f", i ? "," : "", ks[i], rates[i][PROBE_ROUNDS / 2]);
    }
    fputc('\n', out);
}

int vk_probe(FILE* out) {
    SnxVkInstanceInfo ii = {.app_name = "ScarletNX", .portability = true};
    SnxVkInstance inst = {0};
    SnxVkExtensions exts = {0};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mem = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = probe_spv_size, .pCode = probe_spv};
    VkDescriptorSetLayout dlayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPhysicalDevice phys;
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures f;
    const char* step = "volkInitialize";
    VkResult r;
    void* own;
    int heap = 0, has_budget;
    u32 family, h;
    u64 heap_bytes, budget_bytes;

    fputs("probe: gpu\n", out);
    fflush(out);
    probe_env("MESA_SHADER_CACHE_DISABLE", "true");
    probe_env("__GL_SHADER_DISK_CACHE", "0");
    own = backend_vk_driver();
    if (own)
        volkInitializeCustom((PFN_vkGetInstanceProcAddr)own);
    else if ((r = volkInitialize()) != VK_SUCCESS)
        goto failed;
    step = "vkCreateInstance";
    if ((r = snx_vk_instance_create(&ii, &inst)) != VK_SUCCESS) goto failed;
    step = "vkEnumeratePhysicalDevices";
    r = VK_ERROR_INCOMPATIBLE_DRIVER;
    if (!(phys = snx_vk_best_device(inst.instance, VK_API_VERSION_1_3, probe_score, NULL))) goto failed;
    vkGetPhysicalDeviceProperties2(phys, &props);
    vkGetPhysicalDeviceFeatures(phys, &f);
    has_budget =
        snx_vk_device_extensions(phys, &exts) && snx_vk_extensions_has(&exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    snx_vk_extensions_free(&exts);
    if (has_budget) mem.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(phys, &mem);
    /* the biggest device local heap, as memory.c's heap_find picks it */
    for (h = 0; h < mem.memoryProperties.memoryHeapCount; h++)
        if ((mem.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (!(mem.memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
             mem.memoryProperties.memoryHeaps[h].size > mem.memoryProperties.memoryHeaps[heap].size))
            heap = (int)h;
    heap_bytes = mem.memoryProperties.memoryHeaps[heap].size;
    /* where the driver gives no budget, VMA's own guess: 80% of the heap */
    budget_bytes = has_budget ? budget.heapBudget[heap] : heap_bytes / 10 * 8;
    fprintf(out, "gpu=%s\ngpu_driver=%s %s\ngpu_type=%s\ngpu_bc=%d\ngpu_astc=%d\n", props.properties.deviceName,
            p12.driverName, p12.driverInfo,
            props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? "discrete"
            : props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                                    : "other",
            f.textureCompressionBC ? 1 : 0, f.textureCompressionASTC_LDR ? 1 : 0);
    fprintf(out, "gpu_heap_mb=%llu\ngpu_budget_mb=%llu\ngpu_budget_from=%s\n", (unsigned long long)(heap_bytes >> 20),
            (unsigned long long)(budget_bytes >> 20), has_budget ? "driver" : "heap");
    fflush(out);

    step = "vkCreateDevice";
    family = snx_vk_queue_family(phys, VK_QUEUE_COMPUTE_BIT, VK_NULL_HANDLE, 0);
    if (family == UINT32_MAX || (r = snx_vk_device_create(phys, family, &features, NULL, 0, &dev)) != VK_SUCCESS) {
        if (family == UINT32_MAX) r = VK_ERROR_FEATURE_NOT_PRESENT;
        fprintf(out, "probe: pipelines\npipelines_error=%s: %s\n", step, snx_vk_result_str(r));
        goto out;
    }
    volkLoadDevice(dev);
    {
        VkPipelineLayoutCreateInfo pli = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dlayout};
        if ((r = vkCreateDescriptorSetLayout(dev, &dli, NULL, &dlayout)) != VK_SUCCESS)
            step = "vkCreateDescriptorSetLayout";
        else if ((r = vkCreatePipelineLayout(dev, &pli, NULL, &layout)) != VK_SUCCESS)
            step = "vkCreatePipelineLayout";
        else if ((r = vkCreateShaderModule(dev, &mi, NULL, &module)) != VK_SUCCESS)
            step = "vkCreateShaderModule";
        if (r == VK_SUCCESS)
            probe_pipelines(out, dev, module, layout);
        else
            fprintf(out, "probe: pipelines\npipelines_error=%s: %s\n", step, snx_vk_result_str(r));
    }
    if (module) vkDestroyShaderModule(dev, module, NULL);
    if (layout) vkDestroyPipelineLayout(dev, layout, NULL);
    if (dlayout) vkDestroyDescriptorSetLayout(dev, dlayout, NULL);
    vkDestroyDevice(dev, NULL);
out:
    snx_vk_instance_free(&inst);
    fflush(out);
    return 0;
failed:
    fprintf(out, "gpu_error=no Vulkan device answered (%s: %s)\n", step, snx_vk_result_str(r));
    if (inst.instance) snx_vk_instance_free(&inst);
    fflush(out);
    return 1;
}
