/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Distinct C1 MRT sources share a validity mask, but retain their own blend, channel mask and native conversion. */
#include <math.h>
static float mixed_mrt_source(u32 target, u32 component) {
    return component == 0 ? (float)(target + 1) / 8.0f : component == 1 ? .25f : component == 2 ? .75f : .5f;
}
static int mixed_mrt_program(u64 address, u32 count) {
    u64 code[64] = {0};
    code[9] = (1ull << (4 * count)) - 1; /* Four packed register exports per target, with no fragment inputs. */
    u32 at = 10;
    for (u32 i = 0; i <= 4 * count; i++) {
        if (!(i % 3)) code[at++] = 0x001f8000fc0007e0ull;
        if (i == 4 * count) code[at++] = 0xe30000000007000full;
        else {
            float value = mixed_mrt_source(i / 4, i % 4);
            u32 bits;
            memcpy(&bits, &value, 4);
            code[at++] = 0x0100000000071000ull | (u64)bits << 20 | i;
        }
    }
    return invalidate_write_code(address, code, 64);
}
static u32 mixed_mrt_byte(float value, int srgb) {
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    if (srgb) value = value <= .0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - .055f;
    return (u32)floorf(value * 255.0f + .5f);
}
static void mixed_mrt_blend(DsBlend* blend, u32 count) {
    blend->logic_op = 0;
    for (u32 i = 0; i < count; i++) {
        blend->en[i] = i % 3 != 2;
        blend->mask[i] = i % 3 == 0 ? 15 : i % 3 == 1 ? 5 : 11;
        blend->cop[i] = blend->aop[i] = i % 3 == 1 ? DS_BLEND_MAX : DS_BLEND_ADD;
        blend->sc[i] = blend->sa[i] = blend->dc[i] = blend->da[i] = DS_FACTOR_ONE;
    }
}
static int check_mixed_coverage_mrt(void) {
    u32 limit = vk_coverage_capture_limit(g_vk.props.limits.maxColorAttachments);
    if (limit < 2) { puts("SKIP mixed MRT: two source attachments plus validity are unavailable"); return 0; }
    const u64 gpu = 0xad0000000ull, cpu = HEAP_BASE + 0x11800000;
    const u64 stream = gpu + 0x6000;
    CHECK(!mem_map(cpu, 0x20000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x20000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 151;
    const float positions[24] = {-1,-1,0,1, 3,-1,0,1, -1,3,0,1, -1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    const float transform[6] = {2, 2, .5f, 2, 2, .5f};
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    CHECK(!gmem_write(stream, positions, sizeof positions));
    CHECK(mesh_program(gpu, vertex, 3, 0));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32); r[0x583] = (u32)gpu;
    r[0x810] = r[0x850] = 1; r[0x64b] = 1; r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111; r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16; r[0x701] = (u32)(stream >> 32); r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof positions - 1) >> 32); r[0x7c1] = (u32)(stream + sizeof positions - 1);
    DsSurf descriptions[7];
    Surf* targets[7];
    for (u32 i = 0; i < limit; i++) {
        u64 address = gpu + 0x8000 + i * 0x1000;
        rt_clear_format(channel, address, 4, 4, 0, 0, i % 3 == 0 ? 0xc0 : i % 3 == 1 ? 0xd5 : 0xd6);
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        descriptions[i] = channel->ds.tgt.color[0];
        targets[i] = vk_rt_find(address);
        CHECK(targets[i]);
    }
    vk_lock();
    u32 raster = 0;
    for (u32 n = 2; n <= 16 && !raster; n *= 2)
        if (vk_capture_get_mixed_targets(targets, 2, n)) raster = n;
    if (!raster) {
        vk_unlock(); free(channel);
        puts("SKIP mixed MRT: multisample RGBA32F capture images unavailable");
        return 0;
    }
    CHECK(!vk_capture_get_mixed_targets(targets, 0, raster));
    CHECK(!vk_capture_get_mixed_targets(targets, limit + 1, raster));
    Surf* bad[2] = {targets[0], targets[0]};
    CHECK(!vk_capture_get_mixed_targets(bad, 2, raster));
    Surf fake = *targets[1];
    bad[1] = &fake;
    fake.sw++;
    CHECK(!vk_capture_get_mixed_targets(bad, 2, raster));
    fake = *targets[1]; fake.layers = 2;
    CHECK(!vk_capture_get_mixed_targets(bad, 2, raster));
    fake = *targets[1]; fake.f.is_int = 1;
    CHECK(!vk_capture_get_mixed_targets(bad, 2, raster));
    fake = *targets[1]; fake.msaa = DS_MSAA_2X1;
    CHECK(!vk_capture_get_mixed_targets(bad, 2, raster));
    fake = *targets[1]; fake.gva = targets[0]->gva;
    CHECK(!vk_capture_get_mixed_targets(bad, 2, raster));
    vk_unlock();
    /* Different GPU addresses can still alias the same guest backing. No native image operation may escape this gate. */
    gmmu_map(gpu + 0x18000, cpu + 0x8000, 0x1000);
    vk_lock();
    fake = *targets[1]; fake.gva = gpu + 0x18000;
    CHECK(!vk_capture_get_mixed_targets(bad, 2, raster));
    vk_unlock();
    gmmu_space_unmap(gpu + 0x18000, 0x1000);
    int native = g_vk.has_mixed_samples;
    g_vk.has_mixed_samples = 0;
    u32 draws = 0, pixels = 0;
    for (u32 count = 2; count <= limit; count++) {
        u64 fragment = gpu + 0x2000 + count * 0x400;
        u64 primary_fragment = gpu + 0x10000 + count * 0x400;
        CHECK(mixed_mrt_program(fragment, count));
        CHECK(mixed_mrt_program(primary_fragment, 1));
        r[0x851] = (u32)(fragment - gpu);
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        DsState* state = &channel->ds;
        state->tgt = (DsTargets){.count = count, .srgb_write = 1};
        for (u32 i = 0; i < count; i++) state->tgt.color[i] = descriptions[i];
        state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = state->dyn.alpha_test = 0;
        state->rkey.sample_shading = state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
        mixed_mrt_blend(&state->blend, count);
        u32 full_outputs = (1u << (4 * count)) - 1;
        CHECK(vk_capture_mixed_outputs_supported(full_outputs, count, &state->blend));
        CHECK(vk_capture_mixed_outputs_supported(15, count, &state->blend));
        CHECK(!vk_capture_mixed_outputs_supported(full_outputs, 0, &state->blend));
        CHECK(!vk_capture_mixed_outputs_supported(full_outputs, 8, &state->blend));
        CHECK(!vk_capture_mixed_outputs_supported(full_outputs | 1u << (4 * count), count, &state->blend));
        for (u32 i = 0; i < count; i++) {
            u32 partial = (full_outputs & ~(15u << (i * 4))) | 3u << (i * 4);
            CHECK(!vk_capture_mixed_outputs_supported(partial, count, &state->blend));
            u8 mask = state->blend.mask[i];
            state->blend.mask[i] = 0;
            CHECK(vk_capture_mixed_outputs_supported(partial, count, &state->blend));
            state->blend.mask[i] = mask;
        }
        for (u32 i = 0; i < DS_GENS; i++) state->gen[i] = ds_generation_next();
        const u32 masks[4] = {0, 1u << (raster - 1), 0xaaaau & ((1u << raster) - 1), (1u << raster) - 1};
        vk_lock();
        VkCapture* capture = vk_capture_get_mixed_targets(targets, count, raster);
        CHECK(capture && capture->mixed_count == count);
        VkDescriptorSet immutable[7] = {0};
        u64 immutable_frame[7] = {0};
        for (u32 mode = 0; mode < 5; mode++)
            for (u32 mask = 0; mask < 4; mask++) {
                u32 outputs = mode == 4 ? 15 : full_outputs;
                if (mode == 4) {
                    /* RT1 and later stay bound with nonzero masks, but the guest shader exports only RT0. */
                    mixed_mrt_blend(&state->blend, count);
                    state->gen[DS_BLEND] = ds_generation_next();
                    state->prog.code[5] = primary_fragment;
                    state->gen[DS_PROGRAMS] = ds_generation_next();
                }
                const float initial[4] = {0, 1, 0, 1};
                for (u32 i = 0; i < count; i++) {
                    DrawClear clear = {.slot = i, .color_mask = 15};
                    memcpy(clear.color, initial, sizeof initial);
                    vk_render_clear(state, &clear, &channel->owner);
                }
                state->rkey.sample_mask = masks[mask];
                state->gen[DS_RASTER_KEY] = ds_generation_next();
                DrawCall call = {.topology = 4, .count = 6, .instances = 1, .raster_samples = raster,
                                .coverage_mode = mode ? 3 : 0, .coverage_table_enable = mode == 2};
                if (mode == 3) {
                    /* A bound but masked target must not reject inert SRC1 or advanced blend state after the
                       other targets' guest pass. The entire destination remains its independently cleared value. */
                    u32 inert = count - 1;
                    state->blend.mask[inert] = 0;
                    state->blend.en[inert] = 1;
                    state->blend.sc[inert] = state->blend.sa[inert] = DS_FACTOR_SRC1_COLOR;
                    state->blend.dc[inert] = state->blend.da[inert] = DS_FACTOR_ONE_MINUS_SRC1_COLOR;
                    state->gen[DS_BLEND] = ds_generation_next();
                    call.advanced_blend[inert] = nvn_set_field(
                        nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV, NVN_BLEND_ADVANCED_MODE_OVERLAY),
                                      NVN_BLEND_ADV_PMS, 1), NVN_BLEND_ADV_NPM, 1);
                }
                for (u32 i = 0; i < 16; i++) call.coverage_table[i] = (u8)((3 * i + 5) % 17);
                u64 before = g_vk_drawn;
                vk_render_draw(state, &call, &channel->owner);
                CHECK(g_vk_drawn == before + 1);
                u32 population = 0;
                for (u32 bit = 0; bit < raster; bit++) population += masks[mask] >> bit & 1;
                float factor = mode ? (float)population / (float)raster : 1.0f;
                if (mode == 2 && population)
                    factor = (float)((3 * ((population - 1) * 16 / raster) + 5) % 17) / 16.0f;
                for (u32 i = 0; i < count; i++) {
                    Surf* source = vk_capture_mixed_source(capture, i);
                    CHECK(source && source->f.vk == VK_FORMAT_R32G32B32A32_SFLOAT && !source->storage_capable);
                    VkDescriptorSet set = i ? capture->extra[i - 1].mixed_set : capture->mixed_set;
                    u64 frame = i ? capture->extra[i - 1].mixed_frame : capture->mixed_frame;
                    CHECK(set);
                    if ((mode == 3 && i == count - 1) || (mode == 4 && i)) {
                        CHECK(set == immutable[i] && frame == immutable_frame[i]);
                    } else CHECK(frame == vk_frame_id());
                    for (u32 j = 0; j < i; j++) CHECK(set != immutable[j]);
                    if (immutable_frame[i] == frame && immutable[i]) CHECK(set == immutable[i]);
                    immutable[i] = set;
                    immutable_frame[i] = frame;
                    VkBuffer buffer;
                    u32 bytes = i % 3 == 0 ? 256 : 64;
                    u8* data = vk_readback_buffer(bytes, &buffer);
                    CHECK(data);
                    vk_render_end();
                    vk_surf_use(vk_cmd(), targets[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                             .imageExtent = {4, 4, 1}};
                    rec_CmdCopyImageToBuffer(vk_cmd(), targets[i]->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
                    vk_finish(); vk_readback_ready();
                    for (u32 pixel = 0; pixel < 16; pixel++)
                        for (u32 component = 0; component < 4; component++) {
                            float expected = initial[component], value = mixed_mrt_source(i, component) * factor;
                            if (population && (state->blend.mask[i] >> component & 1) &&
                                (outputs >> (i * 4 + component) & 1))
                                expected = i % 3 == 0 ? expected + 2 * value
                                         : i % 3 == 1 ? (value > expected ? value : expected) : value;
                            if (i % 3 == 0) {
                                float actual;
                                memcpy(&actual, data + pixel * 16 + component * 4, 4);
                                CHECK(actual == expected);
                            } else {
                                u32 wanted = mixed_mrt_byte(expected, i % 3 == 2 && component < 3);
                                int difference = (int)data[pixel * 4 + component] - (int)wanted;
                                CHECK(difference >= -1 && difference <= 1);
                            }
                        }
                    pixels += 16;
                }
                draws++;
                if (!mode && mask == 3) {
                    u64 frame = vk_frame_id();
                    vk_frame_next(); vk_cmd();
                    CHECK(vk_frame_id() != frame && capture->mixed_frame == frame);
                    for (u32 i = 1; i < count; i++) CHECK(capture->extra[i - 1].mixed_frame == frame);
                }
            }
        vk_unlock();
    }
    g_vk.has_mixed_samples = native;
    free(channel);
    printf("PASS portable C1 mixed MRT: targets2..%u, %u draws, %u pixels; independent blend/masks/float/UNORM/sRGB checked=%u\n",
           limit, draws, pixels, pixels);
    return 0;
}
