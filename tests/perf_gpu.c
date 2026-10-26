/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU half of the performance regression test: the block linear deswizzle and swizzle shader (the one every
   texture upload and render target write-back runs) against a reference compute shader that copies the same 16 byte
   runs in the same workgroup shape, with the same dispatch grid. Both move 8 MiB a dispatch. The ratio of their
   times is limited by tests/perf_baseline.txt, so the host's absolute speed, its clock and a driver that runs both
   shaders slower cancel out, while a worse address calculation, bounds check or memory pattern in the block linear
   shader shows up.

   It times only this one shader. ASTC, texture conversion, VIC, NVDEC, the 3D pipeline, command recording, barriers,
   staging, present pacing and real frames are outside it; one of those needs its own test or a game run. A change
   that slows both this shader and the reference copy equally cancels out of the ratio.

   One compute queue, two 8 MiB buffers and a few small dispatches a sample: no window, no swapchain, no game data.
   Timestamps
   on the device measure the dispatch itself, so a busy host does not inflate it; without them the wall clock around
   a submission is the measurement, and the submission overhead is in both sides of the ratio. A local run skips
   when the machine has no Vulkan device or too little free memory to trust a timing; CI runs it as a failure in that
   case instead, so the CI image cannot silently stop exercising it. */
#include "support/perf.h"
#include "support/perf_copy_spv.h"
#include "nx/gpu/texture/blocklinear_spv.h"
#include "volk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#define SAMPLE_BYTES    (8u << 20)
#define RUN_BYTES       16u
#define RUNS            1024u
#define ROWS            (SAMPLE_BYTES / (RUNS * RUN_BYTES))
#define ROW_BYTES       (RUNS * RUN_BYTES)
#define BLOCK_ROW_BYTES ((((ROW_BYTES + 63u) >> 6) << 9))
#define BLOCK_ROW       (BLOCK_ROW_BYTES / RUN_BYTES)
#define SLAB            (((ROWS + 7u) >> 3) * BLOCK_ROW_BYTES / RUN_BYTES)
#define GRID_X          ((RUNS + 7u) >> 3)
#define GRID_Y          ((ROWS + 7u) >> 3)
#define ROUNDS          201 /* the first pair is dropped: the GPU's clock settles after the warm-up */
#define CANARY_ROUNDS   2   /* a couple of pairs first: only a massive regression stops before the full block */

typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkQueryPool queries;
    int timestamps;
    VkBuffer src, dst;
    VkDeviceMemory src_memory, dst_memory;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet set;
    VkPipeline block_pipeline;
    VkPipeline copy_pipeline;
    VkPhysicalDeviceProperties props;
    uint32_t run_row; /* the reference shader's uvec4s in a row of workgroups */
    uint32_t reps;    /* dispatches in one sample: enough of them to cover a few milliseconds */
} Gpu;

static Gpu g;

/* volk looks for the loader under its soname, which a Homebrew MoltenVK is not on by default. SNX_VULKAN_LIBRARY
   names one to load by path, and the two prefixes Homebrew uses are tried after it */
static int load_vulkan(void) {
#if defined(_WIN32)
    return 0;
#else
    static void* library;
    const char* explicit = getenv("SNX_VULKAN_LIBRARY");
    const char* paths[6];
    unsigned i, count = 0;
    if (explicit && *explicit) paths[count++] = explicit;
#if defined(__APPLE__)
    paths[count++] = "/opt/homebrew/lib/libvulkan.1.dylib";
    paths[count++] = "/usr/local/lib/libvulkan.1.dylib";
    paths[count++] = "/opt/homebrew/lib/libvulkan.dylib";
    paths[count++] = "/usr/local/lib/libvulkan.dylib";
#endif
    for (i = 0; i < count; ++i) {
        PFN_vkGetInstanceProcAddr get;
        void* symbol;
        library = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (!library) continue;
        symbol = dlsym(library, "vkGetInstanceProcAddr");
        if (!symbol) continue;
        memcpy(&get, &symbol, sizeof get);
        volkInitializeCustom(get);
        if (vkGetInstanceProcAddr) return 1;
    }
    return 0;
#endif
}

static int vk_ok(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return 1;
    fprintf(stderr, "perf_gpu: %s failed: %d\n", what, (int)result);
    return 0;
}

