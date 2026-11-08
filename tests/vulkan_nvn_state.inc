/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Fixed point size, alpha reference and depth bounds change actual covered pixels. No guest data or shaders. */
/* the pixels of the 8x8 RGBA32F target at target holding the drawn color, float (1, 0, 0, 0.5), in covered: bit
   y * 8 + x. A pixel neither that nor the clear's blue fails, named with what drew it */
static int fixed_state_covered(u64 target, const char* what, u64* covered) {
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
    *covered = 0;
    for (u32 i = 0; i < 64; i++) {
        const u8* pixel = pixels + i * 16;
        u32 rgba[4];
        for (u32 c = 0; c < 4; c++) rgba[c] = snx_ld_le32(pixel + c * 4);
        if (rgba[0] == 0x3f800000 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 0x3f000000)
            *covered |= 1ull << i;
        else if (!(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0x3f800000 && rgba[3] == 0x3f800000)) {
            fprintf(stderr, "%s fixed state pixel%u float RGBA bits=%08x,%08x,%08x,%08x\n", what, i, rgba[0], rgba[1],
                    rgba[2], rgba[3]);
            CHECK(0);
        }
    }
    return 0;
}
static int nvn_state_pixels(const DsState* state, const DrawCall* call, DrawOwner* owner, u64 target, u32 want) {
    DrawClear clear = {.color_mask = 15, .color = {0, 0, 0x3f800000, 0x3f800000}};
    vk_render_clear(state, &clear, owner);
    vk_render_draw(state, call, owner);
    vk_render_end();
    u64 covered;
    CHECK(!fixed_state_covered(target, "NVN", &covered));
    u32 count = (u32)snx_popcount64(covered);
    if (count != want) fprintf(stderr, "NVN fixed state covered %u pixels, expected %u\n", count, want);
    CHECK(count == want);
    return 0;
}

static int check_nvn_fixed_state(void) {
    const u64 gpu = 0x4c4000000ull, cpu = HEAP_BASE + 0x710000;
    const u64 position = gpu + 0x2000, target = gpu + 0x8000, depth = gpu + 0xc000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const u64 fragment[] = {storage_mov(0, 0x3f800000), storage_mov(1, 0), storage_mov(2, 0),
                            storage_mov(3, 0x3f000000), 0xe30000000007000full};
    const f32 center[4] = {0, 0, 0, 1}, transform[6] = {4, 4, 0.5f, 4, 4, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 119;
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(position, center, sizeof center));
    CHECK(mesh_program(gpu, vertex, 3, 0));
    CHECK(mesh_program(gpu + 0x1000, fragment, 5, 1));
    g_renderer->method_barrier(5);
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x850] = 1;
    r[0x851] = 0x1000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 8u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(position >> 32);
    r[0x702] = (u32)position;
    r[0x7c0] = (u32)((position + 15) >> 32);
    r[0x7c1] = (u32)(position + 15);
    vk_lock();
    int is_depth;
    rt_clear_format(channel, target, 8, 8, 0, 1, nvn_rt_format(NVN_FORMAT_RGBA32F, &is_depth));
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->dyn.alpha_test = 1;
    state->dyn.alpha_func = DS_CMP_GREATER;
    state->dyn.alpha_ref = 0.25f;
    state->gen[DS_DYNAMIC] = ds_generation_next();
    DrawCall call = {.topology = 0, .count = 1, .instances = 1};
    CHECK(!nvn_state_pixels(state, &call, &channel->owner, target, 1));
    u32 side = g_vk.feat.largePoints && g_vk.props.limits.pointSizeRange[1] >= 4 ? 4 : 1;
    call.state_flags = DS_DRAW_POINT_SIZE;
    call.point_size = (f32)side;
    CHECK(!nvn_state_pixels(state, &call, &channel->owner, target, side * side));
    state->dyn.alpha_ref = 0.75f;
    state->gen[DS_DYNAMIC] = ds_generation_next();
    CHECK(!nvn_state_pixels(state, &call, &channel->owner, target, 0));
    state->dyn.alpha_ref = 0.25f;
    state->gen[DS_DYNAMIC] = ds_generation_next();
    if (g_vk.feat.depthBounds) {
        state->tgt.depth =
            (DsSurf){.gva = depth, .width = 8, .height = 8, .format = 0x0a, .layers = 1, .layer_stride = 512};
        state->gen[DS_TARGETS] = ds_generation_next();
        state->gen[DS_TARGET_FORMATS] = ds_generation_next();
        DrawClear clear = {.aspects = 1, .depth = 0.5f};
        vk_render_clear(state, &clear, &channel->owner);
        call.state_flags |= DS_DRAW_DEPTH_BOUNDS;
        call.depth_bounds[0] = 0.2f;
        call.depth_bounds[1] = 0.4f;
        CHECK(!nvn_state_pixels(state, &call, &channel->owner, target, 0));
        call.depth_bounds[0] = 0.4f;
        call.depth_bounds[1] = 0.6f;
        CHECK(!nvn_state_pixels(state, &call, &channel->owner, target, side * side));
        call.depth_bounds[0] = 0.7f;
        call.depth_bounds[1] = 0.9f;
        call.state_flags &= ~DS_DRAW_DEPTH_BOUNDS;
        CHECK(!nvn_state_pixels(state, &call, &channel->owner, target, side * side));
    } else
        puts("SKIP NVN depth bounds output: the device has no depthBounds feature");
    vk_unlock();
    free(channel);
    puts("PASS NVN fixed state: alpha reference, fixed point size and available depth bounds exact=1");
    return 0;
}
