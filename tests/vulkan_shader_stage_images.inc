/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Actual Maxwell SULD.D.32 and SUST.P buffer-image operations, through the renderer's per-stage descriptors.
   Evaluation invocations use distinct tessellation coordinates to avoid concurrent writes to one texel. */
static int stage_image_program(u64 address, int stage, int images) {
    u64 ops[40];
    u32 count = 0;
    if (images) {
        if (stage == ST_TES) {
            ops[count++] = 0xefd800000007ff00ull | 0xffull << 39 | 0x2f0ull << 20 | 1ull << 47;
            ops[count++] = 0x5cb0018000070a00ull; /* F2I.U32.F32.TRUNC R0, R0 */
            ops[count++] = 0x5cb0018000170a01ull; /* F2I.U32.F32.TRUNC R1, R1 */
            ops[count++] = 0x3848000000170000ull; /* SHL R0, R0, 1 */
            ops[count++] = 0x5c10000000170001ull; /* IADD R1, R0, R1: 2*x+y */
            ops[count++] = 0x3810000000170100ull; /* IADD R0, R1, 1: source texel */
            ops[count++] = 0x3810000000270101ull; /* IADD R1, R1, 2: destination texel */
        } else {
            ops[count++] = storage_mov(0, 1);
            ops[count++] = storage_mov(1, 2);
        }
        ops[count++] = 0x4c98078804a70004ull; /* MOV R4, c[2][0x128] */
        ops[count++] = 0x4c98078804b70006ull; /* MOV R6, c[2][0x12c] */
        ops[count++] = 0xeb10020200470008ull; /* SULD.D.32 R8, [R0], R4 */
        ops[count++] = 0xeb203a0200f77614ull & ~(0xffull | 0xffull << 8 | 0xfull << 16 | 0xffull << 39 | 7ull << 33) |
                       8ull | 1ull << 8 | 7ull << 16 | 6ull << 39 | 1ull << 33;
    }
    /* A fixed point needs no vertex buffer and leaves the image operations' registers untouched. */
    for (u32 i = 0; i < 4; i++) ops[count++] = storage_mov(16 + i, i == 3 ? 0x3f800000 : 0);
    ops[count++] = 0xeff1ff800707ff10ull; /* AST a[0x70], R16..R19 */
    if (stage == ST_TCS) {
        ops[count++] = storage_mov(12, 0x3f800000);
        for (u32 i = 0; i < 6; i++)
            ops[count++] = 0xeff07f808007ff0cull | (u64)(i * 4) << 20; /* AST.P tessellation level */
    }
    if (stage == ST_GS) {
        ops[count++] = 0xfbe000800ff71414ull; /* OUT.EMIT R20, R20, RZ */
        ops[count++] = 0xfbe001000ff71414ull; /* OUT.CUT R20, R20, RZ */
    }
    ops[count++] = 0xe30000000007000full;
    u64 code[64] = {0};
    if (stage == ST_TCS || stage == ST_GS) code[1] = 1u << 24; /* one output invocation */
    if (stage == ST_GS) {
        code[1] |= 1ull << 56; /* point output */
        code[2] = 1;
    }
    u32 at = 10;
    for (u32 i = 0; i < count; i++) {
        if (!(i % 3)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[i];
    }
    CHECK(at <= sizeof code / sizeof *code);
    return invalidate_write_code(address, code, sizeof code / sizeof *code);
}

static int check_shader_stage_images(void) {
    if (!g_vk.feat.vertexPipelineStoresAndAtomics || !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("SKIP graphics image stages: vertex-pipeline storage writes or unformatted image writes unavailable");
        return 0;
    }
    const u64 gpu = 0x4f8000000ull, cpu = HEAP_BASE + 0x730000;
    const u64 pool = gpu + 0x4000, cbuf = gpu + 0x5000, source = gpu + 0x6000, output = gpu + 0x6100;
    const u64 target = gpu + 0x8000;
    const u32 format = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16;
    const u32 headers[16] = {format, (u32)source, (u32)(source >> 32), 0, 7u | 6u << 23, 0, 0, 0,
                             format, (u32)output, (u32)(output >> 32), 0, 7u | 6u << 23, 0, 0, 0};
    u32 input[8], initial[8];
    for (u32 i = 0; i < 8; i++) {
        input[i] = 0x13570000u + i * 0x10203u;
        initial[i] = 0xcafe0000u + i;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 117;
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!storage_write_words(pool, headers, 16));
    CHECK(!storage_write_words(cbuf + 0x128, (const u32[2]){0, 1}, 2));
    const u64 end = 0xe30000000007000full;
    CHECK(mesh_program(gpu + 0x3000, &end, 1, 1));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32), r[0x583] = (u32)gpu;
    r[0x55d] = (u32)(pool >> 32), r[0x55e] = (u32)pool, r[0x55f] = 1;
    r[0x810] = r[0x850] = 1;
    r[0x851] = 0x3000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 0;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    for (u32 stage = ST_VS; stage <= ST_GS; stage++) {
        channel->ds.bind.cb_addr[stage][2] = cbuf;
        channel->ds.bind.cb_size[stage][2] = 0x200;
    }
    u32 tested = 0;
    for (int stage = ST_VS; stage <= ST_GS; stage++) {
        if (g_vk.buffer_image_slots[stage] < 2) continue;
        int tessellation = stage == ST_TCS || stage == ST_TES;
        if (tessellation && (!g_vk.feat.tessellationShader || g_vk_stage_sets < 5)) continue;
        if (stage == ST_GS && (!g_vk.feat.geometryShader || g_vk_stage_sets < 5)) continue;
        CHECK(stage_image_program(gpu, ST_VS, stage == ST_VS));
        if (tessellation) {
            CHECK(stage_image_program(gpu + 0x1000, ST_TCS, stage == ST_TCS));
            CHECK(stage_image_program(gpu + 0x1800, ST_TES, stage == ST_TES));
        }
        if (stage == ST_GS) CHECK(stage_image_program(gpu + 0x2000, ST_GS, 1));
        CHECK(!storage_write_words(source, input, 8));
        CHECK(!storage_write_words(output, initial, 8));
        g_renderer->method_barrier(5);
        r[0x820] = r[0x830] = tessellation;
        r[0x821] = 0x1000, r[0x831] = 0x1800;
        r[0x840] = stage == ST_GS;
        r[0x841] = 0x2000;
        r[0xc8] = 1;  /* triangle domain, equal spacing, point output */
        r[0x373] = 1; /* one input control point */
        DrawCall call = {.topology = tessellation ? 14 : 0, .count = 1, .instances = 1};
        vk_lock();
        rt_clear(channel, target, 4, 4, 0, 1);
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        u64 drawn = g_vk_drawn;
        vk_render_draw(&channel->ds, &call, &channel->owner);
        vk_render_end();
        CHECK(g_vk_drawn == drawn + 1);
        vk_finish();
        vk_unlock();
        u8 actual[32], original[32];
        CHECK(!gmem_read(output, actual, sizeof actual));
        CHECK(!gmem_read(source, original, sizeof original));
        for (u32 i = 0; i < 8; i++) {
            u32 wanted = i >= 2 && i < (stage == ST_TES ? 5u : 3u) ? input[i - 1] : initial[i];
            if (snx_ld_le32(actual + i * 4) != wanted)
                fprintf(stderr, "graphics image stage%d texel%u: %08x expected %08x\n", stage, i,
                        snx_ld_le32(actual + i * 4), wanted);
            CHECK(snx_ld_le32(actual + i * 4) == wanted);
            CHECK(snx_ld_le32(original + i * 4) == input[i]);
        }
        tested++;
    }
    free(channel);
    printf("PASS graphics image stages: %u vertex/control/evaluation/geometry load-store programs exact=1\n", tested);
    return 0;
}
