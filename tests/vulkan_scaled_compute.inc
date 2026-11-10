/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compute programs over a render target at a resolution scale of 2. The target is 16x8 R32_UINT, so its image is
   32x16, and one block of 16x8 threads covers it in the guest's texels, each thread at its own (TID.X, TID.Y):
   - a store of each thread's packed ID (x, y << 16) through a header over the target: guest memory and every pixel of
     the target's image hold it, not only the image's top-left quarter
   - a fetch (TLD) of the target at the thread's texel, stored into an image of the guest's size: texel (x, y) of the
     target, where the image's pixel (x, y) is the target's texel (x / 2, y / 2). Through a header of the target's
     format, and through an R32_FLOAT one, which reads the target's bits as floats
   - a size query (TXQ) of the target: the guest's 16x8 */
#define SC_GPU  0x860000000ull
#define SC_CPU  (HEAP_BASE + 0x5500000)
#define SC_SPAN 0x40000ull
#define SC_W    16u
#define SC_H    8u
enum { SC_STORE, SC_FETCH, SC_FETCH_FLOAT, SC_QUERY };

/* what thread (x, y) stores: 2.0's bits and its packed ID, a float that is no subnormal */
static u32 sc_id(u32 x, u32 y) { return 0x40000000u | x | y << 16; }
/* the R32 texels of a SC_W x SC_H block linear surface at gva, in rows */
static int sc_texels(u64 gva, u32* texels) {
    u8 bytes[SC_W * SC_H * 4];
    CHECK(!gmem_read(gva, bytes, sizeof bytes));
    for (u32 y = 0; y < SC_H; y++)
        for (u32 x = 0; x < SC_W; x++)
            texels[y * SC_W + x] = snx_ld_le32(bytes + bl_offset(x * 4, y, 0, SC_W * 4, SC_H, 0, 0));
    return 0;
}
/* every pixel of s's image, in rows of s->sw. Under vk_lock */
static int sc_pixels(Surf* s, u32* pixels) {
    VkBuffer buffer;
    u64 bytes = (u64)s->sw * s->sh * 4;
    u8* d = vk_readback_buffer(bytes, &buffer);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {s->sw, s->sh, 1}};
    CHECK(d);
    vk_render_end();
    vk_surf_use(vk_cmd(), s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    for (u64 i = 0; i < bytes / 4; i++) pixels[i] = snx_ld_le32(d + i * 4);
    return 0;
}
/* the program of a case at SC_GPU, dispatched as one block of SC_W x SC_H threads. Its image's handle is c[2][0x128],
   its texture's c[2][0x12c] */
