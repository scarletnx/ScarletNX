/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GPU timestamps include resolver work, and the adaptive method's extra finalizer dispatch and dependency.
   Capture marking, image clears, setup, readbacks and CPU work remain outside the interval. All three outputs
   must match byte for byte before timing is reported. */
#include "nx/gpu/vk/capture.h"
#include "nx/gpu/shader/capture_tiles_spirv.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "common/utilities/endian.h"

static u32* capture_bench_mark_code(size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 4096);
    spv_emit_shader_capability(&e);
    u32 u = spv_emit_type_u32(&e), f = spv_emit_type_f32(&e), b = spv_emit_type_bool(&e);
    u32 v4 = spv_emit_type_vec4(&e, f), uv3 = spv_emit_type_vec3(&e, u);
    u32 array = spv_emit_type_runtime_array(&e, v4, 16), block = spv_emit_type_struct_one(&e, array);
    u32 input = spv_emit_storage_buffer(&e, block, 0, 0), tiles = shader_capture_tiles_declare(&e, u, 0, 1);
    u32 invocation = spv_emit_builtin(&e, 1, uv3, 28);
    u32 fn = spv_emit_function(&e, 0, spv_emit_type_void(&e), NULL, 0, NULL);
    u32 id = spv_emit_vec4_extract(&e, u, spv_emit_load(&e, uv3, invocation), 0);
    u32 count = SPV(&e, 68, u, input, 0); /* OpArrayLength */
    u32 inside = SPV(&e, SPV_OP_U_LESS_THAN, b, id, count);
    u32 apply = spv_emit_id(&e), done = spv_emit_id(&e);
    spv_emit_selection_merge(&e, done);
    spv_emit_branch_conditional(&e, inside, apply, done);
    spv_emit_label(&e, apply);
    u32 pointer = SPV(&e, SPV_OP_ACCESS_CHAIN, spv_emit_type_storage_pointer(&e, v4), input,
                      spv_emit_constant_u32(&e, u, 0), id);
    u32 position = spv_emit_load(&e, v4, pointer);
    u32 layer = spv_emit_convert_f_to_u(&e, u, spv_emit_vec4_extract(&e, f, position, 2));
    u32 live = SPV(&e, SPV_OP_F_ORD_GREATER_THAN, b, spv_emit_vec4_extract(&e, f, position, 3),
                   spv_emit_constant_f32(&e, f, 0));
    shader_capture_tile_mark(&e, f, u, b, tiles, position, layer, live);
    spv_emit_branch(&e, done);
    spv_emit_label(&e, done);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    const u32 interfaces[] = {invocation, input, tiles};
    spv_emit_compute_entry_str(&e, fn, "main", interfaces, 3);
    spv_emit_local_size(&e, fn, 64, 1, 1);
    u32* result = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return result;
}
static VkPipeline capture_bench_pipeline(VkPipelineLayout layout, u32* code, size_t words) {
    VkShaderModule module = vk_module_spv(code, words);
    free(code);
    if (!module) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                  .module = module,
                                                  .pName = "main"}, .layout = layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    return result == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}
