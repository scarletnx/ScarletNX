/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

enum { VARYING_W = 256, VARYING_H = 256 };

/* a program whose generics 0 and 1 a fragment stage interpolates with perspective */
static int varying_program(u64 address, const u64* ops, u32 count, int fragment) {
    u64 code[64] = {0};
    u32 at = 10;
    if (fragment) {
        code[3] = 0xaaaa;
        code[9] = 15;
    }
    for (u32 i = 0; i < count; i++) {
        if (!(i % 3)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[i];
    }
    return invalidate_write_code(address, code, 64);
}

/* Draws the plane and reads what each fragment got: generic 0, the position itself, over its w, and generic 1,
   (2 x + 3 w, y - w), over that w. They are (x, y) and (2 x + 3, y - 1) of the fragment's own place in clip space,
   which its pixel gives. The largest difference, in pixels of the target, over the rows the plane covers whole, for
   each generic */
static int varying_error(GpuChannel* channel, u64 target, u32 topology, float error[2]) {
    u64 drawn = g_vk_drawn;
    const u32 bytes = VARYING_W * VARYING_H * 16;
    vk_lock();
    rt_clear_format(channel, target, VARYING_W, VARYING_H, 0, 0, 0xC0); /* RGBA32F */
    channel->r3d_dirty = ~0u;                                           /* the registers were set directly */
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = topology, .count = topology == 7 ? 4 : 6, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(bytes, &buffer);
    CHECK(surface && surface->w == VARYING_W && surface->h == VARYING_H && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {VARYING_W, VARYING_H, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    error[0] = error[1] = 0;
    /* the plane is y = -1 before the division: it ends at the horizon, the middle row */
    for (u32 y = 0; y < VARYING_H / 2 - 4; y++)
        for (u32 x = 0; x < VARYING_W; x++) {
            float got[4];
            memcpy(got, data + (y * VARYING_W + x) * 16, 16);
            float cx = ((float)x + 0.5f) / VARYING_W * 2 - 1, cy = ((float)y + 0.5f) / VARYING_H * 2 - 1;
            float want[4] = {cx, cy, 2 * cx + 3, cy - 1};
            const float pixels[4] = {VARYING_W / 2.0f, VARYING_H / 2.0f, VARYING_W / 4.0f, VARYING_H / 2.0f};
            for (u32 k = 0; k < 4; k++) {
                float off = (got[k] - want[k]) * pixels[k];
                if (!(off >= 0)) off = -off; /* a NaN counts as the largest */
                if (!(off <= error[k / 2])) error[k / 2] = off == off ? off : 1e9f;
            }
        }
    vk_unlock();
    return 0;
}

/* A fragment program's perspective inputs on a huge primitive clipped at the eye: a plane from behind the eye to 5000
   units in front of it, 40000 wide, as one game draws its sea of clouds. The console interpolates the vertices'
   values; a host GPU that clips the primitive first interpolates the clipped vertices' values with less precision
   than the position (25 pixels off on this plane, RADV on an RX 6700 XT), and a program that divides such an input by
   its w to find its own pixel reads a neighbour's depth */
static int check_varying_passes(void) {
    const u64 address = 0xb20000000ull, cpu = HEAP_BASE + 0x16000000;
    const u64 vertices = address + 0x12000, target = address + 0x100000;
/* ALD Rd, a[0x80 + 4 d]: a word of the vertex's attributes 0 and 1, a register each */
#define VARYING_ALD(d) (0xefd87f8000070000ull | (0x80ull + 4 * (d)) << 20 | 0xff00ull | (u64)(d))
    /* then AST a[0x70] and a[0x80] from R0..3, the position and generic 0, the position again, and a[0x90] from R4..7,
       generic 1 */
    const u64 vertex[] = {VARYING_ALD(0),        VARYING_ALD(1),        VARYING_ALD(2),        VARYING_ALD(3),
                          VARYING_ALD(4),        VARYING_ALD(5),        VARYING_ALD(6),        VARYING_ALD(7),
                          0xeff1ff800707ff00ull, 0xeff1ff800807ff00ull, 0xeff1ff800907ff04ull, 0xe30000000007000full};
#undef VARYING_ALD
    const u64 fragment[] = {0xe000000000070000ull | 0x20ull << 30 | 0, /* IPA R0, a[0x80] */
                            0xe000000000070000ull | 0x21ull << 30 | 1, /* IPA R1, a[0x84] */
                            0xe000000000070000ull | 0x24ull << 30 | 2, /* IPA R2, a[0x90] */
                            0xe000000000070000ull | 0x25ull << 30 | 3, /* IPA R3, a[0x94] */
                            0xe30000000007000full};
    /* the plane's corners in clip space: two behind the eye (w -50), two far in front of it, z from -w at the near
       plane, 0.1 from the eye */
    const float corner[4][4] = {
        {-20000, -1, -50.2f, -50}, {20000, -1, -50.2f, -50}, {20000, -1, 4999.8f, 5000}, {-20000, -1, 4999.8f, 5000}};
    const u32 triangles[6] = {0, 1, 2, 0, 2, 3};
    const float transform[6] = {VARYING_W / 2.0f, VARYING_H / 2.0f, 0.5f, VARYING_W / 2.0f, VARYING_H / 2.0f, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 126;
    CHECK(mem_map(cpu, 0x400000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(address, cpu, 0x400000);
    CHECK(varying_program(address, vertex, 12, 0));
    CHECK(varying_program(address + 0x10000, fragment, 5, 1));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = VARYING_W << 16;
    r[0x3fe] = VARYING_H << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 0 | 1u << 21 | 7u << 27;            /* stream 0 at 0: RGBA32F */
    r[0x459] = 0 | 16u << 7 | 1u << 21 | 7u << 27; /* stream 0 at 16: RGBA32F */
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)(vertices >> 32);
    r[0x702] = (u32)vertices;
    float error[2];
    for (u32 topology = 4; topology <= 7; topology += 3) {
        u8 bytes[6 * 32];
        u32 count = topology == 7 ? 4 : 6;
        for (u32 v = 0; v < count; v++) {
            const float* p = corner[topology == 7 ? v : triangles[v]];
            float values[8] = {p[0], p[1], p[2], p[3], 2 * p[0] + 3 * p[3], p[1] - p[3], 0, p[3]};
            for (u32 i = 0; i < 8; i++) {
                u32 word;
                memcpy(&word, values + i, 4);
                snx_st_le32(bytes + v * 32 + 4 * i, word);
            }
        }
        CHECK(!gmem_write(vertices, bytes, count * 32));
        r[0x7c0] = (u32)((vertices + count * 32 - 1) >> 32);
        r[0x7c1] = (u32)(vertices + count * 32 - 1);
        CHECK(!varying_error(channel, target, topology, error));
        printf("varying passes: %s of a plane clipped at the eye: the position generic over its w is %.4f pixels off "
               "at most, another generic %.4f\n",
               topology == 7 ? "a quad" : "triangles", (double)error[0], (double)error[1]);
        /* the position pass, on every driver but NVIDIA's (which needs none): the fragment's own place */
        CHECK(g_vk.driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY || error[0] < 1.0f / 64);
    }
    vk_lock();
    vk_render_end();
    vk_unlock();
    free(channel);
    return 0;
}
