/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Each input vertex has its own position and generic color. The evaluation program selects one input and emits
   a point. Fixed positive levels generate that point, and zero outer levels cull the patch. Exact positions and
   colors catch a control stage that forwards the wrong input vertex or attribute. */
static int nvn_tess_pixels(const DsState* state, const DrawCall* call, DrawOwner* owner, u64 target, u32 vertex,
                           int visible) {
    DrawClear clear = {.color_mask = 15, .color = {0, 0, 0x3f800000, 0x3f800000}};
    vk_render_clear(state, &clear, owner);
    vk_render_draw(state, call, owner);
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* pixels = vk_readback_buffer(8 * 8 * 16, &buffer);
    CHECK(surface && pixels);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {8, 8, 1}};
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 64; i++) {
        u32 expected[4] = {0, 0, 0x3f800000, 0x3f800000};
        if (visible && i == 4 * 8 + 2 + vertex * 2) {
            expected[2] = 0;
            expected[vertex] = 0x3f800000;
            expected[3] = 0x3f000000;
        }
        for (u32 c = 0; c < 4; c++) {
            u32 value = snx_ld_le32(pixels + i * 16 + c * 4);
            if (value != expected[c]) {
                fprintf(stderr, "NVN tessellation vertex%u visible%d pixel(%u,%u) component%u: %08x expected %08x\n",
                        vertex, visible, i % 8, i / 8, c, value, expected[c]);
                CHECK(0);
            }
        }
    }
    return 0;
}

static int check_nvn_default_tessellation(void) {
    if (!g_vk.feat.tessellationShader || g_vk_stage_sets < 5) {
        puts("SKIP NVN evaluation-only tessellation: host tessellation feature or descriptor sets unavailable");
        return 0;
    }
    const u64 gpu = 0x4c6000000ull, cpu = HEAP_BASE + 0x690000;
    const u64 position = gpu + 0x3000, color = gpu + 0x4000, target = gpu + 0x8000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xefd9ff800907ff00ull, 0xeff1ff800807ff00ull,
                          0xe30000000007000full};
    u64 evaluation[] = {0,
                        0xefd982000707ff00ull,
                        0xeff1ff800707ff00ull,
                        0xefd982000807ff00ull,
                        0xeff1ff800807ff00ull,
                        0xe30000000007000full}; /* ALD vertex register R4 */
    u64 fragment[5];
    /* Pixel centers avoid an edge tie when a host rasterizes the one-pixel point. */
    const f32 positions[12] = {-0.375f, 0.125f, 0, 1, 0.125f, 0.125f, 0, 1, 0.625f, 0.125f, 0, 1};
    const f32 colors[12] = {1, 0, 0, 0.5f, 0, 1, 0, 0.5f, 0, 0, 1, 0.5f};
    const f32 transform[6] = {4, 4, 0.5f, 4, 4, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 118;
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(position, positions, sizeof positions));
    CHECK(!gmem_write(color, colors, sizeof colors));
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(gpu, vertex, 5, 0));
    for (u32 i = 0; i < 3; i++) {
        evaluation[0] = storage_mov(4, i);
        CHECK(mesh_program(gpu + 0x1000 + i * 0x800, evaluation, 6, 0));
    }
    CHECK(mesh_program(gpu + 0x2800, fragment, 5, 1));
    g_renderer->method_barrier(5);
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x830] = 1;
    r[0x831] = 0x1000; /* evaluation, without a control program */
    r[0x850] = 1;
    r[0x851] = 0x2800;
    r[0xc8] = 1;  /* triangle domain, equal spacing, point output */
    r[0x373] = 3; /* input control points */
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 8u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = stream ? color : position;
        r[0x458 + stream] = stream | 1u << 21 | 7u << 27;
        r[0x700 + stream * 4] = 0x1000 | 16;
        r[0x701 + stream * 4] = (u32)(source >> 32);
        r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + 47) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + 47);
    }
    vk_lock();
    int is_depth;
    rt_clear_format(channel, target, 8, 8, 0, 1, nvn_rt_format(NVN_FORMAT_RGBA32F, &is_depth));
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 14,
                     .count = 3,
                     .instances = 1,
                     .state_flags = DS_DRAW_TESS_LEVELS,
                     .tess_levels = {1, 1, 1, 1, 1, 1}};
    for (u32 vertex_index = 0; vertex_index < 3; vertex_index++) {
        r[0x831] = 0x1000 + vertex_index * 0x800;
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        for (u32 i = 0; i < 6; i++) call.tess_levels[i] = 1;
        CHECK(!nvn_tess_pixels(&channel->ds, &call, &channel->owner, target, vertex_index, 1));
        call.tess_levels[0] = call.tess_levels[1] = call.tess_levels[2] = 0;
        CHECK(!nvn_tess_pixels(&channel->ds, &call, &channel->owner, target, vertex_index, 0));
        for (u32 i = 0; i < 6; i++) call.tess_levels[i] = 2;
        CHECK(!nvn_tess_pixels(&channel->ds, &call, &channel->owner, target, vertex_index, 1));
    }
    if (g_vk.feat.shaderClipDistance && g_vk.feat.shaderTessellationAndGeometryPointSize) {
        /* Different producer/consumer clip indices must still have matching eight-element interfaces. */
        for (u32 read_last = 0; read_last < 2; read_last++)
            for (u32 negative = 0; negative < 2; negative++) {
                u64 builtins_vertex[10];
                memcpy(builtins_vertex, vertex, 4 * sizeof *vertex);
                builtins_vertex[4] = storage_mov(8, negative ? 0xbf800000u : 0x3f800000u);
                builtins_vertex[5] = 0xeff000000007ff08ull | (0x2dcull << 20);
                builtins_vertex[6] = storage_mov(8, 0x3f800000u);
                builtins_vertex[7] = 0xeff000000007ff08ull | (0x6cull << 20);
                builtins_vertex[8] = 0xe30000000007000full;
                u64 builtins_evaluation[] = {
                    storage_mov(4, 0), evaluation[1], evaluation[2], evaluation[3], evaluation[4],
                    0xefd800000007ff08ull | (4ull << 39) | (0x6cull << 20),
                    0xeff000000007ff08ull | (0x6cull << 20),
                    0xefd800000007ff08ull | (4ull << 39) | ((read_last ? 0x2dcull : 0x2c0ull) << 20),
                    0xeff000000007ff08ull | (0x2c0ull << 20), 0xe30000000007000full};
                CHECK(mesh_program(gpu, builtins_vertex, 9, 0));
                CHECK(mesh_program(gpu + 0x5000, builtins_evaluation, 10, 0));
                g_renderer->method_barrier(5);
                r[0x831] = 0x5000;
                channel->r3d_dirty = ~0u;
                mx_draw_state_sync(channel);
                channel->ds.prog.clip_enable = 1;
                channel->ds.gen[DS_PROGRAMS] = ds_generation_next();
                for (u32 i = 0; i < 6; i++) call.tess_levels[i] = 1;
                CHECK(!nvn_tess_pixels(&channel->ds, &call, &channel->owner, target, 0,
                                      !(read_last && negative)));
            }
    }
    vk_unlock();
    free(channel);
    puts("PASS NVN evaluation-only tessellation: position/generic forwarding and fixed levels exact=1");
    return 0;
}
