/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Draws whose indices Vulkan can't read as they are, which the renderer makes on the GPU (render.c's index_convert and
   quad_indices): quads of vertex numbers and of 8, 16 and 32-bit and inline indices, 8-bit indices widened, and
   restart indices other than all ones, in strips. Vertices 0 to 3 are a red quad over the whole target and 4 to 7 a
   green one, so a quad drawn from the wrong vertices shows red, and a strip that takes its restart index (2, the red
   quad's top right corner) for a vertex draws a second triangle with red in it */

/* a program at address of count instructions (a scheduling word before every three), a fragment program's header
   taking a screen-linear RGBA input when fragment is set */
static int index_program(u64 address, const u64* ops, u32 count, int fragment) {
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

/* the 4x4 target's pixels after a draw call: how many are green and blue (the clear), and whether any has red */
typedef struct {
    u32 green, blue, red;
} IndexPixels;

/* the render predicate index_draw's draw runs on (its clear doesn't), 0 for none */
static u32 g_index_pred;
static int index_draw(GpuChannel* channel, u64 target, const DrawCall* call, IndexPixels* px) {
    u64 drawn = g_vk_drawn;
    vk_lock();
    rt_clear(channel, target, 4, 4, 0, 1);
    channel->r3d_dirty = ~0u; /* the registers were set directly */
    mx_draw_state_sync(channel);
    channel->owner.pred = g_index_pred;
    vk_render_draw(&channel->ds, call, &channel->owner);
    channel->owner.pred = 0;
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(64, &buffer);
    CHECK(surface && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    *px = (IndexPixels){0};
    for (u32 i = 0; i < 16; i++) {
        const u8* p = data + i * 4;
        px->green += !p[0] && p[1] == 255 && !p[2];
        px->blue += !p[0] && !p[1] && p[2] == 255;
        px->red += p[0] != 0;
    }
    vk_unlock();
    return 0;
}

/* indices n of size isz (1, 2 or 4) at the index buffer's address, from index first on, and the restart index ri
   (restart on when ri isn't ~0) */
static int index_buffer(GpuChannel* channel, u64 at, const u32* idx, u32 n, u32 isz, u32 ri) {
    u8 bytes[64];
    u32* r = channel->r3d;
    for (u32 i = 0; i < n; i++)
        if (isz == 1)
            bytes[i] = (u8)idx[i];
        else if (isz == 2)
            snx_st_le16(bytes + i * 2, (u16)idx[i]);
        else
            snx_st_le32(bytes + i * 4, idx[i]);
    CHECK(!gmem_write(at, bytes, (size_t)n * isz));
    r[0x5F2] = (u32)(at >> 32);
    r[0x5F3] = (u32)at;
    r[0x5F4] = (u32)((at + (u64)n * isz - 1) >> 32);
    r[0x5F5] = (u32)(at + (u64)n * isz - 1);
    r[0x5F6] = isz == 1 ? 0 : isz == 2 ? 1 : 2;
    r[0x591] = ri != ~0u;
    r[0x592] = ri;
    return 0;
}

/* a whole green quad: every pixel green */
static int index_all_green(const IndexPixels* px) { return px->green == 16 && !px->red; }
/* one green triangle of the strip, half the target, and nothing red: a restart index taken for vertex 2 draws red */
static int index_half_green(const IndexPixels* px) {
    return px->green >= 6 && px->blue >= 6 && px->green + px->blue == 16 && !px->red;
}

static int index_cases(GpuChannel* channel, u64 target, u64 indices) {
    IndexPixels px;
    const u32 quad[5] = {9, 4, 5, 6, 7}, quad0[4] = {0, 1, 2, 3}, strip[5] = {4, 5, 7, 2, 4};
    /* quads of vertex numbers from vertex 4 */
    DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_all_green(&px));
    /* indexed quads from index 1 (a word apart, or not, for every size), and through the base vertex */
    for (u32 isz = 1; isz <= 4; isz *= 2) {
        CHECK(!index_buffer(channel, indices, quad, 5, isz, ~0u));
        call = (DrawCall){.topology = 7, .indexed = 1, .first = 1, .count = 4, .instances = 1};
        CHECK(!index_draw(channel, target, &call, &px));
        if (!index_all_green(&px)) fprintf(stderr, "indexed quads of %u bytes: %u green, %u red\n", isz, px.green, px.red);
        CHECK(index_all_green(&px));
        CHECK(!index_buffer(channel, indices, quad0, 4, isz, ~0u));
        call = (DrawCall){.topology = 7, .indexed = 1, .count = 4, .base_vertex = 4, .instances = 1};
        CHECK(!index_draw(channel, target, &call, &px));
        CHECK(index_all_green(&px));
    }
    /* inline indices: quads, and a strip with its restart index */
    call = (DrawCall){.topology = 7, .indexed = 3, .first = 1, .count = 4, .instances = 1, .inline_indices = quad};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_all_green(&px));
    channel->r3d[0x591] = 1;
    channel->r3d[0x592] = 2;
    call = (DrawCall){.topology = 5, .indexed = 3, .count = 5, .instances = 1, .inline_indices = strip};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_half_green(&px));
    /* strips of every index size whose restart index is 2, an odd number of indices, and 8-bit ones restarting at
       0xFF, Vulkan's own */
    for (u32 isz = 1; isz <= 4; isz *= 2) {
        CHECK(!index_buffer(channel, indices, strip, 5, isz, 2));
        call = (DrawCall){.topology = 5, .indexed = 1, .count = 5, .instances = 1};
        CHECK(!index_draw(channel, target, &call, &px));
        if (!index_half_green(&px))
            fprintf(stderr, "strip of %u byte indices: %u green, %u blue, %u red\n", isz, px.green, px.blue, px.red);
        CHECK(index_half_green(&px));
    }
    const u32 strip_ff[4] = {4, 5, 7, 0xFF}, list[6] = {4, 5, 7, 5, 6, 7};
    CHECK(!index_buffer(channel, indices, strip_ff, 4, 1, 0xFF));
    call = (DrawCall){.topology = 5, .indexed = 1, .count = 4, .instances = 1};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_half_green(&px));
    /* 8-bit indices of a list, without restart */
    CHECK(!index_buffer(channel, indices, list, 6, 1, ~0u));
    call = (DrawCall){.topology = 4, .indexed = 1, .count = 6, .instances = 1};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_all_green(&px));
    channel->r3d[0x591] = 0;
    return 0;
}

