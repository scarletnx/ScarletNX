/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* A compute program samples a depth target as a draw does: a 16x16 Z32F target cleared to 0.25, read with TEX.LZ
   at (0.5, 0.5) through a ZF32 header that names it, and stored with STG through an NVN descriptor in cbuf 0. One
   game's fog lighting samples its sun's shadow cascades and another depth target in a compute program, and they read
   0: compute bound a depth target as no texture */
static int check_compute_depth_texture(void) {
    const u64 gpu = 0x9a0000000ull, cpu = HEAP_BASE + 0x1e000000;
    const u64 depth = gpu, constants = gpu + 0x10000, textures = gpu + 0x11000, pool = gpu + 0x12000;
    const u64 sampler = gpu + 0x12800, output = gpu + 0x13000, code_at = gpu + 0x14000;
    CHECK(mem_map(cpu, 0x20000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x20000);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    u32* r = ch->r3d;
    r[0x54E] = 1;
    r[0x3F8] = (u32)(depth >> 32);
    r[0x3F9] = (u32)depth;
    r[0x3FA] = 0x0A;   /* Z32F */
    r[0x3FB] = 0x1000; /* pitch linear */
    r[0x3FC] = 256;    /* a 1024 byte layer */
    r[0x48A] = r[0x48B] = 16;
    r[0x48C] = 1;
    float clear = 0.25f;
    memcpy(&r[0x364], &clear, 4);
    engine_clear(ch, 1);
    vk_render_end();
    /* ZF32, 16x16 2D, normalized, pitch linear rows of 64 bytes */
    const u32 tic[8] = {0x2F, (u32)depth, (u32)(depth >> 32) | 2u << 21, 64 >> 5, 15 | 1u << 23, 15 | 1u << 31, 0, 0};
    CHECK(!storage_write_words(pool, tic, 8));
    CHECK(!storage_write_words(sampler, (const u32[8]){0}, 8)); /* nearest, no compare */
    CHECK(!storage_write_words(textures + 0x128, (const u32[1]){0}, 1));
    CHECK(!storage_write_words(constants + 0x110, (const u32[3]){(u32)output, (u32)(output >> 32), 8}, 3));
    CHECK(!storage_write_words(output, (const u32[1]){0xcdcdcdcd}, 1));
    u64 sample = 0xc000000000000000ull | 1ull << 55 | 74ull << 36 | 7ull << 51 | 1ull << 31 | 2ull << 28 |
                 0xffull << 20 | 7ull << 16 | 2ull << 8 | 8; /* TEX.LZ R8, R2, c[0x128], 2D, R */
    const u64 ops[] = {0x4c98078004470000ull,  /* MOV R0, c[0][0x110] */
                       0x4c98078004570001ull,  /* MOV R1, c[0][0x114] */
                       storage_mov(2, 0x3f000000), storage_mov(3, 0x3f000000), sample,
                       0xeedc200000070008ull,  /* STG.E [R0], R8 */
                       0xe30000000007000full}; /* EXIT */
    u8 code[256] = {0};
    u32 at = 0;
    for (u32 i = 0; i < sizeof ops / sizeof *ops; i++) {
        if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    CHECK(!gmem_write(code_at, code, sizeof code));
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
    ch->rcomp[KEPLER_COMPUTE_TIC] = (u32)(pool >> 32);
    ch->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)pool;
    ch->rcomp[KEPLER_COMPUTE_TSC] = (u32)(sampler >> 32);
    ch->rcomp[KEPLER_COMPUTE_TSC + 1] = (u32)sampler;
    ch->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
    u64 before = g_vk_dispatches;
    vk_lock();
    Surf* target = vk_rt_find(depth);
    int is_depth = target && target->f.depth;
    DispatchState d;
    mx_dispatch_state(qmd, ch->rcomp, code_at, &d);
    vk_dispatch(&d);
    vk_compute_land();
    vk_unlock();
    u8 word[4];
    CHECK(!gmem_read(output, word, sizeof word));
    float got;
    u32 bits = snx_ld_le32(word);
    memcpy(&got, &bits, 4);
    int ran = g_vk_dispatches == before + 1;
    if (!is_depth || !ran || got != 0.25f)
        fprintf(stderr, "compute depth texture: a depth target %d, ran %d, read %g (%08x), want 0.25\n", is_depth, ran,
                (double)got, bits);
    CHECK(is_depth && ran && got == 0.25f);
    free(ch);
    CHECK(mem_unmap(cpu, 0x20000) == 0);
    printf("compute depth texture: a compute program reads a depth target's 0.25\n");
    return 0;
}

/* A compute program's depth compare (TEX.LZ.DC, the reference 0.5 in R4) through header 0 and sampler 0, which the
   caller writes at pool and sampler; what it read through the descriptor at c[0][0x110] in *got, and the validation
   layer's messages while it ran (none without the layer) in *messages */
static int compute_compare(u64 gpu, u64 cpu, const u32 tic[8], u32 tsc0, const u32* texels, float* got, int* messages) {
    const u64 constants = gpu, textures = gpu + 0x1000, pool = gpu + 0x2000, sampler = gpu + 0x2800;
    const u64 output = gpu + 0x3000, code_at = gpu + 0x4000, image = gpu + 0x8000;
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x10000);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    if (texels) CHECK(!storage_write_words(image, texels, 256));
    CHECK(!storage_write_words(pool, tic, 8));
    CHECK(!storage_write_words(sampler, (const u32[8]){tsc0}, 8));
    CHECK(!storage_write_words(textures + 0x128, (const u32[1]){0}, 1));
    CHECK(!storage_write_words(constants + 0x110, (const u32[3]){(u32)output, (u32)(output >> 32), 8}, 3));
    CHECK(!storage_write_words(output, (const u32[1]){0xcdcdcdcd}, 1));
    u64 sample = 0xc000000000000000ull | 1ull << 55 | 1ull << 50 | 74ull << 36 | 7ull << 51 | 1ull << 31 | 2ull << 28 |
                 4ull << 20 | 7ull << 16 | 2ull << 8 | 8; /* TEX.LZ.DC R8, R2, R4, c[0x128], 2D, R */
    const u64 ops[] = {0x4c98078004470000ull,             /* MOV R0, c[0][0x110] */
                       0x4c98078004570001ull,             /* MOV R1, c[0][0x114] */
                       storage_mov(2, 0x3f000000),        /* (0.5, 0.5) */
                       storage_mov(3, 0x3f000000),
                       storage_mov(4, 0x3f000000), /* the reference, 0.5 */
                       sample,
                       0xeedc200000070008ull,  /* STG.E [R0], R8 */
                       0xe30000000007000full}; /* EXIT */
    u8 code[256] = {0};
    u32 at = 0;
    for (u32 i = 0; i < sizeof ops / sizeof *ops; i++) {
        if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    CHECK(!gmem_write(code_at, code, sizeof code));
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
    ch->rcomp[KEPLER_COMPUTE_TIC] = (u32)(pool >> 32);
    ch->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)pool;
    ch->rcomp[KEPLER_COMPUTE_TSC] = (u32)(sampler >> 32);
    ch->rcomp[KEPLER_COMPUTE_TSC + 1] = (u32)sampler;
    ch->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
    u64 before = g_vk_dispatches;
    u32 validation = g_vk_validation_messages;
    vk_lock();
    DispatchState d;
    mx_dispatch_state(qmd, ch->rcomp, code_at, &d);
    vk_dispatch(&d);
    vk_compute_land();
    vk_finish();
    vk_unlock();
    u8 word[4];
    CHECK(!gmem_read(output, word, sizeof word));
    u32 bits = snx_ld_le32(word);
    memcpy(got, &bits, 4);
    *messages = (int)(g_vk_validation_messages - validation);
    CHECK(g_vk_dispatches == before + 1);
    free(ch);
    CHECK(mem_unmap(cpu, 0x10000) == 0);
    return 0;
}

