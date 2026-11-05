/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Capture, save/import, and redraw a transform-feedback result without reading it on the CPU between those steps.
   Separate owner slots and temporary pause/resume must retain their own byte counters. */
static int check_nvn_xfb(void) {
    if (!g_vk.has_xfb) {
        puts("SKIP NVN transform feedback: device has no capture extension");
        return 0;
    }
    const u64 gpu = 0x4c8000000ull, cpu = HEAP_BASE + 0x720000;
    const u64 stream = gpu + 0x2000, output_a = gpu + 0x4000, output_b = gpu + 0x5000, output_c = gpu + 0x6000;
    const u64 control_a = gpu + 0x1000, control_b = gpu + 0x1100, control_c = gpu + 0x1200;
    const u32 owner_a = 0x80000003u, owner_b = 0x80000004u;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const u64 fragment[] = {0xe30000000007000full};
    const float positions[12] = {-1, -1, 1, 1, 3, -1, 1, 1, -1, 3, 1, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel && g_renderer->xfb_control);
    channel->id = 123;
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(mesh_program(gpu, vertex, 3, 0));
    CHECK(mesh_program(gpu + 0x800, fragment, 1, 1));
    CHECK(!gmem_write(stream, positions, sizeof positions));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x850] = 1;
    r[0x851] = 0x800;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 0;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof positions - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof positions - 1);
    r[0x1d1] = 1;
    r[0xe0] = 1;
    r[0xe1] = (u32)(output_a >> 32);
    r[0xe2] = (u32)output_a;
    r[0xe3] = 256;
    r[0x1c1] = 4;
    r[0x1c2] = 16;
    r[0xa00] = 0x1f1e1d1c;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    u8 initial[256], captured[256];
    memset(initial, 0xcd, sizeof initial);
    for (u32 temporary = 0; temporary < 2; temporary++) {
        DrawOwner a = {0}, b = {0};
        CHECK(!gmem_write(output_a, initial, sizeof initial));
        CHECK(!gmem_write(output_b, initial, sizeof initial));
        CHECK(!gmem_write(output_c, initial, sizeof initial));
        CHECK(!gmem_write(control_a, initial, 32));
        CHECK(!gmem_write(control_b, initial, 32));
        CHECK(!gmem_write(control_c, initial, 32));
        state->owner = owner_a;
        state->bind.xfb[0].addr = output_a;
        state->vbuf.addr[0] = stream;
        state->vbuf.size[0] = sizeof positions;
        state->gen[DS_BIND] = ds_generation_next();
        state->gen[DS_VBUF] = ds_generation_next();
        CHECK(g_renderer->xfb_control(owner_a, control_a, 0, 16, &a));
        DrawCall draw = {.topology = 0, .count = 3, .instances = 1};
        u64 drawn = g_vk_drawn;
        g_renderer->draw(state, &draw, &a);
        CHECK(g_vk_drawn == drawn + 1);
        CHECK(g_renderer->xfb_control(owner_a, temporary ? 0 : control_a, 2, 16, &a));
        state->owner = owner_b;
        state->bind.xfb[0].addr = output_b;
        state->gen[DS_BIND] = ds_generation_next();
        CHECK(g_renderer->xfb_control(owner_b, control_b, 0, 16, &b));
        draw.count = 1;
        g_renderer->draw(state, &draw, &b);
        CHECK(g_renderer->xfb_control(owner_b, control_b, 1, 16, &b));
        state->owner = owner_a;
        state->bind.xfb[0].addr = output_a;
        state->gen[DS_BIND] = ds_generation_next();
        CHECK(g_renderer->xfb_control(owner_a, temporary ? 0 : control_a, 3, 16, &a));
        draw.first = 1;
        g_renderer->draw(state, &draw, &a);
        CHECK(g_renderer->xfb_control(owner_a, control_a, 1, 16, &a));
        /* The source stream and vertex count below are still GPU-owned capture results. */
        state->bind.xfb[0].addr = output_c;
        state->vbuf.addr[0] = output_a;
        state->vbuf.size[0] = 64;
        state->gen[DS_BIND] = ds_generation_next();
        state->gen[DS_VBUF] = ds_generation_next();
        CHECK(g_renderer->xfb_control(owner_a, control_c, 0, 16, &a));
        draw = (DrawCall){.topology = 0, .count = 1, .instances = 1, .xfb_control = control_a};
        g_renderer->draw(state, &draw, &a);
        CHECK(g_renderer->xfb_control(owner_a, control_c, 1, 16, &a));
        u32 control[8];
        CHECK(!gmem_read(control_a, control, sizeof control));
        CHECK(control[0] == 16 && control[1] == 64 && control[2] == 0 && control[3] == 0 && control[4] == 0);
        CHECK(control[5] == 0xcdcdcdcdu && control[6] == 0xcdcdcdcdu && control[7] == 0xcdcdcdcdu);
        CHECK(!gmem_read(control_b, control, sizeof control) && control[0] == 16 && control[1] == 16);
        CHECK(control[5] == 0xcdcdcdcdu && control[6] == 0xcdcdcdcdu && control[7] == 0xcdcdcdcdu);
        CHECK(!gmem_read(control_c, control, sizeof control) && control[0] == 16 && control[1] == 64);
        CHECK(control[5] == 0xcdcdcdcdu && control[6] == 0xcdcdcdcdu && control[7] == 0xcdcdcdcdu);
        memcpy(initial, positions, sizeof positions);
        memcpy(initial + sizeof positions, positions + 4, 16);
        CHECK(!gmem_read(output_a, captured, sizeof captured) && !memcmp(captured, initial, sizeof captured));
        CHECK(!gmem_read(output_c, captured, sizeof captured) && !memcmp(captured, initial, sizeof captured));
        memset(initial, 0xcd, sizeof initial);
        memcpy(initial, positions, 16);
        CHECK(!gmem_read(output_b, captured, sizeof captured) && !memcmp(captured, initial, sizeof captured));
        memset(initial, 0xcd, sizeof initial);
    }
    static const u32 cases[][2] = {{16, 48}, {16, 49}, {0, 99}, {1, 0xffffffffu}, {0xffffffffu, 0xffffffffu}};
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        CHECK(!gmem_write(control_a, cases[i], sizeof cases[i]));
        vk_lock();
        VkBuffer arguments, readback;
        u64 offset;
        CHECK(vk_xfb_indirect(control_a, &arguments, &offset));
        u8* result = vk_readback_buffer(16, &readback);
        CHECK(result);
        VkBufferCopy copy = {offset, 0, 16};
        rec_CmdCopyBuffer(vk_cmd(), arguments, readback, 1, &copy);
        vk_finish();
        vk_readback_ready();
        CHECK(snx_ld_le32(result) == (cases[i][0] ? cases[i][1] / cases[i][0] : 0) && snx_ld_le32(result + 4) == 1 &&
              snx_ld_le32(result + 8) == 0 && snx_ld_le32(result + 12) == 0);
        vk_unlock();
    }
    free(channel);
    puts("PASS NVN transform feedback: capture, control save/restore, owner isolation and GPU indirect redraw exact=1");
    return 0;
}
