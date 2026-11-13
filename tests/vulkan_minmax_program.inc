/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A sampler's min/max reduction on a device whose sampler has none (samplerFilterMinmax off, as PanVK on Bifrost and
   Turnip on an Adreno 610): the compute program takes the least or the greatest of the texels itself
   (vk_tex_minmax_in_program_tic, shader_minmax_sample), where the sampler would average them. A TEX.LZ of a 4x4 R32F
   texture of 10 y + x + 1/4, linear both ways and no mip filter, stored by SUST: between four texels, on a column's
   centre (the next column weighs nothing), across the edge where the sampler repeats, and on one texel's centre. What
   it reads must be the least or greatest of the texels the footprint weighs, as the device's own min/max filter gives
   where it has one */
static int minmax_dispatch(GpuChannel* channel, u64 address, u32 c, u32 x, u32 y, u64 target, float* got) {
    u64 sample = 0xc000000000000000ull | 1ull << 55 | 74ull << 36 | 7ull << 51 | 1ull << 31 | 2ull << 28 |
                 0xffull << 20 | 7ull << 16 | 8; /* TEX.LZ R8, R0, c[0x128], 2D, R */
    u64 store = (0xeb20000000070008ull | 3ull << 33 | 1ull << 20 | 1ull << 51) | 75ull << 36; /* SUST.P [R0], R8 */
    u64 instructions[] = {storage_mov(0, x),    storage_mov(1, y), sample, storage_mov(0, 0), storage_mov(1, 0), store,
                          0xe30000000007000full};
    u8 code[256] = {0}, zeros[64] = {0}, result[64];
    u32 at = 0, qmd[64] = {0};
    u64 dispatches = g_vk_dispatches;
    for (u32 i = 0; i < sizeof instructions / sizeof instructions[0]; i++) {
        if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, instructions[i]);
    }
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 2;
    qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)(address + 0x20000);
    qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)((address + 0x20000) >> 32) | 0x200u << 15;
    CHECK(!gmem_write(address + 0x1000 + c * 0x200, code, sizeof code));
    CHECK(!gmem_write(target, zeros, sizeof zeros));
    vk_lock();
    {
        DispatchState d;
        mx_dispatch_state(qmd, channel->rcomp, address + 0x1000 + c * 0x200, &d);
        vk_dispatch(&d);
    }
    vk_unlock();
    CHECK(g_vk_dispatches == dispatches + 1);
    CHECK(!gmem_read(target, result, sizeof result));
    u32 bits = snx_ld_le32(result);
    memcpy(got, &bits, 4);
    return 0;
}

static int check_minmax_program(void) {
    static const struct {
        float u, v;
        u32 wrap; /* the TSC's: 2 clamp to edge, 0 repeat */
        float least, greatest;
    } cases[] = {{0.5f, 0.5f, 2, 11.25f, 22.25f},
                 {0.375f, 0.5f, 2, 11.25f, 21.25f},
                 {1.0f, 0.5f, 0, 10.25f, 23.25f},
                 {0.875f, 0.125f, 2, 3.25f, 3.25f}};
    const u64 address = 0x750000000ull;
    const u64 cbuf = address + 0x20000, descriptors = address + 0x21000, samplers = address + 0x22000;
    const u64 source = address + 0x30000, target = address + 0x32000;
    VkFormatProperties r32f;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R32_SFLOAT, &r32f);
    if (!(r32f.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
        !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("min/max in the program: R32F has no linear filter or storage writes are unavailable, skipped");
        return 0;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    CHECK(!mem_map(HEAP_BASE + 0x11E00000, 0x40000, MS_NORMAL, PERM_RW));
    gmmu_map(address, HEAP_BASE + 0x11E00000, 0x40000);
    channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(descriptors >> 32);
    channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)descriptors;
    channel->rcomp[KEPLER_COMPUTE_TSC] = (u32)(samplers >> 32);
    channel->rcomp[KEPLER_COMPUTE_TSC + 1] = (u32)samplers;
    channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
    CHECK(!storage_write_words(cbuf + 0x128, (const u32[2]){0, 1}, 2));
    /* R32F, 4x4 texels, one level, block linear of one GOB, normalized; the output a header of its own */
    u32 word0 = 0x0f | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    u32 tic[8] = {word0, (u32)source, (u32)(source >> 32) | 3u << 21, 0, 3u | 1u << 23, 3u | 1u << 31, 0, 0};
    u32 output_tic[8];
    memcpy(output_tic, tic, sizeof tic);
    output_tic[1] = (u32)target;
    u8 input[512] = {0};
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            float value = 10.0f * (float)y + (float)x + 0.25f;
            u32 bits;
            memcpy(&bits, &value, 4);
            snx_st_le32(input + bl_offset(x * 4, y, 0, 16, 4, 0, 0), bits);
        }
    CHECK(!gmem_write(source, input, sizeof input));
    CHECK(!storage_write_words(descriptors, tic, 8));
    CHECK(!storage_write_words(descriptors + 32, output_tic, 8));
    int minmax = vk_format_minmax_filter(VK_FORMAT_R32_SFLOAT);
    VkBool32 had = g_vk.feat12.samplerFilterMinmax;
    u32 count = 0, program = 0;
    /* the device's own filter first, where it has one for R32F, then the program's on a device with none */
    for (int device = minmax ? 1 : 0; device >= 0; device--) {
        g_vk.feat12.samplerFilterMinmax = device ? had : VK_FALSE;
        for (u32 c = 0; c < sizeof cases / sizeof cases[0]; c++)
            for (u32 reduction = 1; reduction <= 2; reduction++) {
                /* linear magnification and minification, no mip filter, the case's wrap, the reduction */
                u32 w = cases[c].wrap;
                u32 tsc[8] = {w | w << 3 | w << 6, 2u | 2u << 4 | 1u << 6 | reduction << 10};
                u32 x, y;
                float got, want = reduction == 1 ? cases[c].least : cases[c].greatest;
                memcpy(&x, &cases[c].u, 4);
                memcpy(&y, &cases[c].v, 4);
                CHECK(!storage_write_words(samplers, tsc, 8));
                if (minmax_dispatch(channel, address, c, x, y, target, &got)) {
                    g_vk.feat12.samplerFilterMinmax = had;
                    return 1;
                }
                if (got != want)
                    fprintf(stderr, "min/max %s: the %s at (%g, %g) %s reads %g, not %g\n",
                            device ? "by the device's sampler" : "in the program",
                            reduction == 1 ? "least" : "greatest", (double)cases[c].u, (double)cases[c].v,
                            w ? "clamped" : "repeated", (double)got, (double)want);
                if (got != want) {
                    g_vk.feat12.samplerFilterMinmax = had;
                    return 1;
                }
                count++;
                program += !device;
            }
    }
    g_vk.feat12.samplerFilterMinmax = had;
    printf("min/max in the program: %u compute samples, %u of them taken by the program on a device without the "
           "filter (the device's own %s)\n",
           count, program, minmax ? "agrees" : "has none for R32F");
    free(channel);
    return 0;
}