/* A compute program's depth compare on a color texture compares its first component, as the console does and a draw's
   fragment program here does: a 16x16 R32F texture of 0.25 under a GEQUAL sampler, 0.5 >= 0.25, reads 1. Compute bound
   it to a compare sampler, which Vulkan has no result for on a color view (VUID-vkCmdDispatch-None-06479: one game's
   opening, its R32F 4x4 texture). And a compare whose handle names no surface reads the depth image a draw binds there,
   the farthest depth, so 0.5 passes LEQUAL and reads 1: compute bound the black RGBA8 null image to a compare sampler */
static int check_compute_compares(void) {
    const u32 r32f = 0x7017ff8f; /* R32 float, x from R, y and z 0, w 1 */
    u32 texels[256];
    float quarter = 0.25f, got;
    int messages;
    memcpy(&texels[0], &quarter, 4);
    for (u32 i = 1; i < 256; i++) texels[i] = texels[0];
    const u64 gpu = 0x9a1000000ull, image = gpu + 0x8000;
    const u32 color[8] = {r32f, (u32)image, (u32)(image >> 32) | 2u << 21, 64 >> 5, 15 | 1u << 23, 15 | 1u << 31, 0, 0};
    CHECK(!compute_compare(gpu, HEAP_BASE + 0x1e020000, color, 1u << 9 | 6u << 10, texels, &got, &messages));
    if (got != 1.0f || messages)
        fprintf(stderr, "compute compare of a color texture: read %g, want 1; %d validation messages\n", (double)got,
                messages);
    CHECK(got == 1.0f && !messages);
    CHECK(!compute_compare(0x9a2000000ull, HEAP_BASE + 0x1e030000, (const u32[8]){0}, 0, NULL, &got, &messages));
    if (got != 1.0f || messages)
        fprintf(stderr, "compute compare without a texture: read %g, want 1; %d validation messages\n", (double)got,
                messages);
    CHECK(got == 1.0f && !messages);
    printf("compute compares: a color texture's first component in the program, a missing one against the farthest "
           "depth\n");
    return 0;
}
