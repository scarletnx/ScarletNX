/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ordinary draw is an independent native depth/stencil oracle. The programmable draw must retain its raw
   aspect bytes and guest storage effects while ordered artistic blending changes only the surviving colors.
   Repeated indices exercise native vertex reuse; GS and indirect commands must remain whole commands. */
#include "nx/gpu/nvn/nvn_blend.h"
#include "nx/gpu/vk/depth_shadow.h"
#include "support/nvn_advanced_blend_ref.h"

static const float late_shadow_source[2][4] = {{0.375f, 0.125f, 0.25f, 0.5f},
                                               {0.0625f, 0.5f, 0.375f, 0.75f}};
static void late_shadow_atomic(u64* code, u32* count, int vertex) {
    if (vertex) {
        code[(*count)++] = 0xefd800000007ff00ull | 255ull << 39 | 0x2fcull << 20; /* vertex index */
        code[(*count)++] = storage_mov(1, 0);
    } else {
        code[(*count)++] = 0xe000000000070000ull | 0x1cull << 30;
        code[(*count)++] = 0xe000000000070001ull | 0x1dull << 30;
        code[(*count)++] = 0x5cb0018000070a00ull;
        code[(*count)++] = 0x5cb0018000170a01ull;
    }
    code[(*count)++] = 0x4c98078804a70004ull; /* image handle c[2][0x128] */
    code[(*count)++] = storage_mov(8, 1);
    code[(*count)++] = (0xeb50020a00070008ull & ~(7ull << 33)) | 3ull << 33;
}
static int late_shadow_programs(u64 gpu) {
    u64 code[64];
    u32 n = 0;
    late_shadow_atomic(code, &n, 1);
    code[n++] = 0xefd9ff800807ff00ull; code[n++] = 0xeff1ff800707ff00ull;
    code[n++] = 0xefd9ff800907ff04ull; code[n++] = 0xeff1ff800807ff04ull;
    code[n++] = 0xe30000000007000full;
    CHECK(mesh_program(gpu, code, n, 0));
    for (u32 kill = 0; kill < 2; kill++) {
        n = 0;
        late_shadow_atomic(code, &n, 0);
        if (kill) code[n++] = 0xe33000000007000full;
        for (u32 i = 0; i < 4; i++) code[n++] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
        code[n++] = 0xe30000000007000full;
        CHECK(mesh_program(gpu + 0x800 + kill * 0x800, code, n, 1));
    }
    n = 0;
    code[n++] = storage_mov(0, 0); code[n++] = storage_mov(1, 0);
    code[n++] = 0x4c98078804a70004ull; code[n++] = storage_mov(8, 1);
    code[n++] = (0xeb50020a00070008ull & ~(7ull << 33)) | 3ull << 33;
    for (u32 primitive = 0; primitive < 2; primitive++) {
        for (u32 i = 0; i < 4; i++) {
            u32 bits;
            memcpy(&bits, &late_shadow_source[primitive][i], sizeof bits);
            code[n++] = storage_mov(8 + i, bits);
        }
        for (u32 vertex = 0; vertex < 3; vertex++) {
            code[n++] = storage_mov(4, vertex);
            code[n++] = 0xefd982000707ff00ull; code[n++] = 0xeff1ff800707ff00ull;
            code[n++] = 0xeff1ff800807ff08ull;
            code[n++] = 0xfbe000800ff70c0cull;
        }
        code[n++] = 0xfbe001000ff70c0cull;
    }
    code[n++] = 0xe30000000007000full;
    u64 geometry[96] = {0};
    geometry[1] = 1u << 24 | 7ull << 56; geometry[2] = 6;
    u32 at = 10;
    for (u32 i = 0; i < n; i++) {
        if (!(i % 3)) geometry[at++] = 0x001f8000fc0007e0ull;
        geometry[at++] = code[i];
    }
    u8 geometry_bytes[sizeof geometry];
    for (u32 i = 0; i < sizeof geometry / sizeof *geometry; i++)
        snx_st_le64(geometry_bytes + i * 8, geometry[i]);
    CHECK(!gmem_write(gpu + 0x2800, geometry_bytes, sizeof geometry_bytes));
    n = 0;
    code[n++] = storage_mov(0, 0); code[n++] = storage_mov(1, 0);
    code[n++] = 0x4c98078804a70004ull; code[n++] = storage_mov(8, 1);
    code[n++] = (0xeb50020a00070008ull & ~(7ull << 33)) | 3ull << 33;
    /* Three level-one evaluation points form a full-screen triangle: position.xy=4*tessCoord.xy-1. */
    code[n++] = 0xefd800000007ff00ull | 255ull << 39 | 0x2f0ull << 20 | 1ull << 47;
    for (u32 axis = 0; axis < 2; axis++) {
        code[n++] = 0x1e00000000070000ull | 0x40800000ull << 20 | (u64)axis << 8 | axis;
        code[n++] = 0x0800000000070000ull | 0xbf800000ull << 20 | (u64)axis << 8 | axis;
    }
    code[n++] = storage_mov(2, 0); code[n++] = storage_mov(3, 0x3f800000);
    code[n++] = 0xeff1ff800707ff00ull;
    for (u32 i = 0; i < 4; i++) {
        u32 bits;
        memcpy(&bits, &late_shadow_source[0][i], sizeof bits);
        code[n++] = storage_mov(8 + i, bits);
    }
    code[n++] = 0xeff1ff800807ff08ull; code[n++] = 0xe30000000007000full;
    CHECK(mesh_program(gpu + 0x3000, code, n, 0));
    return 0;
}
static int late_shadow_read(u64 target, u64 depth, u8 output[336]) {
    vk_render_end();
    Surf *color = vk_rt_find(target), *zeta = vk_rt_find(depth);
    CHECK(color && zeta && zeta->f.vk == VK_FORMAT_D32_SFLOAT_S8_UINT);
    VkBuffer buffer;
    u8* bytes = vk_readback_buffer(336, &buffer);
    CHECK(bytes);
    vk_surf_use(vk_cmd(), color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), color->img, color->layout, buffer, 1, &copy);
    vk_surf_use(vk_cmd(), zeta, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferImageCopy aspects[2] = {
        {.bufferOffset = 256, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}},
        {.bufferOffset = 320, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}}};
    rec_CmdCopyImageToBuffer(vk_cmd(), zeta->img, zeta->layout, buffer, 2, aspects);
    vk_finish(); vk_readback_ready();
    memset(output, 0, 336);
    memcpy(output, bytes, color->f.is_int ? 64 : 256);
    memcpy(output + 256, bytes + 256, 80);
    return 0;
}
static int late_shadow_reset_counters(u64 gpu) {
    u8 bytes[512];
    for (u32 image = 0; image < 4; image++) {
        memset(bytes, 0xcd, sizeof bytes);
        u32 width = image == 0 ? 4 : 8;
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < width; x++)
            snx_st_le32(bytes + bl_offset(x * 4, y, 0, width * 4, 4, 0, 0), 0);
        CHECK(!gmem_write(gpu + 0x6000 + image * 512, bytes, sizeof bytes));
    }
    g_renderer->method_barrier(5);
    return 0;
}
static int late_shadow_counters(u64 gpu, u8 output[2048]) {
    vk_unlock();
    int result = gmem_read(gpu + 0x6000, output, 2048);
    vk_lock();
    CHECK(!result);
    return 0;
}
static int late_shadow_xfb_reset(u64 gpu, const DsState* state, DrawOwner* owner) {
    u8 data[128];
    memset(data, 0xcd, sizeof data);
    CHECK(!gmem_write(gpu + 0xc000, data, sizeof data));
    CHECK(!storage_write_words(gpu + 0xe000, (const u32[8]){0}, 8));
    g_renderer->method_barrier(5);
    CHECK(vk_render_xfb_control(state->owner, gpu + 0xe000, 0, 16, owner));
    return 0;
}
static int late_shadow_xfb_read(u64 gpu, u8 output[160]) {
    vk_unlock();
    int data = gmem_read(gpu + 0xc000, output, 128);
    int control = gmem_read(gpu + 0xe000, output + 128, 32);
    vk_lock();
    CHECK(!data && !control);
    return 0;
}
static int late_shadow_bytes(const char* what, u32 coverage, u32 test, const u8* actual,
                              const u8* expected, size_t size) {
    for (size_t i = 0; i < size; i++)
        if (actual[i] != expected[i]) {
            fprintf(stderr, "late shadow %s coverage%u case%u byte%zu: %02x expected%02x\n",
                    what, coverage, test, i, actual[i], expected[i]);
            return 0;
        }
    return 1;
}
static int late_shadow_drawn(const char* phase, u32 coverage, u32 test, const DrawCall* call,
                              u64 drawn, u64 skipped) {
    if (g_vk_drawn == drawn + 1 && g_vk_skipped == skipped) return 1;
    fprintf(stderr, "late shadow %s coverage%u case%u: draw delta%llu skip delta%llu topology%u indexed%u "
                    "indirect%llx count%u records%u\n", phase, coverage, test,
            (unsigned long long)(g_vk_drawn - drawn), (unsigned long long)(g_vk_skipped - skipped),
            call->topology, call->indexed, (unsigned long long)call->indirect, call->count, call->draw_count);
    return 0;
}
static int check_late_depth_shadow(void) {
    if (!g_vk.has_fragment_interlock || !g_vk.has_depth_range_unrestricted || !g_vk.feat.fragmentStoresAndAtomics ||
        !g_vk.feat.vertexPipelineStoresAndAtomics || !vk_capture_tiles_supported() ||
        !g_vk.feat.shaderStorageImageReadWithoutFormat || !g_vk.feat.shaderStorageImageWriteWithoutFormat ||
        !g_vk.image_slots[ST_FS] || !g_vk.image_slots[ST_VS] || !g_vk_preserve_infnan) {
        puts("SKIP late depth shadow: ordered interlock, raw storage or stage atomic support unavailable");
        return 0;
    }
    const u64 gpu = 0xac0000000ull, cpu = HEAP_BASE + 0x1a000000;
    const u64 target = gpu + 0x8000, depth = gpu + 0xa000;
    GpuChannel* channel = sample_raster_channel(gpu, cpu, 0);
    CHECK(channel);
    CHECK(!late_shadow_programs(gpu));
    gmmu_map(gpu + 0xf000, cpu + 0xa000, 0x1000); /* distinct GVA, same depth backing */
    const u32 indices[6] = {0,1,2,0,1,2};
    CHECK(!storage_write_words(gpu + 0x3800, indices, 6));
    const u32 opposite_faces[6] = {0,1,2,0,2,1};
    CHECK(!storage_write_words(gpu + 0x3820, opposite_faces, 6));
    const u32 commands[10] = {3,1,0,0,0,3,1,3,0,0};
    CHECK(!storage_write_words(gpu + 0x3900, commands, 10));
    CHECK(!storage_write_words(gpu + 0x3940, (const u32[1]){2}, 1));
    float vertices[24];
    const float positions[12] = {-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    for (u32 i = 0; i < 3; i++) {
        memcpy(vertices + i * 8, positions + i * 4, 16);
        memcpy(vertices + i * 8 + 4, late_shadow_source[0], 16);
    }
    CHECK(!gmem_write(gpu + 0x2000, vertices, sizeof vertices));
    const u32 format = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16 |
                       2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    for (u32 image = 0; image < 4; image++) {
        u64 address = gpu + 0x6000 + image * 512;
        const u32 header[8] = {format, (u32)address, (u32)(address >> 32) | 3u << 21, 0,
                               (image ? 7u : 3u) | 1u << 23, 3u | 1u << 31, 0, 0};
        CHECK(!storage_write_words(gpu + 0x4000 + image * 32, header, 8));
    }
    CHECK(!storage_write_words(gpu + 0x5000 + 0x128, (const u32[1]){0}, 1));
    CHECK(!storage_write_words(gpu + 0x5200 + 0x128, (const u32[1]){1}, 1));
    CHECK(!storage_write_words(gpu + 0x5400 + 0x128, (const u32[1]){2}, 1));
    CHECK(!storage_write_words(gpu + 0x5600 + 0x128, (const u32[1]){3}, 1));
    channel->r3d[0x55d] = (u32)((gpu + 0x4000) >> 32);
    channel->r3d[0x55e] = (u32)(gpu + 0x4000); channel->r3d[0x55f] = 3;
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
    channel->r3d_dirty = ~0u; mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->index = (DsIndex){.addr = gpu + 0x3800, .end = gpu + 0x3817, .format = 2};
    state->tgt.depth = (DsSurf){.gva = depth, .width = 4, .height = 4, .format = 0x19,
                                .layers = 1, .layer_stride = 512};
    state->bind.cb_addr[ST_FS][2] = gpu + 0x5000; state->bind.cb_size[ST_FS][2] = 0x200;
    state->bind.cb_addr[ST_VS][2] = gpu + 0x5200; state->bind.cb_size[ST_VS][2] = 0x200;
    state->bind.cb_addr[ST_GS][2] = gpu + 0x5400; state->bind.cb_size[ST_GS][2] = 0x200;
    state->bind.cb_addr[ST_TES][2] = gpu + 0x5600; state->bind.cb_size[ST_TES][2] = 0x200;
    const float destination[4] = {0.5625f,0.1875f,0.375f,0.75f};
    float viewport_scale_x = state->vp.scale[0][0];
    u32 native = g_vk.advanced_blend_caps.available, checked = 0, feedback_checked = 0;
    int native_coverage = g_vk.has_coverage_to_color;
    g_vk.advanced_blend_caps.available = 0;
    g_vk.has_coverage_to_color = 0;
    if (!g_vk.has_xfb) puts("SKIP late depth shadow XFB: native transform feedback is unavailable");
    if (!g_vk.feat.depthBounds) puts("SKIP late depth shadow bounds: native depth bounds is unavailable");
    /* Whole indexed, GPU-counted and output-producing-stage commands cover former source-capture rejections. */
    for (u32 coverage = 0; coverage < 2; coverage++) for (u32 test = 0; test < 14; test++) {
        if (test == 7 && !g_vk.has_draw_indirect_count) continue;
        if (test == 8 && (!g_vk.feat.geometryShader || g_vk_stage_sets < 5 || !g_vk.image_slots[ST_GS])) continue;
        if (test == 9 && (!g_vk.feat.tessellationShader || g_vk_stage_sets < 5 || !g_vk.image_slots[ST_TES])) continue;
        if (test == 12 && !g_vk.feat.depthBounds) continue;
        u8 baseline[336], actual[336], baseline_counters[2048], actual_counters[2048];
        u8 baseline_xfb[160], actual_xfb[160];
        int feedback = (test == 10 || test == 13) && g_vk.has_xfb;
        u64 color_target = target + coverage * 512;
        state->tgt.color[0] = (DsSurf){.gva = color_target, .width = 4, .height = 4,
            .format = coverage ? 0xe4u : 0xc0u, .layers = 1, .layer_stride = 512};
        state->prog.on &= ~28u;
        state->prog.code[1] = gpu;
        state->prog.code[ST_FS + 1] = gpu + (test == 4 ? 0x1000 : 0x800);
        state->prog.early_z = 0;
        state->prog.xfb_on = (u8)feedback;
        state->xfb = (ShXfb){0};
        state->bind.xfb[0].on = (u32)feedback;
        if (feedback) {
            state->xfb.count[0] = 4; state->xfb.stride[0] = 16;
            for (u32 k = 0; k < 4; k++) state->xfb.layout[0][k] = (u8)(32 + k);
            state->bind.xfb[0].addr = gpu + 0xc000; state->bind.xfb[0].size = 128;
        }
        state->dyn.alpha_test = test == 5; state->dyn.alpha_func = DS_CMP_GREATER; state->dyn.alpha_ref = 0.75f;
        state->dyn.depth_test = 1; state->dyn.depth_cmp = DS_CMP_LESS; state->dyn.depth_write = test == 1;
        state->dyn.stencil_test = 1; state->dyn.bias_on = 0;
        state->rkey.depth_clamp = 0;
        state->dyn.cull = DS_CULL_NONE;
        state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = state->rkey.sample_shading = 0;
        state->rkey.sample_mask = test == 10 ? 0 : ~0u;
        /* A zero-width viewport suppresses raster samples, after VS stores and transform feedback. Keep the
           fixed mask fully on so this case cannot pass through the independent mask0 behavior. */
        state->vp.scale[0][0] = test == 13 ? 0 : viewport_scale_x;
        if (test == 13) {
            CHECK(state->vp.transform_on && state->rkey.sample_mask == ~0u);
            CHECK(vk_viewport_from(&state->dyn, &state->vp, 0).width == 0);
        }
        for (u32 face = 0; face < 2; face++) {
            state->dyn.face[face].cmp = test == 3 ? DS_CMP_NEVER : DS_CMP_ALWAYS;
            state->dyn.face[face].fail = DS_STENCIL_INVERT;
            state->dyn.face[face].zfail = DS_STENCIL_INCREMENT_AND_CLAMP;
            state->dyn.face[face].pass = test == 1 ? DS_STENCIL_REPLACE : DS_STENCIL_KEEP;
            state->dyn.face[face].ref = 9;
            state->dyn.face[face].cmask = state->dyn.face[face].wmask = 255;
            if (test == 11) {
                /* Opposite winding uses both faces; distinct partial masks retain bits from the previous pass. */
                state->dyn.face[face].pass = DS_STENCIL_REPLACE;
                state->dyn.face[face].ref = face ? 5 : 9;
                state->dyn.face[face].cmask = face ? 0xaf : 0x5f;
                state->dyn.face[face].wmask = face ? 0xf5 : 0x0f;
            }
        }
        state->index.addr = gpu + (test == 11 ? 0x3820 : 0x3800);
        state->index.end = state->index.addr + sizeof indices - 1;
        DrawCall call = {.topology = 4, .indexed = 1, .count = test == 3 ? 3 : 6, .instances = 1};
        u32 primitives = test == 3 || test == 9 ? 1 : 2;
        if (test == 6 || test == 7) {
            call.count = 1; call.indirect = gpu + 0x3900; call.draw_count = 2; call.stride = 20;
            if (test == 7) call.indirect_count = gpu + 0x3940;
        }
        if (test == 8) {
            state->prog.on |= 1u << 4; state->prog.code[ST_GS + 1] = gpu + 0x2800;
            call.indexed = 0; call.count = 3;
        }
        if (test == 9) {
            state->prog.on |= 1u << 3; state->prog.code[ST_TES + 1] = gpu + 0x3000;
            state->prog.tess_params = 0x301; state->rkey.patch_vertices = 1;
            call.indexed = 0; call.topology = 14; call.count = 1;
            call.state_flags = DS_DRAW_TESS_LEVELS;
            for (u32 i = 0; i < 6; i++) call.tess_levels[i] = 1;
        }
        if (test == 12) {
            /* Stored depth1 fails bounds even though incoming depth0.5 is in range and passes LESS. Bounds
               precede stencil operations; guest fragment atomics still have their ordinary late-test effects. */
            call.state_flags = DS_DRAW_DEPTH_BOUNDS;
            call.depth_bounds[0] = 0;
            call.depth_bounds[1] = .5f;
        }
        DrawClear clear = {.color_mask = 15, .aspects = 3, .depth = test == 2 ? 0.25f : 1, .stencil = 4};
        memcpy(clear.color, destination, sizeof destination);
        if (coverage) clear.color[0] = 9;
        state->blend.en[0] = 0; state->blend.mask[0] = 0;
        for (u32 i = 0; i < DS_GENS; i++) state->gen[i] = ds_generation_next();
        CHECK(!late_shadow_reset_counters(gpu));
        if (feedback) CHECK(!late_shadow_xfb_reset(gpu, state, &channel->owner));
        vk_render_clear(state, &clear, &channel->owner);
        if (!coverage && !test) {
            Surf* depth_surface = vk_rt_find(depth);
            CHECK(depth_surface && vk_depth_shadow_range_aliases(depth_surface, depth + 4, 4));
            CHECK(vk_depth_shadow_range_aliases(depth_surface, gpu + 0xf000, 4));
            CHECK(vk_depth_shadow_range_aliases(depth_surface, 0, 4));
            CHECK(vk_depth_shadow_range_aliases(depth_surface, UINT64_MAX - 1, 4));
            CHECK(!vk_depth_shadow_range_aliases(depth_surface, target, 256));
        }
        u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(late_shadow_drawn("native baseline", coverage, test, &call, drawn, skipped));
        if (feedback) {
            vk_render_end();
            CHECK(vk_render_xfb_control(state->owner, gpu + 0xe000, 1, 16, &channel->owner));
        }
        CHECK(!late_shadow_read(color_target, depth, baseline));
        CHECK(!late_shadow_counters(gpu, baseline_counters));
        if (feedback) CHECK(!late_shadow_xfb_read(gpu, baseline_xfb));
        CHECK(!late_shadow_reset_counters(gpu));
        if (feedback) CHECK(!late_shadow_xfb_reset(gpu, state, &channel->owner));
        vk_render_clear(state, &clear, &channel->owner);
        state->blend.en[0] = !coverage; state->blend.mask[0] = 15;
        state->gen[DS_BLEND] = ds_generation_next();
        if (coverage) call.multisample_flags = DS_MS_COVERAGE_TO_COLOR;
        else call.advanced_blend[0] = nvn_set_field(nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV,
            NVN_BLEND_ADVANCED_MODE_HARDLIGHT), NVN_BLEND_ADV_PMS, 1), NVN_BLEND_ADV_NPM, 1);
        drawn = g_vk_drawn; skipped = g_vk_skipped;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(late_shadow_drawn("portable draw", coverage, test, &call, drawn, skipped));
        if (feedback) {
            vk_render_end();
            CHECK(vk_render_xfb_control(state->owner, gpu + 0xe000, 1, 16, &channel->owner));
        }
        CHECK(!late_shadow_read(color_target, depth, actual));
        CHECK(late_shadow_bytes("native depth/stencil", coverage, test, actual + 256, baseline + 256, 80));
        if (test == 12 || test == 13)
            for (u32 pixel = 0; pixel < 16; pixel++) {
                CHECK(snx_ld_le32(baseline + 256 + pixel * 4) == 0x3f800000u);
                CHECK(baseline[320 + pixel] == 4 && actual[320 + pixel] == 4);
            }
        CHECK(!late_shadow_counters(gpu, actual_counters));
        CHECK(late_shadow_bytes("native storage counters", coverage, test, actual_counters, baseline_counters,
                                   sizeof actual_counters));
        if (test == 10 || test == 13)
            for (u32 vertex = 0; vertex < 3; vertex++)
                CHECK(snx_ld_le32(actual_counters + 512 + bl_offset(vertex * 4, 0, 0, 32, 4, 0, 0)) > 0);
        if (feedback) {
            CHECK(!late_shadow_xfb_read(gpu, actual_xfb));
            CHECK(late_shadow_bytes("native XFB", coverage, test, actual_xfb, baseline_xfb, sizeof actual_xfb));
            u8 expected[128]; memset(expected, 0xcd, sizeof expected);
            for (u32 vertex = 0; vertex < 6; vertex++) memcpy(expected + vertex * 16, late_shadow_source[0], 16);
            CHECK(late_shadow_bytes("XFB payload/guards", coverage, test, actual_xfb, expected, sizeof expected));
            CHECK(snx_ld_le32(actual_xfb + 128) == 16 && snx_ld_le32(actual_xfb + 132) == 96);
            feedback_checked++;
        }
        u8 fs_expected[512];
        memset(fs_expected, 0xcd, sizeof fs_expected);
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++)
            snx_st_le32(fs_expected + bl_offset(x * 4, y, 0, 16, 4, 0, 0), test == 10 || test == 13 ? 0 : primitives);
        CHECK(late_shadow_bytes("fragment atomic oracle", coverage, test, actual_counters, fs_expected,
                                   sizeof fs_expected));
        float expected[4]; memcpy(expected, destination, sizeof expected);
        if (test != 2 && test != 3 && test != 4 && test != 5 && test != 10 && test != 12 && test != 13)
            for (u32 p = 0; p < (test == 1 ? 1u : primitives); p++) {
                float prior[4]; memcpy(prior, expected, sizeof prior);
                nvn_advanced_blend_reference(NVN_BLEND_ADVANCED_MODE_HARDLIGHT, 1, 0,
                    late_shadow_source[test == 8 ? p : 0], prior, expected);
            }
        for (u32 pixel = 0; pixel < 16; pixel++) for (u32 component = 0; component < (coverage ? 1u : 4u); component++) {
            if (coverage) {
                u32 bits = snx_ld_le32(actual + pixel * 4);
                u32 expected_bits = test == 2 || test == 3 || test == 4 || test == 5 || test == 10 || test == 12 ||
                    test == 13 ? 9 : 1;
                if (bits != expected_bits)
                    fprintf(stderr, "late shadow coverage case%u pixel%u: %u expected%u\n",
                        test, pixel, bits, expected_bits);
                CHECK(bits == expected_bits);
                checked++;
                continue;
            }
            float value; memcpy(&value, actual + pixel * 16 + component * 4, sizeof value);
            if (!(fabsf(value - expected[component]) < 0.00005f))
                fprintf(stderr, "late shadow case%u pixel%u component%u: %g expected%g\n",
                    test, pixel, component, value, expected[component]);
            CHECK(fabsf(value - expected[component]) < 0.00005f);
            checked++;
        }
    }
    g_vk.advanced_blend_caps.available = native;
    g_vk.has_coverage_to_color = native_coverage;
    vk_unlock(); free(channel);
    printf("PASS late depth shadow: blend/coverage whole indexed/indirect/GS/TES draws, fixed-mask/empty-viewport VS and %u XFB paths, native D32F/S8 bytes, storage counts checked=%u exact=1\n",
           feedback_checked, checked);
    return 0;
}
