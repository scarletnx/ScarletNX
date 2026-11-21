/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Actual renderer draws alternate heap and legacy descriptor state in one command buffer. The color oracle is
   independent of descriptor encoding. The forced legacy draw exercises backend selection, not a simulated driver
   error; unsupported create-state rejection is covered separately by the registry's mock lifecycle test. */
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/internal_spv.h"   /* snx_null_fs_spv */
#include "nx/gpu/vk/pipeline_stats.h" /* vk_pipeline_stats_flags */
#include "nx/gpu/vk/shading_rate.h"   /* vk_vrs_pipeline_flags */
#include "common/platform/thread.h"   /* plat_tid */

typedef struct {
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorSet set;
    VkBuffer output;
    u64 offset;
} DescriptorHeapAux;
static int descriptor_heap_aux_create(DescriptorHeapAux* aux) {
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    CHECK(VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &descriptors, NULL, &aux->descriptors)));
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &aux->descriptors};
    CHECK(VK_OK(vkCreatePipelineLayout(g_vk.dev, &layout, NULL, &aux->layout)));
    size_t words;
    u32* code = snx_test_spirv_compute(&words);
    CHECK(code);
    VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = words * sizeof *code, .pCode = code};
    VkShaderModule module;
    CHECK(VK_OK(vkCreateShaderModule(g_vk.dev, &module_info, NULL, &module)));
    free(code);
    VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = aux->layout};
    CHECK(VK_OK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pipeline, NULL, &aux->pipeline)));
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    CHECK(vk_stage_output(4, g_vk.props.limits.minStorageBufferOffsetAlignment, &aux->output, &aux->offset, NULL));
    aux->set = vk_desc_alloc(aux->descriptors);
    CHECK(aux->set);
    VkDescriptorBufferInfo buffer = {aux->output, aux->offset, 4};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = aux->set,
        .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer};
    vkUpdateDescriptorSets(g_vk.dev, 1, &write, 0, NULL);
    return 0;
}
/* The submissions since before[] by reason (VK_SUB_*), as " other 0 present 0 ..." */
static void descriptor_heap_submits_said(const u64* before, char* said, size_t size) {
    static const char* const why[VK_SUB_N] = {"other",    "present", "finish", "1 ms",
                                              "readback", "frame",   "split",  "background"};
    size_t n = 0;
    said[0] = 0;
    for (int i = 0; i < VK_SUB_N && n < size; i++) {
        int wrote = snprintf(said + n, size - n, " %s %llu", why[i] ? why[i] : "?",
                             (unsigned long long)(vk_submits(i) - before[i]));
        if (wrote < 0) break;
        n += (size_t)wrote;
    }
}
static int check_descriptor_heaps(void) {
    if (!g_vk.has_descriptor_heaps) {
        puts("SKIP descriptor heap rendering: selected device backend is not descriptor heaps");
        return 0;
    }
    if (!g_vk.has_border_color || g_vk.border_color_max < 2) {
        puts("SKIP descriptor heap rendering: two custom border colors are unavailable");
        return 0;
    }
    const u64 gpu = 0x850000000ull, cpu = HEAP_BASE + 0x10b00000;
    const u64 pool = gpu, samplers = gpu + 0x1000, source = gpu + 0x4000, target = gpu + 0x8000;
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    u8 texels[512] = {0};
    const u8 source_color[4] = {51, 102, 153, 204};
    for (u32 y = 0; y < 2; y++)
        for (u32 x = 0; x < 2; x++) memcpy(texels + gob_x_offset(4 * x) + gob_y_offset(y), source_color, 4);
    CHECK(!gmem_write(source, texels, sizeof texels));
    NvnTexture image = {.gpu = source};
    image.d.target = NVN_TEXTURE_TARGET_2D;
    image.d.format = NVN_FORMAT_RGBA8;
    image.d.width = image.d.height = 2;
    image.d.depth = image.d.levels = 1;
    NvnView swizzled = {.set = NVN_VIEW_SWIZZLE, .swizzle = {4, 3, 2, 5}};
    u32 tic[8], tsc[8];
    CHECK(nvn_tic_encode(&image, NULL, 0, 0, tic));
    CHECK(!storage_write_words(pool, tic, 8));
    CHECK(nvn_tic_encode(&image, &swizzled, 0, 0, tic));
    CHECK(!storage_write_words(pool + 32, tic, 8));
    NvnSamplerPar sampler = {.wrap_s = NVN_WRAP_MODE_CLAMP_TO_BORDER,
        .wrap_t = NVN_WRAP_MODE_CLAMP_TO_BORDER, .wrap_r = NVN_WRAP_MODE_CLAMP_TO_BORDER,
        .anisotropy = 1, .lod_max = 1, .border = {1, .25f, .5f, 1}};
    for (u32 i = 0; i < 3; i++) {
        if (i == 1) sampler.mag_filter = NVN_MAG_FILTER_LINEAR;
        if (i == 2) {
            const float border[4] = {.125f, 1, .75f, .5f};
            memcpy(sampler.border, border, sizeof border);
        }
        nvn_tsc_encode(&sampler, tsc);
        CHECK(!storage_write_words(samplers + 32 * i, tsc, 8));
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 143;
    vk_lock();
    rt_clear(channel, target, 7, 4, 0, 0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.tex_pool = pool;
    state->bind.smp_pool = samplers;
    state->bind.tex_max = 1;
    state->bind.smp_max = 2;
    state->bind.smp_mode = 0;
    state->blend = (DsBlend){0};
    state->blend.mask[0] = 15;
    state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
    state->rkey.sample_mask = ~0u;
    state->dyn.alpha_test = 0;
    DescriptorHeapAux aux = {0};
    u32 begun = 0;
    u64 before[VK_SUB_N] = {0}; /* the submissions by reason when column 0's command buffer was recording */
    /* The renderer submits on its own clock and counts: after 1 ms of recording, or when a frame slot's command
       buffers, rings or descriptors run out (VK_SUB_PERIODIC, VK_SUB_FRAME), which a slow or loaded runner reaches
       inside the transition now and then. A pass a submit split runs again; one the transition makes itself splits
       every pass, and fails at this many in a row */
    const u32 split_most = 8;
    u32 split = 0;
    for (u32 warm = 1;; warm = 0) {
        /* First use can upload a new view or allocate a legacy descriptor arena and submit. Warm every variant
           before checking state invalidation, then erase its pixels so it cannot hide a bad measured draw. */
        if (!warm) {
            vk_render_end();
            vk_finish();
            DrawClear clear = {.color_mask = 15, .color = {0, 0, 0x3f800000, 0x3f800000}};
            vk_render_clear(state, &clear, &channel->owner);
            if (!aux.pipeline) CHECK(!descriptor_heap_aux_create(&aux));
        }
        u64 drawn = g_vk_drawn;
        int split_here = 0;
        for (u32 column = 0; column < 7 && !split_here; column++) {
            if (!warm && column == 1) {
                vk_render_end();
                VkCommandBuffer cb = vk_cmd();
                split_here = vk_cb_begun() != begun;
                if (split_here) break;
                rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, aux.pipeline);
                rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, aux.layout, 0, 1, &aux.set, 0, NULL);
                rec_CmdDispatch(cb, 1, 1, 1);
            }
            g_vk.has_descriptor_heaps = column != 2;
            DrawTexture rectangle = {.x0 = (float)column, .w = 1, .h = 4, .sx0 = -2, .sy0 = -2};
            rectangle.state.multisample_flags = DS_MS_A2C_DITHER;
            if (column == 4) rectangle.handle = 1u << 20; /* same color, different native sampler state */
            if (column == 5) rectangle.handle = 2u << 20;
            if (column == 6) rectangle.handle = 1, rectangle.sx0 = rectangle.sy0 = .5f;
            vk_render_draw_texture(state, &rectangle);
            CHECK(g_vk_drawn == drawn + column + 1);
            if (!warm) {
                if (!column) {
                    begun = vk_cb_begun();
                    for (int i = 0; i < VK_SUB_N; i++) before[i] = vk_submits(i);
                }
                /* Require the adjacent legacy-compute/heap transition itself to stay in one command buffer; later
                   independent color cases may submit. */
                if (column == 1) split_here = vk_cb_begun() != begun;
            }
        }
        if (!warm && !split_here) break;
        if (split_here) {
            char said[256];
            descriptor_heap_submits_said(before, said, sizeof said);
            split++;
            printf("descriptor heap transition: measured pass %u changed command buffer %u to %u; submits since, by "
                   "reason:%s\n",
                   split, begun, vk_cb_begun(), said);
            if (split == split_most)
                fprintf(stderr, "descriptor heap transition: %u measured passes in a row were split by a submit\n",
                        split);
            CHECK(split < split_most);
        }
    }
    g_vk.has_descriptor_heaps = 1;
    vk_render_end();
    VkBuffer readback;
    u8* value = vk_readback_buffer(4, &readback);
    CHECK(value);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    rec_CmdPipelineBarrier2(vk_cmd(), &dependency);
    VkBufferCopy copy = {aux.offset, 0, 4};
    rec_CmdCopyBuffer(vk_cmd(), aux.output, readback, 1, &copy);
    vk_finish();
    vk_readback_ready();
    CHECK(snx_ld_le32(value) == 42);
    Surf* surface = vk_rt_find(target);
    CHECK(surface);
    u8 actual[7 * 4 * 4];
    CHECK(!ms_read_pixels(surface, 7, 4, actual));
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 7; x++) {
            const u8 expected[3][4] = {{255, 64, 128, 255}, {32, 255, 191, 128}, {153, 102, 51, 204}};
            for (u32 component = 0; component < 4; component++) {
                int delta = (int)actual[(y * 7 + x) * 4 + component] - expected[x == 6 ? 2 : x == 5][component];
                if (delta < -1 || delta > 1)
                    fprintf(stderr, "descriptor heap column%u row%u component%u: %u expected %u\n", x, y, component,
                            actual[(y * 7 + x) * 4 + component], expected[x == 6 ? 2 : x == 5][component]);
                CHECK(delta >= -1 && delta <= 1);
            }
        }
    vkDestroyPipeline(g_vk.dev, aux.pipeline, NULL);
    vkDestroyPipelineLayout(g_vk.dev, aux.layout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, aux.descriptors, NULL);
    vk_render_forget_state();
    vk_unlock();
    free(channel);
    printf("PASS descriptor heap rendering: custom colors, view swizzle, same-command-buffer legacy compute and %s fallback exact=1\n",
           g_vk.has_descriptor_buffers ? "buffer" : "set");
    return 0;
}

