/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The fixed state the 3D engine's own draws take from its registers (maxwell/draw_state.c mx_draw_fixed), drawn
   through its draw (r3d_draw) and counted in the pixels covered: what a homebrew's deko3d and a game's own driver
   write, where vulkan_nvn_state.inc's check hands the renderer the NVN path's DrawCall */

/* Vertex 0 the centre of the 8x8 target, window (4, 4); 1 to 3 a triangle over the whole target at clip z -0.5,
   depth 0.25 in the depth mode from -1 to 1 a channel starts in; 4 to 9 the rectangle from clip (-1, 0) to
   (-0.5, 1), two triangles, which a mirror or a transposition moves elsewhere; 10 to 12 the triangle over the whole
   target again, at clip z 0.5 */
static const f32 g_mx_fixed_vertices[][4] = {{0, 0, 0, 1},     {-1, -1, -0.5f, 1}, {3, -1, -0.5f, 1}, {-1, 3, -0.5f, 1},
                                             {-1, 0, 0, 1},    {-0.5f, 0, 0, 1},   {-0.5f, 1, 0, 1},  {-1, 0, 0, 1},
                                             {-0.5f, 1, 0, 1}, {-1, 1, 0, 1},      {-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1},
                                             {-1, 3, 0.5f, 1}};

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

/* SET_DEPTH_BOUNDS_TEST (0x66F) and its bounds (0x3E7, 0x3E8) keep a pixel whose depth buffer holds a value inside
   them, both ends in, whatever the fragment's depth: the 3D engine's draws tested no bounds. A triangle over the
   target at depth 0.25 over a buffer of 0.5, the depth test on, always passing and writing nothing, as edenprobe's
   fixed suite draws it: bounds around the fragment's depth but not the buffer's reject every pixel */
static int check_maxwell_depth_bounds(void) {
    static const struct {
        f32 near, far;
        u32 on, want;
    } cases[] = {
        {0.2f, 0.3f, 1, 0},   /* the fragment's 0.25 inside, the buffer's 0.5 not */
        {0.4f, 0.6f, 1, 64},  /* the buffer's inside */
        {0.5f, 0.75f, 1, 64}, /* on the near end */
        {0.25f, 0.5f, 1, 64}, /* on the far end */
        {0.6f, 0.9f, 1, 0},   /* past it */
        {0.6f, 0.9f, 0, 64},  /* the same bounds, the test off */
    };
    const u64 gpu = 0x4c2010000ull, depth = gpu + 0xc000;
    const f32 stored = 0.5f;
    if (!g_vk.feat.depthBounds) {
        puts("SKIP 3D engine depth bounds: the device has no depthBounds feature");
        return 0;
    }
    GpuChannel* channel = NULL;
    CHECK(!mx_fixed_channel(158, gpu, HEAP_BASE + 0x5450000, &channel));
    u32* r = channel->r3d;
    r[0x54E] = 1; /* a Z32_FLOAT target of 8 by 8, block linear one GOB high */
    r[0x3F8] = (u32)(depth >> 32);
    r[0x3F9] = (u32)depth;
    r[0x3FA] = 0x0A;
    r[0x3FB] = 0;
    r[0x3FC] = 512 / 4;
    r[0x48A] = r[0x48B] = 8;
    r[0x48C] = 1;
    r[0x4B3] = 1;     /* the depth test, */
    r[0x4BA] = 0;     /* no writes, */
    r[0x4C3] = 0x207; /* always passing */
    memcpy(&r[0x364], &stored, sizeof stored);
    vk_lock();
    engine_clear(channel, 1);
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        r[MAXWELL3D_DEPTH_BOUNDS_ENABLE] = cases[i].on;
        memcpy(&r[MAXWELL3D_DEPTH_BOUNDS], &cases[i].near, sizeof cases[i].near);
        memcpy(&r[MAXWELL3D_DEPTH_BOUNDS + 1], &cases[i].far, sizeof cases[i].far);
        u64 covered;
        CHECK(!mx_fixed_draw(channel, 4, 1, 3, &covered));
        u32 count = (u32)snx_popcount64(covered);
        if (count != cases[i].want)
            fprintf(stderr, "3D engine depth bounds %g to %g, test %s, over a buffer of 0.5: %u pixels, expected %u\n",
                    (double)cases[i].near, (double)cases[i].far, cases[i].on ? "on" : "off", count, cases[i].want);
        CHECK(count == cases[i].want);
    }
    vk_unlock();
    free(channel);
    puts("PASS 3D engine depth bounds: the buffer's depth tested against them, ends in, off keeps every pixel");
    return 0;
}