/* What the test picked, for the line it prints: an integrated GPU and an APU are hardware, and only their smaller
   memory system separates them from a discrete card here */
static const char* device_kind(int software) {
    if (software) return "software device";
    switch (g.props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        default: return "hardware device";
    }
}

/* An instance with the portability enumeration a MoltenVK build needs, and every device the loader has */
static int make_instance(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char* extensions[1];
    uint32_t count = 0, i;
    int portability = 0;
    VkExtensionProperties* available;
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "ScarletNX performance test";
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || !count) return 0;
    available = (VkExtensionProperties*)calloc(count, sizeof *available);
    if (!available) return 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, available) == VK_SUCCESS)
        for (i = 0; i < count; ++i)
            if (!strcmp(available[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) portability = 1;
    free(available);
    if (portability) {
        extensions[0] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
        info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    info.pApplicationInfo = &app;
    if (!vk_ok(vkCreateInstance(&info, NULL, &g.instance), "vkCreateInstance")) return 0;
    volkLoadInstance(g.instance);
    return 1;
}

/* A discrete GPU before an integrated one, a real one before a virtual or software device, and the first with a
   graphics and compute queue among equals. This is the runtime's own order (vk/device.c's device_score): with two
   discrete cards the test and a game start from the same device, and the ratio of two shaders measured on either
   answers the same question, so a machine with several GPUs runs the test instead of skipping it. An integrated GPU
   or APU needs nothing special here: the test uses one compute queue and two 8 MiB buffers that are filled and
   written on the GPU, never mapped, so unified memory is fine. A machine with only lavapipe still runs (and is
   reported as software) */
static int choose_device(void) {
    VkPhysicalDevice devices[16];
    VkPhysicalDeviceProperties properties;
    uint32_t count = 16, i, families = 0;
    int best = -1, best_score = -1;
    if (vkEnumeratePhysicalDevices(g.instance, &count, devices) != VK_SUCCESS || !count) return 0;
    for (i = 0; i < count; ++i) {
        int score;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;
        switch (properties.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 4; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 3; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 2; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 1; break;
            default: score = 0; break;
        }
        if (score <= best_score) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        if (!families) continue;
        {
            VkQueueFamilyProperties* queues = (VkQueueFamilyProperties*)calloc(families, sizeof *queues);
            uint32_t f;
            if (!queues) return 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, queues);
            for (f = 0; f < families; ++f)
                if (queues[f].queueCount && (queues[f].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                                                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                    best = (int)i;
                    best_score = score;
                    break;
                }
            free(queues);
        }
    }
    if (best < 0) return 0;
    g.phys = devices[best];
    vkGetPhysicalDeviceProperties(g.phys, &g.props);
    return 1;
}

static int make_device(void) {
    uint32_t count = 0, i;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    const float priority = 1.0f;
    VkQueueFamilyProperties* queues;
    int family = -1;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &count, NULL);
    if (!count) return 0;
    queues = (VkQueueFamilyProperties*)calloc(count, sizeof *queues);
    if (!queues) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &count, queues);
    /* The family the runtime's block linear work runs on: graphics and compute, as a game's device has */
    for (i = 0; i < count; ++i)
        if (queues[i].queueCount && (queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                                        (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            family = (int)i;
            g.timestamps = queues[i].timestampValidBits != 0;
            break;
        }
    free(queues);
    if (family < 0) return 0;
    g.timestamps = g.timestamps && g.props.limits.timestampPeriod > 0;
    queue.queueFamilyIndex = (uint32_t)family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    if (!vk_ok(vkCreateDevice(g.phys, &info, NULL, &g.device), "vkCreateDevice")) return 0;
    volkLoadDevice(g.device);
    vkGetDeviceQueue(g.device, (uint32_t)family, 0, &g.queue);
    return 1;
}

static int make_buffer(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory) {
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkPhysicalDeviceMemoryProperties memory_properties;
    uint32_t i, type = UINT32_MAX;
    info.size = size;
    info.usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk_ok(vkCreateBuffer(g.device, &info, NULL, buffer), "vkCreateBuffer")) return 0;
    vkGetBufferMemoryRequirements(g.device, *buffer, &requirements);
    vkGetPhysicalDeviceMemoryProperties(g.phys, &memory_properties);
    for (i = 0; i < memory_properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX)
        for (i = 0; i < memory_properties.memoryTypeCount; ++i)
            if (requirements.memoryTypeBits & (1u << i)) {
                type = i;
                break;
            }
    if (type == UINT32_MAX) {
        fprintf(stderr, "perf_gpu: no memory type for an 8 MiB buffer\n");
        return 0;
    }
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (!vk_ok(vkAllocateMemory(g.device, &allocate, NULL, memory), "vkAllocateMemory")) return 0;
    if (!vk_ok(vkBindBufferMemory(g.device, *buffer, *memory, 0), "vkBindBufferMemory")) return 0;
    return 1;
}

static int make_buffers(void) {
    if (!make_buffer(SAMPLE_BYTES, &g.src, &g.src_memory)) return 0;
    if (!make_buffer(SAMPLE_BYTES, &g.dst, &g.dst_memory)) return 0;
    return 1;
}

static int make_pipeline(VkShaderModule module, VkPipeline* pipeline) {
    VkComputePipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = g.layout;
    return vk_ok(vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &info, NULL, pipeline),
                 "vkCreateComputePipelines");
}

