/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Native color-only mixed-sample coverage. Full triangles and explicit raster sample masks give known coverage
   counts. Custom coefficients test all17 representable values; distinct unused entries expose an incorrect index. */
static int coverage_r1_state_cases(GpuChannel* channel, u64 target, u64 stream, const float vertices[24],
                                    const float source[4], const float destination[4]);
static int coverage_ratio_one(GpuChannel* channel, u64 target, u64 stream, const float vertices[24],
                               const float source[4], const float destination[4]) {
    if (!g_vk_preserve_infnan) {
        puts("SKIP NVN ratio-one coverage: signed-zero/infinity/NaN preservation is unavailable");
        return 0;
    }
    if (!vk_ms_supported(VK_FORMAT_R32G32B32A32_SFLOAT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, DS_MSAA_2X2)) {
        puts("SKIP NVN ratio-one coverage: four-sample RGBA32F is unavailable");
        return 0;
    }
    channel->r3d[0x574] = DS_MSAA_2X2;
    rt_clear_format(channel, target, 8, 8, 0, 0, 0xc0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->dyn.alpha_test = state->dyn.depth_test = state->dyn.stencil_test = 0;
    state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
    state->rkey.sample_mask = 5;
    state->blend.en[0] = 0;
    state->blend.mask[0] = 15;
    u32 checks = 0;
    const u8 coefficients[] = {0, 5, 16};
    for (u32 shading = 0; shading < (g_vk.feat.sampleRateShading ? 2u : 1u); shading++)
        for (u32 mode = 0; mode < 4; mode++)
            for (u32 coefficient = 0; coefficient < 3; coefficient++) {
                state->rkey.sample_shading = (u8)shading;
                for (u32 i = 0; i < DS_GENS; i++) state->gen[i] = ds_generation_next();
                DrawClear clear = {.color_mask = 15};
                memcpy(clear.color, destination, sizeof clear.color);
                vk_render_clear(state, &clear, &channel->owner);
                DrawCall call = {.topology = 4, .count = 3, .instances = 1, .raster_samples = 4,
                                 .coverage_mode = mode, .coverage_table_enable = 1};
                memset(call.coverage_table, 13, sizeof call.coverage_table);
                call.coverage_table[0] = coefficients[coefficient];
                u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
                vk_render_draw(state, &call, &channel->owner);
                CHECK(g_vk_drawn == drawn + 1 && g_vk_skipped == skipped);
                vk_render_end();
                Surf* surface = vk_rt_find(target);
                CHECK(surface && surface->msaa == DS_MSAA_2X2 && surface->w == 4 && surface->h == 4);
                VkBuffer buffer;
                u8* bytes = vk_readback_buffer(1024, &buffer);
                VkImage grid = vk_ms_unpack(vk_cmd(), surface, 0, 0, 8);
                CHECK(bytes && grid);
                VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                          .imageExtent = {8, 8, 1}};
                rec_CmdCopyImageToBuffer(vk_cmd(), grid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
                vk_finish();
                vk_readback_ready();
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++)
                        for (u32 sample = 0; sample < 4; sample++) {
                            float pixel[4];
                            memcpy(pixel, bytes + ((2 * y + sample / 2) * 8 + 2 * x + sample % 2) * 16, 16);
                            for (u32 component = 0; component < 4; component++) {
                                int modulated = component == 3 ? (mode & 2) != 0 : (mode & 1) != 0;
                                float factor = modulated ? (float)coefficients[coefficient] / 16 : 1;
                                float expected = sample & 1 ? destination[component] : source[component] * factor;
                                if (!(fabsf(pixel[component] - expected) < 0.000001f))
                                    fprintf(stderr, "Ratio-one shading%u mode%u factor%u pixel%u,%u sample%u component%u: %g != %g\n",
                                            shading, mode, coefficients[coefficient], x, y, sample, component,
                                            pixel[component], expected);
                                CHECK(fabsf(pixel[component] - expected) < 0.000001f);
                                checks++;
                            }
                        }
            }
    CHECK(!coverage_r1_state_cases(channel, target, stream, vertices, source, destination));
    printf("PASS NVN ratio-one coverage: table entry0, original sample shading, alpha ordering and zero-times-infinity checked=%u exact=1\n", checks);
    return 0;
}
static int coverage_r1_state_cases(GpuChannel* channel, u64 target, u64 stream, const float vertices[24],
                                    const float source[4], const float destination[4]) {
    DsState* state = &channel->ds;
    for (u32 test = 0; test < 5; test++) {
        state->rkey.sample_shading = 0;
        state->rkey.alpha_to_coverage = test == 2 || test == 3;
        state->rkey.alpha_to_one = test == 1 || test == 2;
        state->rkey.sample_mask = test == 2 ? ~0u : 5;
        state->dyn.alpha_test = test == 0;
        state->dyn.alpha_func = DS_CMP_GREATER;
        state->dyn.alpha_ref = 0.5f;
        for (u32 i = 0; i < DS_GENS; i++) state->gen[i] = ds_generation_next();
        DrawClear clear = {.color_mask = 15};
        memcpy(clear.color, destination, sizeof clear.color);
        vk_render_clear(state, &clear, &channel->owner);
        DrawCall call = {.topology = 4, .count = 3, .instances = 1, .raster_samples = 4,
                         .coverage_mode = test == 4 ? 1u : test < 2 ? 2u : 3u,
                         .coverage_table_enable = 1,
                         .multisample_flags = test == 3 ? DS_MS_A2C_DITHER : 0};
        call.coverage_table[0] = test == 0 || test == 4 ? 0 : 5;
        if (test == 4) {
            float infinite[24];
            memcpy(infinite, vertices, sizeof infinite);
            for (u32 vertex = 0; vertex < 3; vertex++) infinite[8 * vertex + 4] = INFINITY;
            CHECK(!gmem_write(stream, infinite, sizeof infinite));
            g_renderer->method_barrier(5);
        }
        u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(g_vk_drawn == drawn + 1 && g_vk_skipped == skipped);
        vk_render_end();
        Surf* surface = vk_rt_find(target);
        CHECK(surface);
        VkBuffer buffer;
        u8* bytes = vk_readback_buffer(1024, &buffer);
        VkImage grid = vk_ms_unpack(vk_cmd(), surface, 0, 0, 8);
        CHECK(bytes && grid);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {8, 8, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), grid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        vk_finish();
        vk_readback_ready();
        u32 mask = test == 2 ? 7 : 5;
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                u32 pixel_mask = mask;
                if (test == 3) {
                    static const u8 ranks[4][4] = {
                        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
                    pixel_mask = 0;
                    for (u32 bit = 0; bit < 4; bit++) {
                        u32 ordinal = (bit + 4 - x) % 4;
                        float threshold = ((float)ordinal + (float)(ranks[y][x] + 1) / 16.0f) / 4.0f;
                        if (source[3] >= threshold) pixel_mask |= 1u << bit;
                    }
                    pixel_mask &= state->rkey.sample_mask;
                }
                for (u32 sample = 0; sample < 4; sample++) {
                    float pixel[4];
                    memcpy(pixel, bytes + ((2 * y + sample / 2) * 8 + 2 * x + sample % 2) * 16, 16);
                    for (u32 component = 0; component < 4; component++) {
                        float expected = destination[component];
                        int nan = 0;
                        if (pixel_mask >> sample & 1) {
                            expected = component == 3 && state->rkey.alpha_to_one ? 1 : source[component];
                            int scale = component == 3 ? (call.coverage_mode & 2) : (call.coverage_mode & 1);
                            if (scale) expected *= (float)call.coverage_table[0] / 16;
                            nan = test == 4 && component == 0;
                        }
                        int exact = nan ? isnan(pixel[component]) : pixel[component] == expected;
                        if (!exact)
                            fprintf(stderr, "Ratio-one state%u pixel%u,%u sample%u component%u: %g != %g%s\n",
                                    test, x, y, sample, component, pixel[component], expected, nan ? " (NaN)" : "");
                        CHECK(exact);
                    }
                }
            }
    }
    CHECK(!gmem_write(stream, vertices, 24 * sizeof *vertices));
    g_renderer->method_barrier(5);
    return 0;
}
static int check_nvn_coverage(void) {
    if (!g_vk.has_mixed_samples) {
        puts("SKIP NVN coverage modulation: native mixed-sample coverage is unavailable on this device");
    }
    const u64 gpu = 0x4dc000000ull, cpu = HEAP_BASE + 0x680000;
    const u64 stream = gpu + 0x2000, target = gpu + 0x4000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xefd9ff800907ff00ull, 0xeff1ff800807ff00ull,
                          0xe30000000007000full};
    const float positions[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float source[4] = {1, 0.5f, 0.25f, 0.75f}, destination[4] = {0.125f, 0.25f, 0.5f, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 121;
    gmmu_map(gpu, cpu, 0x10000);
    u64 fragment[5];
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(gpu, vertex, 5, 0));
    CHECK(mesh_program(gpu + 0x800, fragment, 5, 1));
    float vertices[24];
    for (u32 i = 0; i < 3; i++) {
        memcpy(vertices + i * 8, positions + i * 4, 16);
        memcpy(vertices + i * 8 + 4, source, 16);
    }
    CHECK(!gmem_write(stream, vertices, sizeof vertices));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x850] = 1;
    r[0x851] = 0x800;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x459] = 16u << 7 | 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof vertices - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof vertices - 1);
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    channel->ds.dyn.depth_test = channel->ds.dyn.depth_write = channel->ds.dyn.stencil_test = 0;
    channel->ds.gen[DS_DYNAMIC] = ds_generation_next();
    channel->ds.blend.en[0] = 0;
    channel->ds.gen[DS_BLEND] = ds_generation_next();
    for (u32 raster = 2; g_vk.has_mixed_samples && raster <= 4; raster *= 2) {
        if (!(g_vk.props.limits.framebufferNoAttachmentsSampleCounts & raster)) continue;
        for (u32 count = 0; count <= raster; count += count == 0 ? 1 : raster - 1)
            for (u32 mode = 0; mode < 4; mode++)
                for (u32 coefficient = 0; coefficient < (mode && count ? 18u : 1u); coefficient++) {
                    DrawClear clear = {.color_mask = 15};
                    memcpy(clear.color, destination, sizeof destination);
                    vk_render_clear(&channel->ds, &clear, &channel->owner);
                    DrawCall call = {.topology = 4,
                                     .count = 3,
                                     .instances = 1,
                                     .raster_samples = raster,
                                     .coverage_mode = mode,
                                     .coverage_table_enable = coefficient != 17};
                    for (u32 i = 0; i < 16; i++) call.coverage_table[i] = (u8)((i * 3 + 5) % 17);
                    if (count && coefficient != 17) call.coverage_table[(count - 1) * 16 / raster] = (u8)coefficient;
                    channel->ds.rkey.sample_mask = count ? (1u << count) - 1 : 0;
                    channel->ds.gen[DS_RASTER_KEY] = ds_generation_next();
                    u64 drawn = g_vk_drawn;
                    vk_render_draw(&channel->ds, &call, &channel->owner);
                    CHECK(g_vk_drawn == drawn + 1);
                    vk_render_end();
                    Surf* surface = vk_rt_find(target);
                    VkBuffer buffer;
                    u8* actual = vk_readback_buffer(16, &buffer);
                    CHECK(surface && actual);
                    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT);
                    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                .imageOffset = {1, 1, 0},
                                                .imageExtent = {1, 1, 1}};
                    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                                             &region);
                    vk_finish();
                    vk_readback_ready();
                    float pixel[4];
                    memcpy(pixel, actual, sizeof pixel);
                    float factor = coefficient == 17 ? (float)count / raster : (float)coefficient / 16;
                    for (u32 component = 0; component < 4; component++) {
                        int modulated = component == 3 ? (mode & 2) != 0 : (mode & 1) != 0;
                        float expected = !count ? destination[component] : source[component] * (modulated ? factor : 1);
                        if (!(fabsf(pixel[component] - expected) < 0.000001f))
                            fprintf(stderr, "coverage N%u covered%u mode%u coefficient%u component%u: %g != %g\n",
                                    raster, count, mode, coefficient, component, pixel[component], expected);
                        CHECK(fabsf(pixel[component] - expected) < 0.000001f);
                    }
                }
    }
    CHECK(!coverage_ratio_one(channel, target + 0x2000, stream, vertices, source, destination));
    vk_unlock();
    free(channel);
    if (g_vk.has_mixed_samples)
        puts("PASS NVN coverage modulation: color-only native mixed samples, masks, table and linear factors exact=1");
    return 0;
}

