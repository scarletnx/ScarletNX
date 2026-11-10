/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce fragment program at a resolution scale of 2: a triangle over all of an 8x8 target, whose image is 16x16,
   and a program that fetches (TLD) a texture at its fragment's own coordinates, as a pass that takes another's
   result a pixel for a pixel does. The fragment's position is in the guest's pixels:
   - an 8x8 texture from guest memory: pixel (x, y) of the target's image holds its texel (x / 2, y / 2). With the
     position in the image's pixels, the texture filled the top-left quarter of the image, and the rest was fetched
     past its edge
   - another 8x8 target, which a draw filled with each fragment's position: pixel (x, y) holds that target's pixel
     (x, y), the one where the fragment lies in its guest pixel, not one pixel for the four of a guest pixel */
#define SF_GPU 0x870000000ull
#define SF_CPU (HEAP_BASE + 0x5540000)
#define SF_W   8u
enum { SF_POOL = 0x8000, SF_SAMPLERS = 0x9000, SF_CONSTANTS = 0xa000 };

/* an Ounce program at gva: its SPH, word 0 sph0 and one more of its words, and its code */
static int sf_program(u64 gva, u32 sph0, u32 index, u32 word, const Sm86Word* code, u32 n) {
    u8 bytes[SM86_SPH_BYTES + 16 * 16] = {0};
    snx_st_le32(bytes, sph0);
    snx_st_le32(bytes + index * 4, word);
    for (u32 i = 0; i < n; i++) sm86_put(bytes + SM86_SPH_BYTES, i, code[i]);
    sm86_put(bytes + SM86_SPH_BYTES, n, sm86_bra(n, n, 7, 0)); /* the BRA to itself after the EXIT */
    return gmem_write(gva, bytes, SM86_SPH_BYTES + 16 * (n + 1));
}
/* a fragment program at SF_GPU + at, render target 0 from R0-R3: IPA R4, a[0x70]; IPA R5, a[0x74], the fragment's x
   and y, into R0 and R1 instead with no handle; then F2I.TRUNC of both and TLD.LZ R0-R3, R4, R5, c[2][handle] */
static int sf_fragment(u32 at, u32 handle) {
    Sm86Word code[8];
    u32 n = 0;
    for (u32 k = 0; k < 2; k++) {
        code[n] = sm86_fix(0x326);
        sm86_set(&code[n], 16, 8, (handle ? 4 : 0) + k);
        sm86_set(&code[n], 32, 8, 255);
        sm86_set(&code[n], 64, 8, (0x70 + 4 * k) >> 2);
        sm86_set(&code[n++], 81, 3, 7);
    }
    for (u32 k = 0; handle && k < 2; k++) {
        code[n] = sm86_alu(0x105, 1, 4 + k, 255);
        sm86_set(&code[n], 32, 8, 4 + k);
        sm86_set(&code[n], 64, 8, 255);
        sm86_set(&code[n], 72, 1, 1);
        sm86_set(&code[n], 75, 2, 2);
        sm86_set(&code[n], 78, 2, 3);
        sm86_set(&code[n++], 84, 2, 2);
    }
    if (handle) {
        code[n] = sm86_fix(0xb66);
        sm86_set(&code[n], 16, 8, 0);
        sm86_set(&code[n], 24, 8, 4);
        sm86_set(&code[n], 32, 8, 255);
        sm86_set(&code[n], 40, 19, 2u << 14 | handle >> 2);
        sm86_set(&code[n], 61, 3, 1);
        sm86_set(&code[n], 64, 8, 255);
        sm86_set(&code[n], 72, 4, 0xf);
        sm86_set(&code[n++], 87, 3, 1);
    }
    code[n++] = sm86_exit();
    return sf_program(SF_GPU + at, 2u | 4u << 5 | 5u << 10, 18, 0xf, code, n); /* render target 0's RGBA */
}
/* the triangle over all of the SF_W x SF_W target of the 3D engine's format, cleared first, with the fragment program
   at SF_GPU + program: every pixel of the target's image into pixels, texel bytes each. Under vk_lock */
