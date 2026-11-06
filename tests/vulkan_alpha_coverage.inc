/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Uniform alpha and a full-screen triangle remove geometry from the oracle. Undithered four-sample coverage
   contains sample i exactly when alpha >= (i+1)/4. Fixed and guest masks intersect that prefix. Alpha-to-one must
   change the stored alpha only after coverage uses the original alpha, without a native alphaToOne requirement.
   Read every sample of every pixel. */
static int alpha_coverage_program(u64 address, int guest_mask) {
    const u64 ops[] = {
        0x0100000000071000ull, /* R0=0 */
        0x0103f80000071001ull, /* R1=1 */
        0x0100000000071002ull, /* R2=0 */
        0x4c98078000070003ull | 3ull << 34 | 0x80ull << 20, /* R3=c[3][0x200]: original alpha. */
        0x4c98078000070004ull | 3ull << 34 | 0x81ull << 20, /* R4=c[3][0x204]: optional SampleMask. */
        0xe30000000007000full};
    u64 code[64] = {0};
    code[9] = 15 | (guest_mask ? 1ull << 32 : 0); /* SPH18 RGBA output, SPH19 SampleMask output after R0..R3. */
    u32 at = 10;
    for (u32 k = 0; k < sizeof ops / sizeof *ops; k++) {
        if (!(k % 3)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[k];
    }
    return invalidate_write_code(address, code, sizeof code / sizeof *code);
}
static int check_alpha_coverage(void) {
    if (!vk_ms_supported(VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, DS_MSAA_2X2)) {
        puts("SKIP undithered alpha coverage: four-sample RGBA8 is unavailable");
        return 0;
    }
    const u64 gpu = 0x8e0000000ull, cpu = HEAP_BASE + 0x11600000;
    const u64 constants = gpu + 0x3000, target = gpu + 0x4000;
    GpuChannel* channel = sample_raster_channel(gpu, cpu, 0);
    CHECK(channel);
    channel->id = 147;
    CHECK(alpha_coverage_program(gpu + 0x1000, 0));
    CHECK(alpha_coverage_program(gpu + 0x1400, 1));
    g_renderer->method_barrier(5);
    static const u32 alpha_words[] = {
        0x00000000, 0x3e000000, 0x3e7ef9db, 0x3e800000, 0x3f000000,
        0x3f400000, 0x3f7d70a4, 0x3f800000, 0x7fc00001}; /*0,.125,.249,.25,.5,.75,.99,1,NaN. */
    static const u32 fixed_masks[] = {15, 10, 5}, guest_masks[] = {15, 10, 5, 0};
    u32 draws = 0, samples_checked = 0;
    for (u32 to_one = 0; to_one < 2; to_one++)
        for (u32 guest = 0; guest < sizeof guest_masks / sizeof *guest_masks; guest++)
            for (u32 fixed = 0; fixed < sizeof fixed_masks / sizeof *fixed_masks; fixed++)
                for (u32 test = 0; test < sizeof alpha_words / sizeof *alpha_words; test++) {
                    u32 parameters[2] = {alpha_words[test], guest_masks[guest]};
                    CHECK(!storage_write_words(constants + 0x200, parameters, 2));
                    channel->r3d[0x851] = guest ? 0x1400 : 0x1000;
                    vk_lock();
                    ms_target(channel, target, 1);
                    const float blue[4] = {0, 0, 1, 1};
                    memcpy(channel->r3d + 0x360, blue, sizeof blue);
                    engine_clear(channel, 15 << 2);
                    channel->r3d_dirty = ~0u;
                    mx_draw_state_sync(channel);
                    DsState* state = &channel->ds;
                    state->bind.cb_addr[ST_FS][3] = constants;
                    state->bind.cb_size[ST_FS][3] = 0x400;
                    state->dyn.alpha_test = state->dyn.depth_test = state->dyn.stencil_test = 0;
                    state->blend.en[0] = 0; state->blend.mask[0] = 15;
                    state->rkey.alpha_to_coverage = 1;
                    state->rkey.alpha_to_one = (u8)to_one;
                    state->rkey.sample_shading = 0;
                    state->rkey.sample_mask = fixed_masks[fixed];
                    state->gen[DS_RASTER_KEY] = state->gen[DS_BLEND] = state->gen[DS_DYNAMIC] = ds_generation_next();
                    DrawCall call = {.topology = 4, .count = 3, .instances = 1, .multisample_flags = 0};
                    u64 before = g_vk_drawn;
                    vk_render_draw(state, &call, &channel->owner);
                    vk_render_end();
                    CHECK(g_vk_drawn == before + 1);
                    Surf* surface = vk_rt_find(target);
                    CHECK(surface && vk_surf_samples(surface) == VK_SAMPLE_COUNT_4_BIT);
                    u8 actual[256];
                    CHECK(!ms_read_samples(surface, actual));
                    float alpha;
                    memcpy(&alpha, &alpha_words[test], 4);
                    u32 expected_mask = 0;
                    for (u32 sample = 0; sample < 4; sample++)
                        if (alpha >= (float)(sample + 1) / 4) expected_mask |= 1u << sample;
                    expected_mask &= fixed_masks[fixed] & guest_masks[guest];
                    /* Vulkan's floating-point to normalized fixed-point conversion permits either adjacent
                       integer; an integral scaled value is exact. In particular alpha0.5 may store127 or128.
                       https://docs.vulkan.org/spec/latest/chapters/fundamentals.html#fundamentals-fixedfpconv */
                    float scaled_alpha = to_one ? 255 : expected_mask ? alpha * 255.0f : 0;
                    u8 alpha_low = (u8)scaled_alpha;
                    u8 alpha_high = alpha_low + (scaled_alpha > (float)alpha_low);
                    for (u32 y = 0; y < 4; y++)
                        for (u32 x = 0; x < 4; x++)
                            for (u32 sample = 0; sample < 4; sample++) {
                                const u8* value = actual + (2 * y + sample / 2) * 32 + (2 * x + sample % 2) * 4;
                                int covered = (int)(expected_mask >> sample & 1);
                                const u8 green[4] = {0, 255, 0, alpha_high};
                                const u8* expected = covered ? green : (const u8[4]){0, 0, 255, 255};
                                int matches = !memcmp(value, expected, 3) &&
                                    (covered ? value[3] >= alpha_low && value[3] <= alpha_high : value[3] == 255);
                                if (!matches)
                                    fprintf(stderr, "A2C alpha%08x alphaOne%u guest%#x fixed%#x pixel%u,%u sample%u: %u,%u,%u,%u expected%u,%u,%u,%u mask%#x\n",
                                            alpha_words[test], to_one, guest ? guest_masks[guest] : ~0u, fixed_masks[fixed],
                                            x, y, sample, value[0], value[1], value[2], value[3], expected[0], expected[1],
                                            expected[2], expected[3], expected_mask);
                                CHECK(matches);
                                samples_checked++;
                            }
                    vk_unlock();
                    draws++;
                }
    free(channel);
    printf("PASS undithered alpha coverage: %u draws/%u samples, thresholds, fixed/guest masks and alpha-to-one ordering exact=1\n",
           draws, samples_checked);
    return 0;
}
