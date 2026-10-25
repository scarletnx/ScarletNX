/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

static int mesh_program(u64 address, const u64* ops, u32 count, int fragment) {
    u64 code[64] = {0};
    u32 at = 10;
    if (fragment) {
        code[3] = 0xff; /* screen-linear RGBA input */
        code[9] = 15;
    }
    for (u32 i = 0; i < count; i++) {
        if (!(i % 3)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[i];
    }
    return invalidate_write_code(address, code, 64);
}

static int mesh_pixel_check(GpuChannel* channel, u64 target, u32 instance, const u8 expected[4], u32 mask) {
    u64 drawn = g_vk_drawn;
    vk_lock();
    rt_clear(channel, target, 4, 4, 0, 0);
    channel->r3d_dirty = ~0u; /* the registers were set directly */
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .base_instance = instance, .instances = 1};
    vk_render_draw(&channel->ds, &call, channel);
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(4, &buffer);
    CHECK(surface && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageOffset = {1, 1, 0}, .imageExtent = {1, 1, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    int matches = 1;
    for (u32 i = 0; i < 4; i++)
        if ((mask & (1u << i)) && data[i] != expected[i]) matches = 0;
    if (!matches) {
        fprintf(stderr, "mesh instance %u: got %u,%u,%u,%u, expected %u,%u,%u,%u\n", instance,
                data[0], data[1], data[2], data[3], expected[0], expected[1], expected[2], expected[3]);
        CHECK(matches);
    }
    vk_unlock();
    return 0;
}

static int mesh_pixel(GpuChannel* channel, u64 target, u32 instance, const u8 expected[4]) {
    return mesh_pixel_check(channel, target, instance, expected, 15);
}

/* The backing allocation deliberately contains a second record beyond the
   guest binding's limit. Only the bound bytes may supply mesh attributes. */
static int check_mesh_inputs(void) {
    const u64 address = 0x470000000ull, cpu = HEAP_BASE + 0x740000;
    const u64 positions = address + 0x11ff0, colors = address + 0x14000, target = address + 0x18000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    u64 fragment[5];
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float color[8] = {0, 1, 0, 1, 1, 0, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 126;
    gmmu_map(address, cpu, 0x20000);
    /* The mesh's position stream crosses unrelated physical pages. */
    gmmu_map(address + 0x12000, HEAP_BASE + 0x768000, 0x1000);
    u8 bytes[48];
    for (u32 i = 0; i < 12; i++) {
        u32 word;
        memcpy(&word, position + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
    CHECK(!gmem_write(positions, bytes, sizeof bytes));
    for (u32 i = 0; i < 8; i++) {
        u32 word;
        memcpy(&word, color + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
    CHECK(!gmem_write(colors, bytes, 32));
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(address, vertex, 5, 0));
    CHECK(mesh_program(address + 0x10000, fragment, 5, 1));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1; /* as reset_3d starts a channel */
    memcpy(r + 0x280, transform, sizeof transform);
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = stream ? colors : positions;
        r[0x458 + stream] = stream | 1u << 21 | 7u << 27; /* RGBA32F */
        r[0x700 + stream * 4] = 0x1000 | 16;
        r[0x701 + stream * 4] = (u32)(source >> 32);
        r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + (stream ? 16 : 48) - 1) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + (stream ? 16 : 48) - 1);
    }
    r[0x621] = r[0x707] = 1; /* color per instance */
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 255, 0, 255}));
    /* RASTERIZE_ENABLE off discards the primitives, with no capture: the target keeps its red clear */
    r[MAXWELL3D_RASTERIZE_ENABLE] = 0;
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){255, 0, 0, 255}));
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    channel->r3d_dirty = ~0u;
    vk_lock();
    VkBuffer output;
    u64 offset;
    CHECK(vk_buf_copy_writable(colors, 32, &output, &offset));
    vk_buf_writable_commit();
    vk_unlock();
    /* Instance 1 exists in the allocation, but not in the guest binding. */
    CHECK(!mesh_pixel(channel, target, 1, (const u8[4]){0, 0, 0, 0}));
    /* Empty RGBA fetches zero even in W; an absent RGB component defaults to 1. */
    r[0x7c2] = (u32)((colors - 1) >> 32);
    r[0x7c3] = (u32)(colors - 1);
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 0, 0, 0}));
    r[0x459] = 1 | 2u << 21 | 7u << 27; /* RGB32F */
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 0, 0, 255}));
    /* Reusing a CPU-written mesh binding keeps its new contents, without an
       explicit GPU-side memory write stamp. */
    const u32 blue[4] = {0, 0, 0x3f800000, 0x3f800000};
    for (u32 i = 0; i < 4; i++) snx_st_le32(GP(cpu + 0x14000) + i * 4, blue[i]);
    g_renderer->mem_may_have_changed();
    r[0x459] = 1 | 1u << 21 | 7u << 27;
    r[0x7c3] = (u32)(colors + 15);
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 0, 255, 255}));
    /* The address can keep its binding while a pool remaps its physical pages. */
    const u64 replacement = HEAP_BASE + 0x769000;
    const u32 yellow[4] = {0x3f800000, 0x3f800000, 0, 0x3f800000};
    for (u32 i = 0; i < 4; i++) snx_st_le32(GP(replacement) + i * 4, yellow[i]);
    gmmu_map(colors, replacement, 0x1000);
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){255, 255, 0, 255}));
    /* A format starts at its own relative offset within the binding. */
    r[0x459] = 1 | 4u << 7 | 2u << 21 | 7u << 27;
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){255, 0, 255, 255}));
    /* A rebased attribute can overlap the next record. Its last component
       must not consume the poison just past the guest's limit. */
    snx_st_le32(bytes, 0x3f800000);
    CHECK(!gmem_write(colors + 16, bytes, 4));
    r[0x459] = 1 | 4u << 7 | 1u << 21 | 7u << 27;
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel_check(channel, target, 0, (const u8[4]){0, 0, 0, 0}, 8));
    /* Splitting a large relative offset into another Vulkan binding must not
       expand a short guest binding, or place its offset outside the allocation. */
    for (u32 i = 0; i < 4; i++) snx_st_le32(bytes + i * 4, yellow[i]);
    CHECK(!gmem_write(colors + 4096, bytes, 16)); /* poison beyond the short binding */
    r[0x459] = 1 | 4096u << 7 | 1u << 21 | 7u << 27;
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 0, 0, 0}));
    for (u32 i = 0; i < 4; i++) snx_st_le32(bytes + i * 4, blue[i]);
    CHECK(!gmem_write(colors + 4096, bytes, 16));
    r[0x704] = 0x1000; /* zero stride reuses the offset record */
    r[0x7c3] = (u32)(colors + 4096 + 15);
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 0, 255, 255}));
    printf("mesh inputs: 10 range, format, update and remapping cases checked (strict bounds %s)\n",
           g_vk.has_vertex_robustness2 ? "on" : "unavailable");
    free(channel);
    return 0;
}