static int sf_draw(GpuChannel* channel, u64 target, u32 format, u32 program, u8* pixels, u32 texel) {
    u64 drawn = g_vk_drawn, bytes = 4ull * SF_W * SF_W * texel;
    channel->r3d[0x851] = program;
    rt_clear_format(channel, target, SF_W, SF_W, 0, 0, format);
    channel->r3d_dirty = ~0u; /* the registers were set directly */
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.tex_pool = SF_GPU + SF_POOL;
    state->bind.smp_pool = SF_GPU + SF_SAMPLERS;
    state->bind.tex_max = 2;
    state->bind.smp_max = 0;
    state->bind.cb_addr[ST_FS][2] = SF_GPU + SF_CONSTANTS;
    state->bind.cb_size[ST_FS][2] = 256;
    state->blend.en[0] = 0;
    state->blend.mask[0] = 15;
    state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = state->dyn.alpha_test = 0;
    state->rkey.sample_mask = ~0u;
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(state, &call, &channel->owner);
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(bytes, &buffer);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {2 * SF_W, 2 * SF_W, 1}};
    CHECK(g_vk_drawn == drawn + 1);
    CHECK(surface && surface->sw == 2 * SF_W && surface->sh == 2 * SF_W && surface->f.bpp == texel && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(pixels, data, bytes);
    return 0;
}
static u32 sf_texel(u32 x, u32 y) { return (10 + 30 * x) | (20 + 30 * y) << 8 | (x + SF_W * y) << 16 | 255u << 24; }
static int check_scaled_fragment(void) {
    const u64 target = SF_GPU + 0x4000, texture = SF_GPU + 0x6000, positions = SF_GPU + 0xc000, copy = SF_GPU + 0xe000;
    const u32 rgba32f = 0x01 | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    /* sample_raster_channel gives the triangle, its position the first attribute, and maps the memory */
    GpuChannel* channel = sample_raster_channel(SF_GPU, SF_CPU, 0);
    CHECK(channel);
    Sm86Word code[4];
    /* the vertex program: ALD R0-R3, a[0x80]; AST a[0x70], R0-R3 */
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
    CHECK(!sf_program(SF_GPU, 0x000c0481u, 13, 0xfu << 12, code, 3)); /* the output map's position */
    /* the fragment programs: the fetch of the texture, the fragment's position, the fetch of the positions' target */
    CHECK(!sf_fragment(0x800, 0x40) && !sf_fragment(0x1000, 0) && !sf_fragment(0x1800, 0x44));
    g_renderer->method_barrier(5); /* other code at the addresses */
    u8 texels[512] = {0}, table[256] = {0};
    for (u32 y = 0; y < SF_W; y++)
        for (u32 x = 0; x < SF_W; x++)
            snx_st_le32(texels + bl_offset(x * 4, y, 0, SF_W * 4, SF_W, 0, 0), sf_texel(x, y));
    CHECK(!gmem_write(texture, texels, sizeof texels));
    /* header 1: the texture, RGBA8; header 2: the positions' target, RGBA32F */
    NvnTexture image = {.gpu = texture};
    image.d.target = NVN_TEXTURE_TARGET_2D;
    image.d.format = NVN_FORMAT_RGBA8;
    image.d.width = image.d.height = SF_W;
    image.d.depth = image.d.levels = 1;
    u32 tic[8];
    CHECK(nvn_tic_encode(&image, NULL, 0, 0, tic));
    CHECK(!storage_write_words(SF_GPU + SF_POOL + 32, tic, 8));
    CHECK(!storage_write_words(SF_GPU + SF_POOL + 64,
                               (const u32[8]){rgba32f, (u32)positions, (u32)(positions >> 32) | 3u << 21, 0,
                                              (SF_W - 1) | 1u << 23, SF_W - 1, 0, 0},
                               8));
    CHECK(!storage_write_words(SF_GPU + SF_SAMPLERS, (const u32[8]){2u | 2u << 3 | 2u << 6}, 8));
    snx_st_le32(table + 0x40, 1); /* the handles: a header each, sampler 0 */
    snx_st_le32(table + 0x44, 2);
    CHECK(!gmem_write(SF_GPU + SF_CONSTANTS, table, sizeof table));
    u32* r = channel->r3d;
    const float transform[6] = {SF_W / 2, SF_W / 2, 0.5f, SF_W / 2, SF_W / 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x3fd] = r[0x3fe] = SF_W << 16;
    float scale = g_vk_scale;
    int sm86 = g_shader_sm86, bad = 0;
    u8 pixels[4 * SF_W * SF_W * 16];
    g_vk_scale = 2.0f;
    g_shader_sm86 = 1;
    vk_lock();
    CHECK(!sf_draw(channel, target, 0xD5, 0x800, pixels, 4));
    for (u32 y = 0; y < 2 * SF_W; y++)
        for (u32 x = 0; x < 2 * SF_W; x++) {
            u32 pixel = snx_ld_le32(pixels + (y * 2 * SF_W + x) * 4);
            if (pixel != sf_texel(x / 2, y / 2) && bad++ < 4)
                fprintf(stderr,
                        "scaled fragment: pixel %u,%u of the %ux%u image of a target at 2x holds %08x, not the "
                        "texture's texel %u,%u, %08x\n",
                        x, y, 2 * SF_W, 2 * SF_W, pixel, x / 2, y / 2, sf_texel(x / 2, y / 2));
        }
    CHECK(!bad);
    /* the positions, then their fetch: both targets hold each pixel's own position, in the guest's pixels */
    for (u32 pass = 0; pass < 2; pass++) {
        CHECK(!sf_draw(channel, pass ? copy : positions, 0xC0, pass ? 0x1800 : 0x1000, pixels, 16));
        for (u32 y = 0; y < 2 * SF_W; y++)
            for (u32 x = 0; x < 2 * SF_W; x++) {
                float at[2];
                memcpy(at, pixels + (y * 2 * SF_W + x) * 16, sizeof at);
                if ((at[0] != ((float)x + 0.5f) / 2 || at[1] != ((float)y + 0.5f) / 2) && bad++ < 4)
                    fprintf(stderr, "scaled fragment: pixel %u,%u of a target at 2x %s (%g, %g), not (%g, %g)\n", x, y,
                            pass ? "fetched from the target of the positions its pixel" : "has the position",
                            (double)at[0], (double)at[1], ((double)x + 0.5) / 2, ((double)y + 0.5) / 2);
            }
        CHECK(!bad);
    }
    vk_unlock();
    g_shader_sm86 = sm86;
    g_vk_scale = scale;
    free(channel);
    puts("scaled fragment: an Ounce fragment program at 2x fetches a texture and a target at its own pixel");
    return 0;
}