static VkShaderModule make_module(const uint32_t* code, uint64_t size) {
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule module = VK_NULL_HANDLE;
    info.codeSize = (size_t)size;
    info.pCode = code;
    if (!vk_ok(vkCreateShaderModule(g.device, &info, NULL, &module), "vkCreateShaderModule")) return VK_NULL_HANDLE;
    return module;
}

static int make_pipelines(void) {
    VkDescriptorSetLayoutBinding bindings[2] = {{.binding = 0,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
                                                {.binding = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 16 * sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorPoolSize pool_size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkDescriptorBufferInfo buffers[2] = {{.buffer = g.src, .offset = 0, .range = VK_WHOLE_SIZE},
                                         {.buffer = g.dst, .offset = 0, .range = VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2] = {{0}, {0}};
    VkShaderModule block, copy;
    unsigned i;
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    if (!vk_ok(vkCreateDescriptorSetLayout(g.device, &set_info, NULL, &g.set_layout), "vkCreateDescriptorSetLayout"))
        return 0;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &g.set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (!vk_ok(vkCreatePipelineLayout(g.device, &layout_info, NULL, &g.layout), "vkCreatePipelineLayout")) return 0;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (!vk_ok(vkCreateDescriptorPool(g.device, &pool_info, NULL, &g.descriptor_pool), "vkCreateDescriptorPool"))
        return 0;
    allocate.descriptorPool = g.descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &g.set_layout;
    if (!vk_ok(vkAllocateDescriptorSets(g.device, &allocate, &g.set), "vkAllocateDescriptorSets")) return 0;
    for (i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(g.device, 2, writes, 0, NULL);
    block = make_module(blocklinear_spv, blocklinear_spv_size);
    copy = make_module(perf_copy_spv, perf_copy_spv_size);
    if (block == VK_NULL_HANDLE || copy == VK_NULL_HANDLE) return 0;
    if (!make_pipeline(block, &g.block_pipeline)) return 0;
    if (!make_pipeline(copy, &g.copy_pipeline)) return 0;
    vkDestroyShaderModule(g.device, block, NULL);
    vkDestroyShaderModule(g.device, copy, NULL);
    return 1;
}

static int make_commands(void) {
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!vk_ok(vkCreateCommandPool(g.device, &pool, NULL, &g.pool), "vkCreateCommandPool")) return 0;
    allocate.commandPool = g.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (!vk_ok(vkAllocateCommandBuffers(g.device, &allocate, &g.cb), "vkAllocateCommandBuffers")) return 0;
    if (!vk_ok(vkCreateFence(g.device, &fence, NULL, &g.fence), "vkCreateFence")) return 0;
    if (g.timestamps) {
        VkQueryPoolCreateInfo queries = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queries.queryCount = 2;
        if (!vk_ok(vkCreateQueryPool(g.device, &queries, NULL, &g.queries), "vkCreateQueryPool")) return 0;
    }
    return 1;
}

static int submit(void) {
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkResult result;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &g.cb;
    result = vkQueueSubmit(g.queue, 1, &info, g.fence);
    if (!vk_ok(result, "vkQueueSubmit")) return 0;
    if (!vk_ok(vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) return 0;
    return vk_ok(vkResetFences(g.device, 1, &g.fence), "vkResetFences");
}

/* The one submission that puts a known pattern in the source buffer */
static int fill_source(void) {
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!vk_ok(vkResetCommandBuffer(g.cb, 0), "vkResetCommandBuffer")) return 0;
    if (!vk_ok(vkBeginCommandBuffer(g.cb, &begin), "vkBeginCommandBuffer")) return 0;
    vkCmdFillBuffer(g.cb, g.src, 0, SAMPLE_BYTES, 0x9e3779b9u);
    if (!vk_ok(vkEndCommandBuffer(g.cb), "vkEndCommandBuffer")) return 0;
    return submit();
}

static int begin_sample(void) {
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!vk_ok(vkResetCommandBuffer(g.cb, 0), "vkResetCommandBuffer")) return 0;
    if (!vk_ok(vkBeginCommandBuffer(g.cb, &begin), "vkBeginCommandBuffer")) return 0;
    if (g.timestamps) {
        vkCmdResetQueryPool(g.cb, g.queries, 0, 2);
        vkCmdWriteTimestamp(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.queries, 0);
    }
    return 1;
}

static int end_sample(double* ns) {
    uint64_t start = 0, end = 0;
    if (g.timestamps) vkCmdWriteTimestamp(g.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, 1);
    if (!vk_ok(vkEndCommandBuffer(g.cb), "vkEndCommandBuffer")) return 0;
    start = perf_now_ns();
    if (!submit()) return 0;
    end = perf_now_ns();
    if (g.timestamps) {
        uint64_t stamps[2];
        if (!vk_ok(vkGetQueryPoolResults(g.device, g.queries, 0, 2, sizeof stamps, stamps, sizeof stamps[0],
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                   "vkGetQueryPoolResults"))
            return 0;
        *ns = (double)(stamps[1] - stamps[0]) * (double)g.props.limits.timestampPeriod;
    } else {
        *ns = (double)(end - start);
    }
    return 1;
}

static int sample_block(double* ns) {
    uint32_t params[16];
    uint32_t rep;
    memset(params, 0, sizeof params);
    params[0] = RUNS;
    params[1] = ROWS;
    params[2] = RUNS; /* the linear rows' pitch, in runs */
    params[6] = 0;    /* bh */
    params[7] = 0;    /* hmask */
    params[8] = 0;    /* bd */
    params[9] = 0;    /* dmask */
    params[10] = 5;   /* bsh */
    params[11] = BLOCK_ROW;
    params[12] = SLAB;
    params[15] = 0; /* deswizzle */
    if (!begin_sample()) return 0;
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.block_pipeline);
    vkCmdBindDescriptorSets(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &g.set, 0, NULL);
    vkCmdPushConstants(g.cb, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, params);
    for (rep = 0; rep < g.reps; ++rep) {
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
        if (rep)
            vkCmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &barrier, 0, NULL, 0, NULL);
        vkCmdDispatch(g.cb, GRID_X, GRID_Y, 1);
    }
    return end_sample(ns);
}

static int sample_copy(double* ns) {
    uint32_t params[4] = {g.run_row, 0, 0, 0};
    uint32_t rep;
    if (!begin_sample()) return 0;
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.copy_pipeline);
    vkCmdBindDescriptorSets(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &g.set, 0, NULL);
    vkCmdPushConstants(g.cb, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, params);
    for (rep = 0; rep < g.reps; ++rep) {
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
        if (rep)
            vkCmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &barrier, 0, NULL, 0, NULL);
        vkCmdDispatch(g.cb, GRID_X, GRID_Y, 1);
    }
    return end_sample(ns);
}

