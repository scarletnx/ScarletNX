/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/nvn/nvn_blend.h"
#include "support/nvn_advanced_blend_ref.h"
#include "support/blend_image_program.h"

/* Ordered attachment blending and guest storage images share a fragment program, with independent oracles for
   all sixteen blended pixels, all copied integer texels, the source image and destination padding. */
static int check_advanced_blend_images(void) {
    if (!g_vk.has_fragment_interlock || !g_vk.feat.fragmentStoresAndAtomics ||
        !g_vk.feat.shaderStorageImageReadWithoutFormat || !g_vk.feat.shaderStorageImageWriteWithoutFormat ||
        g_vk.image_slots[ST_FS] < 2) {
        puts("SKIP advanced blend with guest image: ordered storage-image blending is unavailable");
        return 0;
    }
    u32 native_available = g_vk.advanced_blend_caps.available;
    g_vk.advanced_blend_caps.available = 0;
    const u64 gpu = 0x4f9000000ull, cpu = HEAP_BASE + 0x740000;
    const u64 stream = gpu + 0x2000, pool = gpu + 0x4000, cbuf = gpu + 0x5000;
    const u64 source_image = gpu + 0x6000, stored_image = gpu + 0x7000, target = gpu + 0x8000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float source_color[4] = {0.125f, 0.0625f, 0.1875f, 0.25f};
    const float destination[4] = {0.5625f, 0.1875f, 0.375f, 0.75f};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    const u32 format = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    const u32 headers[16] = {
        format, (u32)source_image, (u32)(source_image >> 32) | 3u << 21, 0, 3u | 1u << 23, 3u | 1u << 31, 0, 0,
        format, (u32)stored_image, (u32)(stored_image >> 32) | 3u << 21, 0, 3u | 1u << 23, 3u | 1u << 31, 0, 0};
    u8 input[512], initial[512], expected[512], result[512];
    memset(input, 0xa5, sizeof input);
    memset(initial, 0xcd, sizeof initial);
    memcpy(expected, initial, sizeof expected);
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            u64 offset = bl_offset(x * 4, y, 0, 16, 4, 0, 0);
            u32 value = 0x13570000u + (4 * y + x) * 0x10203u;
            snx_st_le32(input + offset, value);
            snx_st_le32(expected + offset, value);
        }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 116;
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(stream, position, sizeof position));
    CHECK(!gmem_write(source_image, input, sizeof input));
    CHECK(!gmem_write(stored_image, initial, sizeof initial));
    CHECK(!storage_write_words(pool, headers, 16));
    CHECK(!storage_write_words(pool + 0x100, (const u32[8]){0}, 8));
    CHECK(!storage_write_words(cbuf + 0x128, (const u32[2]){0, 1}, 2));
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(mesh_program(gpu + 0x800, g_blend_image_instructions,
                       sizeof g_blend_image_instructions / sizeof *g_blend_image_instructions, 1));
    g_renderer->method_barrier(5);
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32), r[0x583] = (u32)gpu;
    r[0x55d] = (u32)(pool >> 32), r[0x55e] = (u32)pool, r[0x55f] = 1;
    r[0x557] = (u32)((pool + 0x100) >> 32), r[0x558] = (u32)(pool + 0x100), r[0x559] = 0;
    r[0x850] = 1, r[0x851] = 0x800;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(stream >> 32), r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof position - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof position - 1);
    channel->ds.bind.cb_addr[ST_FS][2] = cbuf;
    channel->ds.bind.cb_size[ST_FS][2] = 0x200;
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DrawClear clear = {.color_mask = 15};
    memcpy(clear.color, destination, sizeof destination);
    vk_render_clear(&channel->ds, &clear, &channel->owner);
    u64 advanced = nvn_set_field(0, NVN_BLEND_ADV, NVN_BLEND_ADVANCED_MODE_OVERLAY);
    advanced = nvn_set_field(advanced, NVN_BLEND_ADV_PMS, 1);
    advanced = nvn_set_field(advanced, NVN_BLEND_ADV_NPM, 1);
    channel->ds.blend.en[0] = 1;
    channel->ds.blend.mask[0] = 15;
    channel->ds.gen[DS_BLEND] = ds_generation_next();
    DrawCall draw = {.topology = 4, .count = 3, .instances = 1};
    draw.advanced_blend[0] = advanced;
    u64 drawn = g_vk_drawn;
    vk_render_draw(&channel->ds, &draw, &channel->owner);
    vk_render_end();
    CHECK(g_vk_drawn == drawn + 1);
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* actual = vk_readback_buffer(16 * 16, &buffer);
    CHECK(surface && actual);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    vk_finish();
    vk_readback_ready();
    float blended[4];
    nvn_advanced_blend_reference(NVN_BLEND_ADVANCED_MODE_OVERLAY, 1, 0, source_color, destination, blended);
    for (u32 pixel = 0; pixel < 16; pixel++)
        for (u32 component = 0; component < 4; component++) {
            float value;
            memcpy(&value, actual + pixel * 16 + component * 4, sizeof value);
            if (!(fabsf(value - blended[component]) < 0.00005f))
                fprintf(stderr, "advanced blend with image pixel%u component%u: %g expected %g\n", pixel, component,
                        value, blended[component]);
            CHECK(fabsf(value - blended[component]) < 0.00005f);
        }
    vk_unlock();
    CHECK(!gmem_read(stored_image, result, sizeof result));
    CHECK(!memcmp(result, expected, sizeof result));
    CHECK(!gmem_read(source_image, result, sizeof result));
    CHECK(!memcmp(result, input, sizeof result));
    g_vk.advanced_blend_caps.available = native_available;
    free(channel);
    puts(
        "PASS advanced blend with guest image: sixteen blended pixels, independent integer stores and padding exact=1");
    return 0;
}