/* Indirect draws (DrawCall.indirect): the arguments, and a multi-draw's count, are guest memory the GPU reads, in
   VkDrawIndirectCommand's and VkDrawIndexedIndirectCommand's layouts, and nothing of them is read on the CPU. A fan of
   vertices 4 to 7 (the green quad), a list through a base vertex, and two records under a count: the second draws the
   red quad over the green one only when the count says two */
static int put_words(u64 at, const u32* w, u32 n) {
    u8 bytes[64];
    for (u32 i = 0; i < n; i++) snx_st_le32(bytes + 4 * i, w[i]);
    CHECK(!gmem_write(at, bytes, 4 * (size_t)n));
    return 0;
}
static int indirect_cases(GpuChannel* channel, u64 target, u64 indices, u64 args) {
    IndexPixels px;
    const u32 list0[6] = {0, 1, 3, 1, 2, 3};
    const u32 fan[4] = {4, 1, 4, 0}; /* count, instances, first vertex, base instance */
    CHECK(!put_words(args, fan, 4));
    DrawCall call = {.topology = 6, .count = 1, .instances = 1, .indirect = args, .draw_count = 1, .stride = 16};
    CHECK(!index_draw(channel, target, &call, &px));
    if (!index_all_green(&px)) fprintf(stderr, "indirect fan: %u green, %u red\n", px.green, px.red);
    CHECK(index_all_green(&px));
    /* indices 0..3 through base vertex 4 (16-bit), then the same records under a count of 1 and of 2, the second
       record's base vertex 0 the red quad */
    CHECK(!index_buffer(channel, indices, list0, 6, 2, ~0u));
    const u32 two[10] = {6, 1, 0, 4, 0, 6, 1, 0, 0, 0}; /* count, instances, first index, base vertex, base instance */
    CHECK(!put_words(args, two, 10));
    call = (DrawCall){.topology = 4, .indexed = 1, .count = 1, .instances = 1, .indirect = args, .draw_count = 1,
                      .stride = 20};
    CHECK(!index_draw(channel, target, &call, &px));
    if (!index_all_green(&px)) fprintf(stderr, "indirect indexed: %u green, %u red\n", px.green, px.red);
    CHECK(index_all_green(&px));
    if (g_vk.has_draw_indirect_count) {
        const u32 one = 1, both = 2;
        CHECK(!put_words(args + 0x40, &one, 1));
        call.indirect_count = args + 0x40;
        call.draw_count = 2;
        CHECK(!index_draw(channel, target, &call, &px));
        CHECK(index_all_green(&px));
        CHECK(!put_words(args + 0x40, &both, 1));
        CHECK(!index_draw(channel, target, &call, &px));
        if (px.red != 16) fprintf(stderr, "indirect count of 2: %u green, %u red\n", px.green, px.red);
        CHECK(px.red == 16);
    }
    /* a render enable comparing two 64-bit values 16 bytes apart on the GPU (vk_query_pred_compare): equal draws under
       IF_EQUAL (VK_PRED_EQUAL), different doesn't */
    vk_lock();
    vk_query_reset(0x7ff0); /* an owner of its own: the query pool predicates are made with */
    vk_unlock();
    if (g_vk.has_cond_render && vk_query_pred_ok()) {
        const u32 values[6] = {7, 9, 0, 0, 7, 9};
        u32 slot = 0;
        for (int differ = 0; differ < 2; differ++) {
            const u32 changed[2] = {7, 10};
            CHECK(!put_words(args + 0x80, values, 6));
            if (differ) CHECK(!put_words(args + 0x90, changed, 2));
            call = (DrawCall){.topology = 6, .count = 4, .first = 4, .instances = 1};
            vk_lock();
            CHECK(vk_query_pred_compare(args + 0x80, &slot));
            vk_unlock();
            g_index_pred = (slot + 1) | VK_PRED_EQUAL;
            CHECK(!index_draw(channel, target, &call, &px));
            g_index_pred = 0;
            vk_lock();
            vk_query_pred_drop(slot);
            vk_unlock();
            if (differ ? px.blue != 16 : !index_all_green(&px))
                fprintf(stderr, "render enable on values %s: %u green, %u blue\n", differ ? "different" : "equal",
                        px.green, px.blue);
            CHECK(differ ? px.blue == 16 : index_all_green(&px));
        }
        puts("render enable: two values in guest memory compared on the GPU");
    }
    printf("indirect draws: a fan, indexed through a base vertex%s checked on the GPU\n",
           g_vk.has_draw_indirect_count ? ", and a count of one and two records" : "");
    return 0;
}

