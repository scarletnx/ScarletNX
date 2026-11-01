/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* the 4x4 RGBA32F target's x and y (red and green) after a triangle over all of it, at the end of a draw */
static int frag_coord_draw(MxRegs* channel, u64 target, float out[4][4][2]) {
    u64 drawn = g_vk_drawn;
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xC0); /* RGBA32F */
    channel->r3d_dirty = ~0u;                           /* the registers were set directly */
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(256, &buffer);
    CHECK(surface && surface->w == 4 && surface->h == 4 && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) memcpy(out[y][x], data + (y * 4 + x) * 16, 8);
    vk_unlock();
    return 0;
}

/* every pixel's x and y as the fragment reads them: x + 0.5, and y + 0.5 counted down from the target's first row, or
   up from the bottom of a surface clip clip_height high (y_from_bottom) */
static int frag_coord_expect(MxRegs* channel, u64 target, int y_from_bottom, u32 clip_height) {
    float got[4][4][2];
    CHECK(!frag_coord_draw(channel, target, got));
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            float want_x = (float)x + 0.5f, want_y = y_from_bottom ? (float)clip_height - ((float)y + 0.5f) : y + 0.5f;
            if (got[y][x][0] != want_x || got[y][x][1] != want_y) {
                fprintf(stderr, "frag coord: pixel (%u, %u) read (%g, %g), expected (%g, %g)\n", x, y, got[y][x][0],
                        got[y][x][1], want_x, want_y);
                CHECK(0);
            }
        }
    return 0;
}

/* A fragment program's position (IPA of a[0x70] and a[0x74]) with the window origin upper left and lower left
   (WINDOW_ORIGIN 0x4EB bit 0): in a lower-left window y counts up from the bottom of the surface clip (0x3FE's height),
   as the viewport and the scissor measure it. A game's GL driver leaves its gl_FragCoord transform (c0[0xF00]) the
   identity for the window it draws lower left, and a full-screen pass that samples by the fragment's position drew
   its source upside down while y counted down from the first row */
static int check_frag_coord_origin(void) {
    const u64 address = 0x498000000ull, cpu = HEAP_BASE + 0x740000;
    const u64 positions = address + 0x12000, target = address + 0x18000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xe30000000007000full};
    const u64 fragment[] = {0xe000000000070000ull | 0x1Cull << 30 | 0, /* IPA R0, a[0x70]: the fragment's x */
                            0xe000000000070000ull | 0x1Dull << 30 | 1, /* IPA R1, a[0x74]: its y */
                            0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    MxRegs* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 127;
    gmmu_map(address, cpu, 0x20000);
    u8 bytes[48];
    for (u32 i = 0; i < 12; i++) {
        u32 word;
        memcpy(&word, position + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
    CHECK(!gmem_write(positions, bytes, sizeof bytes));
    CHECK(mesh_program(address, vertex, 3, 0));
    CHECK(mesh_program(address + 0x10000, fragment, 3, 1));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 0 | 1u << 21 | 7u << 27; /* stream 0 at 0: RGBA32F */
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(positions >> 32);
    r[0x702] = (u32)positions;
    r[0x7c0] = (u32)((positions + sizeof bytes - 1) >> 32);
    r[0x7c1] = (u32)(positions + sizeof bytes - 1);
    r[0x4eb] = 0; /* upper left: y counts down from the first row */
    CHECK(!frag_coord_expect(channel, target, 0, 0));
    r[0x4eb] = 1; /* lower left: up from the bottom of the surface clip, here the target's */
    CHECK(!frag_coord_expect(channel, target, 1, 4));
    r[0x4eb] = 0x11; /* the facing flip changes nothing of the position */
    CHECK(!frag_coord_expect(channel, target, 1, 4));
    r[0x4eb] = 0x10;
    CHECK(!frag_coord_expect(channel, target, 0, 0));
    /* a surface clip 8 high over the 4-row target, the viewport over all of it: y from the clip's bottom, 8 */
    r[0x3fe] = 8u << 16;
    transform[1] = transform[4] = 4;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x4eb] = 1;
    CHECK(!frag_coord_expect(channel, target, 1, 8));
    printf("frag coord: the fragment's y from the first row upper left, from the surface clip's bottom lower left\n");
    free(channel);
    return 0;
}