static void capture_bench_tile_barrier(VkCapture* capture) {
    VkBufferMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = capture->tile_buffer, .size = capture->tile_bytes};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                   .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier};
    rec_CmdPipelineBarrier2(vk_cmd(), &dependency);
}
static int capture_bench_mark(VkCapture* capture, VkDescriptorSetLayout descriptors, VkPipelineLayout layout,
                               VkPipeline pipeline, u32 workload) {
    u32 count = workload == 1 ? capture->tile_count : 64;
    VkBuffer input;
    u64 offset;
    float* positions = (float*)vk_stage((u64)count * 16, 16, &input, &offset);
    CHECK(positions);
    for (u32 i = 0; i < count; i++) {
        positions[4 * i] = workload == 1 ? (float)((i % 64) * 8) + 0.5f : (float)(16 + i % 8) + 0.5f;
        positions[4 * i + 1] = workload == 1 ? (float)((i / 64) * 8) + 0.5f : (float)(16 + i / 8) + 0.5f;
        positions[4 * i + 2] = 0;
        positions[4 * i + 3] = workload == 3 ? 0 : 1;
    }
    VkDescriptorSet set = vk_desc_alloc(descriptors);
    CHECK(set);
    const VkDescriptorBufferInfo buffers[2] = {{input, offset, (u64)count * 16},
                                               {capture->tile_buffer, 0, capture->tile_bytes}};
    VkWriteDescriptorSet writes[2];
    for (u32 i = 0; i < 2; i++) writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
    capture_bench_tile_barrier(capture);
    rec_CmdBindPipeline(vk_cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    rec_CmdBindDescriptorSets(vk_cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    rec_CmdDispatch(vk_cmd(), (count + 63) / 64, 1, 1);
    capture_bench_tile_barrier(capture);
    VkBuffer readback;
    u8* actual = vk_readback_buffer(capture->tile_bytes, &readback);
    CHECK(actual);
    const VkBufferCopy copy = {0, 0, capture->tile_bytes};
    rec_CmdCopyBuffer(vk_cmd(), capture->tile_buffer, readback, 1, &copy);
    vk_finish();
    vk_readback_ready();
    u32 expected = workload == 1 ? capture->tile_count : workload == 3 ? 0 : 1;
    CHECK(snx_ld_le32(actual) == expected);
    CHECK(snx_ld_le32(actual + 4) == 1 && snx_ld_le32(actual + 8) == 1);
    u8 seen[4096] = {0};
    for (u32 i = 0; i < expected; i++) {
        u32 tile = snx_ld_le32(actual + (SH_CAPTURE_TILE_HEADER + capture->tile_count + i) * 4);
        CHECK(tile < capture->tile_count && !seen[tile]);
        CHECK(workload == 1 || tile == 130);
        CHECK(snx_ld_le32(actual + (SH_CAPTURE_TILE_HEADER + tile) * 4) == 1);
        seen[tile] = 1;
    }
    return 0;
}
static void capture_bench_clear(Surf* image, const float values[4]) {
    vk_surf_use(vk_cmd(), image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue color;
    memcpy(color.float32, values, 16);
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    rec_CmdClearColorImage(vk_cmd(), image->img, image->layout, &color, 1, &range);
}
static int capture_bench_validity(VkCapture* capture, u32 workload) {
    const float zero[4] = {0}, one[4] = {1, 0, 0, 0};
    capture_bench_clear(&capture->valid, workload == 1 ? one : zero);
    if (workload != 0) return 0;
    vk_surf_use(vk_cmd(), &capture->valid, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = capture->valid.att, .imageLayout = capture->valid.layout,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {512, 512}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
    rec_CmdBeginRendering(vk_cmd(), &rendering);
    VkClearAttachment clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                               .clearValue.color = {.float32 = {1, 0, 0, 0}}};
    const VkClearRect rectangle = {{{16, 16}, {8, 8}}, 0, 1};
    rec_CmdClearAttachments(vk_cmd(), 1, &clear, 1, &rectangle);
    rec_CmdEndRendering(vk_cmd());
    return 0;
}
static int capture_bench_double_compare(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static int bench_capture_tiles(void) {
    if (!vk_capture_tiles_supported() || !g_vk.props.limits.timestampComputeAndGraphics) {
        puts("SKIP capture tile GPU benchmark: storage atomics or graphics timestamps are unavailable");
        return 0;
    }
    vk_lock();
    vk_render_end();
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &family_count, NULL);
    VkQueueFamilyProperties* families = malloc((size_t)family_count * sizeof *families);
    CHECK(families && g_vk.gfx_family < family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &family_count, families);
    u32 timestamp_bits = families[g_vk.gfx_family].timestampValidBits;
    free(families);
    CHECK(timestamp_bits && timestamp_bits <= 64);
    u64 timestamp_mask = timestamp_bits == 64 ? ~(u64)0 : ((u64)1 << timestamp_bits) - 1;
    Surf target = {.w = 512, .h = 512, .sw = 512, .sh = 512, .layers = 1, .levels = 1, .depth3d = 1,
        .storage_capable = 1, .type = VK_IMAGE_VIEW_TYPE_2D,
        .f = {.vk = VK_FORMAT_R32G32B32A32_SFLOAT, .bpp = 16, .bw = 1, .bh = 1}};
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = target.f.vk, .extent = {512, 512, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vk_image_create(&image_info, &target.img, &target.mem));
    VkCapture* capture = vk_capture_get(&target);
    CHECK(capture && (capture->flags & SH_CAPTURE_TILED) && capture->tile_count == 4096);
    VkDescriptorSetLayout layouts[2];
    VkPipelineLayout pipeline_layouts[2];
    for (u32 kind = 0; kind < 2; kind++) {
        VkDescriptorSetLayoutBinding bindings[4];
        u32 count = kind ? 4 : 2;
        for (u32 i = 0; i < count; i++) bindings[i] = (VkDescriptorSetLayoutBinding){i,
            !kind || i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = count, .pBindings = bindings};
        CHECK(VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &info, NULL, &layouts[kind])));
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1, .pSetLayouts = &layouts[kind]};
        CHECK(VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &pipeline_layouts[kind])));
    }
    size_t words;
    u32* code = capture_bench_mark_code(&words);
    VkPipeline marking = capture_bench_pipeline(pipeline_layouts[0], code, words);
    CHECK(marking);
    const u64 blend = 23ull << 37 | 1ull << 46;
    VkPipeline resolve[3];
    for (u32 method = 0; method < 3; method++) {
        code = shader_blend_resolve_spirv(blend, 15 | (method ? SH_CAPTURE_TILED : 0) |
                                             (method == 2 ? SH_CAPTURE_ADAPTIVE : 0), &words);
        resolve[method] = capture_bench_pipeline(pipeline_layouts[1], code, words);
        CHECK(resolve[method]);
    }
    VkQueryPoolCreateInfo query_info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 6};
    VkQueryPool queries;
    CHECK(VK_OK(vkCreateQueryPool(g_vk.dev, &query_info, NULL, &queries)));
    const float source[4] = {0.125f, 0.0625f, 0.1875f, 0.25f};
    const float destination[4] = {0.5625f, 0.1875f, 0.375f, 0.75f};
    capture_bench_clear(&capture->source, source);
    for (u32 workload = 0; workload < 4; workload++) {
        CHECK(vk_capture_clear(capture));
        CHECK(!capture_bench_validity(capture, workload));
        CHECK(!capture_bench_mark(capture, layouts[0], pipeline_layouts[0], marking, workload));
        if (!workload) {
            /* Simulate a capture that never records its resolve. Dirty recovery must clear the old flags. */
            CHECK(capture->tile_dirty);
            CHECK(vk_capture_clear(capture));
            CHECK(!capture_bench_validity(capture, workload));
            CHECK(!capture_bench_mark(capture, layouts[0], pipeline_layouts[0], marking, workload));
        }
        double times[3][8];
        for (u32 round = 0; round < 10; round++) {
            const u64 bytes = 512ull * 512 * 16;
            VkBuffer readback;
            u8* pixels = vk_readback_buffer(bytes * 3, &readback);
            CHECK(pixels);
            rec_CmdResetQueryPool(vk_cmd(), queries, 0, 6);
            for (u32 pair = 0; pair < 3; pair++) {
                u32 method = (pair + round) % 3;
                capture_bench_clear(&target, destination);
                Surf* images[3] = {&capture->source, &capture->valid, &target};
                VkDescriptorImageInfo image[3];
                VkWriteDescriptorSet writes[4];
                VkDescriptorSet set = vk_desc_alloc(layouts[1]);
                CHECK(set);
                for (u32 i = 0; i < 3; i++) {
                    image[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, vk_tex_storage_view(images[i], VK_IMAGE_VIEW_TYPE_2D),
                                                       VK_IMAGE_LAYOUT_GENERAL};
                    CHECK(image[i].imageView);
                    writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                        .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .pImageInfo = &image[i]};
                    vk_surf_use(vk_cmd(), images[i], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | (i == 2 ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0));
                }
                VkDescriptorBufferInfo buffer = {capture->tile_buffer, 0, capture->tile_bytes};
                writes[3] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                    .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &buffer};
                vkUpdateDescriptorSets(g_vk.dev, 4, writes, 0, NULL);
                capture_bench_tile_barrier(capture);
                rec_CmdWriteTimestamp2(vk_cmd(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, queries, method * 2);
                if (method == 2) CHECK(vk_capture_finalize_tiles(capture));
                rec_CmdBindPipeline(vk_cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, resolve[method]);
                rec_CmdBindDescriptorSets(vk_cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layouts[1], 0, 1, &set, 0, NULL);
                if (method) rec_CmdDispatchIndirect(vk_cmd(), capture->tile_buffer,
                                                   method == 2 ? SH_CAPTURE_ADAPTIVE_DISPATCH : 0);
                else rec_CmdDispatch(vk_cmd(), 64, 64, 1);
                rec_CmdWriteTimestamp2(vk_cmd(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, queries, method * 2 + 1);
                vk_surf_use(vk_cmd(), &target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy copy = {.bufferOffset = method * bytes,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {512, 512, 1}};
                rec_CmdCopyImageToBuffer(vk_cmd(), target.img, target.layout, readback, 1, &copy);
            }
            vk_finish();
            vk_readback_ready();
            CHECK(!memcmp(pixels, pixels + bytes, (size_t)bytes));
            CHECK(!memcmp(pixels, pixels + 2 * bytes, (size_t)bytes));
            u64 timestamps[6];
            CHECK(VK_OK(vkGetQueryPoolResults(g_vk.dev, queries, 0, 6, sizeof timestamps, timestamps,
                                              sizeof(u64), VK_QUERY_RESULT_64_BIT)));
            if (round >= 2)
                for (u32 method = 0; method < 3; method++)
                    times[method][round - 2] = (double)((timestamps[method * 2 + 1] - timestamps[method * 2]) & timestamp_mask) *
                                               g_vk.props.limits.timestampPeriod;
        }
        capture->tile_dirty = 0; /* the timed tiled resolver reset every enqueued flag */
        CHECK(vk_capture_clear(capture));
        CHECK(!capture_bench_mark(capture, layouts[0], pipeline_layouts[0], marking, workload));
        static const char* const names[4] = {"sparse", "full_control", "rejected", "empty"};
        for (u32 method = 0; method < 3; method++) {
            qsort(times[method], 8, sizeof(double), capture_bench_double_compare);
            double median = (times[method][3] + times[method][4]) * 0.5;
            printf("GPU_TIMESTAMP_BENCH workload=capture_tiles_%s method=%s operations=%u gpu_ns=%.0f min_gpu_ns=%.0f max_gpu_ns=%.0f samples=8 exact=1\n",
                   names[workload], method == 2 ? "adaptive_full" : method ? "tiled" : "full_grid", workload == 1 ? 4096 : workload == 3 ? 0 : 1,
                   median, times[method][0], times[method][7]);
        }
    }
    vk_render_forget_state();
    vkDestroyQueryPool(g_vk.dev, queries, NULL);
    vkDestroyPipeline(g_vk.dev, marking, NULL);
    for (u32 i = 0; i < 3; i++) vkDestroyPipeline(g_vk.dev, resolve[i], NULL);
    for (u32 i = 0; i < 2; i++) {
        vkDestroyPipelineLayout(g_vk.dev, pipeline_layouts[i], NULL);
        vkDestroyDescriptorSetLayout(g_vk.dev, layouts[i], NULL);
    }
    capture->target = NULL;
    for (int i = 0; i < target.nsv; i++) vk_image_view_destroy(g_vk.dev, target.sv[i].v, NULL);
    vk_image_destroy(target.img, target.mem);
    vk_unlock();
    puts("PASS capture tile interoperability: unique append, repeated flag reuse, dirty recovery and paired output bytes exact=1");
    return 0;
}
