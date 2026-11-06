/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Each mip, layer and spatial texel has an independent value. TEX/TXD varying offsets come from a TID-addressed
   constant-buffer table, so literal and fixed-uniform specialization cannot hide the runtime path. */
static u32 texture_limit_bits(float value) { u32 bits; memcpy(&bits, &value, 4); return bits; }
static float texture_limit_value(u32 level, u32 layer, u32 x, u32 y, u32 z) {
    return (float)(100000 * layer + 10000 * level + 1024 * z + 32 * y + x);
}
static int texture_limit_seed(const NvnTexture* texture) {
    NvnTexLayout layout;
    nvn_tex_layout(&texture->d, &layout);
    u8* bytes = calloc(1, (size_t)layout.size);
    CHECK(bytes);
    int volume = texture->d.target == NVN_TEXTURE_TARGET_3D;
    u32 layers = texture->d.target == NVN_TEXTURE_TARGET_2D_ARRAY ? texture->d.depth : 1;
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 level = 0; level < texture->d.levels; level++) {
            u32 width = texture->d.width >> level, height = texture->d.height >> level;
            u32 depth = volume ? texture->d.depth >> level : 1;
            if (!width) width = 1;
            if (!height) height = 1;
            if (!depth) depth = 1;
            unsigned tile_h, tile_d;
            nvn_tex_level_tiles(&texture->d, level, &tile_h, &tile_d);
            u64 base = layer * layout.layer_size + nvn_tex_level_offset(&texture->d, level);
            for (u32 z = 0; z < depth; z++)
                for (u32 y = 0; y < height; y++)
                    for (u32 x = 0; x < width; x++) {
                        u64 offset = base + bl_offset(4 * x, y, z, 4 * width, height, tile_h, tile_d);
                        CHECK(offset + 4 <= layout.size);
                        snx_st_le32(bytes + offset, texture_limit_bits(texture_limit_value(level, layer, x, y, z)));
                    }
        }
    CHECK(!gmem_write(texture->gpu, bytes, layout.size));
    free(bytes);
    return 0;
}
static int texture_limit_axis(u32 level, u32 nibble) {
    i32 value = (i32)((16u >> level) * 3 / 8) + (nibble < 8 ? (i32)nibble : (i32)nibble - 16);
    i32 maximum = (i32)(16u >> level) - 1;
    return value < 0 ? 0 : value > maximum ? maximum : value;
}
static int check_txd_varying_offsets(void) {
    if (!g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("SKIP TXD varying offsets: unformatted storage writes are unavailable");
        return 0;
    }
    const u64 gpu = 0x8b0000000ull, cpu = HEAP_BASE + 0x11200000;
    const u64 constants = gpu + 0x1000, offsets = gpu + 0x2000, descriptors = gpu + 0x4000;
    const u64 samplers = gpu + 0x5000, source = gpu + 0x10000, output = gpu + 0x100000;
    CHECK(mem_map(cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x200000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    u32 qmd[64] = {0};
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 2 | 1u << 3;
    qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)constants;
    qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)(constants >> 32) | 0x200u << 15;
    qmd[QMD_CONST_BUFFER_CONFIG + 6] = (u32)offsets;
    qmd[QMD_CONST_BUFFER_CONFIG + 7] = (u32)(offsets >> 32) | 0x1000u << 15;
    channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(descriptors >> 32);
    channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)descriptors;
    channel->rcomp[KEPLER_COMPUTE_TSC] = (u32)(samplers >> 32);
    channel->rcomp[KEPLER_COMPUTE_TSC + 1] = (u32)samplers;
    channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
    CHECK(!storage_write_words(constants + 0x128, (const u32[2]){0, 1}, 2));
    const u32 tsc[8] = {2u | 2u << 3 | 2u << 6, 1u | 1u << 4 | 2u << 6, 0xfffu << 12};
    CHECK(!storage_write_words(samplers, tsc, 8));
    NvnTexture result = {.gpu = output};
    result.d.target = NVN_TEXTURE_TARGET_2D;
    result.d.format = NVN_FORMAT_R32F;
    result.d.width = 256;
    result.d.height = result.d.depth = result.d.levels = 1;
    result.d.flags = NVN_TEXTURE_FLAGS_LINEAR;
    result.d.stride = 1024;
    u32 tic[8];
    CHECK(nvn_tic_encode(&result, NULL, 1, 0, tic));
    CHECK(!storage_write_words(descriptors + 32, tic, 8));
    u32 checks = 0;
    for (u32 kind = 0; kind < 4; kind++) {
        u32 count = kind == 0 ? 1 : kind == 3 ? 3 : 2, array = kind == 2;
        u32 invocations = count == 1 ? 16 : count == 2 ? 256 : 8;
        NvnTexture texture = {.gpu = source + kind * 0x20000};
        texture.d.target = kind == 0 ? NVN_TEXTURE_TARGET_1D : array ? NVN_TEXTURE_TARGET_2D_ARRAY
                                      : kind == 3 ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D;
        texture.d.format = NVN_FORMAT_R32F;
        texture.d.width = 16; texture.d.height = kind == 0 ? 1 : 16;
        texture.d.depth = array ? 2 : kind == 3 ? 16 : 1;
        texture.d.levels = 5;
        CHECK(!texture_limit_seed(&texture));
        CHECK(nvn_tic_encode(&texture, NULL, 0, 0, tic));
        CHECK(!storage_write_words(descriptors, tic, 8));
        u32 packed[256];
        const u32 volume_offsets[8] = {0x000, 0x111, 0xfff, 0x888, 0x777, 0x1f2, 0xe3d, 0x4a7};
        for (u32 k = 0; k < invocations; k++) packed[k] = (count == 3 ? volume_offsets[k] : k) << 16 | (array ? k & 1 : 0);
        CHECK(!storage_write_words(offsets + 0x200, packed, invocations));
        qmd[QMD_BLOCK_DIM_X] = invocations << 16;
        for (u32 ordinary = 0; ordinary < 2; ordinary++)
          for (u32 bindless = 0; bindless < 2; bindless++)
            for (u32 level = 0; level < 3; level++) {
                u64 ops[32];
                u32 n = 0;
                CHECK(!storage_write_words(samplers + 4,
                    (const u32[1]){1u | 1u << 4 | (ordinary ? 3u : 2u) << 6}, 1));
                ops[n++] = 0xf0c8000002170000ull; /* TID.X supplies the per-invocation table address and output X. */
                ops[n++] = 0x3848000000270002ull; /* SHL R2,R0,2 */
                ops[n++] = storage_mov(1, 0);
                u32 type = kind == 0 ? 0 : kind == 3 ? 4 : array ? 3 : 2;
                if (ordinary) {
                    if (bindless) ops[n++] = 0x4c98078000070010ull | 2ull << 34 | 74ull << 20;
                    ops[n++] = 0xef94000000070012ull | 3ull << 36 | 0x200ull << 20 | 2ull << 8;
                    if (array) ops[n++] = 0x0400000000070004ull | 1ull << 20 | 18ull << 8;
                    for (u32 axis = 0; axis < count; axis++) ops[n++] = storage_mov(4 + array + axis, 0x3ec00000);
                    ops[n++] = storage_mov(16 + bindless, texture_limit_bits((float)level + .5f));
                    /* TEX offsets use the low nibbles after the explicit LOD, unlike TXD's high-half packing. */
                    ops[n++] = 0x3828000000070000ull | 16ull << 20 | 18ull << 8 | (17 + bindless);
                    ops[n++] = (bindless ? 0xdeb8000000000000ull : 0xc038000000000000ull | 74ull << 36) |
                        3ull << (bindless ? 37 : 55) | 1ull << (bindless ? 36 : 54) | 1ull << 31 |
                        (u64)type << 28 | 16ull << 20 | 7ull << 16 | 4ull << 8 | 20;
                } else {
                    if (bindless) ops[n++] = 0x4c98078000070003ull | 2ull << 34 | 74ull << 20;
                    for (u32 axis = 0; axis < count; axis++) ops[n++] = storage_mov(4 + axis, 0x3ec00000);
                    ops[n++] = 0xef94000000070000ull | 3ull << 36 | 0x200ull << 20 | 2ull << 8 | (4 + count);
                    for (u32 k = 0; k < 2 * count; k++)
                        ops[n++] = storage_mov(8 + k, k ? 0 : texture_limit_bits((float)(1u << level) / 16));
                    ops[n++] = (bindless ? 0xde40000000000000ull : 0xde00000000000000ull | 74ull << 36) |
                               7ull << 51 | 1ull << 35 | 1ull << 31 | (u64)type << 28 | 8ull << 20 |
                               7ull << 16 | (u64)(bindless ? 3 : 4) << 8 | 20;
                }
                ops[n++] = 0xeb20000000070014ull | 3ull << 33 | 1ull << 20 | 1ull << 51 | 75ull << 36;
                ops[n++] = 0xe30000000007000full;
                u8 code[512] = {0};
                u32 at = 0;
                for (u32 k = 0; k < n; k++) {
                    if (!(k % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
                    snx_st_le64(code + 8 * at++, ops[k]);
                }
                u64 address = gpu + 0x180000 + (ordinary * 24 + kind * 6 + bindless * 3 + level) * 0x200;
                CHECK(!gmem_write(address, code, sizeof code));
                g_renderer->method_barrier(5);
                u32 untouched[256];
                for (u32 k = 0; k < 256; k++) untouched[k] = 0xdeadbeef;
                CHECK(!storage_write_words(output, untouched, 256));
                u64 before = g_vk_dispatches;
                vk_lock();
                DispatchState dispatch;
                mx_dispatch_state(qmd, channel->rcomp, address, &dispatch);
                vk_dispatch(&dispatch);
                vk_compute_land();
                vk_unlock();
                CHECK(g_vk_dispatches == before + 1);
                u8 actual[1024];
                CHECK(!gmem_read(output, actual, sizeof actual));
                for (u32 k = 0; k < 256; k++) {
                    if (k >= invocations) { CHECK(snx_ld_le32(actual + k * 4) == untouched[k]); continue; }
                    u32 offset = packed[k] >> 16;
                    u32 x = (u32)texture_limit_axis(level, offset & 15);
                    u32 y = count >= 2 ? (u32)texture_limit_axis(level, offset >> 4 & 15) : 0;
                    u32 z = count == 3 ? (u32)texture_limit_axis(level, offset >> 8 & 15) : 0;
                    float value = texture_limit_value(level, array ? k & 1 : 0, x, y, z);
                    if (ordinary) {
                        x = (u32)texture_limit_axis(level + 1, offset & 15);
                        y = count >= 2 ? (u32)texture_limit_axis(level + 1, offset >> 4 & 15) : 0;
                        z = count == 3 ? (u32)texture_limit_axis(level + 1, offset >> 8 & 15) : 0;
                        value = .5f * (value + texture_limit_value(level + 1, array ? k & 1 : 0, x, y, z));
                    }
                    u32 expected = texture_limit_bits(value);
                    if (snx_ld_le32(actual + k * 4) != expected)
                        fprintf(stderr, "%s varying kind%u bindless%u mip%u word%08x: %08x expected%08x\n",
                                ordinary ? "TEX" : "TXD", kind, bindless, level, packed[k], snx_ld_le32(actual + k * 4), expected);
                    CHECK(snx_ld_le32(actual + k * 4) == expected);
                    checks++;
                }
            }
    }
    free(channel);
    printf("PASS TEX/TXD varying offsets: %u bound/bindless1D/2D/array/3D samples, signed domains and fractional mips exact=1\n", checks);
    return 0;
}

/* Fractional minimum levels use trilinear mip interpolation. The fragment coordinates are constant, so the
   independently chosen clamp controls the mip. Low-half layer bits must never become a spatial coordinate. */
static int check_texture_min_lod(void) {
    if (!g_vk.feat.shaderResourceMinLod) {
        puts("SKIP TEX minimum LOD: shaderResourceMinLod is unavailable");
        return 0;
    }
    const u64 gpu = 0x8b0200000ull, cpu = HEAP_BASE + 0x11400000;
    const u64 stream = gpu + 0x8000, pool = gpu + 0x9000, samplers = gpu + 0xa000;
    const u64 constants = gpu + 0xb000, clamp_buffer = gpu + 0xc000, target = gpu + 0x40000;
    CHECK(mem_map(cpu, 0x80000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x80000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 146;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    CHECK(!gmem_write(stream, position, sizeof position));
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(!storage_write_words(constants + 0x128, (const u32[1]){0}, 1));
    CHECK(!storage_write_words(samplers,
                              (const u32[8]){2u | 2u << 3 | 2u << 6, 1u | 1u << 4 | 3u << 6, 0xfffu << 12}, 8));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32); r[0x583] = (u32)gpu;
    r[0x55d] = (u32)(pool >> 32); r[0x55e] = (u32)pool; r[0x55f] = 0;
    r[0x557] = (u32)(samplers >> 32); r[0x558] = (u32)samplers; r[0x559] = 0;
    r[0x810] = r[0x850] = 1;
    r[0x64b] = 1; r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111; r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(stream >> 32); r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof position - 1) >> 32); r[0x7c1] = (u32)(stream + sizeof position - 1);
    u32 checks = 0;
    const u32 clamp_values[] = {0, 0x100, 0x140, 0x280, 0x3c0};
    for (u32 array = 0; array < 2; array++) {
        NvnTexture texture = {.gpu = gpu + 0x10000 + array * 0x10000};
        texture.d.target = array ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D;
        texture.d.format = NVN_FORMAT_R32F;
        texture.d.width = texture.d.height = 16;
        texture.d.depth = array ? 2 : 1; texture.d.levels = 5;
        CHECK(!texture_limit_seed(&texture));
        u32 tic[8];
        CHECK(nvn_tic_encode(&texture, NULL, 0, 0, tic));
        CHECK(!storage_write_words(pool, tic, 8));
        for (u32 with_offset = 0; with_offset < 2; with_offset++)
          for (u32 bindless = 0; bindless < 2; bindless++)
            for (u32 bias = 0; bias < 2; bias++) {
                u64 ops[16];
                u32 count = 0;
                ops[count++] = storage_mov(1, 0);
                ops[count++] = storage_mov(2, 0);
                ops[count++] = storage_mov(3, 0x3f800000);
                ops[count++] = 0x4c98078000070004ull | 3ull << 34 | 0x80ull << 20;
                ops[count++] = storage_mov(5, 0x3ec00000);
                ops[count++] = storage_mov(6, 0x3ec00000);
                if (bindless) ops[count++] = 0x4c98078000070010ull | 2ull << 34 | 74ull << 20;
                if (bias) ops[count++] = storage_mov(16 + bindless, 0x3fa00000);
                if (with_offset) ops[count++] = 0x4c98078000070000ull | 3ull << 34 | 0x84ull << 20 |
                    (16 + bindless + bias);
                ops[count++] = (bindless ? 0xdeb8000000000000ull : 0xc038000000000000ull | 74ull << 36) |
                               7ull << 51 | 1ull << (bindless ? 40 : 58) | (bias ? 2ull << (bindless ? 37 : 55) : 0) |
                               (u64)with_offset << (bindless ? 36 : 54) |
                               1ull << 31 | (u64)(array ? 3 : 2) << 28 | 16ull << 20 | 7ull << 16 | 4ull << 8;
                ops[count++] = 0xe30000000007000full;
                u32 program = 0x1000 + (array * 8 + with_offset * 4 + bindless * 2 + bias) * 0x200;
                CHECK(mesh_program(gpu + program, ops, count, 1));
                g_renderer->method_barrier(5);
                r[0x851] = program;
                for (u32 test = 0; test < sizeof clamp_values / sizeof *clamp_values; test++) {
                    u32 packed = clamp_values[test] << 16 | (array ? 1u : 0x4321u);
                    const u32 offset_values[] = {0, 0x1f, 0x81, 0xf7, 0x78};
                    u32 offset = with_offset ? offset_values[test] : 0;
                    CHECK(!storage_write_words(clamp_buffer + 0x200, &packed, 1));
                    CHECK(!storage_write_words(clamp_buffer + 0x210, &offset, 1));
                    vk_lock();
                    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
                    channel->r3d_dirty = ~0u;
                    mx_draw_state_sync(channel);
                    DsState* state = &channel->ds;
                    state->bind.cb_addr[ST_FS][2] = constants; state->bind.cb_size[ST_FS][2] = 0x200;
                    state->bind.cb_addr[ST_FS][3] = clamp_buffer; state->bind.cb_size[ST_FS][3] = 0x400;
                    state->blend.en[0] = 0; state->blend.mask[0] = 15;
                    u64 before = g_vk_drawn;
                    DrawCall draw = {.topology = 4, .count = 3, .instances = 1};
                    vk_render_draw(state, &draw, &channel->owner);
                    vk_render_end();
                    CHECK(g_vk_drawn == before + 1);
                    Surf* surface = vk_rt_find(target);
                    VkBuffer buffer;
                    u8* actual = vk_readback_buffer(16 * 16, &buffer);
                    CHECK(surface && actual);
                    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                VK_ACCESS_2_TRANSFER_READ_BIT);
                    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                .imageExtent = {4, 4, 1}};
                    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
                    vk_finish();
                    vk_readback_ready();
                    u32 low = clamp_values[test] >> 8, high = low < 4 ? low + 1 : 4;
                    float fraction = (float)(clamp_values[test] & 255) / 256;
                    u32 lo_x = (u32)texture_limit_axis(low, offset & 15);
                    u32 lo_y = (u32)texture_limit_axis(low, offset >> 4 & 15);
                    u32 hi_x = (u32)texture_limit_axis(high, offset & 15);
                    u32 hi_y = (u32)texture_limit_axis(high, offset >> 4 & 15);
                    float lo = texture_limit_value(low, array, lo_x, lo_y, 0);
                    float hi = texture_limit_value(high, array, hi_x, hi_y, 0);
                    float expected[4] = {lo + (hi - lo) * fraction, 0, 0, 1};
                    for (u32 pixel = 0; pixel < 16; pixel++)
                        for (u32 component = 0; component < 4; component++) {
                            float value;
                            memcpy(&value, actual + pixel * 16 + component * 4, 4);
                            if (fabsf(value - expected[component]) > 0.01f)
                                fprintf(stderr, "TEX LC array%u bindless%u bias%u clamp%u offset%#x pixel%u component%u: %g expected%g\n",
                                        array, bindless, bias, clamp_values[test], offset, pixel, component, value, expected[component]);
                            CHECK(fabsf(value - expected[component]) <= 0.01f);
                        }
                    vk_unlock();
                    checks++;
                }
            }
    }
    free(channel);
    printf("PASS TEX minimum LOD: %u fragment2D/array bound/bindless fractional/bias/offset draws exact=1\n", checks);
    return 0;
}
