/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Small triangles expose changed sample positions. The same native draw setup tests coverage after sample depth. */
static GpuChannel* sample_raster_channel(u64 gpu, u64 cpu, int edge) {
    GpuChannel* channel = calloc(1, sizeof *channel);
    if (!channel) return NULL;
    channel->id = 138;
    if (mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW)) { free(channel); return NULL; }
    gmmu_map(gpu, cpu, 0x10000);
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xefd9ff800907ff00ull,
                          0xeff1ff800807ff00ull, 0xe30000000007000full};
    u64 fragment[5];
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    if (!mesh_program(gpu, vertex, 5, 0) || !mesh_program(gpu + 0x800, fragment, 5, 1)) {
        free(channel); return NULL;
    }
    float position[12] = {-1, -1, 0.5f, 1, edge ? 1 : 3, -1, 0.5f, 1, -1, edge ? 1 : 3, 0.5f, 1};
    float vertices[24];
    for (u32 i = 0; i < 3; i++) {
        memcpy(vertices + 8 * i, position + 4 * i, 16);
        memcpy(vertices + 8 * i + 4, (const float[4]){0, 1, 0, 1}, 16);
    }
    if (gmem_write(gpu + 0x2000, vertices, sizeof vertices)) { free(channel); return NULL; }
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32); r[0x583] = (u32)gpu;
    r[0x850] = 1; r[0x851] = 0x800;
    r[0x64b] = 1; r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x459] = 16u << 7 | 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)((gpu + 0x2000) >> 32); r[0x702] = (u32)(gpu + 0x2000);
    r[0x7c0] = (u32)((gpu + 0x2000 + sizeof vertices - 1) >> 32);
    r[0x7c1] = (u32)(gpu + 0x2000 + sizeof vertices - 1);
    return channel;
}
static int check_custom_sample_locations(void) {
    if (!g_vk.sample_locations.available || !g_vk.sample_locations.variable || !(g_vk.sample_locations.counts & 4)) {
        puts("SKIP NVN custom raster samples: variable four-sample locations are unavailable");
        return 0;
    }
    const u64 gpu = 0x4ee000000ull, cpu = HEAP_BASE + 0x10100000, target = gpu + 0x4000, zeta = gpu + 0x6000;
    GpuChannel* channel = sample_raster_channel(gpu, cpu, 1);
    CHECK(channel);
    vk_lock();
    ms_target(channel, target, 1);
    channel->r3d[0x54e] = 1;
    channel->r3d[0x3f8] = (u32)(zeta >> 32); channel->r3d[0x3f9] = (u32)zeta;
    channel->r3d[0x3fa] = 0x0a; channel->r3d[0x3fb] = 0; channel->r3d[0x3fc] = 0;
    channel->r3d[0x48a] = channel->r3d[0x48b] = 8; channel->r3d[0x48c] = 1;
    const float one = 1;
    memcpy(channel->r3d + 0x364, &one, 4);
    const float blue[4] = {0, 0, 1, 1};
    memcpy(channel->r3d + 0x360, blue, sizeof blue);
    engine_clear(channel, 1 | 15 << 2);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    channel->ds.dyn.depth_mode = 1;
    channel->ds.dyn.depth_test = channel->ds.dyn.depth_write = 1;
    channel->ds.dyn.depth_cmp = DS_CMP_LESS;
    channel->ds.dyn.alpha_test = channel->ds.dyn.stencil_test = 0;
    channel->ds.blend.en[0] = 0;
    channel->ds.rkey.sample_mask = ~0u;
    DrawCall call = {.topology = 4, .count = 3, .instances = 1,
                     .multisample_flags = DS_MS_SAMPLE_LOCATIONS};
    call.sample_locations[0] = call.sample_locations[1] = 0x22;
    call.sample_locations[2] = call.sample_locations[3] = 0xee;
    u64 drawn = g_vk_drawn;
    vk_render_draw(&channel->ds, &call, &channel->owner);
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    Surf *surface = vk_rt_find(target), *depth = vk_rt_find(zeta);
    CHECK(surface && depth && depth->sample_locations_compatible);
    CHECK(depth->sample_locations.samples == 4 && depth->sample_locations.location[2] == 0xee);
    u8 actual[256];
    CHECK(!ms_read_samples(surface, actual));
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 sample = 0; sample < 4; sample++) {
                int green = 8 * (x + y) + (sample < 2 ? 2 : 14) < 32;
                const u8* pixel = actual + (2 * y + sample / 2) * 32 + (2 * x + sample % 2) * 4;
                CHECK(!memcmp(pixel, green ? (const u8[4]){0, 255, 0, 255} : (const u8[4]){0, 0, 255, 255}, 4));
            }
    /* A depth layout transition must retain the sample positions used to produce its contents. */
    vk_surf_use(vk_cmd(), depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_finish();
    vk_unlock();
    free(channel);
    puts("PASS NVN custom raster samples: sample colors and depth metadata exact=1");
    return 0;
}
static int check_coverage_to_color(void) {
    if ((!g_vk.has_coverage_to_color && !g_vk.has_post_depth_coverage) ||
        !vk_ms_supported(VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          0, DS_MSAA_2X2)) {
        puts("SKIP NVN coverage-to-color: four-sample R32UI or post-depth coverage is unavailable");
        return 0;
    }
    const u64 gpu = 0x4ef000000ull, cpu = HEAP_BASE + 0x10200000, target = gpu + 0x4000, zeta = gpu + 0x6000;
    GpuChannel* channel = sample_raster_channel(gpu, cpu, 0);
    CHECK(channel);
    int native = g_vk.has_coverage_to_color;
    u32 paths = native && g_vk.has_post_depth_coverage ? 2 : 1;
    u32 split_paths = g_vk.has_post_depth_coverage && g_vk.feat.sampleRateShading ? 2 : 0;
    if (!split_paths) puts("SKIP coverage-to-color split invocations: sample shading with post-depth coverage is unavailable");
    for (u32 variant = 0; variant < 2 * paths + split_paths; variant++) {
        int split = variant >= 2 * paths;
        int signed_target = split ? variant - 2 * paths : variant / paths;
        g_vk.has_coverage_to_color = !split && native && !(variant % paths);
        vk_lock();
        ms_target(channel, target, 1);
        channel->r3d[0x204] = signed_target ? 0xe3 : 0xe4; /* R32I and R32UI */
        channel->r3d[0x54e] = 1;
        channel->r3d[0x3f8] = (u32)(zeta >> 32); channel->r3d[0x3f9] = (u32)zeta;
        channel->r3d[0x3fa] = 0x0a; channel->r3d[0x3fb] = 0; channel->r3d[0x3fc] = 0;
        channel->r3d[0x48a] = channel->r3d[0x48b] = 8; channel->r3d[0x48c] = 1;
        const float one = 1;
        memcpy(channel->r3d + 0x364, &one, 4);
        engine_clear(channel, 1);
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        DsState* state = &channel->ds;
        state->dyn.depth_mode = 1;
        state->dyn.depth_test = state->dyn.depth_write = 1;
        state->dyn.depth_cmp = DS_CMP_LESS;
        state->dyn.stencil_test = state->dyn.alpha_test = 0;
        state->rkey.sample_mask = 5;
        /* Force separate fragment invocations while the fallback must still report the complete post-depth
           pixel mask10 to each surviving sample. This exposes drivers whose SampleMaskIn contains only a split. */
        state->rkey.sample_shading = (u8)split;
        state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
        state->blend.en[0] = 0;
        DrawCall call = {.topology = 4, .count = 3, .instances = 1};
        vk_render_draw(state, &call, &channel->owner); /* depth0.5 in samples0 and2 */
        DrawClear clear = {.color_mask = 15};
        vk_render_clear(state, &clear, &channel->owner);
        state->dyn.depth_write = 0;
        state->rkey.sample_mask = ~0u;
        state->gen[DS_DYNAMIC] = state->gen[DS_RASTER_KEY] = ds_generation_next();
        call.multisample_flags = DS_MS_COVERAGE_TO_COLOR;
        u64 drawn = g_vk_drawn;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(g_vk_drawn == drawn + 1);
        vk_render_end();
        Surf* surface = vk_rt_find(target);
        CHECK(surface);
        u8 actual[256];
        CHECK(!ms_read_samples(surface, actual));
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++)
                for (u32 sample = 0; sample < 4; sample++) {
                    u32 value = snx_ld_le32(actual + (2 * y + sample / 2) * 32 + (2 * x + sample % 2) * 4);
                    if (value != (sample & 1 ? 10u : 0u))
                        fprintf(stderr, "Coverage-to-color variant%u pixel%u,%u sample%u: %#x != %#x\n",
                                variant, x, y, sample, value, sample & 1 ? 10u : 0u);
                    CHECK(value == (sample & 1 ? 10u : 0u));
                }
        vk_unlock();
    }
    g_vk.has_coverage_to_color = native;
    free(channel);
    printf("PASS NVN coverage-to-color: signed/unsigned post-depth masks, %u forced sample-shading fallback variants exact=1\n", split_paths);
    return 0;
}
