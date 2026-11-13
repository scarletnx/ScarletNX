/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The polygon mode Line and Point on a device without them (fillModeNonSolid off): the renderer draws a list of each
   primitive's sides or vertices instead (render.c draw_edges, edge_indices), which must draw what the device's own
   mode draws, where it has one, and not the filled polygons the pipeline takes. On every device quads, quad strips
   and polygons drawn as lines go as that list too, without the diagonals of the triangles Vulkan draws them as;
   with face culling they keep the device's mode, which culls. A rectangle of corners on pixel
   centres, (2.5, 3.5) to (13.5, 11.5) of a 16x16 target, drawn green over blue in every topology the 3D engine has
   for polygons, from vertex numbers, 16-bit and inline indices, through a base vertex and over a restart. Its outline
   is what a list of its four sides draws, as lines: quads and polygons drawn as lines show it, with no diagonal */
#define PL_SIDE  16u
#define PL_BYTES (PL_SIDE * PL_SIDE * 4)
enum { PL_POINT = 0x1B00, PL_LINE = 0x1B01, PL_FILL = 0x1B02 };

/* the target's pixels once the work recorded so far is done, under vk_lock */
static int pl_pixels(u64 target, u8* out) {
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(PL_BYTES, &buffer);
    CHECK(surface && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {PL_SIDE, PL_SIDE, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, data, PL_BYTES);
    return 0;
}
/* call drawn in the polygon mode mode (both faces) over a blue clear, into px */
static int pl_draw(GpuChannel* channel, u64 target, const DrawCall* call, u32 mode, u8* px) {
    u64 drawn = g_vk_drawn;
    vk_lock();
    rt_clear(channel, target, PL_SIDE, PL_SIDE, 0, 1);
    channel->r3d[MAXWELL3D_POLYGON_MODE_FRONT] = channel->r3d[MAXWELL3D_POLYGON_MODE_BACK] = mode;
    channel->r3d_dirty = ~0u; /* the registers were set directly */
    mx_draw_state_sync(channel);
    vk_render_draw(&channel->ds, call, &channel->owner);
    CHECK(g_vk_drawn == drawn + 1);
    CHECK(!pl_pixels(target, px));
    vk_unlock();
    return 0;
}
/* how many of px's pixels are green */
static u32 pl_green(const u8* px) {
    u32 n = 0;
    for (u32 i = 0; i < PL_SIDE * PL_SIDE; i++) n += !px[4 * i] && px[4 * i + 1] == 255 && !px[4 * i + 2];
    return n;
}
/* px the same as want, or a line naming the first pixel that differs */
static int pl_same(const char* what, const u8* px, const u8* want) {
    for (u32 i = 0; i < PL_SIDE * PL_SIDE; i++)
        if (memcmp(px + 4 * i, want + 4 * i, 4)) {
            fprintf(stderr,
                    "polygon lines: %s: pixel (%u, %u) is %02x %02x %02x, expected %02x %02x %02x (%u green, expected "
                    "%u)\n",
                    what, i % PL_SIDE, i / PL_SIDE, px[4 * i], px[4 * i + 1], px[4 * i + 2], want[4 * i],
                    want[4 * i + 1], want[4 * i + 2], pl_green(px), pl_green(want));
            return 1;
        }
    return 0;
}

/* The draws the list stands in for. native: the images the device's own polygon mode drew of the rectangle as two
   triangles, as lines and as points; NULL where it has none. mode: whether the device's mode is on for these */
static int pl_cases(GpuChannel* channel, u64 target, u64 indices, u64 args, const u8* outline, const u8* filled,
                    const u8* native_lines, const u8* native_points, int mode) {
    u8 px[PL_BYTES];
    /* strip: the same two triangles as (1 2 0) and its second turned, (0 2 3); quad_strip: the quad (0 1 2 3) */
    const u32 two[6] = {0, 1, 2, 0, 2, 3}, strip[4] = {1, 2, 0, 3}, quad_strip[4] = {0, 1, 3, 2},
              quad[4] = {0, 1, 2, 3};
    /* two triangles: each with its three sides, the diagonal they share drawn */
    CHECK(!index_buffer(channel, indices, two, 6, 2, ~0u));
    DrawCall call = {.topology = 4, .indexed = 1, .count = 6, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    CHECK(pl_green(px) < pl_green(filled));
    if (native_lines) CHECK(!pl_same("two triangles as lines", px, native_lines));
    CHECK(!pl_draw(channel, target, &call, PL_POINT, px));
    CHECK(pl_green(px) < pl_green(filled));
    if (native_points) CHECK(!pl_same("two triangles as points", px, native_points));
    /* a strip and a fan of the same two triangles */
    CHECK(!index_buffer(channel, indices, strip, 4, 2, ~0u));
    call = (DrawCall){.topology = 5, .indexed = 1, .count = 4, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    if (native_lines) CHECK(!pl_same("a triangle strip as lines", px, native_lines));
    call = (DrawCall){.topology = 6, .count = 4, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    if (native_lines) CHECK(!pl_same("a triangle fan as lines", px, native_lines));
    /* quads, a quad strip and a polygon: the outline, from vertex numbers, inline indices and through a base vertex
       (vertices 4 to 7 are the same corners) */
    call = (DrawCall){.topology = 7, .count = 4, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    CHECK(!pl_same("a quad as lines", px, outline));
    call = (DrawCall){.topology = 7, .indexed = 3, .count = 4, .instances = 1, .inline_indices = quad};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    CHECK(!pl_same("a quad of inline indices as lines", px, outline));
    CHECK(!index_buffer(channel, indices, quad, 4, 2, ~0u));
    call = (DrawCall){.topology = 7, .indexed = 1, .count = 4, .base_vertex = 4, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    CHECK(!pl_same("a quad through a base vertex as lines", px, outline));
    CHECK(!index_buffer(channel, indices, quad_strip, 4, 2, ~0u));
    call = (DrawCall){.topology = 8, .indexed = 1, .count = 4, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    CHECK(!pl_same("a quad strip as lines", px, outline));
    call = (DrawCall){.topology = 9, .first = 4, .count = 4, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    CHECK(!pl_same("a polygon as lines", px, outline));
    /* a restart index (8, a vertex far off) ends the polygon, and the two vertices after it are no polygon */
    const u32 restarted[7] = {0, 1, 2, 3, 8, 0, 2};
    CHECK(!index_buffer(channel, indices, restarted, 7, 2, 8));
    call = (DrawCall){.topology = 9, .indexed = 1, .count = 7, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    channel->r3d[0x591] = 0;
    CHECK(!pl_same("a polygon restarting as lines", px, outline));
    /* an indirect fan: the CPU hasn't its count, so it is drawn by the device's mode, or filled, which the log says */
    const u32 record[4] = {4, 1, 0, 0}; /* count, instances, first vertex, base instance */
    CHECK(!put_words(args, record, 4));
    call = (DrawCall){.topology = 6, .count = 1, .instances = 1, .indirect = args, .draw_count = 1, .stride = 16};
    CHECK(!pl_draw(channel, target, &call, PL_LINE, px));
    CHECK(!pl_same(mode ? "an indirect fan as lines" : "an indirect fan as lines, filled", px,
                   mode ? native_lines : filled));
    return 0;
}

static int check_polygon_lines(void) {
    const u64 address = 0x4E8000000ull, cpu = HEAP_BASE + 0x11D00000;
    const u64 positions = address + 0x12000, colors = address + 0x13000, indices = address + 0x14000,
              args = address + 0x16000, target = address + 0x18000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    u64 fragment[5];
    /* the corners in the order quads take them, twice (vertices 4 to 7 for a base vertex), then a far one at 8 */
    const float corner[8] = {2.5f, 3.5f, 13.5f, 3.5f, 13.5f, 11.5f, 2.5f, 11.5f};
    const float transform[6] = {8, 8, 0.5f, 8, 8, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 126;
    CHECK(!mem_map(cpu, 0x20000, MS_NORMAL, PERM_RW));
    gmmu_map(address, cpu, 0x20000);
    for (u32 v = 0; v < 9; v++) {
        const float p[4] = {v < 8 ? corner[(v & 3) * 2] / 8 - 1 : 0.9f, v < 8 ? corner[(v & 3) * 2 + 1] / 8 - 1 : -0.9f,
                            0, 1};
        const float c[4] = {0, 1, 0, 1};
        u8 bytes[16];
        for (u32 i = 0; i < 4; i++) {
            u32 word;
            memcpy(&word, p + i, 4);
            snx_st_le32(bytes + i * 4, word);
        }
        CHECK(!gmem_write(positions + v * 16, bytes, 16));
        for (u32 i = 0; i < 4; i++) {
            u32 word;
            memcpy(&word, c + i, 4);
            snx_st_le32(bytes + i * 4, word);
        }
        CHECK(!gmem_write(colors + v * 16, bytes, 16));
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
    r[0x3fd] = r[0x3fe] = PL_SIDE << 16;
    r[0x680] = 0x1111;
    memcpy(r + 0x280, transform, sizeof transform);
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = stream ? colors : positions;
        r[0x458 + stream] = stream | 1u << 21 | 7u << 27; /* RGBA32F */
        r[0x700 + stream * 4] = 0x1000 | 16;
        r[0x701 + stream * 4] = (u32)(source >> 32);
        r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + 9 * 16 - 1) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + 9 * 16 - 1);
    }
    /* the references: the outline as a list of the four sides, the rectangle filled, and the device's own polygon
       modes on the two triangles where it has them */
    static u8 outline[PL_BYTES], filled[PL_BYTES], lines[PL_BYTES], points[PL_BYTES];
    const u32 sides[8] = {0, 1, 1, 2, 2, 3, 3, 0}, two[6] = {0, 1, 2, 0, 2, 3};
    CHECK(!index_buffer(channel, indices, sides, 8, 2, ~0u));
    DrawCall call = {.topology = 1, .indexed = 1, .count = 8, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_FILL, outline));
    CHECK(!index_buffer(channel, indices, two, 6, 2, ~0u));
    call = (DrawCall){.topology = 4, .indexed = 1, .count = 6, .instances = 1};
    CHECK(!pl_draw(channel, target, &call, PL_FILL, filled));
    /* the rectangle covers 11 x 8 pixels; its outline, some of them */
    if (pl_green(filled) != 88 || !pl_green(outline) || pl_green(outline) >= 88)
        fprintf(stderr, "polygon lines: the rectangle filled has %u green pixels (88 expected), its outline %u\n",
                pl_green(filled), pl_green(outline));
    CHECK(pl_green(filled) == 88 && pl_green(outline) && pl_green(outline) < 88);
    VkBool32 had = g_vk.feat.fillModeNonSolid;
    if (had) {
        CHECK(!pl_draw(channel, target, &call, PL_LINE, lines));
        CHECK(!pl_draw(channel, target, &call, PL_POINT, points));
    }
    if (had) {
        CHECK(!pl_cases(channel, target, indices, args, outline, filled, lines, points, 1));
        /* face culling of the face the rectangle doesn't show: a quad as lines keeps the device's mode, which would
           cull it, and with it the diagonal */
        u8 px[PL_BYTES];
        CHECK(!index_buffer(channel, indices, two, 6, 2, ~0u));
        channel->r3d[0x646] = 1;
        channel->r3d[0x648] = 0x405; /* back */
        CHECK(!pl_draw(channel, target, &call, PL_FILL, px));
        if (pl_green(px) != pl_green(filled)) channel->r3d[0x648] = 0x404; /* the rectangle's back is shown: front */
        DrawCall quad = {.topology = 7, .count = 4, .instances = 1};
        CHECK(!pl_draw(channel, target, &quad, PL_LINE, px));
        channel->r3d[0x646] = 0;
        CHECK(!pl_same("a quad as lines with face culling", px, lines));
    }
    g_vk.feat.fillModeNonSolid = VK_FALSE; /* a device with no polygon mode but fill, as PanVK on Bifrost */
    int failed = pl_cases(channel, target, indices, args, outline, filled, had ? lines : NULL, had ? points : NULL, 0);
    g_vk.feat.fillModeNonSolid = had;
    CHECK(!failed);
    printf("polygon lines: quads and polygons drawn as their outline, and polygons drawn as lines and points without "
           "the device's polygon modes, checked (%s)\n",
           had ? "against the device's own" : "the device has none to compare with");
    free(channel);
    return 0;
}
