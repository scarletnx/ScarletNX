/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A vertex program that votes, on a device whose subgroups leave the vertex stage out (an Adreno 830 lists fragment
   and compute only), runs each invocation as lane 0 alone (spirv_translate.c's lone_lane). Two forms of a game's
   vertex programs, whose draws that phone skipped: a warp-aggregated atomic, where the highest live lane adds the live
   lanes' count to a word, and a block branched around when the whole warp agrees (VOTE.ALL), which every lane of a
   warp that doesn't agree runs with its own predicate. A draw of 37 points (a warp and a part of one) counts its
   vertices and stores each vertex's result, once with the device's subgroups and once with the vertex stage taken out
   of them: both must give the count and the results the console gives. No extracted shaders or game data are used. */
#define LONE_POINTS 37

/* the program, at offset (from the code's base) of address: marker makes each pass's program a program of its own */
static int lone_lane_program(u64 address, u32 offset, u32 marker) {
    const u64 ops[] = {
        0xEFD87F802FC7FF04ull,       /* ALD R4, a[0x2fc]: the vertex number */
        0x50D8E38000070002ull,       /* VOTE.ALL R2, PT, PT: the live lanes */
        0xF0C8000000070001ull,       /* S2R R1, SR_LANEID */
        0x5C30000000270000ull,       /* FLO.U32 R0, R2: the highest live lane */
        0x5C08000000270003ull,       /* POPC R3, R2: how many live */
        0x5B64038000170007ull,       /* ISETP.EQ.U32.AND P0, PT, R0, R1, PT */
        0x4C98078004870000ull,       /* MOV R0, c[0x0][0x120] */
        0x4C98078004970001ull,       /* MOV R1, c[0x0][0x124] */
        0xEBF9000000000003ull,       /* @P0 RED.E.ADD [R0], R3 */
        0x0400000000170405ull,       /* LOP32I.AND R5, R4, 1 */
        0x5B6503800FF70517ull,       /* ISETP.EQ.U32.AND P2, PT, R5, RZ, PT: an even vertex */
        0x50D86100000700FFull,       /* VOTE.ALL RZ, P3, P2 */
        0x010000000007F00Aull,       /* MOV32I R10, 0 */
        0x010000000013F00Aull,       /* @P3 MOV32I R10, 1 */
        0xE24000000183000Full,       /* @P3 BRA past the next two and their scheduling word */
        0x01000000002AF00Aull,       /* @!P2 MOV32I R10, 2 */
        0x010000000012F00Aull,       /* @P2 MOV32I R10, 1 */
        0x3848000000270406ull,       /* SHL R6, R4, 2 */
        0x4C10800004C70606ull,       /* IADD.CC R6, R6, c[0x0][0x130] */
        0x4C10080004D7FF07ull,       /* IADD.X R7, RZ, c[0x0][0x134] */
        0xEEDC20000007060Aull,       /* STG.E [R6], R10 */
        storage_mov(13, marker),     /* MOV32I R13, marker */
        storage_mov(16, 0),          /* the position: x */
        storage_mov(17, 0),          /* y */
        storage_mov(18, 0),          /* z */
        storage_mov(19, 0x3f800000), /* w */
        0xEFF1FF800707FF10ull,       /* AST a[0x70], R16..R19 */
        0xE30000000007000Full,
    };
    return mesh_program(address + offset, ops, sizeof ops / sizeof *ops, 0);
}

/* one draw with the vertex stage's subgroups as stages has them: 0 when its count and results are the console's */
static int lone_lane_pass(GpuChannel* channel, u64 address, u64 cpu, u32 offset, VkShaderStageFlags stages,
                          const char* what) {
    const u64 cbuf = address + 0x3000, counter = address + 0x4000, results = address + 0x5000;
    u8 zeros[4 * LONE_POINTS] = {0};
    CHECK(lone_lane_program(address, offset, 0x5a50 + offset));
    CHECK(!gmem_write(counter, zeros, 4));
    CHECK(!gmem_write(results, zeros, sizeof zeros));
    const u32 count_desc[3] = {(u32)counter, (u32)(counter >> 32), 4};
    const u32 result_desc[3] = {(u32)results, (u32)(results >> 32), sizeof zeros};
    CHECK(!storage_write_words(cbuf + 0x120, count_desc, 3));
    CHECK(!storage_write_words(cbuf + 0x130, result_desc, 3));
    g_renderer->method_barrier(5);
    channel->r3d[0x811] = offset; /* VertexB's program */
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    VkShaderStageFlags device_stages = g_vk_subgroup_stages;
    g_vk_subgroup_stages = stages;
    vk_lock();
    u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
    DrawCall call = {.topology = 0, .count = LONE_POINTS, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_finish();
    int drew = g_vk_drawn == drawn + 1 && g_vk_skipped == skipped;
    vk_unlock();
    g_vk_subgroup_stages = device_stages;
    u32 count = snx_ld_le32(GP(cpu + 0x4000)), wrong = 0, first = LONE_POINTS;
    for (u32 v = 0; v < LONE_POINTS; v++)
        if (snx_ld_le32(GP(cpu + 0x5000 + 4 * v)) != (v % 2 ? 2u : 1u)) wrong++, first = first < v ? first : v;
    if (!drew || count != LONE_POINTS || wrong)
        fprintf(stderr,
                "lone lane, %s: drawn %d, %u vertices counted (expected %u), %u results wrong (first: vertex %u, %u)\n",
                what, drew, count, LONE_POINTS, wrong, first,
                first < LONE_POINTS ? snx_ld_le32(GP(cpu + 0x5000 + 4 * first)) : 0);
    CHECK(drew && count == LONE_POINTS && !wrong);
    return 0;
}

static int check_lone_lane(void) {
    if (!g_vk.feat.vertexPipelineStoresAndAtomics) {
        puts("SKIP lone lane: the device has no stores or atomics in vertex programs");
        return 0;
    }
    const u64 address = 0x8d8000000ull, cpu = HEAP_BASE + 0x11b00000;
    const u64 fragment[] = {0xE30000000007000Full};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(address, cpu, 0x10000);
    CHECK(mesh_program(address + 0x2000, fragment, 1, 1));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 141;
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x2000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 0;
    memcpy(r + 0x280, transform, sizeof transform);
    channel->ds.bind.cb_addr[0][0] = address + 0x3000;
    channel->ds.bind.cb_size[0][0] = 0x1000;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    channel->ds.owner = channel->id;
    int has_vertex = (g_vk_subgroup_stages & VK_SHADER_STAGE_VERTEX_BIT) != 0;
    CHECK(!lone_lane_pass(channel, address, cpu, 0, g_vk_subgroup_stages,
                          has_vertex ? "the device's vertex subgroups" : "a device without vertex subgroups"));
    CHECK(!lone_lane_pass(channel, address, cpu, 0x1000, g_vk_subgroup_stages & ~VK_SHADER_STAGE_VERTEX_BIT,
                          "the vertex stage taken out of the subgroups"));
    channel->ds.bind.cb_addr[0][0] = 0;
    channel->ds.bind.cb_size[0][0] = 0;
    free(channel);
    printf("PASS lone lane: a warp-aggregated atomic counted %u vertices and VOTE.ALL's branch kept each vertex's "
           "result, %s and as lane 0 alone\n",
           LONE_POINTS, has_vertex ? "with the device's vertex subgroups" : "on a device without them");
    return 0;
}
