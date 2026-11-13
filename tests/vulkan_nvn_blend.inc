/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Check the portable advanced-blend reductions through real render pipelines against coverage-weight equations.
   Float targets keep the comparison separate from UNORM quantization; all source/destination values stay in[0,1]. */
#include "nx/gpu/nvn/nvn_blend.h"
#include "support/nvn_advanced_blend_ref.h"

static int check_nvn_advanced_blend(void) {
    const u64 gpu = 0x4d8000000ull, cpu = HEAP_BASE + 0x6a0000;
    const u64 stream = gpu + 0x2000, target = gpu + 0x4000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xefd9ff800907ff00ull, 0xeff1ff800807ff00ull,
                          0xe30000000007000full};
    const float positions[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float source[4] = {0.125f, 0.375f, 0.25f, 0.5f}, destination[4] = {0.5625f, 0.1875f, 0.375f, 0.75f};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    static const u32 modes[] = {
        NVN_BLEND_ADVANCED_MODE_ZERO,     NVN_BLEND_ADVANCED_MODE_SRC,      NVN_BLEND_ADVANCED_MODE_DST,
        NVN_BLEND_ADVANCED_MODE_SRC_OVER, NVN_BLEND_ADVANCED_MODE_DST_OVER, NVN_BLEND_ADVANCED_MODE_SRC_IN,
        NVN_BLEND_ADVANCED_MODE_DST_IN,   NVN_BLEND_ADVANCED_MODE_SRC_OUT,  NVN_BLEND_ADVANCED_MODE_DST_OUT,
        NVN_BLEND_ADVANCED_MODE_SRC_ATOP, NVN_BLEND_ADVANCED_MODE_DST_ATOP, NVN_BLEND_ADVANCED_MODE_XOR,
        NVN_BLEND_ADVANCED_MODE_SCREEN};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 122;
    gmmu_map(gpu, cpu, 0x10000);
    u64 fragment[5];
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(gpu, vertex, 5, 0));
    CHECK(mesh_program(gpu + 0x800, fragment, 5, 1));
    float vertices[24];
    for (u32 i = 0; i < 3; i++) {
        memcpy(vertices + i * 8, positions + i * 4, 16);
        memcpy(vertices + i * 8 + 4, source, 16);
    }
    CHECK(!gmem_write(stream, vertices, sizeof vertices));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x850] = 1;
    r[0x851] = 0x800;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x459] = 16u << 7 | 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof vertices - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof vertices - 1);
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    for (u32 mode = 0; mode < sizeof modes / sizeof *modes; mode++) {
        DrawClear clear = {.color_mask = 15};
        memcpy(clear.color, destination, sizeof destination);
        vk_render_clear(&channel->ds, &clear, &channel->owner);
        u64 packed = nvn_set_field(0, NVN_BLEND_ADV, modes[mode]);
        packed = nvn_set_field(packed, NVN_BLEND_ADV_PMS, 1);
        packed = nvn_set_field(packed, NVN_BLEND_ADV_NPM, 1);
        CHECK(nvn_blend_advanced(&channel->ds.blend, 0, packed));
        channel->ds.gen[DS_BLEND] = ds_generation_next();
        DrawCall draw = {.topology = 4, .count = 3, .instances = 1};
        u64 drawn = g_vk_drawn;
        vk_render_draw(&channel->ds, &draw, &channel->owner);
        CHECK(g_vk_drawn == drawn + 1);
        vk_render_end();
        Surf* surface = vk_rt_find(target);
        VkBuffer buffer;
        u8* actual = vk_readback_buffer(16, &buffer);
        CHECK(surface && actual);
        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageOffset = {1, 1, 0},
                                    .imageExtent = {1, 1, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        vk_finish();
        vk_readback_ready();
        float expected[4], pixel[4];
        memcpy(pixel, actual, sizeof pixel);
        nvn_advanced_blend_reference(modes[mode], 1, 0, source, destination, expected);
        for (u32 component = 0; component < 4; component++) {
            if (!(fabsf(pixel[component] - expected[component]) < 0.000001f))
                fprintf(stderr, "advanced blend mode%u component%u: %g instead of %g\n", modes[mode], component,
                        pixel[component], expected[component]);
            CHECK(fabsf(pixel[component] - expected[component]) < 0.000001f);
        }
    }
    vk_unlock();
    free(channel);
    puts("PASS NVN advanced blend: portable Porter-Duff and screen equations through real pipelines exact=1");
    return 0;
}
