/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Reuse the real Maxwell full-screen programs from the equation checks. Exercise the storage-image variants
   separately from their arithmetic, including depth rejection that differs between samples of one pixel. */
static int advanced_target_variants(GpuChannel* channel, u64 target) {
    const u64 cpu = HEAP_BASE + 0x10500000;
    CHECK(!mem_map(cpu, 0x70000, MS_NORMAL, PERM_RW));
    gmmu_map(target + 0x10000, cpu, 0x70000);
    DsState* state = &channel->ds;
    const float source[4] = {0.125f, 0.0625f, 0.1875f, 0.25f};
    const float destination[4] = {0.5625f, 0.1875f, 0.375f, 0.75f};
    const u64 packed = nvn_set_field(nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV,
                           NVN_BLEND_ADVANCED_MODE_OVERLAY), NVN_BLEND_ADV_PMS, 1), NVN_BLEND_ADV_NPM, 1);
    for (u32 test = 0; test < 6; test++) {
        int srgb = test == 1, array = test == 2, multisample = test == 3 || test == 5, depth = test >= 4;
        u64 address = target + 0x10000 + test * 0x10000;
        state->tgt.color[0] = (DsSurf){.gva = address, .width = multisample ? 8 : 4,
            .height = multisample ? 8 : 4, .format = srgb ? 0xd6 : 0xd5,
            .layers = array ? 2 : 1, .layer_stride = 512};
        state->tgt.msaa = multisample ? DS_MSAA_2X2 : 0;
        state->tgt.srgb_write = srgb;
        state->tgt.depth = depth ? (DsSurf){.gva = address + 0x8000, .width = multisample ? 8 : 4,
            .height = multisample ? 8 : 4, .format = 0x0a, .layers = 1, .layer_stride = 512} : (DsSurf){0};
        state->dyn.alpha_test = state->dyn.stencil_test = 0;
        state->dyn.depth_test = depth;
        state->dyn.depth_write = 0;
        state->dyn.depth_cmp = DS_CMP_LESS;
        state->rkey.sample_mask = ~0u;
        state->gen[DS_TARGETS] = state->gen[DS_TARGET_FORMATS] = state->gen[DS_DYNAMIC] =
            state->gen[DS_RASTER_KEY] = ds_generation_next();
        DrawClear clear = {.color_mask = 15, .aspects = depth ? 1 : 0, .depth = test == 4 ? 0.25f : 1};
        memcpy(clear.color, destination, sizeof destination);
        vk_render_clear(state, &clear, &channel->owner);
        DrawCall call = {.topology = 4, .count = 3, .instances = 1};
        if (test == 5) {
            /* Give samples0/2 depth0.5, leaving samples1/3 at1. The next LESS test must preserve only0/2. */
            state->dyn.depth_write = 1;
            state->rkey.sample_mask = 5;
            state->blend.en[0] = 0;
            state->blend.mask[0] = 0;
            state->gen[DS_DYNAMIC] = state->gen[DS_RASTER_KEY] = state->gen[DS_BLEND] = ds_generation_next();
            vk_render_draw(state, &call, &channel->owner);
            state->dyn.depth_write = 0;
            state->rkey.sample_mask = ~0u;
        }
        state->blend.en[0] = 1;
        state->blend.mask[0] = 15;
        state->gen[DS_DYNAMIC] = state->gen[DS_RASTER_KEY] = state->gen[DS_BLEND] = ds_generation_next();
        call.advanced_blend[0] = packed;
        u64 drawn = g_vk_drawn;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(g_vk_drawn == drawn + 1);
        vk_render_end();
        Surf* surface = vk_rt_find(address);
        CHECK(surface && surface->layers == (u32)(array ? 2 : 1));
        u8 pixels[256];
        if (multisample)
            CHECK(!ms_read_samples(surface, pixels));
        else
            CHECK(!ms_read_pixels(surface, 4, 4, pixels));
        float blended[4];
        nvn_advanced_blend_reference(NVN_BLEND_ADVANCED_MODE_OVERLAY, 1, 0, source, destination, blended);
        for (u32 sample = 0; sample < (multisample ? 4u : 1u); sample++) {
            int rejected = test == 4 || (test == 5 && (5u >> sample & 1));
            const float* expected = rejected ? destination : blended;
            const u8* pixel = multisample ? pixels + (2 + sample / 2) * 32 + (2 + sample % 2) * 4 : pixels + 20;
            for (u32 component = 0; component < 4; component++) {
                float value = expected[component];
                if (srgb && component < 3)
                    value = value <= 0.0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
                int want = (int)lroundf(value * 255), difference = (int)pixel[component] - want;
                if (difference < -2 || difference > 2) {
                    fprintf(stderr, "advanced target%u sample%u component%u: %u expected%d\n",
                            test, sample, component, pixel[component], want);
                    return 1;
                }
            }
        }
    }
    puts("PASS shader advanced targets: linear/sRGB, array image, four samples, depth and per-sample depth rejection exact=1");
    return 0;
}
