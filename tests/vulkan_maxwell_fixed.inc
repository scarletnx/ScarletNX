/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The fixed state the 3D engine's own draws take from its registers (maxwell/draw_state.c mx_draw_fixed), drawn
   through its draw (r3d_draw) and counted in the pixels covered: what a homebrew's deko3d and a game's own driver
   write, where vulkan_nvn_state.inc's check hands the renderer the NVN path's DrawCall */

/* Vertex 0 the centre of the 8x8 target, window (4, 4) */
static const f32 g_mx_fixed_vertices[][4] = {{0, 0, 0, 1}};

/* a channel drawing into the 8x8 RGBA32F target at gpu + 0x8000 with the vertices above, attribute 0 the position,
   in red (1, 0, 0, 0.5) over a blue clear, as check_nvn_fixed_state draws: the code at gpu, the vertices at
   gpu + 0x2000, all in the 64 KiB at gpu that cpu backs, in *made (freed by the caller). The registers are a
   channel's after reset_3d where a draw reads them */
static int mx_fixed_channel(u32 id, u64 gpu, u64 cpu, GpuChannel** made) {
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const u64 fragment[] = {storage_mov(0, 0x3f800000), storage_mov(1, 0), storage_mov(2, 0),
                            storage_mov(3, 0x3f000000), 0xe30000000007000full};
    const f32 transform[6] = {4, 4, 0.5f, 4, 4, 0.5f}, one = 1.0f;
    const u64 position = gpu + 0x2000, end = position + sizeof g_mx_fixed_vertices - 1;
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    *made = channel;
    channel->id = id;
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0); /* past the heap */
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(position, g_mx_fixed_vertices, sizeof g_mx_fixed_vertices));
    CHECK(mesh_program(gpu, vertex, 3, 0));
    CHECK(mesh_program(gpu + 0x1000, fragment, 5, 1));
    g_renderer->method_barrier(5);
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x850] = 1;
    r[0x851] = 0x1000;
    r[0x64b] = 1;
    r[0x286] = MAXWELL3D_VIEWPORT_SWIZZLE_IDENTITY;
    r[0x3fd] = r[0x3fe] = 8u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(&r[MAXWELL3D_POINT_SIZE], &one, sizeof one);
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(position >> 32);
    r[0x702] = (u32)position;
    r[0x7c0] = (u32)(end >> 32);
    r[0x7c1] = (u32)end;
    int is_depth;
    rt_clear_format(channel, gpu + 0x8000, 8, 8, 0, 1, nvn_rt_format(NVN_FORMAT_RGBA32F, &is_depth));
    return 0;
}

/* the pixels the 3D engine's draw of count vertices from first, of its topology, covers over a blue clear, in
   covered (fixed_state_covered's bits) */
static int mx_fixed_draw(GpuChannel* channel, u32 topology, u32 first, u32 count, u64* covered) {
    u64 drawn = g_vk_drawn;
    engine_clear(channel, 15 << 2); /* the color, blue: rt_clear_format's clear color */
    channel->r3d_dirty = ~0u;       /* the registers were set directly */
    r3d_draw(channel, 0, topology, first, count, 0, 0, 1);
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    return fixed_state_covered(channel->r3d[0x201] | (u64)channel->r3d[0x200] << 32, "3D engine", covered);
}

/* SET_POINT_SIZE (0x546) sizes the points of a program that writes none, changed between two draws: the 3D engine's
   draws drew every point 1 pixel wide, whatever it held. A point at window (4, 4), nudged into the pixel after the
   border (render.c), of side 4 covers 4 by 4 pixels and of side 8 the whole target */
static int check_maxwell_point_size(void) {
    static const f32 sides[3] = {4, 1, 8};
    GpuChannel* channel = NULL;
    CHECK(!mx_fixed_channel(157, 0x4c2000000ull, HEAP_BASE + 0x5440000, &channel));
    vk_lock();
    for (u32 i = 0; i < 3; i++) {
        f32 side = sides[i];
        if (!g_vk.feat.largePoints || g_vk.props.limits.pointSizeRange[1] < side) {
            printf("SKIP 3D engine point size %g: the device draws points up to %g wide%s\n", (double)side,
                   (double)g_vk.props.limits.pointSizeRange[1], g_vk.feat.largePoints ? "" : " (no largePoints)");
            continue;
        }
        memcpy(&channel->r3d[MAXWELL3D_POINT_SIZE], &side, sizeof side);
        u64 covered;
        CHECK(!mx_fixed_draw(channel, 0, 0, 1, &covered));
        u32 count = (u32)snx_popcount64(covered), want = (u32)(side * side);
        if (count != want)
            fprintf(stderr, "3D engine point size %g covered %u pixels, expected %u\n", (double)side, count, want);
        CHECK(count == want);
    }
    vk_unlock();
    free(channel);
    puts("PASS 3D engine point size: points of side 4, 1 and 8 cover their squares");
    return 0;
}
