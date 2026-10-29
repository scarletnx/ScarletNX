/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Multisampled render targets and textures (gpu/vk/msaa.c): a 4x4 target of the 3D engine's 2x2 sample layout
   (MSAA mode 2, its registers' sizes 8x8 samples) cleared blue and drawn with a green triangle over pixels (0, 0),
   (4, 0), (0, 4). Its samples land in guest memory as the console lays them out, sample i of pixel (x, y) at texel
   (2 x + i % 2, 2 y + i / 2): with the standard sample locations ((0.375, 0.125), (0.875, 0.375), (0.125, 0.625),
   (0.625, 0.875)) a sample is green when x + y plus its offsets is below 4, so the pixels where x + y is 3 have
   samples 0 and 2 green and 1 and 3 blue. The resolve (vkCmdResolveImage, through vk_blit2d as the 2D engine and
   NVN's Downsample reach it) averages them; guest memory's samples load back into the image bit for bit (a pattern
   of every byte value, NaNs included, through the raw views); a texture header of the sample layout at the target
   binds the target, one at other memory loads its samples from there, and a single-sampled header of the samples'
   size over the target takes them as the guest's layout of them. Each in block linear and pitch linear memory */

/* the 4x MSAA sample offsets Vulkan's standard locations put in a pixel, in 1/8 pixel */
static const u8 g_ms_sx[4] = {3, 7, 1, 5}, g_ms_sy[4] = {1, 3, 5, 7};
/* the triangle's color at sample s of pixel (x, y): green inside x + y < 4, else the clear's blue */
static int ms_green(u32 x, u32 y, u32 s) { return 8 * (x + y) + g_ms_sx[s] + g_ms_sy[s] < 32; }

/* texel (x, y) of the 8x8 RGBA8 surface at bytes p: one GOB in block linear, rows of 32 bytes in pitch linear */
static const u8* ms_texel(const u8* p, int bl, u32 x, u32 y) {
    return p + (bl ? gob_x_offset(x * 4) + gob_y_offset(y) : y * 32 + x * 4);
}

/* the target at gva of 4x4 pixels in the 2x2 sample layout, in block linear (one GOB high blocks) or pitch linear
   memory: registers in samples */
static void ms_target(GpuChannel* ch, u64 gva, int bl) {
    u32* r = ch->r3d;
    r[0x574] = DS_MSAA_2X2;
    r[0x487] = 1;
    r[0x200] = (u32)(gva >> 32);
    r[0x201] = (u32)gva;
    r[0x202] = bl ? 8 : 32; /* a pitch-linear target's width is its row pitch in bytes */
    r[0x203] = 8;
    r[0x204] = 0xD5; /* A8B8G8R8 */
    r[0x205] = bl ? 0 : 0x1000;
    r[0x206] = 1;
    r[0x207] = 0;
}

/* the surface's samples unpacked into the guest's 8x8 layout of them and read back into out, rows of 32 bytes */
static int ms_read_samples(Surf* s, u8* out) {
    VkBuffer buffer;
    u8* data = vk_readback_buffer(256, &buffer);
    CHECK(data);
    VkCommandBuffer cb = vk_cmd();
    VkImage grid = vk_ms_unpack(cb, s, 0);
    CHECK(grid);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {8, 8, 1}};
    rec_CmdCopyImageToBuffer(cb, grid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, data, 256);
    return 0;
}
/* a single-sampled surface's w x h RGBA8 pixels, layer 0, read back into out */
static int ms_read_pixels(Surf* s, u32 w, u32 h, u8* out) {
    VkBuffer buffer;
    u8* data = vk_readback_buffer((u64)w * h * 4, &buffer);
    CHECK(data && !s->msaa);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, data, (size_t)w * h * 4);
    return 0;
}

/* the 8x8 samples at p (rows of 32 bytes, or the guest's layout of them in memory: in) are the triangle's */
static int ms_samples_drawn(const u8* p, int bl, int in_memory) {
    static const u8 green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
    int mixed = 0;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            u32 n = 0;
            for (u32 s = 0; s < 4; s++) {
                u32 tx = 2 * x + s % 2, ty = 2 * y + s / 2;
                const u8* t = in_memory ? ms_texel(p, bl, tx, ty) : p + ty * 32 + tx * 4;
                const u8* want = !g_vk.props.limits.standardSampleLocations ? (memcmp(t, green, 4) ? blue : green)
                                 : ms_green(x, y, s)                         ? green
                                                                             : blue;
                if (memcmp(t, want, 4)) {
                    fprintf(stderr, "msaa: pixel (%u, %u) sample %u is %u %u %u %u\n", x, y, s, t[0], t[1], t[2], t[3]);
                    return 0;
                }
                n += t[1] != 0;
            }
            mixed += n && n < 4;
        }
    return mixed > 0; /* some pixels have samples of both colors: the target has samples */
}