/* ROUNDS samples of each shader, with the first pair dropped: the GPU's clock has just been idle, and a sample
   taken as it starts is not the shader's cost. The pairs alternate which shader goes first */
static int measure_gpu_block(double* ratios, size_t rounds) {
    size_t i;
    for (i = 0; i <= rounds; ++i) {
        double block = 0, copy = 0;
        if (i & 1) {
            if (!sample_copy(&copy) || !sample_block(&block)) return 0;
        } else {
            if (!sample_block(&block) || !sample_copy(&copy)) return 0;
        }
        if (i) {
            ratios[i - 1] = block / copy;
            if (getenv("SNX_PERF_VERBOSE"))
                printf("perf_gpu: round %llu: block linear %.4f ms, reference %.4f ms, ratio %.2f\n",
                       (unsigned long long)(i - 1), block / 1e6, copy / 1e6, ratios[i - 1]);
        }
    }
    return 1;
}

int main(int argc, char** argv) {
    PerfEnvironment env;
    PerfDecision decision;
    double ratios[ROUNDS], retest_ratios[ROUNDS];
    double reference, warn_percent, fail_percent, warn, fail, ceiling, massive;
    PerfResult first, second;
    const char* baseline;
    char note[256];
    int software;
    const char* key;

    if (argc != 2) {
        fprintf(stderr, "usage: %s BASELINE\n", argv[0]);
        return 2;
    }
    env = perf_environment();
    printf("perf_gpu: %d processors, load %.2f, %llu MiB available, %s\n", env.processors, env.load1,
           (unsigned long long)env.memory_mb, env.strict ? "CI mode (a skip fails)" : "local mode (may skip)");
    decision = perf_decide(&env, "perf_gpu", perf_resource_refusal(&env));
    if (decision != PERF_RUN) return decision == PERF_SKIP ? 77 : 1;
    perf_prepare_thread();
    if (volkInitialize() != VK_SUCCESS && !load_vulkan()) {
        decision = perf_decide(&env, "perf_gpu", "no Vulkan loader (volkInitialize failed)");
        return decision == PERF_SKIP ? 77 : 1;
    }
    if (!make_instance()) {
        decision = perf_decide(&env, "perf_gpu", "the Vulkan loader has no instance");
        return decision == PERF_SKIP ? 77 : 1;
    }
    if (!choose_device()) {
        decision = perf_decide(&env, "perf_gpu", "no Vulkan 1.3 device with a graphics and compute queue");
        return decision == PERF_SKIP ? 77 : 1;
    }
    if (!make_device()) {
        decision = perf_decide(&env, "perf_gpu", "the Vulkan device has no graphics and compute queue");
        return decision == PERF_SKIP ? 77 : 1;
    }
    software = g.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
               g.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU || strstr(g.props.deviceName, "lavapipe") ||
               strstr(g.props.deviceName, "llvmpipe") || strstr(g.props.deviceName, "SwiftShader") ||
               strstr(g.props.deviceName, "software") || strstr(g.props.deviceName, "Software");
    key = software ? "gpu_software" : "gpu_hardware";
    baseline = getenv("SNX_PERF_BASELINE");
    if (!baseline || !*baseline) baseline = argv[1];
    {
        int found = perf_baseline4(baseline, key, &reference, &warn_percent, &fail_percent, &ceiling);
        if (found != 1) {
            fprintf(stderr, "perf_gpu: %s: %s\n", baseline,
                    found < 0 ? "cannot be read or has a line without four numbers" : "has no line for this device");
            return 1;
        }
        warn = reference * (1.0 + warn_percent / 100.0);
        fail = reference * (1.0 + fail_percent / 100.0);
        if (!(reference > 0.0 && warn_percent >= 0.0 && warn_percent < fail_percent && warn < fail && fail < ceiling)) {
            fprintf(stderr, "perf_gpu: %s must have 0 < reference and warn%% < fail%% with warn < fail < ceiling\n",
                    baseline);
            return 1;
        }
    }
    g.run_row = 32 * GRID_X;
    printf("perf_gpu: %s \"%s\", %s, %llu MiB a dispatch\n", device_kind(software), g.props.deviceName,
           g.timestamps ? "device timestamps" : "wall clock", (unsigned long long)(SAMPLE_BYTES >> 20));
    if (!make_buffers() || !make_pipelines() || !make_commands() || !fill_source()) return 1;

    /* One dispatch of each before a clock is read: the pipelines' first use and a cold cache are not the shader's
       cost. Each sample then holds enough dispatches to cover a few milliseconds, so a submission's fixed cost and a
       GPU that is changing its clock do not decide the ratio. Lavapipe's slower dispatches keep the count at one. */
    {
        double warm_block, warm_copy, fastest;
        g.reps = 1;
        if (!sample_block(&warm_block) || !sample_copy(&warm_copy)) return 1;
        fastest = warm_block < warm_copy ? warm_block : warm_copy;
        if (fastest > 0) {
            double reps = 5e6 / fastest;
            if (reps > 256.0) reps = 256.0;
            if (reps < 1.0) reps = 1.0;
            g.reps = (uint32_t)reps;
        }
        if (!g.reps) g.reps = 1;
        printf("perf_gpu: %u dispatch(es) a sample\n", g.reps);
        /* A second warm-up at the length the samples will have. The first timed round is dropped as well, so the
           clock's climb from idle is not part of the ratio */
        if (!sample_block(&warm_block) || !sample_copy(&warm_copy)) return 1;
    }
    if (!measure_gpu_block(ratios, CANARY_ROUNDS)) return 1;
    first = perf_result(ratios, CANARY_ROUNDS);
    massive = ceiling * 1.5;
    printf("perf_gpu: canary ratio %.2f over %d pairs (massive line %.2f)\n", first.ratio, CANARY_ROUNDS, massive);
    if (first.lower_strict > massive) {
        if (!measure_gpu_block(retest_ratios, CANARY_ROUNDS)) return 1;
        second = perf_result(retest_ratios, CANARY_ROUNDS);
        printf("perf_gpu: canary confirmation ratio %.2f (99.9%% lower %.2f)\n", second.ratio, second.lower_strict);
        if (second.lower_strict > massive) {
            snprintf(note, sizeof note,
                     "two canary blocks (%.2f and %.2f) are over the %.2f massive line at 99.9 percent confidence; "
                     "the full block was not run",
                     first.ratio, second.ratio, massive);
            perf_annotation(PERF_ERROR, "perf_gpu", note);
            return 1;
        }
    }
    if (!measure_gpu_block(ratios, ROUNDS)) return 1;
    first = perf_result(ratios, ROUNDS);
    {
        const char* result = getenv("SNX_PERF_RESULT");
        if (result && *result) {
            FILE* file = fopen(result, "w");
            if (file) {
                fprintf(file, "%s %.6f %.6f %.6f %llu\n", key, first.ratio, first.lower, first.upper,
                        (unsigned long long)ROUNDS);
                fclose(file);
            }
        }
    }
    decision = perf_decide_noise(&env, "perf_gpu", first.spread);
    if (decision != PERF_RUN) return decision == PERF_SKIP ? 77 : 1;
    printf("perf_gpu: ratio %.2f (95%% lower %.2f, 99.9%% lower %.2f, reference %.2f, warn %.2f, fail %.2f, "
           "ceiling %.2f, spread %.0f%%)\n",
           first.ratio, first.lower, first.lower_strict, reference, warn, fail, ceiling, first.spread * 100);
    if (first.lower_strict > fail || first.ratio > ceiling) {
        if (!measure_gpu_block(retest_ratios, ROUNDS)) return 1;
        second = perf_result(retest_ratios, ROUNDS);
        printf("perf_gpu: confirmation ratio %.2f (99.9%% lower %.2f, spread %.0f%%)\n", second.ratio,
               second.lower_strict, second.spread * 100);
        if (second.lower_strict > fail) {
            snprintf(note, sizeof note,
                     "two independent blocks (%.2f and %.2f) are over the %.2f fail line at 99.9 percent confidence",
                     first.ratio, second.ratio, fail);
            perf_annotation(PERF_ERROR, "perf_gpu", note);
            return 1;
        }
        snprintf(note, sizeof note,
                 "first block %.2f crossed the %.2f fail line and the confirmation at %.2f did not; likely variance",
                 first.ratio, fail, second.ratio);
        perf_annotation(PERF_WARN, "perf_gpu", note);
        return 0;
    }
    if (first.lower > warn) {
        if (!measure_gpu_block(retest_ratios, ROUNDS)) return 1;
        second = perf_result(retest_ratios, ROUNDS);
        printf("perf_gpu: warning confirmation ratio %.2f (95%% lower %.2f)\n", second.ratio, second.lower);
        if (second.lower > warn)
            snprintf(note, sizeof note,
                     "ratio %.2f is over the %.2f warning line and a second block confirms it; this is advisory and "
                     "may still be variance",
                     first.ratio, warn);
        else
            snprintf(note, sizeof note,
                     "ratio %.2f is over the %.2f warning line but a second block does not confirm it; likely "
                     "variance",
                     first.ratio, warn);
        perf_annotation(PERF_WARN, "perf_gpu", note);
    }

    return 0;
}