/* A one-sample color target and four-sample Z preserve the same guest pixel extent. Lower-left scissor rows,
   masked stencil clears and per-sample depth writes determine the exact reduced coverage of the final color. */
static int check_nvn_depth_coverage(void) {
    if (!vk_ms_supported(VK_FORMAT_D32_SFLOAT_S8_UINT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, DS_MSAA_2X2)) {
        puts("SKIP NVN depth mixed coverage: four-sample D32FS8 is unavailable");
        return 0;
    }
    const u64 gpu = 0xab0000000ull, cpu = HEAP_BASE + 0x19000000;
    const u64 target = gpu + 0x8000, depth = gpu + 0xa000;
    GpuChannel* channel = sample_raster_channel(gpu, cpu, 0);
    CHECK(channel);
    const float source[4] = {0.75f, 0.375f, 0.125f, 0.75f};
    const float destination[4] = {0.25f, 0.375f, 0.5f, 0.75f};
    u64 fragment[5];
    for (u32 i = 0; i < 4; i++) {
        u32 value;
        memcpy(&value, source + i, 4);
        fragment[i] = 0x0100000000071000ull | (u64)value << 20 | i;
    }
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(gpu + 0x1000, fragment, 5, 1));
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->prog.code[5] = gpu + 0x1000;
    state->prog.early_z = 0;
    state->tgt.depth_msaa = DS_MSAA_2X2 + 1;
    state->tgt.depth = (DsSurf){.gva = depth, .width = 8, .height = 8, .format = 0x19,
                               .layers = 1, .layer_stride = 512};
    state->blend.en[0] = 0;
    state->blend.mask[0] = 15;
    state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = state->rkey.sample_shading = 0;
    state->dyn.depth_mode = 1;
    state->dyn.alpha_test = 0;
    state->dyn.cull = DS_CULL_NONE;
    state->dyn.window_origin = DS_ORIGIN_LOWER_LEFT;
    state->dyn.clip_height = 4;
    state->vp.scale[0][2] = 1;
    state->vp.translate[0][2] = 0;
    state->vp.scissor_on = 1;
    state->vp.scissor_x[0] = 4u << 16;
    int native = g_vk.has_mixed_samples;
    u32 checks = 0, paths = native ? 2 : 1;
    for (u32 packed_stencil = 0; packed_stencil < 2; packed_stencil++)
    for (u32 path = 0; path < paths; path++)
        for (u32 reject = 0; reject < 2; reject++) {
            g_vk.has_mixed_samples = native && !path;
            state->tgt.depth.gva = depth + packed_stencil * 0x1000;
            state->tgt.depth.format = packed_stencil ? 0x19 : 0x0a;
            state->rkey.sample_mask = 5;
            state->dyn.depth_test = state->dyn.depth_write = 1;
            state->dyn.stencil_test = (u8)packed_stencil;
            state->dyn.depth_cmp = DS_CMP_LESS;
            state->vp.scissor_y[0] = 2u << 16;
            for (u32 face = 0; face < 2; face++) {
                state->dyn.face[face].cmp = DS_CMP_ALWAYS;
                state->dyn.face[face].fail = state->dyn.face[face].zfail = DS_STENCIL_KEEP;
                state->dyn.face[face].pass = DS_STENCIL_REPLACE;
                state->dyn.face[face].ref = 9;
                state->dyn.face[face].cmask = 255;
                state->dyn.face[face].wmask = 15;
            }
            for (u32 i = 0; i < DS_GENS; i++) state->gen[i] = ds_generation_next();
            DrawClear clear = {.color_mask = 15, .aspects = packed_stencil ? 3u : 1u, .depth = 1, .stencil = 0xa4};
            memcpy(clear.color, destination, sizeof clear.color);
            vk_render_clear(state, &clear, &channel->owner);
            DrawClear stencil = {.aspects = 2, .stencil = 0xf7, .stencil_preserve = 0xf0};
            if (packed_stencil)
                vk_render_clear(state, &stencil, &channel->owner); /* all samples become A7, retaining the high nibble */
            DrawCall call = {.topology = 4, .count = 3, .instances = 1, .raster_samples = 4};
            u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
            vk_render_draw(state, &call, &channel->owner); /* lower rows: samples0/2 depth0.5 and stencilA9 */
            CHECK(g_vk_drawn == drawn + 1 && g_vk_skipped == skipped);
            clear = (DrawClear){.color_mask = 5};
            memcpy(clear.color, destination, sizeof clear.color);
            vk_render_clear(state, &clear, &channel->owner); /* masked color clear must preserve Z */
            state->rkey.sample_mask = ~0u;
            state->dyn.depth_write = 0;
            state->dyn.depth_cmp = packed_stencil ? (reject ? DS_CMP_LESS : DS_CMP_LESS_OR_EQUAL)
                                                  : (reject ? DS_CMP_NEVER : DS_CMP_LESS);
            state->vp.scissor_y[0] = (packed_stencil ? 4u : 2u) << 16;
            for (u32 face = 0; face < 2; face++) {
                state->dyn.face[face].cmp = DS_CMP_EQUAL;
                state->dyn.face[face].ref = 0xa9;
                state->dyn.face[face].wmask = 0;
                state->dyn.face[face].pass = DS_STENCIL_KEEP;
            }
            state->gen[DS_RASTER_KEY] = state->gen[DS_DYNAMIC] = state->gen[DS_VIEWPORTS] = ds_generation_next();
            call.coverage_mode = 3;
            call.coverage_table_enable = 1;
            memset(call.coverage_table, 3, sizeof call.coverage_table);
            call.coverage_table[4] = 5; /* two of four surviving samples */
            drawn = g_vk_drawn;
            vk_render_draw(state, &call, &channel->owner);
            CHECK(g_vk_drawn == drawn + 1 && g_vk_skipped == skipped);
            vk_render_end();
            Surf *color = vk_rt_find(target), *zeta = vk_rt_find(state->tgt.depth.gva);
            CHECK(color && zeta && !color->msaa && color->w == 4 && color->h == 4 &&
                    zeta->msaa == DS_MSAA_2X2 && zeta->w == 4 && zeta->h == 4);
            VkBuffer buffer;
            u8* bytes = vk_readback_buffer(packed_stencil ? 256 : 512, &buffer);
            CHECK(bytes);
            vk_surf_use(vk_cmd(), color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {4, 4, 1}};
            rec_CmdCopyImageToBuffer(vk_cmd(), color->img, color->layout, buffer, 1, &copy);
            if (!packed_stencil) {
                VkImage grid = vk_ms_unpack(vk_cmd(), zeta, 0, 0, 8);
                CHECK(grid);
                copy = (VkBufferImageCopy){.bufferOffset = 256,
                                           .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                           .imageExtent = {8, 8, 1}};
                rec_CmdCopyImageToBuffer(vk_cmd(), grid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
            }
            vk_finish();
            vk_readback_ready();
            for (u32 y = 0; y < 4; y++)
                for (u32 x = 0; x < 4; x++) {
                    float pixel[4];
                    memcpy(pixel, bytes + (4 * y + x) * 16, sizeof pixel);
                    for (u32 k = 0; k < 4; k++) {
                        float expected = y >= 2 && !reject ? source[k] * (5.0f / 16) : destination[k];
                        if (!(fabsf(pixel[k] - expected) < 0.000001f))
                            fprintf(stderr, "Depth coverage stencil%u path%u reject%u pixel%u,%u component%u: %g != %g\n",
                                    packed_stencil, path, reject, x, y, k, pixel[k], expected);
                        CHECK(fabsf(pixel[k] - expected) < 0.000001f);
                        checks++;
                    }
                    for (u32 sample = 0; !packed_stencil && sample < 4; sample++) {
                        float actual;
                        u32 at = 256 + ((2 * y + sample / 2) * 8 + 2 * x + sample % 2) * 4;
                        memcpy(&actual, bytes + at, 4);
                        CHECK(actual == (y >= 2 && !(sample & 1) ? 0.5f : 1));
                        checks++;
                    }
                }
        }
    g_vk.has_mixed_samples = native;
    /* A failed depth allocation must not turn an active test into a color-only draw. Prefer a genuinely
       unsupported sample count; the malformed one-pixel sample grid supplies the same missing-target case on
       devices that support every API count. Inactive depth state must still permit ordinary color output. */
    u32 unavailable = 0, validation = g_vk_validation_messages;
    for (u32 samples = 2; samples <= 16; samples *= 2)
        if (!vk_ms_supported(VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              0, samples == 2 ? DS_MSAA_2X1 : samples == 4 ? DS_MSAA_2X2
                                 : samples == 8 ? DS_MSAA_4X2 : DS_MSAA_4X4)) {
            unavailable = samples;
            break;
        }
    u32 bad_layout = unavailable == 2 ? DS_MSAA_2X1 : unavailable == 4 ? DS_MSAA_2X2
                      : unavailable == 8 ? DS_MSAA_4X2 : DS_MSAA_4X4;
    state->tgt.depth = (DsSurf){.gva = gpu + 0xc000, .format = 0x0a, .layers = 1,
                               .width = unavailable ? 4u << ds_msaa_log2_x(bad_layout) : 1,
                               .height = unavailable ? 4u << ds_msaa_log2_y(bad_layout) : 1};
    state->tgt.depth_msaa = (u8)(bad_layout + 1);
    state->dyn.depth_test = 1;
    state->dyn.depth_write = state->dyn.stencil_test = 0;
    state->dyn.depth_cmp = DS_CMP_LESS;
    state->vp.scissor_y[0] = 4u << 16;
    state->rkey.sample_mask = ~0u;
    for (u32 i = 0; i < DS_GENS; i++) state->gen[i] = ds_generation_next();
    DrawClear initial = {.color_mask = 15};
    memcpy(initial.color, destination, sizeof initial.color);
    vk_render_clear(state, &initial, &channel->owner);
    for (u32 active = 1; active < 3; active++) {
        int tested = active == 1;
        state->dyn.depth_test = (u8)tested;
        state->gen[DS_DYNAMIC] = ds_generation_next();
        DrawCall call = {.topology = 4, .count = 3, .instances = 1, .raster_samples = tested ? 4u : 0};
        u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(g_vk_drawn == drawn + !tested && g_vk_skipped == skipped + tested);
        vk_render_end();
        Surf* surface = vk_rt_find(target);
        CHECK(surface);
        VkBuffer buffer;
        u8* bytes = vk_readback_buffer(256, &buffer);
        CHECK(bytes);
        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {4, 4, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, surface->layout, buffer, 1, &copy);
        vk_finish();
        vk_readback_ready();
        for (u32 pixel = 0; pixel < 16; pixel++) {
            float value[4];
            memcpy(value, bytes + pixel * 16, sizeof value);
            CHECK(!memcmp(value, tested ? destination : source, sizeof value));
            checks += 4;
        }
    }
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    free(channel);
    printf("PASS NVN depth mixed coverage: native/portable color1-Z4, lower-left rows, masked color/stencil and rejection checked=%u exact=1\n", checks);
    return 0;
}