/* the 8x8 bytes pattern: every texel's bytes different, through all values (NaN and denormal floats in them) */
static void ms_pattern(u8* p) {
    for (u32 i = 0; i < 256; i++) p[i] = (u8)(i * 7 + 0x80);
}

/* A program's fetch of each sample of the multisampled texture tic at its pixel (TLDS.MS, translated as OpImageFetch
   with a Sample operand): four draws over a 4x4 single-sampled target, draw k writing sample k of each pixel, compared
   with the samples (rows of 32 bytes, the guest's layout of them). The texture header is entry 0 of the pool at base +
   0x1C000, its handle (0: header 0, sampler 0) in the fragment stage's constant buffer 2 at 0x40 */
static int ms_fetch(GpuChannel* ch, u64 base, const u32 tic[8], const u8* samples) {
    u32* r = ch->r3d;
    const u64 pool = base + 0x1C000, samplers = pool + 0x100, cbuf = pool + 0x200, target = base + 0x1D000;
    u8 bytes[256] = {0}, pixels[64];
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(pool, bytes, 32));
    memset(bytes, 0, 32);
    CHECK(!gmem_write(samplers, bytes, 32)); /* a sampler of zeros: a texel fetch doesn't filter */
    CHECK(!gmem_write(cbuf, bytes, 256));    /* the handle at 0x40: 0 */
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 0;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    ch->ds.bind.cb_addr[4][2] = cbuf;
    ch->ds.bind.cb_size[4][2] = 256;
    /* the 4x4 target, one sample */
    r[0x574] = 0;
    r[0x200] = (u32)(target >> 32), r[0x201] = (u32)target, r[0x202] = 16, r[0x203] = 4, r[0x204] = 0xD5;
    r[0x205] = 0x1000, r[0x206] = 1, r[0x207] = 0;
    for (u32 k = 0; k < 4; k++) {
        const u64 at = base + 0x10200 + k * 0x200;
        const u64 ops[7] = {
            0xe000000000070000ull | 0x1Cull << 30 | 2, /* IPA R2, a[0x70]: the fragment's x */
            0xe000000000070000ull | 0x1Dull << 30 | 3, /* IPA R3, a[0x74]: its y */
            0x5CB0000000070000ull | 3ull << 39 | 2ull << 20 | 2ull << 10 | 2ull << 8 | 2, /* F2I.U32.F32.TRUNC R2, R2 */
            0x5CB0000000070000ull | 3ull << 39 | 3ull << 20 | 2ull << 10 | 2ull << 8 | 3, /* F2I.U32.F32.TRUNC R3, R3 */
            0x0100000000071000ull | (u64)k << 20 | 4,                                     /* MOV32I R4, k */
            /* TLDS.MS R0..R3 (a register a component), the coordinates in R2 and R3, the sample in R4 */
            0xd000000000000000ull | 1ull << 59 | 1ull << 57 | 6ull << 53 | 4ull << 50 | 0x10ull << 36 | 2ull << 28 |
                4ull << 20 | 7ull << 16 | 2ull << 8,
            0xe30000000007000full};
        CHECK(index_program(at, ops, 7, 1));
        r[0x851] = (u32)(at - base);
        DrawCall call = {.topology = 4, .first = 3, .count = 3, .instances = 1}; /* the triangle over all of it */
        u64 drawn = g_vk_drawn;
        vk_lock();
        ch->r3d_dirty = ~0u;
        mx_draw_state_sync(ch);
        vk_render_draw(&ch->ds, &call, &ch->owner);
        vk_render_end();
        CHECK(g_vk_drawn == drawn + 1);
        Surf* d = vk_rt_find(target);
        CHECK(d && !d->msaa && !ms_read_pixels(d, 4, 4, pixels));
        vk_unlock();
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++)
                if (memcmp(pixels + (y * 4 + x) * 4, samples + (2 * y + k / 2) * 32 + (2 * x + k % 2) * 4, 4)) {
                    const u8* p = pixels + (y * 4 + x) * 4;
                    fprintf(stderr, "msaa fetch: sample %u of pixel (%u, %u) read %u %u %u %u\n", k, x, y, p[0], p[1],
                            p[2], p[3]);
                    return 1;
                }
    }
    r[0x851] = 0x10000;
    return 0;
}