/* whether a depth range of -1 to 1 is the device's on this device (VK_DYN_PR_DEPTH_RANGE): pipeline libraries, the
   depth clip control and the range as dynamic state, each as the device reports it and no ban holding for it */
static int mx_depth_range_device(void) {
    static const char member[] = "VK_EXT_extended_dynamic_state3.extendedDynamicState3DepthClipNegativeOneToOne";
    u32 count = 0;
    int clip_control = 0, eds3 = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &count, NULL);
    VkExtensionProperties* list = calloc(count ? count : 1, sizeof *list);
    if (!list) return -1;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &count, list);
    for (u32 i = 0; i < count; i++) {
        clip_control |= !strcmp(list[i].extensionName, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
        eds3 |= !strcmp(list[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    }
    free(list);
    if (!g_vk.has_gpl || !clip_control || !eds3 || vk_ban_find(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME) ||
        vk_ban_find(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) || vk_ban_find(member))
        return 0;
    VkPhysicalDeviceDepthClipControlFeaturesEXT control = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, .pNext = &control};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &dynamic};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &features);
    return control.depthClipControl && dynamic.extendedDynamicState3DepthClipNegativeOneToOne;
}

/* SET_DEPTH_MODE (0x35F) says whether the clip volume's depth goes from -w or from 0. A triangle over the target,
   depth clipped, tested against a buffer cleared just past and just before the depth it must have: under a depth
   scale and offset of 0.5, clip z -0.5 is depth 0.25 from -1 to 1 and before the near plane from 0 to 1, and clip
   z 0.5 is depth 0.75 in both. The range changes from draw to draw. The device makes the range of -1 to 1 where it
   can (VK_EXT_depth_clip_control as dynamic state), else the last stage before rasterization: both are checked by
   what they draw, and that the device is the one where it can be */
static int check_maxwell_depth_range(void) {
    enum { LESS = 0x201, ALWAYS = 0x207 };
    static const struct {
        u32 zero_to_one, first, func, want;
        f32 buffer;
    } cases[] = {
        {0, 1, LESS, 64, 0.3f},                          /* -1 to 1: z -0.5 is 0.25, */
        {0, 1, LESS, 0, 0.2f},                           /* no nearer */
        {1, 1, ALWAYS, 0, 1.0f},                         /* 0 to 1: z -0.5 is before the near plane */
        {1, 10, LESS, 64, 0.8f},                         /* and z 0.5 is 0.5 + 0.5 * 0.5, */
        {1, 10, LESS, 0, 0.7f},                          /* no nearer */
        {0, 10, LESS, 64, 0.8f},                         /* -1 to 1 again: z 0.5 is 0.75 there too, */
        {0, 10, LESS, 0, 0.7f},  {0, 1, LESS, 64, 0.3f}, /* and z -0.5 is drawn again */
    };
    const u64 gpu = 0x4c2030000ull, depth = gpu + 0xc000;
    GpuChannel* channel = NULL;
    CHECK(!mx_fixed_channel(160, gpu, HEAP_BASE + 0x5470000, &channel));
    u32* r = channel->r3d;
    r[0x54E] = 1; /* a Z32_FLOAT target of 8 by 8, block linear one GOB high */
    r[0x3F8] = (u32)(depth >> 32);
    r[0x3F9] = (u32)depth;
    r[0x3FA] = 0x0A;
    r[0x3FB] = 0;
    r[0x3FC] = 512 / 4;
    r[0x48A] = r[0x48B] = 8;
    r[0x48C] = 1;
    r[0x4B3] = 1;        /* the depth test, */
    r[0x4BA] = 0;        /* no writes, */
    r[0x64F] = 1u << 11; /* and depth clipped, not clamped */
    int device = mx_depth_range_device();
    CHECK(device >= 0);
    if (((g_vk.dyn_pr & VK_DYN_PR_DEPTH_RANGE) != 0) != device) {
        fprintf(stderr, "3D engine depth range: the device %s make a range of -1 to 1 and the renderer %s it\n",
                device ? "can" : "can't", g_vk.dyn_pr & VK_DYN_PR_DEPTH_RANGE ? "takes" : "doesn't take");
        CHECK(0);
    }
    vk_lock();
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        r[0x35F] = cases[i].zero_to_one;
        r[0x4C3] = cases[i].func;
        memcpy(&r[0x364], &cases[i].buffer, sizeof cases[i].buffer);
        engine_clear(channel, 1);
        u64 covered;
        CHECK(!mx_fixed_draw(channel, 4, cases[i].first, 3, &covered));
        u32 count = (u32)snx_popcount64(covered);
        if (count != cases[i].want)
            fprintf(stderr,
                    "3D engine depth range %s, clip z %g, %s a buffer of %g (case %u): %u pixels, expected %u\n",
                    cases[i].zero_to_one ? "0 to 1" : "-1 to 1", (double)g_mx_fixed_vertices[cases[i].first][2],
                    cases[i].func == LESS ? "nearer than" : "always passing over", (double)cases[i].buffer, i, count,
                    cases[i].want);
        CHECK(count == cases[i].want);
    }
    vk_unlock();
    free(channel);
    printf("PASS 3D engine depth range: -1 to 1 made by %s, 0 to 1, and one after the other\n",
           device ? "the device" : "the last stage");
    return 0;
}

