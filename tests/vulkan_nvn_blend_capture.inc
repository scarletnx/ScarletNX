/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Native late tests decide capture validity. Guest atomic stores must run once even for a depth-rejected,
   alpha-rejected or killed fragment, and stencil writes must retain their fail/zfail/pass behavior. */
#include "nx/gpu/nvn/nvn_blend.h"
#include "support/nvn_advanced_blend_ref.h"

static int blend_capture_program(u64 address, int kill, int zero_alpha, int varying) {
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
        operations[count++] = varying ? 0xe000000000070000ull | (0x20ull + component) << 30 | component
                                     : 0x0100000000071000ull | (u64)(zero_alpha ? 0 : color[component]) << 20 | component;
    operations[count++] = 0xe30000000007000full;
    return mesh_program(address, operations, count, 1);
}

/* Two overlapping primitives per instance use three different source colors. The expected order is A,A,B,B,C,C,
   which differs from A,B,C,A,B,C. Each unique VS invocation also adds one to its own image texel. */
static int blend_capture_instances(GpuChannel* channel, u64 gpu) {
    if (!g_vk.feat.vertexPipelineStoresAndAtomics || !g_vk.image_slots[ST_VS]) {
        puts("SKIP NVN instanced late blend capture: vertex image atomics are unavailable");
        return 0;
    }
    const u64 vertex[] = {
        0xefd800000007ff00ull | 255ull << 39 | 0x2fcull << 20, /* ALD R0, vertex ID */
        0xefd800000007ff00ull | 255ull << 39 | 0x2f8ull << 20 | 1, /* ALD R1, instance ID */
        0x4c98078804a70004ull, /* MOV R4, c[2][0x128] */
        0x0100000000071008ull | 1ull << 20,
        (0xeb50020a00070008ull & ~(7ull << 33)) | 3ull << 33,
        0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, /* position */
        0xefd9ff800907ff04ull, 0xeff1ff800807ff04ull, /* per-instance color */
        0xefd800000007ff00ull | 255ull << 39 | 0x2f8ull << 20 | 8,
        0xefd800000007ff00ull | 255ull << 39 | 0x2fcull << 20 | 9,
        0x4c9807801917000aull, /* MOV R10, c[0][0x644]: original draw BaseInstance */
        0x010000000007100bull,
        0xeff1ff800a07ff08ull, /* generic words40..42 retain the guest IDs and BaseInstance for XFB */
        0xe30000000007000full
    };
    CHECK(mesh_program(gpu + 0x1400, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(blend_capture_program(gpu + 0x1800, 0, 0, 1));
    const float colors[6][4] = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1},
                               {0.125f, 0.0625f, 0.1875f, 0.25f},
                               {0.375f, 0.125f, 0.25f, 0.5f},
                               {0.0625f, 0.5f, 0.375f, 0.75f}};
    CHECK(!gmem_write(gpu + 0x2800, colors, sizeof colors));
    const u32 format = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16 |
                       2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    const u64 counters = gpu + 0x7000, output = gpu + 0xd000, control = gpu + 0xe000;
    const u32 header[8] = {format, (u32)counters, (u32)(counters >> 32) | 3u << 21,
                           0, 5u | 1u << 23, 3u | 1u << 31, 0, 0};
    CHECK(!storage_write_words(gpu + 0x4020, header, 8));
    CHECK(!storage_write_words(gpu + 0x5528, (const u32[1]){1}, 1));
    DsState* state = &channel->ds;
    state->prog.code[1] = gpu + 0x1400;
    state->prog.code[ST_FS + 1] = gpu + 0x1800;
    state->bind.cb_addr[ST_VS][2] = gpu + 0x5400;
    state->bind.cb_size[ST_VS][2] = 0x200;
    state->bind.cb_inline[ST_VS][0] = DS_CB_BANK0;
    state->bind.cb_size[ST_VS][0] = DS_INLINE_BANK0;
    snx_st_le32(state->inl.bank0[ST_VS] + SH_CB0_BASE_INSTANCE, 3);
    state->bind.tex_max = 1;
    state->vfmt.constant &= ~2u;
    state->vfmt.stream[1] = 1;
    state->vfmt.size[1] = state->vfmt.size[0];
    state->vfmt.type[1] = state->vfmt.type[0];
    state->vfmt.offset[1] = 0;
    state->vfmt.stream_on |= 2;
    state->vfmt.instanced |= 2;
    state->vfmt.stride[1] = 16;
    state->vfmt.divisor[1] = 1;
    state->vbuf.addr[1] = gpu + 0x2800;
    state->vbuf.size[1] = sizeof colors;
    state->dyn.depth_word = 2;
    state->dyn.alpha_test = state->dyn.depth_write = state->dyn.stencil_test = 0;
    state->dyn.depth_test = 1; /* late depth remains active, but cannot reject the equal-depth overlap */
    state->dyn.depth_cmp = DS_CMP_LESS;
    state->prog.early_z = 0;
    const float destination[4] = {0.5625f, 0.1875f, 0.375f, 0.75f};
    const u32 mode = NVN_BLEND_ADVANCED_MODE_HARDLIGHT;
    u64 packed = nvn_set_field(nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV, mode), NVN_BLEND_ADV_PMS, 1),
                                 NVN_BLEND_ADV_NPM, 1);
    DrawCall call = {.topology = 4, .count = 6, .base_instance = 3, .instances = 3};
    call.advanced_blend[0] = packed;
    u32 paths = g_vk.has_xfb ? 2 : 1;
    if (!g_vk.has_xfb) puts("SKIP NVN late blend transform feedback: device has no capture extension");
    for (u32 feedback = 0; feedback < paths; feedback++) {
        u8 initial[512], vs_expected[512], fs_expected[512], expected_xfb[512], actual[512];
        memset(initial, 0xcd, sizeof initial);
        memcpy(vs_expected, initial, sizeof initial);
        memcpy(fs_expected, initial, sizeof initial);
        memcpy(expected_xfb, initial, sizeof initial);
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 6; x++) {
                u64 offset = bl_offset(4 * x, y, 0, 24, 4, 0, 0);
                snx_st_le32(initial + offset, 0);
                snx_st_le32(vs_expected + offset, y ? 1 : 0);
            }
        CHECK(!gmem_write(counters, initial, sizeof initial));
        memset(initial, 0xcd, sizeof initial);
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                u64 offset = bl_offset(4 * x, y, 0, 16, 4, 0, 0);
                snx_st_le32(initial + offset, 0);
                snx_st_le32(fs_expected + offset, 6);
            }
        CHECK(!gmem_write(gpu + 0x6000, initial, sizeof initial));
        memset(initial, 0xcd, sizeof initial);
        CHECK(!gmem_write(output, initial, sizeof initial));
        CHECK(!gmem_write(control, initial, 32));
        g_renderer->method_barrier(5);
        state->prog.xfb_on = (u8)feedback;
        state->xfb = (ShXfb){0};
        state->xfb.count[0] = 7;
        state->xfb.stride[0] = 28;
        for (u32 k = 0; k < 4; k++) state->xfb.layout[0][k] = (u8)(32 + k);
        state->xfb.layout[0][4] = 40;
        state->xfb.layout[0][5] = 41;
        state->xfb.layout[0][6] = 42;
        state->bind.xfb[0].addr = output;
        state->bind.xfb[0].size = sizeof initial;
        state->bind.xfb[0].on = feedback;
        for (u32 i = 0; i < DS_GENS; i++) state->gen[i] = ds_generation_next();
        DrawOwner owner = {0};
        if (feedback) CHECK(vk_render_xfb_control(state->owner, control, 0, 28, &owner));
        DrawClear clear = {.color_mask = 15, .aspects = 3, .depth = 1};
        memcpy(clear.color, destination, sizeof destination);
        vk_render_clear(state, &clear, &owner);
        u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
        vk_render_draw(state, &call, &owner);
        CHECK(g_vk_drawn == drawn + 1 && g_vk_skipped == skipped);
        vk_render_end();
        if (feedback) CHECK(vk_render_xfb_control(state->owner, control, 1, 28, &owner));
        float expected[4];
        memcpy(expected, destination, sizeof expected);
        for (u32 instance = 0; instance < 3; instance++)
            for (u32 primitive = 0; primitive < 2; primitive++) {
                float next[4];
                nvn_advanced_blend_reference(mode, 1, 0, colors[instance + 3], expected, next);
                memcpy(expected, next, sizeof expected);
                for (u32 vertex_index = 0; vertex_index < 3; vertex_index++) {
                    u32 vertex_id = primitive * 3 + vertex_index;
                    u32 at = (instance * 6 + vertex_id) * 28;
                    memcpy(expected_xfb + at, colors[instance + 3], 16);
                    snx_st_le32(expected_xfb + at + 16, instance + 1);
                    snx_st_le32(expected_xfb + at + 20, vertex_id);
                    snx_st_le32(expected_xfb + at + 24, 3);
                }
            }
        float primitive_first[4];
        memcpy(primitive_first, destination, sizeof primitive_first);
        for (u32 primitive = 0; primitive < 2; primitive++)
            for (u32 instance = 0; instance < 3; instance++) {
                float next[4];
                nvn_advanced_blend_reference(mode, 1, 0, colors[instance + 3], primitive_first, next);
                memcpy(primitive_first, next, sizeof primitive_first);
            }
        CHECK(fabsf(primitive_first[0] - expected[0]) > 0.01f);
        Surf* surface = vk_rt_find(gpu + 0x8000);
        CHECK(surface);
        u8 color_bytes[256];
        VkBuffer readback;
        u8* mapped = vk_readback_buffer(sizeof color_bytes, &readback);
        CHECK(mapped);
        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {4, 4, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, surface->layout, readback, 1, &copy);
        vk_finish();
        vk_readback_ready();
        memcpy(color_bytes, mapped, sizeof color_bytes);
        for (u32 pixel = 0; pixel < 16; pixel++) {
            float color[4];
            memcpy(color, color_bytes + 16 * pixel, sizeof color);
            for (u32 k = 0; k < 4; k++) {
                if (!(fabsf(color[k] - expected[k]) < 0.00005f))
                    fprintf(stderr, "Instanced capture feedback%u pixel%u component%u: %g expected %g\n",
                            feedback, pixel, k, color[k], expected[k]);
                CHECK(fabsf(color[k] - expected[k]) < 0.00005f);
            }
        }
        vk_unlock();
        CHECK(!gmem_read(counters, actual, sizeof actual));
        CHECK(!memcmp(actual, vs_expected, sizeof actual));
        CHECK(!gmem_read(gpu + 0x6000, actual, sizeof actual));
        CHECK(!memcmp(actual, fs_expected, sizeof actual));
        if (feedback) {
            CHECK(!gmem_read(output, actual, sizeof actual));
            CHECK(!memcmp(actual, expected_xfb, sizeof actual));
            CHECK(!gmem_read(control, actual, 32));
            CHECK(snx_ld_le32(actual) == 28 && snx_ld_le32(actual + 4) == 504);
        }
        vk_lock();
    }
    printf("PASS NVN instanced late blend capture: instance order, VS/FS atomic counts, guest IDs and %u XFB path exact=1\n",
           paths - 1);
    return 0;
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
    CHECK(blend_capture_program(gpu + 0x800, 0, 0, 0));
    CHECK(blend_capture_program(gpu + 0xc00, 1, 0, 0));
    CHECK(blend_capture_program(gpu + 0x1000, 0, 1, 0));
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
    CHECK(!blend_capture_instances(channel, gpu));
    vk_unlock();
    g_vk.advanced_blend_caps.available = native_available;
    free(channel);
    puts("PASS NVN late blend capture: depth/stencil, KIL/alpha rejection, per-fragment atomic counts, alpha0 and primitive order exact=1");
    return 0;
}
