/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Force ordered programmable blending on RGBA32F, including primitive overlap, masks and discarded fragments. */
#include "nx/gpu/nvn/nvn_blend.h"
#include "support/nvn_advanced_blend_ref.h"
#include "vulkan_nvn_advanced_targets.inc"

static int check_nvn_advanced_shader(void) {
    if (!g_vk.has_fragment_interlock || !g_vk.feat.fragmentStoresAndAtomics ||
        !g_vk.feat.shaderStorageImageReadWithoutFormat ||
        !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("SKIP shader advanced targets: ordered storage-image blending is unavailable");
        puts("SKIP NVN shader advanced blend: ordered storage-image blending is unavailable");
        return 0;
    }
    u32 native_available = g_vk.advanced_blend_caps.available;
    g_vk.advanced_blend_caps.available = 0; /* exercise the same portable path on every device */
    const u64 gpu = 0x4f0000000ull, cpu = HEAP_BASE + 0x7a0000;
    const u64 stream = gpu + 0x2000, target = gpu + 0x4000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xefd9ff800907ff00ull, 0xeff1ff800807ff00ull,
                          0xe30000000007000full};
    const float positions[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    static const float colors[4][8] = {{0.125f, 0.0625f, 0.1875f, 0.25f, 0.5625f, 0.1875f, 0.375f, 0.75f},
                                       {0.75f, 0.5f, 0.25f, 0.75f, 0.75f, 0.25f, 0.5f, 0.75f},
                                       {0, 0, 0, 0, 0.125f, 0.125f, 0.125f, 0.25f},
                                       {0.75f, 0.5f, 0.125f, 0.25f, 0.25f, 0.125f, 0.0625f, 0.25f}};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 124;
    gmmu_map(gpu, cpu, 0x10000);
    u64 fragment[5];
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(gpu, vertex, 5, 0));
    CHECK(mesh_program(gpu + 0x800, fragment, 5, 1));
    float vertices[48];
    for (u32 i = 0; i < 6; i++) {
        memcpy(vertices + i * 8, positions + (i % 3) * 4, 16);
        memcpy(vertices + i * 8 + 4, colors[0], 16);
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
    for (u32 sample = 0; sample < 4; sample++) {
        const float* source = colors[sample];
        const float* destination = colors[sample] + 4;
        for (u32 i = 0; i < 6; i++) memcpy(vertices + i * 8 + 4, source, 16);
        CHECK(!gmem_write(stream, vertices, sizeof vertices));
        g_renderer->method_barrier(2);
        for (u32 mode = NVN_BLEND_ADVANCED_MODE_ZERO; mode <= NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY; mode++)
            for (u32 overlap = 0; overlap < 3; overlap++)
                for (u32 premultiplied = 0; premultiplied < 2; premultiplied++) {
                    /* Extended-range RGB is legal for the arithmetic equations; it distinguishes alpha-clamping from RGB
           clamping. HSL requires normalized unpremultiplied RGB, so that case is excluded there. */
                    if (sample == 3 &&
                        !(mode >= NVN_BLEND_ADVANCED_MODE_PLUS && mode <= NVN_BLEND_ADVANCED_MODE_PLUS_DARKER))
                        continue;
                    DrawClear clear = {.color_mask = 15};
                    memcpy(clear.color, destination, 16);
                    vk_render_clear(&channel->ds, &clear, &channel->owner);
                    u64 packed = nvn_set_field(0, NVN_BLEND_ADV, mode);
                    packed = nvn_set_field(packed, NVN_BLEND_ADV_PMS, premultiplied);
                    packed = nvn_set_field(packed, NVN_BLEND_ADV_OVL, overlap);
                    packed = nvn_set_field(packed, NVN_BLEND_ADV_NPM, 1);
                    channel->ds.blend.en[0] = 1;
                    channel->ds.blend.mask[0] = 15;
                    /* Match the frontend: modes with an exact ordinary-blend reduction retain that fast path. */
                    (void)nvn_blend_advanced(&channel->ds.blend, 0, packed);
                    channel->ds.gen[DS_BLEND] = ds_generation_next();
                    DrawCall draw = {.topology = 4, .count = 3, .instances = 1};
                    draw.advanced_blend[0] = packed;
                    /* One draw touches each sample twice. A whole-draw composite or a noncoherent path cannot pass this. */
                    if (mode == NVN_BLEND_ADVANCED_MODE_OVERLAY && premultiplied && !overlap) draw.count = 6;
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
                    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                                             &region);
                    vk_finish();
                    vk_readback_ready();
                    float expected[4], pixel[4];
                    memcpy(pixel, actual, sizeof pixel);
                    nvn_advanced_blend_reference(mode, (int)premultiplied, overlap, source, destination, expected);
                    if (draw.count == 6) {
                        float intermediate[4];
                        memcpy(intermediate, expected, sizeof intermediate);
                        nvn_advanced_blend_reference(mode, (int)premultiplied, overlap, source, intermediate, expected);
                    }
                    for (u32 component = 0; component < 4; component++) {
                        if (!(fabsf(pixel[component] - expected[component]) < 0.00005f))
                            fprintf(stderr, "advanced blend mode%u component%u: %g instead of %g\n", mode, component,
                                    pixel[component], expected[component]);
                        CHECK(fabsf(pixel[component] - expected[component]) < 0.00005f);
                    }
                }
    }
    /* Alpha testing discards before the ordered read/modify/write. */
    for (u32 partial = 0; partial < 2; partial++) {
        DrawClear clear = {.color_mask = 15};
        memcpy(clear.color, colors[0] + 4, 16);
        vk_render_clear(&channel->ds, &clear, &channel->owner);
        for (u32 i = 0; i < 6; i++) memcpy(vertices + i * 8 + 4, colors[0], 16);
        CHECK(!gmem_write(stream, vertices, sizeof vertices));
        g_renderer->method_barrier(2);
        channel->ds.dyn.alpha_test = !partial;
        channel->ds.dyn.alpha_func = DS_CMP_GREATER;
        channel->ds.dyn.alpha_ref = 0.5f;
        channel->ds.gen[DS_DYNAMIC] = ds_generation_next();
        channel->ds.blend.mask[0] = partial ? 5 : 15; /* retain destination G/A */
        channel->ds.gen[DS_BLEND] = ds_generation_next();
        DrawCall draw = {.topology = 4, .count = 3, .instances = 1};
        draw.advanced_blend[0] = nvn_set_field(
            nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV, NVN_BLEND_ADVANCED_MODE_OVERLAY), NVN_BLEND_ADV_NPM, 1),
            NVN_BLEND_ADV_PMS, 1);
        vk_render_draw(&channel->ds, &draw, &channel->owner);
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
        float pixel[4], expected[4];
        memcpy(pixel, actual, 16);
        memcpy(expected, colors[0] + 4, 16);
        if (partial) {
            float result[4];
            nvn_advanced_blend_reference(NVN_BLEND_ADVANCED_MODE_OVERLAY, 1, 0, colors[0], colors[0] + 4, result);
            expected[0] = result[0];
            expected[2] = result[2];
        }
        for (u32 i = 0; i < 4; i++) CHECK(fabsf(pixel[i] - expected[i]) < 0.00005f);
    }
    for (u32 i = 0; i < 6; i++) memcpy(vertices + i * 8 + 4, colors[0], 16);
    CHECK(!gmem_write(stream, vertices, sizeof vertices));
    g_renderer->method_barrier(2);
    CHECK(!advanced_target_variants(channel, target));
    vk_unlock();
    g_vk.advanced_blend_caps.available = native_available;
    free(channel);
    puts(
        "PASS NVN shader advanced blend: all46 modes, overlap/premultiplication, primitive order, masks and discard checked=1");
    return 0;
}