/* the pixels of columns x0 to x1 and rows y0 to y1 of the 8x8 target, both ends in, as fixed_state_covered's bits */
static u64 mx_fixed_rect(u32 x0, u32 x1, u32 y0, u32 y1) {
    u64 bits = 0;
    for (u32 y = y0; y <= y1; y++)
        for (u32 x = x0; x <= x1; x++) bits |= 1ull << (y * 8 + x);
    return bits;
}

/* The viewport's coordinate swizzle (0x286) moves the picture as the console's does, each of x and y taking the
   component its code names, negated or not: NegativeY through the viewport's sign, the others in the last stage,
   where the swap of x and y drew the picture as it was. The rectangle from clip (-1, 0) to (-0.5, 1) under a scale
   of 4 and offset of 4, y counting down the target's rows from -1 */
static int check_maxwell_viewport_swizzle(void) {
    const struct {
        u32 swizzle;
        u64 want;
    } cases[] = {
        {0x6420, mx_fixed_rect(0, 1, 4, 7)}, /* identity */
        {0x6430, mx_fixed_rect(0, 1, 0, 3)}, /* NegativeY: the viewport's */
        {0x6402, mx_fixed_rect(4, 7, 0, 1)}, /* PositiveY into x, PositiveX into y: transposed */
        {0x6412, mx_fixed_rect(4, 7, 6, 7)}, /* PositiveY into x, NegativeX into y */
        {0x6403, mx_fixed_rect(0, 3, 0, 1)}, /* NegativeY into x, PositiveX into y */
    };
    GpuChannel* channel = NULL;
    CHECK(!mx_fixed_channel(159, 0x4c2020000ull, HEAP_BASE + 0x5460000, &channel));
    vk_lock();
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        channel->r3d[0x286] = cases[i].swizzle;
        u64 covered;
        CHECK(!mx_fixed_draw(channel, 4, 4, 6, &covered));
        if (covered != cases[i].want)
            fprintf(stderr, "3D engine viewport swizzle %04x covered pixels %016llx, expected %016llx\n",
                    cases[i].swizzle, (unsigned long long)covered, (unsigned long long)cases[i].want);
        CHECK(covered == cases[i].want);
    }
    vk_unlock();
    free(channel);
    puts("PASS 3D engine viewport swizzle: NegativeY, a swap of x and y and swaps with a component negated");
    return 0;
}