static int sc_dispatch(GpuChannel* channel, int what) {
    const u64 cbuf = SC_GPU + 0x8000;
    const u64 sust = (0xeb203a0200f77614ull & ~(0xffull | 0xffull << 8 | 0xfull << 16 | 0xffull << 39 | 7ull << 33)) |
                     8ull | 7ull << 16 | 6ull << 39 | 3ull << 33; /* SUST.P [R0, R1], R8, handle R6 */
    u64 ops[16];
    u32 n = 0, at = 0;
    u8 code[256] = {0};
    u32 qmd[64] = {0};
    u64 dispatches = g_vk_dispatches;
    DispatchState d;

    ops[n++] = 0xf0c8000002170000ull; /* S2R R0, TID.X */
    ops[n++] = 0xf0c8000002270001ull; /* S2R R1, TID.Y */
    ops[n++] = 0xf0c8000002070008ull; /* S2R R8, TID: x, y << 16 */
    ops[n++] = storage_mov(9, 0x40000000u);
    ops[n++] = 0x5c10000000970808ull;                       /* IADD R8, R8, R9 */
    if (what != SC_STORE) ops[n++] = 0x4c98078804b70004ull; /* MOV R4, c[2][0x12c] */
    if (what == SC_FETCH || what == SC_FETCH_FLOAT)
        ops[n++] = 0xdd38000080070008ull | 2ull << 28 | 4ull << 20; /* TLD.B R8, [R0, R1], R4 */
    if (what == SC_QUERY) {
        ops[n++] = 0x5c9807800ff70005ull; /* MOV R5, RZ: level 0 */
        ops[n++] = 0xdf50000180470408ull; /* TXQ.B R8, R9: the width and height of R4's */
        ops[n++] = 0x3848000001070909ull; /* SHL R9, R9, 16 */
        ops[n++] = 0x5c10000000970808ull; /* IADD R8, R8, R9 */
    }
    ops[n++] = 0x4c98078804a70006ull; /* MOV R6, c[2][0x128] */
    ops[n++] = sust;
    ops[n++] = 0xe30000000007000full; /* EXIT */
    for (u32 i = 0; i < n; i++) {
        if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    CHECK(!gmem_write(SC_GPU, code, sizeof code));
    g_renderer->method_barrier(5); /* other code at the address */
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = SC_W << 16;
    qmd[QMD_BLOCK_DIM_YZ] = SC_H | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 2;
    qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)cbuf;
    qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)(cbuf >> 32) | 0x200u << 15;
    vk_lock();
    mx_dispatch_state(qmd, channel->rcomp, SC_GPU, &d);
    vk_dispatch(&d);
    vk_unlock();
    CHECK(g_vk_dispatches == dispatches + 1);
    return 0;
}
static int check_scaled_compute(void) {
    const u64 headers = SC_GPU + 0x9000, target = SC_GPU + 0x10000, image = SC_GPU + 0x12000;
    const u32 r32 = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16,
              r32f = 0x0f | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16;
    const u32 rgba = 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    /* 1: the target as an image, 2: the image of the guest's size, 3: the target as a texture, 4: as a float one */
    const u32 tics[5][8] = {
        {0},
        {r32, (u32)target, (u32)(target >> 32) | 3u << 21, 0, (SC_W - 1) | 1u << 23, SC_H - 1, 0, 0},
        {r32, (u32)image, (u32)(image >> 32) | 3u << 21, 0, (SC_W - 1) | 1u << 23, SC_H - 1, 0, 0},
        {r32 | rgba, (u32)target, (u32)(target >> 32) | 3u << 21, 0, (SC_W - 1) | 1u << 23, SC_H - 1, 0, 0},
        {r32f | rgba, (u32)target, (u32)(target >> 32) | 3u << 21, 0, (SC_W - 1) | 1u << 23, SC_H - 1, 0, 0},
    };
    GpuChannel* channel = calloc(1, sizeof *channel);
    u32 texels[SC_W * SC_H], pixels[4 * SC_W * SC_H];
    u8 fill[SC_W * SC_H * 4];
    float scale = g_vk_scale;
    int bad = 0;

    CHECK(channel);
    if (!g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("scaled compute: unformatted writes unavailable, skipped");
        free(channel);
        return 0;
    }
    CHECK(mem_map(SC_CPU, SC_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(SC_GPU, SC_CPU, SC_SPAN);
    channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(headers >> 32);
    channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)headers;
    channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
    CHECK(!storage_write_words(headers, &tics[0][0], 40));
    g_vk_scale = 2.0f;
    rt_clear_format(channel, target, SC_W, SC_H, 0, 1, 0xE4);
    vk_lock();
    Surf* rt = vk_rt_find(target);
    vk_unlock();
    CHECK(rt && rt->sw == 2 * SC_W && rt->sh == 2 * SC_H);

    /* the store */
    CHECK(!storage_write_words(SC_GPU + 0x8000 + 0x128, (const u32[2]){1, 3}, 2));
    CHECK(!sc_dispatch(channel, SC_STORE));
    CHECK(!sc_texels(target, texels));
    for (u32 y = 0; y < SC_H; y++)
        for (u32 x = 0; x < SC_W; x++)
            if (texels[y * SC_W + x] != sc_id(x, y) && bad++ < 4)
                fprintf(stderr, "scaled compute: the store left %08x at the target's texel %u,%u in guest memory\n",
                        texels[y * SC_W + x], x, y);
    CHECK(!bad);
    vk_lock();
    Surf* sampled = vk_tex_from_tic(tics[3]);
    CHECK(sampled && sampled->sw <= 2 * SC_W && sampled->sh <= 2 * SC_H);
    CHECK(!sc_pixels(sampled, pixels));
    for (u32 y = 0; y < sampled->sh; y++)
        for (u32 x = 0; x < sampled->sw; x++) {
            u32 want = sc_id(x * sampled->w / sampled->sw, y * sampled->h / sampled->sh);
            if (pixels[y * sampled->sw + x] != want && bad++ < 4)
                fprintf(stderr,
                        "scaled compute: after the store, pixel %u,%u of the %ux%u image the target's texture reads "
                        "holds %08x, not texel %u,%u's %08x\n",
                        x, y, sampled->sw, sampled->sh, pixels[y * sampled->sw + x], x * sampled->w / sampled->sw,
                        y * sampled->h / sampled->sh, want);
        }
    vk_unlock();
    CHECK(!bad);

    /* the fetches and the size query, into the image of the guest's size */
    for (int what = SC_FETCH; what <= SC_QUERY; what++) {
        static const char* const names[] = {"", "fetch of the target's texel", "fetch of the target's texel as a float",
                                            "target's size"};
        memset(fill, 0xa5, sizeof fill);
        CHECK(!gmem_write(image, fill, sizeof fill));
        CHECK(!storage_write_words(SC_GPU + 0x8000 + 0x128, (const u32[2]){2, what == SC_FETCH_FLOAT ? 4u : 3u}, 2));
        CHECK(!sc_dispatch(channel, what));
        CHECK(!sc_texels(image, texels));
        for (u32 y = 0; y < SC_H; y++)
            for (u32 x = 0; x < SC_W; x++) {
                u32 want = what == SC_QUERY ? (SC_W | SC_H << 16) : sc_id(x, y);
                if (texels[y * SC_W + x] != want && bad++ < 4)
                    fprintf(stderr, "scaled compute: the %s at thread %u,%u gave %08x, not %08x\n", names[what], x, y,
                            texels[y * SC_W + x], want);
            }
        CHECK(!bad);
    }
    g_vk_scale = scale;
    free(channel);
    puts("scaled compute: a store over a target at 2x, a fetch of it and its size, in the guest's texels");
    return 0;
}