static int check_index_draws(void) {
    const u64 address = 0x478000000ull, cpu = HEAP_BASE + 0x7A0000;
    const u64 positions = address + 0x12000, colors = address + 0x13000, indices = address + 0x14000,
              target = address + 0x18000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    u64 fragment[5];
    /* two quads over the whole target, corners in the order quads take them */
    const float corner[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 125;
    gmmu_map(address, cpu, 0x20000);
    u8 bytes[8 * 16];
    for (u32 v = 0; v < 8; v++) {
        const float p[4] = {corner[(v & 3) * 2], corner[(v & 3) * 2 + 1], 0, 1};
        const float c[4] = {v < 4 ? 1.0f : 0.0f, v < 4 ? 0.0f : 1.0f, 0, 1};
        for (u32 i = 0; i < 4; i++) {
            u32 word;
            memcpy(&word, p + i, 4);
            snx_st_le32(bytes + v * 16 + i * 4, word);
        }
        CHECK(!gmem_write(positions + v * 16, bytes + v * 16, 16));
        for (u32 i = 0; i < 4; i++) {
            u32 word;
            memcpy(&word, c + i, 4);
            snx_st_le32(bytes + v * 16 + i * 4, word);
        }
        CHECK(!gmem_write(colors + v * 16, bytes + v * 16, 16));
    }
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(index_program(address, vertex, 5, 0));
    CHECK(index_program(address + 0x10000, fragment, 5, 1));
    u32* r = channel->r3d;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1; /* as reset_3d starts a channel */
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
        r[0x7c0 + stream * 2] = (u32)((source + 8 * 16 - 1) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + 8 * 16 - 1);
    }
    int had8 = g_vk.has_index8;
    CHECK(!index_cases(channel, target, indices));
    g_vk.has_index8 = 0; /* the widening on the GPU, which a device without 8-bit indices takes */
    CHECK(!index_cases(channel, target, indices));
    g_vk.has_index8 = had8;
    CHECK(!indirect_cases(channel, target, indices, address + 0x16000));
    printf("index draws: quads, widened and native 8-bit indices and restart indices checked (8-bit indices %s)\n",
           had8 ? "native" : "unavailable");
    free(channel);
    return 0;
}
