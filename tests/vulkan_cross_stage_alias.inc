/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* TES writes texels0..2 while FS reads disjoint texel3 of the same2D image. A later draw reads texel0 before any
   host wait. Both sampled descriptors and storage descriptors must use GENERAL during the first draw. */
static int cross_alias_tess_program(u64 address, int store) {
    u64 operations[32];
    u32 count = 0;
    if (store) {
        operations[count++] = 0xefd800000007ff00ull | 0xffull << 39 | 0x2f0ull << 20 | 1ull << 47;
        operations[count++] = 0x5cb0018000070a00ull; /* F2I.U32.F32.TRUNC R0, TessCoord.x */
        operations[count++] = 0x5cb0018000170a01ull; /* F2I.U32.F32.TRUNC R1, TessCoord.y */
        operations[count++] = 0x3848000000170000ull; /* SHL R0,R0,1 */
        operations[count++] = 0x5c10000000170000ull; /* IADD R0,R0,R1: unique indices0..2 */
        operations[count++] = storage_mov(1, 0);
        operations[count++] = storage_mov(8, 0x3f400000); /*0.75 */
        operations[count++] = 0xeb20000000070008ull | 3ull << 33 | 1ull << 20 | 1ull << 51 | 74ull << 36;
    }
    /* Pixel center(2.5,2.5) avoids aliased-point edge rules in the4x4 viewport. */
    operations[count++] = storage_mov(16, 0x3e800000);
    operations[count++] = storage_mov(17, 0x3e800000);
    operations[count++] = storage_mov(18, 0);
    operations[count++] = storage_mov(19, 0x3f800000);
    operations[count++] = 0xeff1ff800707ff10ull;
    operations[count++] = 0xe30000000007000full;
    return mesh_program(address, operations, count, 0);
}
static int cross_alias_fragment_program(u64 address, int first) {
    const u64 operations[] = {
        storage_mov(4, first ? 0x3f600000 : 0x3e000000), /* normalized X for texel3 or0 */
        storage_mov(5, 0x3f000000),
        0xc000000000000000ull | 1ull << 55 | 74ull << 36 | 7ull << 51 | 1ull << 31 | 2ull << 28 |
            0xffull << 20 | 7ull << 16 | 4ull << 8,
        0xe30000000007000full};
    return mesh_program(address, operations, sizeof operations / sizeof *operations, 1);
}
static int check_cross_stage_image_alias(void) {
    if (!g_vk.feat.tessellationShader || g_vk_stage_sets < 5 || !g_vk.image_slots[ST_TES] ||
        !g_vk.feat.vertexPipelineStoresAndAtomics || !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("SKIP graphics cross-stage image alias: tessellation storage-image support is unavailable");
        return 0;
    }
    const u64 gpu = 0x810000000ull, cpu = HEAP_BASE + 0x10800000;
    const u64 pool = gpu + 0x4000, samplers = gpu + 0x4100, constants = gpu + 0x5000, source = gpu + 0x6000;
    const u64 targets[2] = {gpu + 0x8000, gpu + 0x9000};
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    NvnTexture image = {.gpu = source};
    image.d.target = NVN_TEXTURE_TARGET_2D;
    image.d.format = NVN_FORMAT_R32F;
    image.d.width = 4; image.d.height = image.d.depth = image.d.levels = 1;
    u32 tic[8];
    CHECK(nvn_tic_encode(&image, NULL, 0, 0, tic));
    CHECK(!storage_write_words(pool, tic, 8));
    CHECK(!storage_write_words(samplers, (const u32[8]){2u | 2u << 3 | 2u << 6}, 8));
    CHECK(!storage_write_words(constants + 0x128, (const u32[1]){0}, 1));
    u8 original[512], expected[512];
    memset(original, 0xcd, sizeof original);
    const u32 values[4] = {0x3e000000, 0x3ec00000, 0x3f200000, 0x3e800000};
    for (u32 x = 0; x < 4; x++) snx_st_le32(original + gob_x_offset(4 * x), values[x]);
    memcpy(expected, original, sizeof expected);
    for (u32 x = 0; x < 3; x++) snx_st_le32(expected + gob_x_offset(4 * x), 0x3f400000);
    CHECK(!gmem_write(source, original, sizeof original));
    CHECK(stage_image_program(gpu, ST_VS, 0));
    CHECK(stage_image_program(gpu + 0x1000, ST_TCS, 0));
    CHECK(cross_alias_tess_program(gpu + 0x1800, 1));
    CHECK(cross_alias_tess_program(gpu + 0x2000, 0));
    CHECK(cross_alias_fragment_program(gpu + 0x3000, 1));
    CHECK(cross_alias_fragment_program(gpu + 0x3400, 0));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 139;
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32); r[0x583] = (u32)gpu;
    r[0x810] = r[0x820] = r[0x830] = r[0x850] = 1;
    r[0x821] = 0x1000; r[0x831] = 0x1800; r[0x851] = 0x3000;
    r[0x982] = 2;
    r[0x64b] = 1; r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    r[0xc8] = 1; r[0x373] = 1; /* triangle domain, point output, one input control point */
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    vk_lock();
    for (u32 i = 0; i < 2; i++) rt_clear_format(channel, targets[i], 4, 4, 0, 0, 0xe5);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.tex_pool = pool; state->bind.smp_pool = samplers;
    state->bind.tex_max = state->bind.smp_max = 0;
    for (u32 stage = 0; stage < DS_GFX_STAGES; stage++) {
        state->bind.cb_addr[stage][2] = constants;
        state->bind.cb_size[stage][2] = 0x200;
    }
    state->blend.en[0] = 0; state->blend.mask[0] = 15;
    state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = state->dyn.alpha_test = 0;
    state->rkey.sample_mask = ~0u;
    DrawCall call = {.topology = 14, .count = 1, .instances = 1};
    u64 first_finishes = 0, first_waits = 0;
    for (u32 draw = 0; draw < 2; draw++) {
        state->tgt.color[0].gva = targets[draw];
        state->prog.code[3] = gpu + (draw ? 0x2000 : 0x1800);
        state->prog.code[5] = gpu + (draw ? 0x3400 : 0x3000);
        state->gen[DS_PROGRAMS] = state->gen[DS_TARGETS] = state->gen[DS_TARGET_FORMATS] = ds_generation_next();
        u64 drawn = g_vk_drawn;
        vk_render_draw(state, &call, &channel->owner);
        CHECK(g_vk_drawn == drawn + 1);
        vk_render_end();
        u64 waits = 0;
        for (u32 i = 0; i < VK_WAIT_N; i++) waits += g_vk_waits[i];
        if (!draw) first_finishes = g_vk_finishes, first_waits = waits;
        else CHECK(g_vk_finishes == first_finishes && waits == first_waits);
    }
    /* The renderer can submit asynchronously every millisecond. Both draws must precede any host wait or
       readback landing; the second sample needs the TES write dependency. */
    for (u32 draw = 0; draw < 2; draw++) {
        Surf* surface = vk_rt_find(targets[draw]);
        CHECK(surface);
        u8 pixels[64];
        CHECK(!ms_read_pixels(surface, 4, 4, pixels));
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                u32 want = x == 2 && y == 2 ? (draw ? 0x3f400000u : 0x3e800000u) : 0x3f800000u;
                u32 got = snx_ld_le32(pixels + (4 * y + x) * 4);
                if (got != want) fprintf(stderr, "Cross-stage alias draw%u pixel%u,%u: %08x expected%08x\n",
                                          draw, x, y, got, want);
                CHECK(got == want);
            }
    }
    vk_unlock();
    u8 result[512];
    CHECK(!gmem_read(source, result, sizeof result));
    CHECK(!memcmp(result, expected, sizeof result));
    free(channel);
    puts("PASS graphics cross-stage image alias: TES stores, disjoint FS samples, next-draw visibility and padding exact=1");
    return 0;
}
