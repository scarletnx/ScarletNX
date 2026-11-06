/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One program address is reused while only its offset word changes. Distinct texels in each mip expose both
   incorrect base-level coordinate adjustment and a shader cache that keeps the old specialized ConstOffset. */
static int check_txd_uniform_offsets(void) {
    if (!g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("SKIP TXD uniform offsets: unformatted storage writes are unavailable");
        return 0;
    }
    const u64 gpu = 0x7f0000000ull, cpu = HEAP_BASE + 0x10600000;
    const u64 constants = gpu + 0x1000, offsets = gpu + 0x1800, descriptors = gpu + 0x2000;
    const u64 samplers = gpu + 0x3000, source = gpu + 0x10000, output = gpu + 0x30000;
    CHECK(mem_map(cpu, 0x80000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x80000);
    NvnTexture texture = {.gpu = source}, result = {.gpu = output};
    texture.d.target = NVN_TEXTURE_TARGET_2D;
    texture.d.format = NVN_FORMAT_R32F;
    texture.d.width = texture.d.height = 16;
    texture.d.depth = 1;
    texture.d.levels = 5;
    result.d.target = NVN_TEXTURE_TARGET_2D;
    result.d.format = NVN_FORMAT_R32F;
    result.d.width = 128;
    result.d.height = result.d.depth = result.d.levels = 1;
    result.d.flags = NVN_TEXTURE_FLAGS_LINEAR;
    result.d.stride = 512;
    NvnTexLayout layout;
    nvn_tex_layout(&texture.d, &layout);
    u8* initial = calloc(1, (size_t)layout.size);
    CHECK(initial);
    for (u32 level = 0; level < 5; level++) {
        u32 size = 16u >> level;
        unsigned tile_h, tile_d;
        nvn_tex_level_tiles(&texture.d, level, &tile_h, &tile_d);
        u64 base = nvn_tex_level_offset(&texture.d, level);
        for (u32 y = 0; y < size; y++)
            for (u32 x = 0; x < size; x++) {
                float value = (float)(1000 * level + 16 * y + x);
                u32 bits;
                memcpy(&bits, &value, sizeof bits);
                u64 at = base + bl_offset(4 * x, y, 0, 4 * size, size, tile_h, tile_d);
                CHECK(at + 4 <= layout.size);
                snx_st_le32(initial + at, bits);
            }
    }
    CHECK(!gmem_write(source, initial, layout.size));
    free(initial);
    u32 tic[8];
    CHECK(nvn_tic_encode(&texture, NULL, 0, 0, tic));
    CHECK(!storage_write_words(descriptors, tic, 8));
    CHECK(nvn_tic_encode(&result, NULL, 1, 0, tic));
    CHECK(!storage_write_words(descriptors + 32, tic, 8));
    const u32 tsc[8] = {2u | 2u << 3 | 2u << 6, 1u | 1u << 4 | 2u << 6, 0xfffu << 12};
    CHECK(!storage_write_words(samplers, tsc, 8));
    CHECK(!storage_write_words(constants + 0x128, (const u32[2]){0, 1}, 2));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    u32 qmd[64] = {0};
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 2 | 1u << 3;
    qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)constants;
    qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)(constants >> 32) | 0x200u << 15;
    qmd[QMD_CONST_BUFFER_CONFIG + 6] = (u32)offsets;
    qmd[QMD_CONST_BUFFER_CONFIG + 7] = (u32)(offsets >> 32) | 0x400u << 15;
    channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(descriptors >> 32);
    channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)descriptors;
    channel->rcomp[KEPLER_COMPUTE_TSC] = (u32)(samplers >> 32);
    channel->rcomp[KEPLER_COMPUTE_TSC + 1] = (u32)samplers;
    channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
    for (u32 load = 0; load < 2; load++)
        for (u32 level = 0; level < 3; level++) {
            float derivative = (float)(1u << level) / 16;
            u32 bits;
            memcpy(&bits, &derivative, sizeof bits);
            const u64 ops[] = {storage_mov(0, 0x3ec00000),
                               storage_mov(1, 0x3ec00000), /* (0.375,0.375) */
                               load ? 0xef9400000007ff02ull | 3ull << 36 | 0x200ull << 20
                                    : 0x4c98078000070002ull | 3ull << 34 | 0x80ull << 20,
                               storage_mov(8, bits),
                               storage_mov(9, 0),
                               storage_mov(10, 0),
                               storage_mov(11, 0),
                               0xde00000000000000ull | 7ull << 51 | 1ull << 35 | 74ull << 36 | 1ull << 31 | 2ull << 28 |
                                   8ull << 20 | 7ull << 16 | 8,
                               storage_mov(0, load * 3 + level),
                               storage_mov(1, 0),
                               0xeb20000000070008ull | 3ull << 33 | 1ull << 20 | 1ull << 51 | 75ull << 36,
                               0xe30000000007000full};
            u8 code[256] = {0};
            u32 at = 0;
            for (u32 i = 0; i < sizeof ops / sizeof *ops; i++) {
                if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
                snx_st_le64(code + 8 * at++, ops[i]);
            }
            CHECK(!gmem_write(gpu + 0x40000 + (load * 3 + level) * 0x200, code, sizeof code));
        }
    g_renderer->method_barrier(5); /* Program bytes are fixed after this point. */
    static const u32 packed[] = {0x10000, 0x220000, 0xfe0000, 0x10000};
    u32 checks = 0;
    for (u32 load = 0; load < 2; load++)
        for (u32 level = 0; level < 3; level++)
            for (u32 value = 0; value < sizeof packed / sizeof *packed; value++) {
                CHECK(!storage_write_words(offsets + 0x200, &packed[value], 1));
                u64 before = g_vk_dispatches;
                vk_lock();
                DispatchState dispatch;
                mx_dispatch_state(qmd, channel->rcomp, gpu + 0x40000 + (load * 3 + level) * 0x200, &dispatch);
                vk_dispatch(&dispatch);
                vk_unlock();
                CHECK(g_vk_dispatches == before + 1);
                i32 x = (i32)((16u >> level) * 3 / 8) + ((i32)(packed[value] << 12) >> 28);
                i32 y = (i32)((16u >> level) * 3 / 8) + ((i32)(packed[value] << 8) >> 28);
                i32 maximum = (i32)(16u >> level) - 1;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
                if (x > maximum) x = maximum;
                if (y > maximum) y = maximum;
                float expected = (float)(1000 * level + 16 * (u32)y + (u32)x);
                u32 expected_bits;
                memcpy(&expected_bits, &expected, sizeof expected_bits);
                u8 actual[4];
                CHECK(!gmem_read(output + (load * 3 + level) * 4, actual, sizeof actual));
                if (snx_ld_le32(actual) != expected_bits)
                    fprintf(stderr, "TXD uniform load%u mip%u offset%08x: %08x expected %08x\n", load, level,
                            packed[value], snx_ld_le32(actual), expected_bits);
                CHECK(snx_ld_le32(actual) == expected_bits);
                checks++;
            }
    free(channel);
    printf("PASS TXD uniform offsets: %u MOV_C/fixed-LDC samples, selected mips and changed-word variants exact=1\n",
           checks);
    return 0;
}