/* base: where the programs and ms_fetch's tables are */
static int ms_case(GpuChannel* ch, u64 base, u64 target, u64 resolved, u64 loaded, int bl) {
    u32* r = ch->r3d;
    u8 samples[256], memory[512], pixels[64];
    /* the target: cleared blue, the triangle drawn */
    ms_target(ch, target, bl);
    float blue[4] = {0, 0, 1, 1};
    memcpy(&r[0x360], blue, sizeof blue);
    vk_lock();
    engine_clear(ch, 15 << 2);
    vk_unlock();
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    u64 drawn = g_vk_drawn;
    vk_lock();
    ch->r3d_dirty = ~0u;
    mx_draw_state_sync(ch);
    vk_render_draw(&ch->ds, &call, &ch->owner);
    vk_render_end();
    vk_unlock();
    CHECK(g_vk_drawn == drawn + 1);
    Surf* s = vk_rt_find(target);
    CHECK(s && s->msaa == DS_MSAA_2X2 && s->w == 4 && s->h == 4 && vk_surf_samples(s) == VK_SAMPLE_COUNT_4_BIT);
    CHECK(s->ms_raw && vk_ms_moves(s));
    vk_lock();
    CHECK(!ms_read_samples(s, samples));
    vk_unlock();
    CHECK(ms_samples_drawn(samples, 0, 0));
    /* written back in the guest's layout of the samples */
    g_renderer->rt_flush(target);
    CHECK(!gmem_read(target, memory, bl ? 512 : 256));
    CHECK(ms_samples_drawn(memory, bl, 1));
    /* resolved by the 2D engine's blit of the samples onto a 4x4 target's pixels: vkCmdResolveImage's average */
    Surf2D src = {.format = 0xD5, .linear = !bl, .depth = 1, .pitch = bl ? 0 : 32, .width = 8, .height = 8,
                  .addr = target};
    Surf2D dst = {.format = 0xD5, .linear = !bl, .depth = 1, .pitch = bl ? 0 : 16, .width = 4, .height = 4,
                  .addr = resolved};
    CHECK(g_renderer->blit(&src, &dst, 0, 0, 4, 4, 0, 0, 2, 2));
    Surf* d = vk_rt_find(resolved);
    CHECK(d && !d->msaa);
    vk_lock();
    CHECK(!ms_read_pixels(d, 4, 4, pixels));
    vk_unlock();
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            const u8* p = pixels + (y * 4 + x) * 4;
            u32 g = 0;
            for (u32 k = 0; k < 4; k++) g += samples[(2 * y + k / 2) * 32 + (2 * x + k % 2) * 4 + 1];
            /* the average of the samples' green, and blue the rest; implementations round it either way */
            if (abs((int)p[1] - (int)(g / 4)) > 1 || abs((int)p[2] - (int)(255 - g / 4)) > 1 || p[0] || p[3] != 255) {
                fprintf(stderr, "msaa: resolved pixel (%u, %u) is %u %u %u %u, its samples' green sums to %u\n", x, y,
                        p[0], p[1], p[2], p[3], g);
                return 1;
            }
        }
    /* a resolve scaled down: the samples resolved, then blitted onto 2x2 pixels (the green corner, the blue one) */
    Surf2D half = dst;
    half.addr = resolved + 0x1000;
    half.width = half.height = 2;
    half.pitch = bl ? 0 : 8;
    CHECK(g_renderer->blit(&src, &half, 0, 0, 2, 2, 0, 0, 4, 4));
    d = vk_rt_find(half.addr);
    CHECK(d);
    vk_lock();
    CHECK(!ms_read_pixels(d, 2, 2, pixels));
    vk_unlock();
    CHECK(pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0);   /* (0, 0): pixels 0-1, 0-1 all green */
    CHECK(pixels[12] == 0 && pixels[13] == 0 && pixels[14] == 255); /* (1, 1): pixels 2-3, 2-3 all blue */
    /* guest memory's samples loaded into the image, bit for bit */
    u8 pattern[256], lay[512] = {0};
    ms_pattern(pattern);
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 8; x++) memcpy((u8*)ms_texel(lay, bl, x, y), pattern + y * 32 + x * 4, 4);
    CHECK(!gmem_write(target, lay, bl ? 512 : 256));
    g_renderer->rt_reload(target);
    s = vk_rt_find(target);
    CHECK(s && s->layout != VK_IMAGE_LAYOUT_UNDEFINED);
    vk_lock();
    CHECK(!ms_read_samples(s, samples));
    vk_unlock();
    CHECK(!memcmp(samples, pattern, 256));
    /* a texture header of the sample layout over the target binds the target itself */
    u32 tic[8] = {0x08 /* A8B8G8R8 */ | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16 | 2u << 19 | 3u << 22 | 4u << 25 |
                      5u << 28,
                  (u32)target,
                  (u32)(target >> 32) | (bl ? 3u : 2u) << 21,
                  bl ? 0 : 32u >> 5,
                  3 | 1u << 23 /* 2D */,
                  3,
                  0,
                  DS_MSAA_2X2 << 8};
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    CHECK(t == s);
    /* a program reads each sample */
    CHECK(!ms_fetch(ch, base, tic, pattern));
    /* one over other memory loads its samples from there */
    CHECK(!gmem_write(loaded, lay, bl ? 512 : 256));
    tic[1] = (u32)loaded;
    tic[2] = (u32)(loaded >> 32) | (bl ? 3u : 2u) << 21;
    tic[7] = DS_MSAA_2X2_VC4 << 8; /* a variant of the same block */
    vk_lock();
    t = vk_tex_from_tic(tic);
    CHECK(t && !t->is_rt && t->msaa == DS_MSAA_2X2 && t->w == 4 && t->h == 4);
    CHECK(!ms_read_samples(t, samples));
    vk_unlock();
    CHECK(!memcmp(samples, pattern, 256));
    /* a single-sampled header of the samples' size over the target takes them as the guest lays them out: the
       target cleared red, which memory (the pattern) hasn't got */
    ms_target(ch, target, bl);
    float red[4] = {1, 0, 0, 1};
    memcpy(&r[0x360], red, sizeof red);
    vk_lock();
    engine_clear(ch, 15 << 2);
    vk_render_end();
    tic[1] = (u32)target;
    tic[2] = (u32)(target >> 32) | (bl ? 3u : 2u) << 21;
    tic[4] = 7 | 1u << 23;
    tic[5] = 7;
    tic[7] = 0;
    t = vk_tex_from_tic(tic);
    CHECK(t && !t->msaa && t->w == 8 && t->h == 8);
    u8 big[256];
    CHECK(!ms_read_pixels(t, 8, 8, big));
    vk_unlock();
    for (u32 i = 0; i < 256; i += 4) CHECK(big[i] == 255 && !big[i + 1] && !big[i + 2] && big[i + 3] == 255);
    return 0;
}

