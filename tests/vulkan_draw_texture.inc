/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* DrawTexture must use raster fragment operations and the bound sampler. A blit cannot satisfy these cases. */
static int check_draw_texture(void) {
    const u64 gpu = 0x4ed000000ull, cpu = HEAP_BASE + 0x10000000;
    const u64 target = gpu + 0x8000, source = gpu + 0x4000;
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 137;
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x10000);
    u8 pixels[512] = {0};
    for (u32 y = 0; y < 2; y++)
        for (u32 x = 0; x < 2; x++) {
            u8 color[4] = {0, x ? 0 : 255, x ? 255 : 0, 128};
            memcpy(pixels + gob_x_offset(4 * x) + gob_y_offset(y), color, 4);
        }
    CHECK(!gmem_write(source, pixels, sizeof pixels));
    NvnTexture texture = {.gpu = source};
    texture.d.target = NVN_TEXTURE_TARGET_2D;
    texture.d.format = NVN_FORMAT_RGBA8;
    texture.d.levels = 1;
    texture.d.width = texture.d.height = 2;
    texture.d.depth = 1;
    u32 tic[8];
    CHECK(nvn_tic_encode(&texture, NULL, 0, 0, tic));
    CHECK(!storage_write_words(gpu, tic, 8));
    CHECK(!storage_write_words(gpu + 0x1000, (const u32[8]){2u | 2u << 3 | 2u << 6}, 8)); /* nearest and clamp to edge */
    for (u32 test = 0; test < 6; test++) {
        vk_lock();
        channel->r3d[0x574] = 0;
        rt_clear(channel, target, 4, 4, 0, 0);
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        DsState* state = &channel->ds;
        state->bind.tex_pool = gpu;
        state->bind.smp_pool = gpu + 0x1000;
        state->bind.tex_max = state->bind.smp_max = 0;
        state->blend = (DsBlend){0};
        state->blend.mask[0] = test == 2 ? DS_COMPONENT_G : 15;
        state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
        state->rkey.sample_mask = ~0u;
        state->dyn.alpha_test = test == 3;
        state->dyn.alpha_func = DS_CMP_GREATER;
        state->dyn.alpha_ref = 0.75f;
        DrawTexture rectangle = {.w = 4, .h = 4, .sx0 = 0.5f, .sy0 = 0.5f};
        rectangle.state.multisample_flags = DS_MS_A2C_DITHER;
        if (test == 1) {
            state->blend.en[0] = 1;
            state->blend.sc[0] = DS_FACTOR_SRC_ALPHA;
            state->blend.dc[0] = DS_FACTOR_ONE_MINUS_SRC_ALPHA;
            state->blend.sa[0] = DS_FACTOR_ONE;
            state->blend.da[0] = DS_FACTOR_ZERO;
        }
        if (test == 4) {
            rectangle.sx0 = rectangle.sy0 = -2;
            rectangle.du = rectangle.dv = 1;
        }
        if (test == 5) {
            if (!vk_ms_supported(VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                                 DS_MSAA_2X2)) {
                vk_unlock();
                puts("SKIP NVN DrawTexture multisample: four-sample RGBA8 is unavailable");
                continue;
            }
            ms_target(channel, target + 0x2000, 1);
            const float red[4] = {1, 0, 0, 1};
            memcpy(channel->r3d + 0x360, red, sizeof red);
            engine_clear(channel, 15 << 2);
            channel->r3d_dirty = ~0u;
            mx_draw_state_sync(channel);
            state->bind.tex_pool = gpu;
            state->bind.smp_pool = gpu + 0x1000;
            state->bind.tex_max = state->bind.smp_max = 0;
            state->blend = (DsBlend){0};
            state->blend.mask[0] = 15;
            state->dyn.alpha_test = 0;
            state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
            state->rkey.sample_mask = 5;
        }
        u64 drawn = g_vk_drawn;
        vk_render_draw_texture(state, &rectangle);
        CHECK(g_vk_drawn == drawn + 1);
        vk_render_end();
        Surf* surface = vk_rt_find(test == 5 ? target + 0x2000 : target);
        CHECK(surface);
        u8 actual[256];
        if (test == 5)
            CHECK(!ms_read_samples(surface, actual));
        else
            CHECK(!ms_read_pixels(surface, 4, 4, actual));
        u32 samples = test == 5 ? 4 : 1;
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++)
                for (u32 sample = 0; sample < samples; sample++) {
                    u8 expected[4] = {0, 255, 0, 128};
                    if (test == 1) expected[0] = 127, expected[1] = 128;
                    if (test == 2) expected[0] = 255, expected[3] = 255;
                    if (test == 3 || (test == 5 && !(5u >> sample & 1))) {
                        expected[0] = expected[3] = 255;
                        expected[1] = expected[2] = 0;
                    }
                    if (test == 4 && x == 3) expected[1] = 0, expected[2] = 255;
                    const u8* pixel = test == 5 ? actual + (2 * y + sample / 2) * 32 + (2 * x + sample % 2) * 4
                                               : actual + (y * 4 + x) * 4;
                    for (u32 component = 0; component < 4; component++) {
                        int delta = (int)pixel[component] - expected[component];
                        if (delta < -1 || delta > 1) {
                            fprintf(stderr, "DrawTexture case%u pixel%u,%u sample%u component%u: %u != %u\n",
                                    test, x, y, sample, component, pixel[component], expected[component]);
                            CHECK(delta >= -1 && delta <= 1);
                        }
                    }
                }
        if (test == 5) puts("PASS NVN DrawTexture multisample: four-sample write mask exact=1");
        vk_unlock();
    }
    free(channel);
    puts("PASS NVN DrawTexture: sampling, blend, mask, alpha test and edge clamp exact=1");
    return 0;
}
