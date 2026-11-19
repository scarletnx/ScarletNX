/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce fragment program's colour into an integer target (sm86_translate.c's epilogue): a triangle over all of a
   4x4 target of R8G8B8A8_UINT, R32_SINT and R8_UINT (one game's mesh draws fill one of those), whose program moves its
   colour's bits into R0-R3. Every pixel holds those bits, and the validation layer, when the run turns it on, has
   nothing to say: a float output's values are undefined in an integer attachment, and the layer reported each of the
   game's draws, where Mesa's drivers happened to keep the bits */
#define OI_GPU 0x9f0000000ull
#define OI_CPU (HEAP_BASE + 0x3ca00000)

static int check_ounce_integer_targets(void) {
    static const struct {
        u32 format, bytes, bits[4];
        const char* name;
    } cases[3] = {{0xD9, 4, {0x12, 0x34, 0x56, 0x78}, "R8G8B8A8_UINT"},
                  {0xE3, 4, {0xfffffffeu, 7, 9, 11}, "R32_SINT"},
                  {0xF6, 1, {5, 6, 7, 8}, "R8_UINT"}};
    /* sample_raster_channel gives the triangle over a 4x4 target, its position the first attribute, and maps the
       memory */
    GpuChannel* channel = sample_raster_channel(OI_GPU, OI_CPU, 0);
    CHECK(channel);
    if (!channel) return 1;
    /* the vertex program: ALD R0-R3, a[0x80]; AST a[0x70], R0-R3 */
    Sm86Word code[8];
    code[0] = sm86_fix(0x321);
    sm86_set(&code[0], 16, 8, 0);
    sm86_set(&code[0], 24, 8, 255);
    sm86_set(&code[0], 32, 8, 255);
    sm86_set(&code[0], 40, 10, 0x80);
    sm86_set(&code[0], 74, 2, 3);
    code[1] = sm86_fix(0x322);
    sm86_set(&code[1], 24, 8, 255);
    sm86_set(&code[1], 32, 8, 0);
    sm86_set(&code[1], 64, 8, 255);
    sm86_set(&code[1], 40, 10, 0x70);
    sm86_set(&code[1], 74, 2, 3);
    code[2] = sm86_exit();
    CHECK(!sf_program(OI_GPU + 0x3000, 0x000c0481u, 13, 0xfu << 12, code, 3)); /* the output map's position */
    /* a fragment program for each case: MOV R0-R3 of its bits, render target 0's RGBA */
    for (u32 i = 0; i < 3; i++) {
        for (u32 k = 0; k < 4; k++) code[k] = sm86_mov_imm(k, cases[i].bits[k]);
        code[4] = sm86_exit();
        CHECK(!sf_program(OI_GPU + 0x3400 + 0x400 * i, 2u | 4u << 5 | 5u << 10, 18, 0xf, code, 5));
    }
    g_renderer->method_barrier(5); /* other code at the addresses */
    int sm86 = g_shader_sm86, bad = 0;
    u32 validation = g_vk_validation_messages;
    u32* r = channel->r3d;
    g_shader_sm86 = 1;
    r[0x811] = 0x3000;
    vk_lock();
    for (u32 i = 0; i < 3; i++) {
        const u64 target = OI_GPU + 0x4000 + 0x1000 * i;
        r[0x851] = 0x3400 + 0x400 * i;
        rt_clear_format(channel, target, 4, 4, 0, 0, cases[i].format);
        channel->r3d_dirty = ~0u; /* the registers were set directly */
        mx_draw_state_sync(channel);
        DsState* state = &channel->ds;
        state->blend = (DsBlend){0};
        state->blend.mask[0] = 15;
        state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = state->dyn.alpha_test = 0;
        state->rkey.sample_mask = ~0u;
        DrawCall call = {.topology = 4, .count = 3, .instances = 1};
        u64 drawn = g_vk_drawn;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(g_vk_drawn == drawn + 1);
        vk_render_end();
        Surf* surface = vk_rt_find(target);
        CHECK(surface && surface->f.is_int && surface->f.bpp == cases[i].bytes);
        if (!surface) continue;
        u8 pixels[64];
        CHECK(!ms_read_pixels(surface, 4, 4, pixels));
        for (u32 p = 0; p < 16; p++) {
            const u8* at = pixels + p * cases[i].bytes;
            u32 got = cases[i].bytes == 4 ? snx_ld_le32(at) : at[0];
            u32 want = cases[i].format == 0xD9
                           ? cases[i].bits[0] | cases[i].bits[1] << 8 | cases[i].bits[2] << 16 | cases[i].bits[3] << 24
                           : cases[i].bits[0];
            if (got != want && bad++ < 4)
                fprintf(stderr,
                        "Ounce integer targets: pixel %u,%u of an %s target holds %08x, not the program's %08x\n",
                        p % 4, p / 4, cases[i].name, got, want);
        }
    }
    vk_unlock();
    CHECK(!bad);
    CHECK(g_vk_validation_messages == validation);
    if (g_vk_validation_messages != validation)
        fprintf(stderr, "Ounce integer targets: %u validation messages\n", g_vk_validation_messages - validation);
    g_shader_sm86 = sm86;
    free(channel);
    puts("PASS headless Vulkan Ounce integer targets: R8G8B8A8_UINT, R32_SINT and R8_UINT hold the program's bits");
    return 0;
}