/* A pipeline of descriptor heaps, its parts and its links, as the driver is asked for them. The driver reads the flags
   of the chained VkPipelineCreateFlags2CreateInfo in place of VkGraphicsPipelineCreateInfo.flags: each call has the
   heaps' bit, no layout and the flags a pipeline of sets has (statistics, shading rate), and the draw's ask for the
   optimized link from the pipeline cache (pipeline.c gpl_cached) keeps FAIL_ON_PIPELINE_COMPILE_REQUIRED, or the draw
   waits for the whole optimized compile that it asked only to find in the cache */
typedef struct {
    VkPipelineCreateFlags2 flags;
    VkGraphicsPipelineLibraryFlagsEXT part; /* the library's part, 0 for a link */
    u32 linked;                             /* the libraries it links */
    int flags2_first, layout, draw;         /* flags2 first in the chain, a layout, made on the draw's thread */
    VkResult result;
} HeapFlagsCall;
enum { HEAP_FLAGS_CALLS = 16 };
static struct {
    PFN_vkCreateGraphicsPipelines create;
    PMutex lock;
    u32 n, draw_tid;
    HeapFlagsCall calls[HEAP_FLAGS_CALLS];
} g_heap_flags = {.lock = PMUTEX_INIT};
static VKAPI_ATTR VkResult VKAPI_CALL heap_flags_create(VkDevice dev, VkPipelineCache cache, u32 n,
                                                        const VkGraphicsPipelineCreateInfo* info,
                                                        const VkAllocationCallbacks* alloc, VkPipeline* out) {
    VkResult r = g_heap_flags.create(dev, cache, n, info, alloc, out);
    pm_lock(&g_heap_flags.lock);
    for (u32 i = 0; i < n && g_heap_flags.n < HEAP_FLAGS_CALLS; i++) {
        HeapFlagsCall* c = &g_heap_flags.calls[g_heap_flags.n++];
        *c = (HeapFlagsCall){.flags = info[i].flags,
                             .layout = info[i].layout != VK_NULL_HANDLE,
                             .draw = plat_tid() == g_heap_flags.draw_tid,
                             .result = r};
        const VkBaseInStructure* first = info[i].pNext;
        for (const VkBaseInStructure* x = first; x; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO) {
                c->flags = ((const VkPipelineCreateFlags2CreateInfo*)(const void*)x)->flags;
                c->flags2_first = x == first;
            } else if (x->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)
                c->part = ((const VkGraphicsPipelineLibraryCreateInfoEXT*)(const void*)x)->flags;
            else if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)
                c->linked = ((const VkPipelineLibraryCreateInfoKHR*)(const void*)x)->libraryCount;
    }
    pm_unlock(&g_heap_flags.lock);
    return r;
}
static int check_heap_pipeline_flags(void) {
    if (!g_vk.has_descriptor_heaps || !g_vk.has_gpl) {
        printf("SKIP descriptor heap pipeline flags: %s\n", !g_vk.has_descriptor_heaps
                                                                ? "graphics descriptors are not heaps"
                                                                : "pipelines are not linked from parts");
        return 0;
    }
    /* pipeline.c's pipe_heaps gives flags2 the bits of VkPipelineCreateFlags, which keep their values there */
    CHECK((VkPipelineCreateFlags2)VK_PIPELINE_CREATE_LIBRARY_BIT_KHR == VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR &&
          (VkPipelineCreateFlags2)VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT ==
              VK_PIPELINE_CREATE_2_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT &&
          (VkPipelineCreateFlags2)VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT ==
              VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT &&
          (VkPipelineCreateFlags2)VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT ==
              VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT &&
          (VkPipelineCreateFlags2)VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT ==
              VK_PIPELINE_CREATE_2_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT &&
          (VkPipelineCreateFlags2)VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR ==
              VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR &&
          (VkPipelineCreateFlags2)VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR ==
              VK_PIPELINE_CREATE_2_CAPTURE_STATISTICS_BIT_KHR &&
          (VkPipelineCreateFlags2)VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT ==
              VK_PIPELINE_CREATE_2_DESCRIPTOR_BUFFER_BIT_EXT);
    size_t words = 0;
    u32* code = snx_test_spirv_graphics(0, &words);
    CHECK(code);
    VkShaderModule vs = vk_module_spv(code, words);
    VkShaderModule fs = vk_module_spv(snx_null_fs_spv, snx_null_fs_spv_size / sizeof *snx_null_fs_spv);
    free(code);
    CHECK(vs && fs);
    /* new modules: their pre-rasterization and fragment parts are new, and the pipeline links them */
    PipeKey k = {.vs = vs,
                 .fs = fs,
                 .topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                 .ncolor = 1,
                 .color = {VK_FORMAT_R8G8B8A8_UNORM},
                 .heaps = 1};
    k.blend[0].mask = 15;
    vk_pipeline_quiesce(); /* no worker makes a pipeline of its own meanwhile */
    g_heap_flags.n = 0;
    g_heap_flags.draw_tid = plat_tid();
    g_heap_flags.create = vkCreateGraphicsPipelines;
    vkCreateGraphicsPipelines = heap_flags_create;
    VkPipeline p = vk_pipeline_get(&k);
    vk_pipeline_quiesce(); /* the optimized link a worker makes after the draw's fast one */
    vkCreateGraphicsPipelines = g_heap_flags.create;
    CHECK(p && g_heap_flags.n && g_heap_flags.n < HEAP_FLAGS_CALLS);
    const VkPipelineCreateFlags2 common =
        (VkPipelineCreateFlags2)vk_vrs_pipeline_flags() | (VkPipelineCreateFlags2)vk_pipeline_stats_flags();
    const VkPipelineCreateFlags2 library =
        VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_2_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    u32 parts = 0, links = 0, asks = 0, missed = 0, fast = 0;
    for (u32 i = 0; i < g_heap_flags.n; i++) {
        const HeapFlagsCall* c = &g_heap_flags.calls[i];
        const int lto = (c->flags & VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT) != 0;
        const int ask = c->draw && lto; /* gpl_cached's: a draw makes no optimized link otherwise */
        const char* wrong = !c->flags2_first ? "its flags outside a first flags2"
                            : !(c->flags & VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT) ? "no heaps' bit"
                            : c->flags & VK_PIPELINE_CREATE_2_DESCRIPTOR_BUFFER_BIT_EXT  ? "the descriptor buffers' bit"
                            : c->layout                                                  ? "a layout"
                            : (c->flags & common) != common              ? "not every flag a pipeline of sets has"
                            : c->part && (c->flags & library) != library ? "no library flags"
                            : !c->part && !c->linked                     ? "no part and no libraries"
                            : ask && !(c->flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)
                                ? "no FAIL_ON_PIPELINE_COMPILE_REQUIRED on the draw's ask of the cache"
                                : NULL;
        if (wrong)
            fprintf(stderr,
                    "descriptor heap pipeline flags: call %u, %s on %s thread (result %d), has %s: flags 0x%llx, "
                    "those of a pipeline of sets 0x%llx\n",
                    i,
                    c->part ? "a part"
                    : lto   ? "an optimized link"
                            : "a fast link",
                    c->draw ? "the draw's" : "a worker's", (int)c->result, wrong, (unsigned long long)c->flags,
                    (unsigned long long)common);
        CHECK(!wrong);
        parts += c->part != 0;
        links += c->linked != 0;
        asks += ask;
        missed += ask && c->result == VK_PIPELINE_COMPILE_REQUIRED;
        fast += c->draw && c->linked && !lto;
    }
    /* an ask the cache could not answer leaves the draw its fast link */
    CHECK(parts && links && (!missed || fast));
    printf("PASS descriptor heap pipeline flags: %u parts and %u links with the heaps' bit, no layout and the flags of "
           "sets 0x%llx; %u asks of the cache from the draw, each with FAIL_ON_PIPELINE_COMPILE_REQUIRED (%u missed, "
           "%u fast links)\n",
           parts, links, (unsigned long long)common, asks, missed, fast);
    return 0;
}

