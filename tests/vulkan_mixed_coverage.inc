/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Force the portable path. Full overlapping triangles, explicit masks and a constant fragment program give an
   independent per-pixel oracle. Native blending must run once per primitive after coverage reduction, including
   odd-only masks whose bit0 is clear. Read all sixteen pixels, not a representative pixel. */
#include "nx/gpu/vk/capture.h"
#include "vulkan_mixed_coverage_mrt.inc"
static int check_mixed_coverage_portable(void) {
    const VkImageFormatProperties narrow = {.maxExtent = {4, 4, 1}, .maxMipLevels = 1, .maxArrayLayers = 1,
                                           .sampleCounts = VK_SAMPLE_COUNT_4_BIT};
    CHECK(vk_capture_validity_format(&narrow, 4, 4, 4, 1) == VK_FORMAT_R8_UNORM);
    CHECK(vk_capture_validity_format(NULL, 4, 4, 4, 1) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(vk_capture_validity_format(&narrow, 2, 4, 4, 1) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(vk_capture_validity_format(&narrow, 4, 5, 4, 1) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(vk_capture_validity_format(&narrow, 4, 4, 5, 1) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(vk_capture_validity_format(&narrow, 4, 4, 4, 2) == VK_FORMAT_R8G8B8A8_UNORM);
    const u64 gpu = 0x8f0000000ull, cpu = HEAP_BASE + 0x11700000;
    const u64 stream = gpu + 0x2000, target = gpu + 0x4000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xefd9ff800907ff00ull,
                          0xeff1ff800807ff00ull, 0xe30000000007000full};
    const float positions[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float source[4] = {1, 0.5f, 0.25f, 0.75f}, destination[4] = {0.125f, 0.25f, 0.5f, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 149;
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    u64 fragment[5];
    for (u32 i = 0; i < 4; i++) {
        u32 bits;
        memcpy(&bits, source + i, sizeof bits);
        fragment[i] = 0x0100000000071000ull | (u64)bits << 20 | i;
    }
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(gpu, vertex, 5, 0));
    CHECK(mesh_program(gpu + 0x800, fragment, 5, 1));
    float vertices[48];
    for (u32 i = 0; i < 6; i++) {
        memcpy(vertices + i * 8, positions + (i % 3) * 4, 16);
        memcpy(vertices + i * 8 + 4, source, 16);
    }
    CHECK(!gmem_write(stream, vertices, sizeof vertices));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32); r[0x583] = (u32)gpu;
    r[0x850] = 1; r[0x851] = 0x800; r[0x64b] = 1;
    r[0x286] = 0x6420; r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111; r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27; r[0x459] = 16u << 7 | 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 32; r[0x701] = (u32)(stream >> 32); r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof vertices - 1) >> 32); r[0x7c1] = (u32)(stream + sizeof vertices - 1);
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = 0;
    state->dyn.alpha_test = 0;
    state->rkey.sample_shading = state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
    state->gen[DS_DYNAMIC] = ds_generation_next();
    int native = g_vk.has_mixed_samples;
    VkBool32 read_without = g_vk.feat.shaderStorageImageReadWithoutFormat;
    VkBool32 write_without = g_vk.feat.shaderStorageImageWriteWithoutFormat;
    VkBool32 storage_multisample = g_vk.feat.shaderStorageImageMultisample;
    g_vk.has_mixed_samples = 0;
    g_vk.feat.shaderStorageImageReadWithoutFormat = 0;
    g_vk.feat.shaderStorageImageWriteWithoutFormat = 0;
    g_vk.feat.shaderStorageImageMultisample = 0;
    u32 draws = 0, pixels = 0, counts = 0, first_raster = 0;
    for (u32 raster = 2; raster <= 16; raster *= 2) {
        Surf* surface = vk_rt_find(target);
        VkCapture* capture = vk_capture_get_mixed(surface, raster);
        if (!capture) continue;
        CHECK(capture->usage == (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        CHECK(!capture->source.storage_capable && !capture->valid.storage_capable);
        VkImageFormatProperties r8;
        int supported_r8 = vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, VK_FORMAT_R8_UNORM,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &r8) == VK_SUCCESS &&
            (r8.sampleCounts & raster) && r8.maxExtent.width >= 4 && r8.maxExtent.height >= 4 && r8.maxArrayLayers;
        CHECK(capture->valid.f.vk == (supported_r8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM));
        CHECK(capture->valid.f.bpp == (supported_r8 ? 1 : 4));
        if (first_raster) CHECK(!capture->mixed_set); /* a different sample shape retires both cached views */
        counts++;
        if (!first_raster) first_raster = raster;
        const u32 all = (1u << raster) - 1;
        const u32 masks[] = {0, 1, all, 0xaaaau & all, 1u << (raster - 1)};
        for (u32 mode = 0; mode < 4; mode++)
            for (u32 custom = 0; custom < 2; custom++)
                for (u32 mask = 0; mask < sizeof masks / sizeof *masks; mask++)
                    for (u32 blend = 0; blend < 3; blend++) {
                        const u32 primitives = blend ? 2 : 1;
                        DrawClear clear = {.color_mask = 15};
                        memcpy(clear.color, destination, sizeof destination);
                        vk_render_clear(state, &clear, &channel->owner);
                        DrawCall call = {.topology = 4, .count = 3 * primitives, .instances = 1,
                            .raster_samples = raster, .coverage_mode = mode, .coverage_table_enable = custom};
                        for (u32 i = 0; i < 16; i++) call.coverage_table[i] = (u8)((i * 5 + 3) % 17);
                        state->rkey.sample_mask = masks[mask];
                        state->gen[DS_RASTER_KEY] = ds_generation_next();
                        state->blend.en[0] = blend != 0;
                        state->blend.mask[0] = blend == 2 ? 5 : 15;
                        state->blend.logic_op = 0;
                        state->blend.cop[0] = state->blend.aop[0] = DS_BLEND_ADD;
                        state->blend.sc[0] = state->blend.sa[0] = blend == 2 ? DS_FACTOR_SRC_ALPHA : DS_FACTOR_ONE;
                        state->blend.dc[0] = state->blend.da[0] = blend == 2 ? DS_FACTOR_ONE_MINUS_SRC_ALPHA : DS_FACTOR_ONE;
                        state->gen[DS_BLEND] = ds_generation_next();
                        u64 before = g_vk_drawn;
                        vk_render_draw(state, &call, &channel->owner);
                        CHECK(g_vk_drawn == before + 1);
                        CHECK(capture->mixed_set && capture->mixed_frame == vk_frame_id());
                        if (mode == 3 && !custom && mask == 1 && !blend) {
                            VkDescriptorSet immutable = capture->mixed_set;
                            u64 frame = capture->mixed_frame;
                            DrawCall changed = call;
                            changed.coverage_table_enable = 1;
                            for (u32 i = 0; i < 16; i++) changed.coverage_table[i] = (u8)((3 * i + 2) % 17);
                            changed.coverage_table[0] = (u8)(16 / raster); /* same observed factor, different table data */
                            before = g_vk_drawn;
                            vk_render_draw(state, &changed, &channel->owner);
                            CHECK(g_vk_drawn == before + 1 && capture->mixed_set == immutable && capture->mixed_frame == frame);
                            draws++;
                        }
                        vk_render_end();
                        VkBuffer buffer;
                        u8* data = vk_readback_buffer(16 * 16, &buffer);
                        surface = vk_rt_find(target);
                        CHECK(data && surface);
                        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_READ_BIT);
                        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                    .imageExtent = {4, 4, 1}};
                        rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
                        vk_finish();
                        vk_readback_ready();
                        u32 covered = 0;
                        for (u32 i = 0; i < raster; i++) if (masks[mask] & (1u << i)) covered++;
                        float expected[4];
                        memcpy(expected, destination, sizeof expected);
                        if (covered) {
                            const float factor = custom ? (float)((((covered - 1) * 16 / raster) * 5 + 3) % 17) / 16
                                                        : (float)covered / (float)raster;
                            float reduced[4];
                            for (u32 c = 0; c < 4; c++)
                                reduced[c] = source[c] * ((c == 3 ? mode & 2 : mode & 1) ? factor : 1);
                            for (u32 primitive = 0; primitive < primitives; primitive++)
                                for (u32 c = 0; c < 4; c++) {
                                    if (blend == 2 && !(5u & (1u << c))) continue;
                                    expected[c] = !blend ? reduced[c] : blend == 1 ? reduced[c] + expected[c]
                                        : reduced[c] * reduced[3] + expected[c] * (1 - reduced[3]);
                                }
                        }
                        for (u32 pixel = 0; pixel < 16; pixel++) {
                            float actual[4];
                            memcpy(actual, data + pixel * 16, sizeof actual);
                            for (u32 c = 0; c < 4; c++) {
                                if (actual[c] != expected[c])
                                    fprintf(stderr, "mixed portable N%u mask%x mode%u table%u blend%u pixel%u c%u: %g != %g\n",
                                            raster, masks[mask], mode, custom, blend, pixel, c, actual[c], expected[c]);
                                CHECK(actual[c] == expected[c]);
                            }
                            pixels++;
                        }
                        draws++;
                        if (mode == 1 && custom && mask == 1 && !blend) {
                            u64 old_frame = capture->mixed_frame;
                            vk_frame_next();
                            vk_cmd();
                            CHECK(vk_frame_id() != old_frame && capture->mixed_frame == old_frame);
                            /* The next resolve must replace its pool-lifetime shortcut without updating the old set. */
                        }
                    }
    }
    /* Native integer logic applies after conversion to normalized attachment bits. Keep the written red and
       the untouched green/blue/alpha binary so this independent oracle has no rounding tolerance. */
    if (first_raster && g_vk.feat.logicOp) {
        const u64 normalized_target = gpu + 0x8000;
        rt_clear_format(channel, normalized_target, 4, 4, 0, 0, 0xd5);
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = state->dyn.alpha_test = 0;
        state->rkey.sample_shading = state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
        state->rkey.sample_mask = 1u << (first_raster - 1);
        state->blend.en[0] = 0; state->blend.mask[0] = 1;
        state->blend.logic_op = VK_LOGIC_OP_XOR + 1;
        state->gen[DS_DYNAMIC] = state->gen[DS_RASTER_KEY] = state->gen[DS_BLEND] = ds_generation_next();
        for (u32 primitives = 1; primitives <= 2; primitives++) {
            DrawClear clear = {.color_mask = 15, .color = {0, 0x3f800000u, 0, 0x3f800000u}};
            vk_render_clear(state, &clear, &channel->owner);
            DrawCall call = {.topology = 4, .count = 3 * primitives, .instances = 1, .raster_samples = first_raster};
            u64 before = g_vk_drawn;
            vk_render_draw(state, &call, &channel->owner);
            CHECK(g_vk_drawn == before + 1);
            vk_render_end();
            VkBuffer buffer;
            u8* data = vk_readback_buffer(64, &buffer);
            Surf* surface = vk_rt_find(normalized_target);
            CHECK(data && surface);
            vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
            rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            vk_finish();
            vk_readback_ready();
            for (u32 pixel = 0; pixel < 16; pixel++) {
                CHECK(data[4 * pixel] == (primitives == 1 ? 255 : 0));
                CHECK(data[4 * pixel + 1] == 255 && data[4 * pixel + 2] == 0 && data[4 * pixel + 3] == 255);
                pixels++;
            }
            draws++;
        }
    }
    g_vk.has_mixed_samples = native;
    g_vk.feat.shaderStorageImageReadWithoutFormat = read_without;
    g_vk.feat.shaderStorageImageWriteWithoutFormat = write_without;
    g_vk.feat.shaderStorageImageMultisample = storage_multisample;
    vk_unlock();
    free(channel);
    if (!counts) puts("SKIP portable mixed coverage: multisample float capture images unavailable");
    else printf("PASS portable mixed coverage: %u draws, %u pixels, %u raster counts; masks/table/native blend/overlap exact=1\n",
                draws, pixels, counts);
    return check_mixed_coverage_mrt();
}
