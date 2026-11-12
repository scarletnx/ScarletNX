/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Dynamic handles alternate the active target and an ordinary texture. All fragments read column1. That column
   changes during the first draw, so an exact pre-draw copy and a refreshed copy on the next draw are both required:
   each later draw follows a texture barrier, as the console's texture reads see earlier draws' writes only after one */
#include "nx/gpu/nvn/nvn_blend.h"

static int check_indexed_texture_feedback(void) {
    if (!g_shader_texture_arrays) {
        puts("SKIP NVN indexed texture feedback: non-uniform sampled image arrays are unavailable");
        puts("SKIP NVN indexed programmable blend: non-uniform sampled image arrays are unavailable");
        return 0;
    }
    const u64 gpu = 0x7d0000000ull, cpu = HEAP_BASE + 0x10400000;
    const u64 target = gpu + 0x4000, ordinary = gpu + 0x6000, pool = gpu + 0x8000;
    const u64 samplers = gpu + 0x9000, constants = gpu + 0xa000;
    /* sample_raster_channel supplies a full-screen triangle and independent fixture memory. */
    GpuChannel* channel = sample_raster_channel(gpu, cpu, 0);
    CHECK(channel);
    u8 initial[64], texture_bytes[512] = {0}, ordinary_pixels[64];
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            u8 before[4] = {(u8)(10 + 30 * x), (u8)(20 + 30 * y), (u8)(30 + 4 * y + x), 255};
            u8 sampled[4] = {180, (u8)(5 + 10 * x), (u8)(10 + 20 * y), 255};
            memcpy(initial + (4 * y + x) * 4, before, 4);
            memcpy(ordinary_pixels + (4 * y + x) * 4, sampled, 4);
            memcpy(texture_bytes + gob_x_offset(4 * x) + gob_y_offset(y), sampled, 4);
        }
    CHECK(!gmem_write(ordinary, texture_bytes, sizeof texture_bytes));
    NvnTexture image = {.gpu = target};
    image.d.target = NVN_TEXTURE_TARGET_2D;
    image.d.format = NVN_FORMAT_RGBA8;
    image.d.width = image.d.height = 4;
    image.d.depth = image.d.levels = 1;
    u32 tic[8];
    CHECK(nvn_tic_encode(&image, NULL, 0, 0, tic));
    CHECK(!storage_write_words(pool, tic, 8));
    image.gpu = ordinary;
    CHECK(nvn_tic_encode(&image, NULL, 0, 0, tic));
    CHECK(!storage_write_words(pool + 32, tic, 8));
    CHECK(!storage_write_words(samplers, (const u32[8]){2u | 2u << 3 | 2u << 6}, 8));
    u8 table[256] = {0};
    for (u32 i = 0; i < 4; i++) snx_st_le32(table + 0x40 + 16 * i, i & 1);
    CHECK(!gmem_write(constants, table, sizeof table));
    const u64 fragment[] = {
        0xe000000000070002ull | 0x1cull << 30, /* IPA R2, fragment X */
        0xe000000000070003ull | 0x1dull << 30, /* IPA R3, fragment Y */
        0x5cb0000000070002ull | 3ull << 39 | 2ull << 20 | 2ull << 10 | 2ull << 8,
        0x5cb0000000070003ull | 3ull << 39 | 3ull << 20 | 2ull << 10 | 2ull << 8,
        0x3848000000470205ull, /* SHL R5, R2, 4 */
        0xef94002004070504ull, /* LDC R4, c[2][R5 +0x40] */
        0x0100000000071002ull | 1ull << 20, /* MOV R2,1: sample the column written through the other handle */
        0xdd380007a0470200ull, /* TLD.B.LZ R0..3, R2, handle R4 */
        0xe30000000007000full};
    CHECK(mesh_program(gpu + 0xb000, fragment, sizeof fragment / sizeof *fragment, 1));
    u64 advanced_fragment[8];
    for (u32 i = 0, output = 0; i < sizeof fragment / sizeof *fragment; i++)
        if (i != 6) advanced_fragment[output++] = fragment[i]; /* retain this fragment's own X coordinate */
    CHECK(mesh_program(gpu + 0xb800, advanced_fragment, sizeof advanced_fragment / sizeof *advanced_fragment, 1));
    channel->r3d[0x851] = 0xb000;
    vk_lock();
    rt_clear(channel, target, 4, 4, 0, 0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.tex_pool = pool; state->bind.smp_pool = samplers;
    state->bind.tex_max = 1; state->bind.smp_max = 0;
    state->bind.cb_addr[ST_FS][2] = constants;
    state->bind.cb_size[ST_FS][2] = sizeof table;
    state->blend.en[0] = 0; state->blend.mask[0] = 15;
    state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = state->dyn.alpha_test = 0;
    state->rkey.sample_mask = ~0u;
    Surf* surface = vk_rt_find(target);
    CHECK(surface);
    VkBuffer upload; u64 offset;
    u8* staged = vk_stage(sizeof initial, 4, &upload, &offset);
    CHECK(staged);
    memcpy(staged, initial, sizeof initial);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {.bufferOffset = offset, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {4, 4, 1}};
    rec_CmdCopyBufferToImage(vk_cmd(), upload, surface->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vk_surf_wrote(surface); vk_rt_drawn(surface); vk_surf_touch(surface);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    u64 uploads_before = g_vk_itable_uploads, reused_before = g_vk_itable_reused, bytes_before = g_vk_itable_bytes;
    u64 uniform_frame = vk_frame_id();
    int advanced = g_vk.has_fragment_interlock && g_vk.feat.fragmentStoresAndAtomics &&
                   g_vk.feat.shaderStorageImageReadWithoutFormat && g_vk.feat.shaderStorageImageWriteWithoutFormat &&
                   surface->storage_capable && g_vk.image_slots[ST_FS];
    u32 native_advanced = g_vk.advanced_blend_caps.available;
    for (u32 draw = 0; draw < (advanced ? 5u : 3u); draw++) {
        if (draw == 2) {
            /* A different table map must upload new bytes rather than retain the earlier alternating map. */
            for (u32 i = 0; i < 4; i++) snx_st_le32(table + 0x40 + 16 * i, i >= 2);
            CHECK(!gmem_write(constants, table, sizeof table));
            DrawClear clear = {.color_mask = 15};
            memcpy(clear.color, (const float[4]){0, 0, 1, 1}, sizeof clear.color);
            vk_render_clear(state, &clear, &channel->owner);
        }
        if (draw == 3) {
            /* Alpha1 and binary RGB make Difference exactly abs(destination-source), with no reference-emitter reuse.
               Force shader blending. Native advanced blend support must not make this table fixture pass. */
            g_vk.advanced_blend_caps.available = 0;
            for (u32 i = 0; i < 4; i++) snx_st_le32(table + 0x40 + 16 * i, i & 1);
            CHECK(!gmem_write(constants, table, sizeof table));
            for (u32 y = 0; y < 4; y++)
                for (u32 x = 0; x < 4; x++)
                    memcpy(texture_bytes + gob_x_offset(4 * x) + gob_y_offset(y), (const u8[4]){0, 255, 0, 255}, 4);
            CHECK(!gmem_write(ordinary, texture_bytes, sizeof texture_bytes));
            state->prog.code[5] = gpu + 0xb800;
            state->blend.en[0] = 1;
            state->gen[DS_PROGRAMS] = state->gen[DS_BLEND] = ds_generation_next();
            call.advanced_blend[0] = nvn_set_field(nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV,
                NVN_BLEND_ADVANCED_MODE_DIFFERENCE), NVN_BLEND_ADV_PMS, 1), NVN_BLEND_ADV_NPM, 1);
            DrawClear clear = {.color_mask = 15};
            memcpy(clear.color, (const float[4]){1, 0, 0, 1}, sizeof clear.color);
            vk_render_clear(state, &clear, &channel->owner);
        }
        u64 drawn = g_vk_drawn, copies = g_vk_rt_copies;
        u64 uploads = g_vk_itable_uploads, reused = g_vk_itable_reused;
        if (draw) g_renderer->method_barrier(0); /* NVN's ORDER_FRAGMENTS | INVALIDATE_TEXTURE */
        vk_render_draw(state, &call, &channel->owner);
        CHECK(g_vk_drawn == drawn + 1 && g_vk_rt_copies > copies);
        if (draw == 1 && vk_frame_id() == uniform_frame)
            CHECK(g_vk_itable_uploads == uploads && g_vk_itable_reused > reused);
        if (draw == 2) CHECK(g_vk_itable_uploads > uploads);
        vk_render_end();
        u8 actual[64];
        CHECK(!ms_read_pixels(surface, 4, 4, actual));
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                const u8 difference[4] = {x & 1 ? 255 : 0, draw == 3 && (x & 1) ? 255 : 0, 0, 255};
                const u8* expected = draw >= 3 ? difference
                    : draw == 2 && x < 2 ? (const u8[4]){0, 0, 255, 255}
                    : (draw || (x & 1) ? ordinary_pixels : initial) + (4 * y + 1) * 4;
                const u8* pixel = actual + (4 * y + x) * 4;
                if (memcmp(pixel, expected, 4)) {
                    fprintf(stderr, "Indexed feedback draw%u pixel%u,%u: %u,%u,%u,%u != %u,%u,%u,%u\n",
                            draw, x, y, pixel[0], pixel[1], pixel[2], pixel[3], expected[0], expected[1],
                            expected[2], expected[3]);
                    CHECK(!memcmp(pixel, expected, 4));
                }
            }
    }
    g_vk.advanced_blend_caps.available = native_advanced;
    vk_unlock();
    free(channel);
    if (advanced)
        puts("PASS NVN indexed programmable blend: Difference output, target snapshots and next-draw refresh exact=1");
    else
        puts("SKIP NVN indexed programmable blend: ordered storage-image blending is unavailable");
    printf("Indexed uniform staging: %llu uploads, %llu reuses, %llu bytes, image bytes checked\n",
           (unsigned long long)(g_vk_itable_uploads - uploads_before),
           (unsigned long long)(g_vk_itable_reused - reused_before),
           (unsigned long long)(g_vk_itable_bytes - bytes_before));
    puts("PASS NVN indexed texture feedback: pre-draw target, ordinary texture, next-draw refresh and changed table exact=1");
    return 0;
}
