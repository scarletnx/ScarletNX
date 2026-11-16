/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Virtual textures (vt_sparse, gpu/vk/vt.c): a 1024x1024 ASTC 8x8 texture of 11 levels, every block a void extent of
   its own colour (its level, column and row), so each texel of each level is known. An Ounce fragment program samples
   a corner of it over a 64x64 target, the nearest texel:
   - at level 0 (TEX.LZ), a quarter of the texture across: the first frame, with no block of level 0 held, reads a
     coarser level and no zeros; within 64 frames every pixel is its texel of level 0, and only the blocks under the
     corner and the ones that cover them in the coarser levels hold memory, a small part of the texture
   - at the level of its derivatives (four texels a pixel: level 2): every pixel its texel of level 2
   - with nothing sampling it for more than vt_frames presents, every block is let go, and a draw brings them back:
     an Ounce program's sample with gradients (TXD) of a pixel's own change its level 2, then TEX.LZ its level 0
   - fetched (TLD), which marks nothing: the texture is held whole from then on, and the fetch reads its texels
   On a texture held RGBA8 (astc_mode's Exact), exact, and on one held BC7, within 3 of each channel. Without vt_sparse, or
   on a device without sparse residency and shaderResourceResidency, the check is skipped. */
#define VTC_GPU 0x8f8000000ull
#define VTC_CPU (HEAP_BASE + 0x1b000000)
#define VTC_W   64u
#define VTC_TEX 1024u
enum { VTC_POOL = 0x8000, VTC_SAMPLERS = 0x9000, VTC_CONSTANTS = 0xa000, VTC_TARGET = 0x4000, VTC_TEXTURE = 0x100000 };

/* the colour of block (bx, by) of level l, as the target's bytes hold it: R, G, B, A */
static u32 vtc_colour(u32 l, u32 bx, u32 by) {
    return (16 + 8 * (bx % 28)) | (16 + 8 * (by % 28)) << 8 | (40 + 20 * l) << 16 | 255u << 24;
}
/* IPA R4, a[0x70]; IPA R5, a[0x74]; FMUL R4, R4, scale; FMUL R5, R5, scale; then TEX R0-R3, R4, R5, c[2][0x40] with
   the LOD mode lod (0 the derivatives', 1 level zero); with fetch 1, F2I.TRUNC of both and TLD.LZ, as sf_fragment;
   with fetch 2, TXD with the gradients (scale, 0) and (0, scale) from R8 to R11, a pixel's own change */
static int vtc_fragment(u32 at, float scale, u32 lod, int fetch) {
    Sm86Word code[12];
    u32 n = 0, bits;
    memcpy(&bits, &scale, 4);
    for (u32 k = 0; k < 2; k++) {
        code[n] = sm86_fix(0x326);
        sm86_set(&code[n], 16, 8, 4 + k);
        sm86_set(&code[n], 32, 8, 255);
        sm86_set(&code[n], 64, 8, (0x70 + 4 * k) >> 2);
        sm86_set(&code[n++], 81, 3, 7);
    }
    for (u32 k = 0; k < 2; k++) {
        if (fetch == 1) {
            code[n] = sm86_alu(0x105, 1, 4 + k, 255);
            sm86_set(&code[n], 32, 8, 4 + k);
            sm86_set(&code[n], 64, 8, 255);
            sm86_set(&code[n], 72, 1, 1);
            sm86_set(&code[n], 75, 2, 2);
            sm86_set(&code[n], 78, 2, 3);
            sm86_set(&code[n++], 84, 2, 2);
        } else {
            code[n] = sm86_alu(0x020, 4, 4 + k, 4 + k);
            sm86_set(&code[n], 32, 32, bits);
            sm86_set(&code[n], 84, 3, 4); /* no scale */
            sm86_set(&code[n++], 64, 8, 255);
        }
    }
    for (u32 k = 0; fetch == 2 && k < 4; k++) code[n++] = sm86_mov_imm(8 + k, k == 0 || k == 3 ? bits : 0);
    code[n] = sm86_fix(fetch == 1 ? 0xb66 : fetch == 2 ? 0xb6c : 0xb60);
    sm86_set(&code[n], 16, 8, 0);
    sm86_set(&code[n], 24, 8, 4);
    sm86_set(&code[n], 32, 8, fetch == 2 ? 8 : 255);
    sm86_set(&code[n], 40, 19, 2u << 14 | 0x40 >> 2);
    sm86_set(&code[n], 61, 3, 1);
    sm86_set(&code[n], 64, 8, 255);
    sm86_set(&code[n], 72, 4, 0xf);
    if (fetch != 2) sm86_set(&code[n], 87, 3, fetch ? 1 : lod);
    n++;
    code[n++] = sm86_exit();
    return sf_program(VTC_GPU + at, 2u | 4u << 5 | 5u << 10, 18, 0xf, code, n);
}
/* a present: the frame's work done, then the virtual textures' turn (the marks that landed, binds, loads) */
static void vtc_present(void) {
    vk_finish();
    vk_vt_frame();
}
/* the triangle over the target with the fragment program at VTC_GPU + program: its 64x64 pixels. Under vk_lock */
static int vtc_draw(GpuChannel* channel, u32 program, u8* pixels) {
    const u64 target = VTC_GPU + VTC_TARGET;
    u64 drawn = g_vk_drawn;
    const float transform[6] = {VTC_W / 2.0f, VTC_W / 2.0f, 0.5f, VTC_W / 2.0f, VTC_W / 2.0f, 0.5f};
    memcpy(channel->r3d + 0x280, transform, sizeof transform);
    channel->r3d[0x3fd] = VTC_W << 16;
    channel->r3d[0x3fe] = VTC_W << 16;
    channel->r3d[0x851] = program;
    rt_clear_format(channel, target, VTC_W, VTC_W, 0, 0, 0xD5);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.tex_pool = VTC_GPU + VTC_POOL;
    state->bind.smp_pool = VTC_GPU + VTC_SAMPLERS;
    state->bind.tex_max = 1;
    state->bind.smp_max = 0;
    state->bind.tex_bank = 2; /* a Maxwell TEX's handle is in the texture bank */
    state->bind.cb_addr[ST_FS][2] = state->bind.cb_addr[ST_VS][2] = VTC_GPU + VTC_CONSTANTS;
    state->bind.cb_size[ST_FS][2] = state->bind.cb_size[ST_VS][2] = 256;
    state->blend.en[0] = 0;
    state->blend.mask[0] = 15;
    state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = state->dyn.alpha_test = 0;
    state->rkey.sample_mask = ~0u;
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(state, &call, &channel->owner);
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(4ull * VTC_W * VTC_W, &buffer);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {VTC_W, VTC_W, 1}};
    CHECK(g_vk_drawn == drawn + 1);
    CHECK(surface && surface->sw == VTC_W && surface->sh == VTC_W && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(pixels, data, 4ull * VTC_W * VTC_W);
    return 0;
}
/* the pixels that aren't their texel of level l, a pixel `step` blocks of it apart (step 0: two pixels a block),
   each channel within tolerance: the first one said */
static u32 vtc_wrong(const u8* pixels, u32 l, u32 step, u32 tolerance, const char* what, int say) {
    u32 wrong = 0;
    for (u32 y = 0; y < VTC_W; y++)
        for (u32 x = 0; x < VTC_W; x++) {
            const u32 want = step ? vtc_colour(l, x / step, y / step) : vtc_colour(l, x / 2, y / 2);
            const u32 got = snx_ld_le32(pixels + (y * VTC_W + x) * 4);
            u32 off = 0;
            for (u32 c = 0; c < 4; c++) {
                const u32 a = got >> (8 * c) & 255, b = want >> (8 * c) & 255;
                if ((a > b ? a - b : b - a) > tolerance) off = 1;
            }
            if (off && !wrong++ && say)
                fprintf(stderr, "virtual texture, %s: pixel (%u, %u) holds %08x, not its texel of level %u, %08x\n",
                        what, x, y, got, l, want);
        }
    return wrong;
}
/* draws with program until every pixel is its texel of level l (vtc_wrong), a present after each draw: the draws it
   took, 0 when 64 were not enough */
static u32 vtc_until(GpuChannel* channel, u32 program, u8* pixels, u32 l, u32 step, u32 tolerance, const char* what) {
    for (u32 frame = 1; frame <= 64; frame++) {
        if (vtc_draw(channel, program, pixels)) return 0;
        vtc_present();
        if (!vtc_wrong(pixels, l, step, tolerance, what, 0)) return frame;
    }
    vtc_wrong(pixels, l, step, tolerance, what, 1);
    return 0;
}
/* the checks on a texture of its own (pass), held as astc_mode's Exact holds it (RGBA8) or as Auto does (BC7 here: its
   blocks are flat colours) */
static int vtc_pass(GpuChannel* channel, u32 pass, int exact, int maxwell) {
    const u32 base = maxwell ? 0x2400 - 0x800 : 0; /* the Maxwell programs lie that far past the Ounce ones */
    g_settings.astc_mode = exact ? 1 : 0;
    g_shader_sm86 = !maxwell;
    channel->r3d[0x811] = maxwell ? 0x2200 : 0; /* the vertex program */
    /* the texture: each level's blocks, void extents of vtc_colour */
    NvnTexture image = {.gpu = VTC_GPU + VTC_TEXTURE + pass * 0x100000ull};
    image.d.target = NVN_TEXTURE_TARGET_2D;
    image.d.format = NVN_FORMAT_RGBA_ASTC_8x8;
    image.d.width = image.d.height = VTC_TEX;
    image.d.depth = 1;
    image.d.levels = 11;
    NvnTexLayout layout;
    nvn_tex_layout(&image.d, &layout);
    const u64 span = (layout.size + 0xFFFF) & ~0xFFFFull;
    CHECK(span <= 0x100000 && !mem_map(VTC_CPU + VTC_TEXTURE + pass * 0x100000ull, span, MS_NORMAL, PERM_RW));
    gmmu_map(image.gpu, VTC_CPU + VTC_TEXTURE + pass * 0x100000ull, span);
    for (u32 l = 0; l < 11; l++) {
        const u32 size = VTC_TEX >> l ? VTC_TEX >> l : 1, blocks = (size + 7) / 8;
        unsigned tile_h, tile_d;
        nvn_tex_level_tiles(&image.d, l, &tile_h, &tile_d);
        u8* level = GP(VTC_CPU + VTC_TEXTURE + pass * 0x100000ull) + nvn_tex_level_offset(&image.d, l);
        for (u32 by = 0; by < blocks; by++)
            for (u32 bx = 0; bx < blocks; bx++) {
                u8* b = level + bl_offset(bx * 16, by, 0, blocks * 16, blocks, tile_h, tile_d);
                const u32 colour = vtc_colour(l, bx, by);
                static const u8 head[8] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
                memcpy(b, head, 8);
                for (u32 c = 0; c < 4; c++) b[8 + 2 * c] = b[9 + 2 * c] = (u8)(colour >> (8 * c));
            }
    }
    u32 tic[8];
    CHECK(nvn_tic_encode(&image, NULL, 0, 0, tic));
    CHECK(!storage_write_words(VTC_GPU + VTC_POOL + 32, tic, 8));
    /* clamped to the edge, the nearest texel of the nearest level, every level allowed */
    CHECK(!storage_write_words(VTC_GPU + VTC_SAMPLERS,
                               (const u32[8]){2u | 2u << 3 | 2u << 6, 1u | 1u << 4 | 2u << 6, 0xfffu << 12}, 8));

    static u8 pixels[4 * VTC_W * VTC_W];
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    CHECK(t && t->upload_valid);
    if (!t->vt) {
        printf("skip: the device makes no sparse image with a mip tail of the format a 1024x1024 ASTC texture is "
               "held in (%d)\n",
               (int)t->f.vk);
        vk_unlock();
        return 0;
    }
    const int bc7 = t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK;
    const u32 tolerance = bc7 ? 3 : 0;
    if (bc7 != !exact)
        fprintf(stderr, "virtual texture: held in format %d with astc_mode=%d\n", (int)t->f.vk, exact ? 1 : 0);
    CHECK(bc7 == !exact && (bc7 || t->f.vk == VK_FORMAT_R8G8B8A8_UNORM));
    const VkSparseShape* shape = vk_image_sparse_shape(t->img);
    CHECK(shape && shape->tail_first);
    /* the levels of blocks to page: those of a block or more each way (the others are held always) */
    u32 paged = 0, all = 0;
    while (paged < shape->tail_first && VTC_TEX >> paged >= shape->gran.width)
        all += shape->bx[paged] * shape->by[paged], paged++;
    CHECK(paged && paged < 11);
    VkVtStats s;
    vk_vt_stats(t, &s);
    const u64 tail_bytes = t->bytes;
    /* the levels held always: a device with no mip tail gives each of the eight under a block 64 KiB */
    const u64 base_bytes = (bc7 ? 1u : 4u) * VTC_TEX * VTC_TEX;
    if (!s.textures || s.whole || s.held || !tail_bytes || tail_bytes > base_bytes / 2)
        fprintf(stderr,
                "virtual texture, made: %u virtual textures, %u whole, %u blocks held, %llu bytes (the levels held "
                "always: half of level 0's %llu at most)\n",
                s.textures, s.whole, s.held, (unsigned long long)tail_bytes, (unsigned long long)base_bytes);
    CHECK(s.textures >= 1 && !s.whole && !s.held && tail_bytes && tail_bytes <= base_bytes / 2);
    /* the first frame: no block of level 0 has memory, and the sample reads a coarser level's texel, of full alpha */
    CHECK(!vtc_draw(channel, base + 0x800, pixels));
    vtc_present();
    u32 zeros = 0, level0 = VTC_W * VTC_W - vtc_wrong(pixels, 0, 0, tolerance, "", 0);
    for (u32 i = 0; i < VTC_W * VTC_W; i++)
        zeros += pixels[4 * i + 3] != 255 || pixels[4 * i + 2] < 40 + 20 - tolerance;
    if (zeros || level0)
        fprintf(stderr,
                "virtual texture, the first frame: %u of %u pixels read no texel of a coarser level (alpha 255, blue "
                "of level 1 or more), %u were level 0's already; pixel (0, 0) holds %08x\n",
                zeros, VTC_W * VTC_W, level0, snx_ld_le32(pixels));
    CHECK(!zeros && !level0);
    /* then level 0's texels, with memory for the corner alone */
    const u32 took = vtc_until(channel, base + 0x800, pixels, 0, 0, tolerance, "level 0 (TEX.LZ)");
    CHECK(took);
    vk_vt_stats(t, &s);
    u32 corner = 0; /* the blocks under a quarter of each level across and down */
    for (u32 l = 0; l < paged; l++) {
        const u32 x = ((VTC_TEX >> l) / 4 + shape->gran.width - 1) / shape->gran.width;
        corner += x * x;
    }
    if (s.whole || s.held != corner || t->bytes <= tail_bytes)
        fprintf(stderr,
                "virtual texture: after the corner's samples %u of its %u blocks hold memory (%u wanted: the ones "
                "under a quarter of each level), %llu bytes (the tail alone %llu), held whole %u\n",
                s.held, all, corner, (unsigned long long)t->bytes, (unsigned long long)tail_bytes, s.whole);
    CHECK(!s.whole && s.held == corner && t->bytes > tail_bytes);
    /* the level of the derivatives: four texels a pixel, level 2, a texel a pixel */
    const u32 took2 = vtc_until(channel, base + 0x1000, pixels, 2, 8, tolerance, "the derivatives' level (TEX)");
    CHECK(took2);
    /* nothing samples it: past vt_frames presents every block is let go */
    for (u32 i = 0; i < 32 + 2 * 8 + 4; i++) vtc_present();
    vk_vt_stats(t, &s);
    if (s.held || t->bytes != tail_bytes)
        fprintf(stderr,
                "virtual texture: %u blocks and %llu bytes held after 52 presents with no sample (0 and %llu)\n",
                s.held, (unsigned long long)t->bytes, (unsigned long long)tail_bytes);
    CHECK(!s.held && t->bytes == tail_bytes);
    /* an Ounce program's sample with gradients (TXD), a pixel's own: level 2 again, from nothing */
    const u32 took_txd =
        maxwell ? 1 : vtc_until(channel, 0xb000, pixels, 2, 8, tolerance, "the gradients' level (TXD)");
    CHECK(took_txd);
    const u32 took3 = vtc_until(channel, base + 0x800, pixels, 0, 0, tolerance, "level 0 again");
    CHECK(took3);
    /* a fetch marks nothing: the texture is held whole, and the fetch reads level 0's texel (x, y) */
    u32 took4 = 0;
    for (u32 frame = 1; frame <= 8 && !took4; frame++) {
        CHECK(!vtc_draw(channel, base + 0x1800, pixels));
        vtc_present();
        if (!vtc_wrong(pixels, 0, 8, tolerance, "", 0)) took4 = frame;
    }
    if (!took4) vtc_wrong(pixels, 0, 8, tolerance, "fetched (TLD)", 1);
    vk_vt_stats(t, &s);
    CHECK(took4 && s.whole == 1 && s.held == all);
    vk_unlock();
    printf(
        "PASS virtual texture, %s program: a 1024x1024 ASTC texture held %s, a corner sampled: a coarser level in the first "
        "frame, level 0's texels after %u draws with %u of %u blocks held, the derivatives' level after %u more, "
        "every block let go unsampled, back after %u draws, whole once fetched (%u draws)\n",
        maxwell ? "a Maxwell" : "an Ounce", bc7 ? "BC7" : "RGBA8", took, corner, all, took2, took3, took4);
    return 0;
}
static int check_vt(void) {
    if (!vk_vt_on()) {
        printf("skip: virtual textures are off (vt_sparse=0, or the device has no sparse residency for them)\n");
        return 0;
    }
    GpuChannel* channel = sample_raster_channel(VTC_GPU, VTC_CPU, 0);
    CHECK(channel);
    Sm86Word code[4];
    code[0] = sm86_fix(0x321); /* the vertex program: ALD R0-R3, a[0x80]; AST a[0x70], R0-R3 */
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
    CHECK(!sf_program(VTC_GPU, 0x000c0481u, 13, 0xfu << 12, code, 3));
    /* a quarter of the texture over the target: four texels of level 0 a pixel */
    CHECK(!vtc_fragment(0x800, 1.0f / 256, 1, 0) && !vtc_fragment(0x1000, 1.0f / 256, 0, 0) &&
          !vtc_fragment(0x1800, 0, 0, 1) && !vtc_fragment(0xb000, 1.0f / 256, 0, 2));
    /* the same as Maxwell programs, with sample_raster_channel's vertex program: IPA R4, a[0x70]; IPA R5, a[0x74];
       FMUL of both by the scale; TEX R0, R4, R5 by the handle at c[2][0x40], at level zero (.LZ) or the derivatives';
       and sf_maxwell_fragment's fetch (F2I.TRUNC, LDC, TLD.B.LZ) */
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xefd9ff800907ff00ull, 0xeff1ff800807ff00ull,
                          0xe30000000007000full};
    CHECK(mesh_program(VTC_GPU + 0x2200, vertex, 5, 0));
    const float quarter = 1.0f / 256;
    u32 quarter_bits;
    memcpy(&quarter_bits, &quarter, 4);
    for (u32 k = 0; k < 2; k++) {
        u64 ops[6];
        for (u32 axis = 0; axis < 2; axis++) {
            ops[axis] = 0xe000000000070000ull | (0x1cull + axis) << 30 | (4 + axis);
            ops[2 + axis] = 0x1e00000000070000ull | (u64)quarter_bits << 20 | (4 + axis) << 8 | (4 + axis);
        }
        ops[4] = 0xc038000000000000ull | 16ull << 36 | 7ull << 51 | (u64)!k << 55 | 0xfull << 31 | 2ull << 28 |
                 16ull << 20 | 7ull << 16 | 4ull << 8;
        ops[5] = 0xe30000000007000full;
        CHECK(mesh_program(VTC_GPU + 0x2400 + k * 0x800, ops, 6, 1));
    }
    const u64 fetch[] = {0xe000000000070000ull | 0x1cull << 30 | 2,
                         0xe000000000070000ull | 0x1dull << 30 | 3,
                         0x5cb0000000070000ull | 3ull << 39 | 2ull << 20 | 2ull << 10 | 2ull << 8 | 2,
                         0x5cb0000000070000ull | 3ull << 39 | 3ull << 20 | 2ull << 10 | 2ull << 8 | 3,
                         0xef9400200007ff04ull | 0x40ull << 20,
                         0xdd380007a0470200ull,
                         0xe30000000007000full};
    CHECK(mesh_program(VTC_GPU + 0x3400, fetch, 7, 1));
    g_renderer->method_barrier(5);
    u8 table[256] = {0};
    snx_st_le32(table + 0x40, 1);
    CHECK(!gmem_write(VTC_GPU + VTC_CONSTANTS, table, sizeof table));
    const int sm86 = g_shader_sm86, frames = g_settings.vt_frames, mode = g_settings.astc_mode;
    g_shader_sm86 = 1;
    g_settings.vt_frames = 32;
    const int failed = vtc_pass(channel, 0, 1, 0) || vtc_pass(channel, 1, 0, 0) || vtc_pass(channel, 2, 1, 1);
    channel->r3d[0x811] = 0;
    g_shader_sm86 = sm86;
    g_settings.vt_frames = frames;
    g_settings.astc_mode = mode;
    free(channel);
    return failed;
}
