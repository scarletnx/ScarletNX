/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Native late tests decide capture validity. Guest atomic stores must run once even for a depth-rejected,
   alpha-rejected or killed fragment, and stencil writes must retain their fail/zfail/pass behavior. */
#include "nx/gpu/nvn/nvn_blend.h"
#include "support/nvn_advanced_blend_ref.h"

static int blend_capture_program(u64 address, int kill, int zero_alpha) {
    u64 operations[20] = {
        0xe000000000070000ull | 0x1cull << 30, /* IPA R0, fragment x */
        0xe000000000070001ull | 0x1dull << 30, /* IPA R1, fragment y */
        0x5cb0018000070a00ull,                 /* F2I.U32.F32.TRUNC R0, R0 */
        0x5cb0018000170a01ull,                 /* F2I.U32.F32.TRUNC R1, R1 */
        0x4c98078804a70004ull,                 /* MOV R4, c[2][0x128] */
        0x0100000000071008ull | 1ull << 20,   /* MOV32I R8,1 */
        (0xeb50020a00070008ull & ~(7ull << 33)) | 3ull << 33 /* SURED.ADD.U32 [R0,R1],R8,R4 */
    };
    u32 count = 7;
    if (kill) operations[count++] = 0xe33000000007000full;
    const u32 color[4] = {0x3e000000, 0x3d800000, 0x3e400000, 0x3e800000};
    for (u32 component = 0; component < 4; component++)
        operations[count++] = 0x0100000000071000ull | (u64)(zero_alpha ? 0 : color[component]) << 20 | component;
    operations[count++] = 0xe30000000007000full;
    return mesh_program(address, operations, count, 1);
}

