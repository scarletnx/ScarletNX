/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Actual renderer draws alternate heap and legacy descriptor state in one command buffer. The color oracle is
   independent of descriptor encoding. The forced legacy draw exercises backend selection, not a simulated driver
   error; unsupported create-state rejection is covered separately by the registry's mock lifecycle test. */
#include "nx/gpu/vk/descriptor_heaps.h"

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
    for (u32 warm = 1; ; warm = 0) {
        /* First use can upload a new view or allocate a legacy descriptor arena and submit. Warm every variant
           before checking state invalidation, then erase its pixels so it cannot hide a bad measured draw. */
        if (!warm) {
            vk_render_end();
            vk_finish();
            DrawClear clear = {.color_mask = 15, .color = {0, 0, 0x3f800000, 0x3f800000}};
            vk_render_clear(state, &clear, &channel->owner);
            CHECK(!descriptor_heap_aux_create(&aux));
        }
        u64 drawn = g_vk_drawn;
        for (u32 column = 0; column < 7; column++) {
            if (!warm && column == 1) {
                vk_render_end();
                VkCommandBuffer cb = vk_cmd();
                CHECK(vk_cb_begun() == begun);
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
                if (!column) begun = vk_cb_begun();
                /* The renderer periodically submits long batches. Require the adjacent legacy-compute/heap
                   transition itself to stay in one command buffer; later independent color cases may submit. */
                if (column == 1 && vk_cb_begun() != begun)
                    fprintf(stderr, "descriptor heap transition column%u changed command buffer %u to%u\n",
                            column, begun, vk_cb_begun());
                if (column == 1) CHECK(vk_cb_begun() == begun);
            }
        }
        if (!warm) break;
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
