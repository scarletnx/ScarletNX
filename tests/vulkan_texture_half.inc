/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/texture_half_instructions.h"

/* Independent binary32/binary16 pairs include both tie parities, half subnormals, normal-boundary values and
   finite overflow. Real R32F texture reads feed TEXS, TLDS, TLD4S and an HADD2.F32+0 packed result. */
static int check_texture_half_rounding(void) {
    static const u32 cases[][2] = {
        {0x3f803000, 0x3c02}, {0x3f801000, 0x3c00}, {0x33800000, 0x0001}, {0x33000000, 0x0000},
        {0x33c00000, 0x0002}, {0xb3c00000, 0x8002}, {0x38800000, 0x0400}, {0x387fc000, 0x03ff},
        {0x477fe000, 0x7bff}, {0x477ff000, 0x7c00}, {0xc77ff000, 0xfc00}, {0x7f7fffff, 0x7c00},
        {0xff7fffff, 0xfc00}, {0x80000000, 0x8000}, {0x00000000, 0x0000}, {0x3f800000, 0x3c00}};
    const u64 gpu = 0x820000000ull, cpu = HEAP_BASE + 0x10900000;
    const u64 constants = gpu + 0x1000, textures = gpu + 0x2000, pool = gpu + 0x3000;
    const u64 sampler = gpu + 0x3800, output = gpu + 0x4000;
    CHECK(mem_map(cpu, 0x20000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x20000);
    for (u32 test = 0; test < sizeof cases / sizeof *cases; test++) {
        NvnTexture texture = {.gpu = gpu + 0x10000 + test * 0x200};
        texture.d.target = NVN_TEXTURE_TARGET_2D;
        texture.d.format = NVN_FORMAT_R32F;
        texture.d.width = texture.d.height = 2;
        texture.d.depth = texture.d.levels = 1;
        u32 tic[8];
        CHECK(nvn_tic_encode(&texture, NULL, 0, 0, tic));
        CHECK(!storage_write_words(pool + test * 32, tic, 8));
        u8 data[512] = {0};
        for (u32 y = 0; y < 2; y++)
            for (u32 x = 0; x < 2; x++) snx_st_le32(data + bl_offset(x * 4, y, 0, 8, 2, 0, 0), cases[test][0]);
        CHECK(!gmem_write(texture.gpu, data, sizeof data));
    }
    CHECK(!storage_write_words(sampler, (const u32[8]){0}, 8));
    CHECK(!storage_write_words(constants + 0x110, (const u32[3]){(u32)output, (u32)(output >> 32), 8}, 3));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    u32 qmd[64] = {0};
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1 | 1u << 2;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)constants;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(constants >> 32) | 0x200u << 15;
    qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)textures;
    qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)(textures >> 32) | 0x200u << 15;
    channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(pool >> 32);
    channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)pool;
    channel->rcomp[KEPLER_COMPUTE_TSC] = (u32)(sampler >> 32);
    channel->rcomp[KEPLER_COMPUTE_TSC + 1] = (u32)sampler;
    channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
    int native = g_shader_half_rounding;
    u32 checks = 0;
    for (u32 backend = 0; backend < (native ? 2u : 1u); backend++) {
        g_shader_half_rounding = backend ? native : 0;
        for (u32 kind = 0; kind < 4; kind++) {
            u64 ops[16];
            u32 count = 0;
            ops[count++] = 0x4c98078004470000ull;
            ops[count++] = 0x4c98078004570001ull;
            ops[count++] = storage_mov(4, kind == 1 ? 0 : 0x3f000000);
            ops[count++] = storage_mov(5, kind == 1 ? 0 : 0x3f000000);
            ops[count++] = storage_mov(9, 0xdeadbeef);
            ops[count++] = storage_mov(10, backend); /* Give each float-control path its own bytecode hash. */
            if (kind == 3) {
                ops[count++] = storage_mov(6, 0);
                ops[count++] = storage_mov(7, 0);
                ops[count++] = 0xdc00000000000000ull | 7ull << 51 | 74ull << 36 | 1ull << 31 | 2ull << 28 |
                               255ull << 20 | 7ull << 16 | 6ull << 8 | 4;
            }
            ops[count++] = texture_half_instruction(kind);
            ops[count++] = 0xeedc200000070008ull;
            ops[count++] = 0xeedc200000470009ull;
            ops[count++] = 0xe30000000007000full;
            u8 code[256] = {0};
            u32 at = 0;
            for (u32 i = 0; i < count; i++) {
                if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
                snx_st_le64(code + 8 * at++, ops[i]);
            }
            u64 address = gpu + 0x5000 + (backend * 4 + kind) * 0x200;
            CHECK(!gmem_write(address, code, sizeof code));
            g_renderer->method_barrier(5);
            for (u32 test = 0; test < sizeof cases / sizeof *cases; test++) {
                CHECK(!storage_write_words(textures + 0x128, &test, 1));
                CHECK(!storage_write_words(output, (const u32[2]){0xcdcdcdcd, 0xcdcdcdcd}, 2));
                u64 before = g_vk_dispatches;
                vk_lock();
                DispatchState dispatch;
                mx_dispatch_state(qmd, channel->rcomp, address, &dispatch);
                vk_dispatch(&dispatch);
                vk_compute_land();
                vk_unlock();
                CHECK(g_vk_dispatches == before + 1);
                u8 actual[8];
                CHECK(!gmem_read(output, actual, sizeof actual));
                u32 half = kind == 3 && cases[test][0] == 0x80000000 ? 0 : cases[test][1];
                u32 first = kind >= 2 ? half | half << 16 : half;
                u32 second = kind == 2 ? first : 0xdeadbeef;
                if (snx_ld_le32(actual) != first || snx_ld_le32(actual + 4) != second)
                    fprintf(stderr, "half texture backend%u kind%u input%08x: %08x/%08x expected %08x/%08x\n", backend,
                            kind, cases[test][0], snx_ld_le32(actual), snx_ld_le32(actual + 4), first, second);
                CHECK(snx_ld_le32(actual) == first && snx_ld_le32(actual + 4) == second);
                checks++;
            }
        }
    }
    g_shader_half_rounding = native;
    free(channel);
    printf("PASS half texture rounding: %u R32F TEXS/TLDS/TLD4S/HADD2 native/software ties and denormals exact=1\n",
           checks);
    return 0;
}