static int check_nvn_blend_capture(void) {
    if (!g_vk.feat.fragmentStoresAndAtomics || !g_vk.feat.shaderStorageImageReadWithoutFormat ||
        !g_vk.feat.shaderStorageImageWriteWithoutFormat || !g_vk.image_slots[ST_FS]) {
        puts("SKIP NVN late blend capture: fragment image writes are unavailable");
        return 0;
    }
    u32 native_available = g_vk.advanced_blend_caps.available;
    g_vk.advanced_blend_caps.available = 0;
    const u64 gpu = 0x8d0000000ull, cpu = HEAP_BASE + 0x11500000;
    const u64 stream = gpu + 0x2000, pool = gpu + 0x4000, cbuf = gpu + 0x5000;
    const u64 counters = gpu + 0x6000, target = gpu + 0x8000, depth = gpu + 0xa000;
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 115;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const float position[24] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1,
                                -1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(blend_capture_program(gpu + 0x800, 0, 0));
    CHECK(blend_capture_program(gpu + 0xc00, 1, 0));
    CHECK(blend_capture_program(gpu + 0x1000, 0, 1));
    CHECK(!gmem_write(stream, position, sizeof position));
    const u32 format = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    const u32 header[8] = {format, (u32)counters, (u32)(counters >> 32) | 3u << 21,
                            0, 3u | 1u << 23, 3u | 1u << 31, 0, 0};
    CHECK(!storage_write_words(pool, header, 8));
    CHECK(!storage_write_words(pool + 0x100, (const u32[8]){0}, 8));
    CHECK(!storage_write_words(cbuf + 0x128, (const u32[1]){0}, 1));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32), r[0x583] = (u32)gpu;
    r[0x55d] = (u32)(pool >> 32), r[0x55e] = (u32)pool, r[0x55f] = 0;
    r[0x557] = (u32)((pool + 0x100) >> 32), r[0x558] = (u32)(pool + 0x100), r[0x559] = 0;
    r[0x850] = 1, r[0x851] = 0x800;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
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
    DsState* state = &channel->ds;
    state->tgt.depth = (DsSurf){.gva = depth, .width = 4, .height = 4, .format = 0x19,
                                .layers = 1, .layer_stride = 512};
    const float source[4] = {0.125f, 0.0625f, 0.1875f, 0.25f};
    const float destination[4] = {0.5625f, 0.1875f, 0.375f, 0.75f};
    for (u32 test = 0; test < 7; test++) {
        u8 initial[512], expected_counters[512], actual_counters[512];
        memset(initial, 0xcd, sizeof initial);
        memcpy(expected_counters, initial, sizeof initial);
        u32 primitives = test == 4 || test == 5 ? 2 : 1;
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                u64 offset = bl_offset(x * 4, y, 0, 16, 4, 0, 0);
                snx_st_le32(initial + offset, 0);
                snx_st_le32(expected_counters + offset, primitives);
            }
        CHECK(!gmem_write(counters, initial, sizeof initial));
        g_renderer->method_barrier(5);
        state->prog.code[ST_FS + 1] = gpu + (test == 3 ? 0xc00 : test == 6 ? 0x1000 : 0x800);
        state->prog.early_z = 0;
        state->dyn.alpha_test = test == 2;
        state->dyn.alpha_func = DS_CMP_GREATER;
        state->dyn.alpha_ref = 0.5f;
        state->dyn.depth_test = 1;
        state->dyn.depth_write = test != 5;
        state->dyn.depth_cmp = DS_CMP_LESS;
        state->dyn.stencil_test = 1;
        for (u32 face = 0; face < 2; face++) {
            state->dyn.face[face].cmp = DS_CMP_ALWAYS;
            state->dyn.face[face].fail = DS_STENCIL_ZERO;
            state->dyn.face[face].zfail = DS_STENCIL_INCREMENT_AND_CLAMP;
            state->dyn.face[face].pass = test == 5 ? DS_STENCIL_INCREMENT_AND_CLAMP : DS_STENCIL_REPLACE;
            state->dyn.face[face].ref = 9;
            state->dyn.face[face].cmask = state->dyn.face[face].wmask = 255;
        }
        state->blend.en[0] = 1;
        state->blend.mask[0] = 15;
        state->tgt.count = 1;
        state->rkey.sample_mask = ~0u;
        state->gen[DS_PROGRAMS] = state->gen[DS_TARGETS] = state->gen[DS_TARGET_FORMATS] =
            state->gen[DS_DYNAMIC] = state->gen[DS_BLEND] = state->gen[DS_RASTER_KEY] = ds_generation_next();
        DrawClear clear = {.color_mask = 15, .aspects = 3, .depth = test == 0 ? 0.25f : 1, .stencil = 4};
        memcpy(clear.color, destination, sizeof destination);
        vk_render_clear(state, &clear, &channel->owner);
        u32 mode = test == 6 ? NVN_BLEND_ADVANCED_MODE_CONTRAST : NVN_BLEND_ADVANCED_MODE_OVERLAY;
        u64 packed = nvn_set_field(nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV, mode), NVN_BLEND_ADV_PMS, 1),
                                    NVN_BLEND_ADV_NPM, 1);
        DrawCall call = {.topology = 4, .count = primitives * 3, .instances = 1};
        call.advanced_blend[0] = packed;
        u64 skipped = g_vk_skipped;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(g_vk_skipped == skipped);
        vk_render_end();
        Surf* color_surface = vk_rt_find(target);
        Surf* depth_surface = vk_rt_find(depth);
        CHECK(color_surface && depth_surface && depth_surface->f.vk == VK_FORMAT_D32_SFLOAT_S8_UINT);
        VkBuffer readback;
        u8* color_bytes = vk_readback_buffer(336, &readback);
        CHECK(color_bytes);
        u8* depth_bytes = color_bytes + 256;
        vk_surf_use(vk_cmd(), color_surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy color_copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                         .imageExtent = {4, 4, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), color_surface->img, color_surface->layout, readback, 1, &color_copy);
        vk_surf_use(vk_cmd(), depth_surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferImageCopy depth_copy[2] = {
            {.bufferOffset = 256, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}},
            {.bufferOffset = 320, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}}};
        rec_CmdCopyImageToBuffer(vk_cmd(), depth_surface->img, depth_surface->layout, readback, 2, depth_copy);
        vk_finish();
        vk_readback_ready();
        int rejected = test == 0 || test == 2 || test == 3;
        float expected[4];
        memcpy(expected, destination, sizeof expected);
        if (!rejected)
            for (u32 primitive = 0; primitive < (test == 5 ? 2u : 1u); primitive++) {
                float intermediate[4];
                memcpy(intermediate, expected, sizeof intermediate);
                nvn_advanced_blend_reference(mode, 1, 0, test == 6 ? (const float[4]){0} : source,
                                               intermediate, expected);
            }
        float expected_depth = rejected ? clear.depth : test == 5 ? 1 : 0.5f;
        u8 expected_stencil = test == 0 ? 5 : test == 2 || test == 3 ? 4 : test == 4 ? 10 : test == 5 ? 6 : 9;
        for (u32 pixel = 0; pixel < 16; pixel++) {
            float actual_depth, actual_color[4];
            memcpy(&actual_depth, depth_bytes + pixel * 4, 4);
            memcpy(actual_color, color_bytes + pixel * 16, 16);
            CHECK(fabsf(actual_depth - expected_depth) < 0.000001f);
            CHECK(depth_bytes[64 + pixel] == expected_stencil);
            for (u32 component = 0; component < 4; component++) {
                if (!(fabsf(actual_color[component] - expected[component]) < 0.00005f))
                    fprintf(stderr, "late capture case%u pixel%u component%u: %g expected %g\n",
                            test, pixel, component, actual_color[component], expected[component]);
                CHECK(fabsf(actual_color[component] - expected[component]) < 0.00005f);
            }
        }
        vk_unlock();
        CHECK(!gmem_read(counters, actual_counters, sizeof actual_counters));
        CHECK(!memcmp(actual_counters, expected_counters, sizeof actual_counters));
        vk_lock();
    }
    vk_unlock();
    g_vk.advanced_blend_caps.available = native_available;
    free(channel);
    puts("PASS NVN late blend capture: depth/stencil, KIL/alpha rejection, per-fragment atomic counts, alpha0 and primitive order exact=1");
    return 0;
}
