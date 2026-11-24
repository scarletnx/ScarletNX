/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A compute program that reads its warp's other lanes, on a device whose subgroups are narrower than a warp's 32 (a Mali's
   16, lavapipe's 8), runs each invocation as a warp of its own where that gives the same (warp_idioms.c). Two of a game's
   idioms, whose dispatches a Mali-G720 left out: an add its lanes combine, which hands out the places of a list, and a
   block's minimum and maximum, reduced by SHFL.BFLY. A dispatch of LONE_GROUPS blocks of 64 writes each invocation's number
   at the place the add gave it and each block's extremes, once with the device's subgroups and once with the compute
   stage taken out of them: both must count every invocation, give each a place of its own and the true extremes. No
   extracted shaders or game data are used. */
#define LONE_GROUPS 5u
#define LONE_COUNT  (64u * LONE_GROUPS)

/* register v reduced over the warp in R30 to R39, and lane 0's (P1) atomic of it at shared byte at */
static u32 lone_reduce(u64* ops, u32 n, u32 v, int max, u32 at) {
    for (u32 s = 0; s < 5; s++) {
        const u32 swapped = 30 + 2 * s;
        ops[n++] = 0xef17007cf0070000ull | swapped | (u64)v << 8 | 1ull << (20 + s); /* SHFL.BFLY, PT, Rv, 1 << s */
        ops[n++] =
            0x5c20038000070000ull | (swapped + 1) | (u64)v << 8 | (u64)swapped << 20 | (u64)max << 42; /* IMNMX */
        v = swapped + 1;
    }
    /* @P1 ATOMS.MIN or .MAX RZ, [at], R39 */
    ops[n++] = 0xec0000000001ffffull | (max ? 2ull : 1ull) << 52 | 39ull << 20 | (u64)(at / 4) << 30;
    return n;
}
static int compute_lone_program(u64 address, u32 marker) {
    u64 ops[96];
    u32 n = 0;
    ops[n++] = 0xf0c8000002170002ull; /* S2R R2, SR_TID.X */
    ops[n++] = 0xf0c800000257000cull; /* S2R R12, SR_CTAID.X */
    ops[n++] = 0x3848000000670c0dull; /* SHL R13, R12, 6 */
    ops[n++] = 0x5c10000000270d0dull; /* IADD R13, R13, R2: the invocation's number */
    ops[n++] = 0xf0c800000007000eull; /* S2R R14, SR_LANEID */
    ops[n++] = 0x5b6403800ff70e0full; /* ISETP.EQ.U32.AND P1, PT, R14, RZ, PT */
    ops[n++] = 0x010ffffffff7f009ull; /* MOV32I R9, 0xffffffff */
    ops[n++] = 0xef5c00000007ff09ull; /* STS [0], R9 */
    ops[n++] = 0xef5c00000047ffffull; /* STS [4], RZ */
    ops[n++] = 0xf0a81b8000070000ull; /* BAR.SYNC */
    n = lone_reduce(ops, n, 13, 0, 0);
    n = lone_reduce(ops, n, 13, 1, 4);
    ops[n++] = 0xf0a81b8000070000ull;                        /* BAR.SYNC */
    ops[n++] = 0x50d8e38000070003ull;                        /* VOTE.ALL R3, PT, PT */
    ops[n++] = 0xf0c8000003970006ull;                        /* S2R R6, SR_LTMASK */
    ops[n++] = 0x5c30000000370005ull;                        /* FLO.U32 R5, R3 */
    ops[n++] = 0x5c08000000370004ull;                        /* POPC R4, R3 */
    ops[n++] = 0x5b64038000e70507ull;                        /* ISETP.EQ.U32.AND P0, PT, R5, R14, PT */
    ops[n++] = 0x4c9807800c470000ull;                        /* MOV R0, c[0][0x310] */
    ops[n++] = 0x4c9807800c570001ull;                        /* MOV R1, c[0][0x314] */
    ops[n++] = 0xed01000000400004ull;                        /* @P0 ATOM.E.ADD R4, [R0], R4 */
    ops[n++] = 0x5c47000000370606ull;                        /* LOP.AND R6, R6, R3 */
    ops[n++] = 0x5c08000000670007ull;                        /* POPC R7, R6 */
    ops[n++] = 0xef17007c20570408ull;                        /* SHFL.IDX R8, PT, R4, R5, 0x1f */
    ops[n++] = 0x5c10000000770808ull;                        /* IADD R8, R8, R7: the invocation's place */
    ops[n++] = 0x3848000000270808ull;                        /* SHL R8, R8, 2 */
    ops[n++] = 0x4c10800000070808ull | (0x320ull / 4) << 20; /* IADD.CC R8, R8, c[0][0x320] */
    ops[n++] = 0x4c1008000007ff09ull | (0x324ull / 4) << 20; /* IADD.X R9, RZ, c[0][0x324] */
    ops[n++] = 0xeedc20000007080dull;                        /* STG.E [R8], R13 */
    ops[n++] = 0xef4c10000007ff10ull;                        /* LDS R16, [0] */
    ops[n++] = 0xef4c10000047ff11ull;                        /* LDS R17, [4] */
    ops[n++] = 0x3848000000370c0aull;                        /* SHL R10, R12, 3 */
    ops[n++] = 0x4c10800000070a0aull | (0x330ull / 4) << 20; /* IADD.CC R10, R10, c[0][0x330] */
    ops[n++] = 0x4c1008000007ff0bull | (0x334ull / 4) << 20; /* IADD.X R11, RZ, c[0][0x334] */
    ops[n++] = 0xeedc200000070a10ull;                        /* STG.E [R10], R16 */
    ops[n++] = 0xeedc200000470a11ull;                        /* STG.E [R10 + 4], R17 */
    ops[n++] = storage_mov(13, marker);                      /* each pass's program one of its own */
    ops[n++] = 0xe30000000007000full;                        /* EXIT */
    u8 code[1024] = {0};
    u32 at = 0;
    for (u32 i = 0; i < n; i++) {
        if (!(at % 4)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    return n <= sizeof ops / sizeof *ops && at * 8 <= sizeof code && !gmem_write(address, code, sizeof code);
}

/* one dispatch with the compute stage's subgroups as stages has them */
static int compute_lone_pass(u64 gpu, u64 code_at, VkShaderStageFlags stages, const char* what) {
    const u64 constants = gpu + 0x3000, counter = gpu + 0x4000, places = gpu + 0x5000, extremes = gpu + 0x6000;
    u32 zeros[LONE_COUNT] = {0}, got[LONE_COUNT], ends[2 * LONE_GROUPS], count = 0, qmd[64] = {0};
    CHECK(compute_lone_program(code_at, 0x5a60 + (u32)(code_at - gpu)));
    CHECK(!gmem_write(counter, zeros, 4) && !gmem_write(places, zeros, sizeof zeros) &&
          !gmem_write(extremes, zeros, sizeof ends));
    const u64 buffers[3] = {counter, places, extremes};
    const u32 bytes[3] = {4, sizeof zeros, sizeof ends};
    for (u32 b = 0; b < 3; b++) {
        const u32 descriptor[4] = {(u32)buffers[b], (u32)(buffers[b] >> 32), bytes[b], 0};
        CHECK(!gmem_write(constants + 0x310 + 16 * b, descriptor, sizeof descriptor));
    }
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)constants;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(constants >> 32) | 0x1000u << 15;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1;
    qmd[QMD_GRID_DIM_X] = LONE_GROUPS;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 64u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_SHARED_ALLOC] = 256;
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    const VkShaderStageFlags device_stages = g_vk_subgroup_stages;
    const u64 dispatches = g_vk_dispatches;
    vk_lock();
    g_vk_subgroup_stages = stages;
    DispatchState d;
    mx_dispatch_state(qmd, ch->rcomp, code_at, &d);
    vk_dispatch(&d);
    vk_compute_land();
    g_vk_subgroup_stages = device_stages;
    vk_unlock();
    free(ch);
    CHECK(!gmem_read(counter, &count, 4) && !gmem_read(places, got, sizeof got) &&
          !gmem_read(extremes, ends, sizeof ends));
    u8 seen[LONE_COUNT] = {0};
    u32 wrong = 0, wrong_ends = 0;
    for (u32 k = 0; k < LONE_COUNT; k++) wrong += got[k] >= LONE_COUNT || seen[got[k]]++;
    for (u32 g = 0; g < LONE_GROUPS; g++) wrong_ends += ends[2 * g] != 64 * g || ends[2 * g + 1] != 64 * g + 63;
    if (g_vk_dispatches != dispatches + 1 || count != LONE_COUNT || wrong || wrong_ends)
        fprintf(
            stderr,
            "compute lone lane, %s: %llu dispatches recorded (1 expected), %u invocations counted (%u expected), %u "
            "places wrong, %u blocks' extremes wrong (the first's are %u and %u)\n",
            what, (unsigned long long)(g_vk_dispatches - dispatches), count, LONE_COUNT, wrong, wrong_ends, ends[0],
            ends[1]);
    CHECK(g_vk_dispatches == dispatches + 1 && count == LONE_COUNT && !wrong && !wrong_ends);
    return 0;
}
static int check_compute_lone_lane(void) {
    const u64 gpu = 0x9f0000000ull, cpu = HEAP_BASE + 0x3c240000;
    const int has_compute = (g_vk_subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!compute_lone_pass(gpu, gpu, g_vk_subgroup_stages,
                             has_compute ? "the device's compute subgroups" : "a device without compute subgroups"));
    CHECK(!compute_lone_pass(gpu, gpu + 0x1000, g_vk_subgroup_stages & ~(VkShaderStageFlags)VK_SHADER_STAGE_COMPUTE_BIT,
                             "the compute stage taken out of the subgroups"));
    CHECK(mem_unmap(cpu, 0x10000) == 0);
    printf(
        "PASS compute lone lane: %u invocations took a place each from a combined add and %u blocks their extremes, %s "
        "and as lanes alone\n",
        LONE_COUNT, LONE_GROUPS, has_compute ? "with the device's compute subgroups" : "on a device without them");
    return 0;
}
