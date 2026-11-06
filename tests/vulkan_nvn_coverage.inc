/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Native color-only mixed-sample coverage. Full triangles and explicit raster sample masks give known coverage
   counts. Custom coefficients test all17 representable values; distinct unused entries expose an incorrect index. */
static int check_nvn_coverage(void) {
    if (!g_vk.has_mixed_samples) {
        puts("SKIP NVN coverage modulation: native mixed-sample coverage is unavailable on this device");
        return 0;
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
    for (u32 raster = 2; raster <= 4; raster *= 2) {
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
    vk_unlock();
    free(channel);
    puts("PASS NVN coverage modulation: color-only native mixed samples, masks, table and linear factors exact=1");
    return 0;
}