static int check_msaa(void) {
    const u64 address = 0x47C000000ull, cpu = HEAP_BASE + 0x7C0000;
    const u64 positions = address + 0x12000, colors = address + 0x13000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    u64 fragment[5];
    /* the triangle (0, 0), (4, 0), (0, 4) of the 4x4 target, green; vertices 3 to 5 cover it all */
    const float corner[12] = {-1, -1, 1, -1, -1, 1, -1, -1, 3, -1, -1, 3}; /* and one over all of it */
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    u32 validation = g_vk_validation_messages;
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    ch->id = 124;
    gmmu_map(address, cpu, 0x20000);
    for (u32 v = 0; v < 6; v++) {
        const float p[4] = {corner[v * 2], corner[v * 2 + 1], 0, 1}, c[4] = {0, 1, 0, 1};
        u8 bytes[16];
        for (u32 k = 0; k < 2; k++) {
            for (u32 i = 0; i < 4; i++) {
                u32 word;
                memcpy(&word, k ? c + i : p + i, 4);
                snx_st_le32(bytes + i * 4, word);
            }
            CHECK(!gmem_write((k ? colors : positions) + v * 16, bytes, 16));
        }
    }
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(index_program(address, vertex, 5, 0));
    CHECK(index_program(address + 0x10000, fragment, 5, 1));
    u32* r = ch->r3d;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    memcpy(r + 0x280, transform, sizeof transform);
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = stream ? colors : positions;
        r[0x458 + stream] = stream | 1u << 21 | 7u << 27; /* RGBA32F */
        r[0x700 + stream * 4] = 0x1000 | 16;
        r[0x701 + stream * 4] = (u32)(source >> 32);
        r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + 6 * 16 - 1) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + 6 * 16 - 1);
    }
    CHECK(!ms_case(ch, address, address + 0x14000, address + 0x15000, address + 0x17000, 1));
    CHECK(!ms_case(ch, address, address + 0x18000, address + 0x19000, address + 0x1B000, 0));
    r[0x574] = 0;
    CHECK(g_vk_validation_messages == validation);
    printf("msaa: 4x targets drawn, written back and loaded in the guest's sample layout, resolved, and sampled, in "
           "block and pitch linear memory%s\n",
           g_vk.props.limits.standardSampleLocations ? " (standard sample locations)" : "");
    free(ch);
    return 0;
}