/* A draw of descriptor heaps binds shader objects where the device has them (shader_objects.c): each object is made
   with the heaps' flag, no set layouts and no push constant range, and its stage's mapping of its set, as a
   pipeline's stage is (pipe_info). Such a draw went through a pipeline before, linked or compiled for each new state
   on a device that takes heaps (an integrated GPU's, vk_descriptors_auto). The objects of heaps and of sets of one
   module are two, as a run that falls back to sets (render.c) draws with both */
typedef struct {
    VkShaderCreateFlagsEXT flags;
    u32 sets, ranges, mappings;
    int mapped; /* a VkShaderDescriptorSetAndBindingMappingInfoEXT in its chain */
} HeapSoCall;
enum { HEAP_SO_CALLS = 8 };
static struct {
    PFN_vkCreateShadersEXT create;
    u32 n;
    HeapSoCall calls[HEAP_SO_CALLS];
} g_heap_so;
static VKAPI_ATTR VkResult VKAPI_CALL heap_so_create(VkDevice dev, uint32_t n, const VkShaderCreateInfoEXT* info,
                                                     const VkAllocationCallbacks* alloc, VkShaderEXT* out) {
    for (u32 i = 0; i < n && g_heap_so.n < HEAP_SO_CALLS; i++) {
        HeapSoCall* c = &g_heap_so.calls[g_heap_so.n++];
        *c = (HeapSoCall){
            .flags = info[i].flags, .sets = info[i].setLayoutCount, .ranges = info[i].pushConstantRangeCount};
        for (const VkBaseInStructure* x = info[i].pNext; x; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT) {
                c->mapped = 1;
                c->mappings = ((const VkShaderDescriptorSetAndBindingMappingInfoEXT*)(const void*)x)->mappingCount;
            }
    }
    return g_heap_so.create(dev, n, info, alloc, out);
}
static int check_heap_shader_objects(void) {
    if (!g_vk.has_descriptor_heaps || !g_vk.has_shader_objects) {
        printf("SKIP descriptor heap shader objects: %s\n",
               !g_vk.has_descriptor_heaps ? "graphics descriptors are not heaps" : "draws go through pipelines");
        return 0;
    }
    size_t words = 0;
    u32* code = snx_test_spirv_graphics(0, &words);
    CHECK(code);
    VkShaderModule vs = vk_module_spv(code, words);
    if (vs) vk_so_keep_spirv(vs, code, words);
    free(code);
    const size_t fs_words = snx_null_fs_spv_size / sizeof *snx_null_fs_spv;
    VkShaderModule fs = vk_module_spv(snx_null_fs_spv, fs_words);
    CHECK(vs && fs);
    vk_so_keep_spirv(fs, snx_null_fs_spv, fs_words);
    PipeKey k = {.vs = vs,
                 .fs = fs,
                 .topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                 .ncolor = 1,
                 .color = {VK_FORMAT_R8G8B8A8_UNORM},
                 .heaps = 1};
    k.blend[0].mask = 15;
    if (!vk_so_takes(&k, fs)) {
        fprintf(stderr, "descriptor heap shader objects: a draw of heaps goes through a pipeline\n");
        return 1;
    }
    vk_pipeline_quiesce(); /* no worker makes an object of its own meanwhile */
    g_heap_so.n = 0;
    g_heap_so.create = vkCreateShadersEXT;
    vkCreateShadersEXT = heap_so_create;
    VkShaderEXT heaps[5], sets[5];
    const int made = vk_so_make(&k, fs, heaps);
    k.heaps = 0;
    const int made_sets = vk_so_make(&k, fs, sets);
    vkCreateShadersEXT = g_heap_so.create;
    CHECK(made && made_sets && heaps[0] && heaps[4] && sets[0] && sets[4]);
    CHECK(heaps[0] != sets[0] && heaps[4] != sets[4]);
    u32 of_heaps = 0, of_sets = 0;
    for (u32 i = 0; i < g_heap_so.n; i++) {
        const HeapSoCall* c = &g_heap_so.calls[i];
        const int heap = (c->flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT) != 0;
        const char* wrong = heap && (c->sets || c->ranges) ? "set layouts or a push constant range"
                            : heap && !c->mapped           ? "no mapping of its set"
                            : !heap && c->mapped           ? "a mapping, without the heaps' flag"
                            : !heap && !c->sets            ? "no set layouts, without the heaps' flag"
                                                           : NULL;
        if (wrong)
            fprintf(stderr,
                    "descriptor heap shader objects: call %u (flags 0x%x, %u set layouts, %u push ranges, %u "
                    "mappings) has %s\n",
                    i, (unsigned)c->flags, c->sets, c->ranges, c->mappings, wrong);
        CHECK(!wrong);
        of_heaps += heap;
        of_sets += !heap;
    }
    CHECK(of_heaps >= 2 && of_sets >= 2);
    printf("PASS descriptor heap shader objects: %u objects with the heaps' flag and their set's mapping, %u of sets "
           "for the same modules\n",
           of_heaps, of_sets);
    return 0;
}
