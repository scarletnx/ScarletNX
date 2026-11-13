/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 */

/* the compute launch at code, of one block of 64 threads asking for shared bytes */
static void shared_clamp_dispatch(GpuChannel* channel, u64 code, u32 shared) {
    u32 qmd[64] = {0};
    DispatchState d;
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_SHARED_ALLOC] = shared;
    qmd[QMD_BLOCK_DIM_X] = 64u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    vk_lock();
    mx_dispatch_state(qmd, channel->rcomp, code, &d);
    vk_dispatch(&d);
    vk_compute_land();
    vk_unlock();
}

/* A launch asking for more shared memory than the device gives a block (here a device that gives 1024 bytes, faked
   on the real one) runs when its accesses stay below what the device gives, and when an address may go past it runs
   too, its shared memory in a storage buffer (ShaderInfo.shared_spill), nothing left out */
static int check_shared_clamp(void) {
    const u64 address = 0x4c0000000ull;
    const u32 asks = 2048, faked = 1024;
    GpuChannel* channel = calloc(1, sizeof(*channel));
    u32 limit = g_vk.props.limits.maxComputeSharedMemorySize;
    u64 dispatches, skipped, ran_below, ran_unbounded, skipped_after;
    char why[512];
    /* each thread stores at its index times 4: 256 bytes */
    const u64 below[] = {0x001f8000fc0007e0ull,
                         0xf0c8000002170000ull /* S2R R0, SR_TID.X */,
                         0x3848000000270000ull /* SHL R0, R0, 2 */,
                         0xef5c000000070000ull /* STS [R0], R0 */,
                         0x001f8000fc0007e0ull,
                         0xe30000000007000full};
    /* the address is a word loaded from shared memory: no bound */
    const u64 unbounded[] = {0x001f8000fc0007e0ull, 0xef4c00000007ff00ull /* LDS R0, [RZ] */,
                             0xef5c000000070000ull /* STS [R0], R0 */, 0xe30000000007000full};

    CHECK(channel);
    CHECK(limit > asks); /* the device's own size takes both launches: only the faked one can turn them away */
    gmmu_map(address, HEAP_BASE + 0x780000, 0x40000); /* a program is read through a 64 KiB window */
    CHECK(invalidate_write_code(address, below, sizeof(below) / sizeof(*below)));
    CHECK(invalidate_write_code(address + 0x100, unbounded, sizeof(unbounded) / sizeof(*unbounded)));
    skipped = vk_cs_skipped(why, sizeof(why));
    g_vk.props.limits.maxComputeSharedMemorySize = faked;
    dispatches = g_vk_dispatches;
    shared_clamp_dispatch(channel, address, asks);
    ran_below = g_vk_dispatches - dispatches;
    dispatches = g_vk_dispatches;
    shared_clamp_dispatch(channel, address + 0x100, asks);
    ran_unbounded = g_vk_dispatches - dispatches;
    g_vk.props.limits.maxComputeSharedMemorySize = limit;
    skipped_after = vk_cs_skipped(why, sizeof(why));
    free(channel);
    if (ran_below != 1 || ran_unbounded != 1 || skipped_after != skipped)
        fprintf(stderr,
                "shared memory clamp: %llu and %llu dispatches recorded, %llu skipped before and %llu after%s\n",
                (unsigned long long)ran_below, (unsigned long long)ran_unbounded, (unsigned long long)skipped,
                (unsigned long long)skipped_after, why);
    CHECK(ran_below == 1);
    CHECK(ran_unbounded == 1);
    CHECK(skipped_after == skipped);
    puts("shared memory clamp: a launch past the device's size runs when its accesses fit, and from a buffer when not");
    return 0;
}

/* Shared memory in a storage buffer (ShaderInfo.shared_spill) where a block asks for more than the device gives: 4
   blocks of 64 threads each ask for 48 KiB on a device that gives 1024 bytes (faked on the real one, as above), store
   (block << 8 | thread) at 0xb000 + 4 thread (the base from the constants, which no bound reaches), wait at the
   barrier and read their word back into memory. Each word comes back: the blocks' parts are apart and the barrier
   orders the buffer. With the program skipped the output keeps its 0xcdcdcdcd */
static int check_shared_spill(void) {
    const u64 gpu = 0x9b0000000ull, cpu = HEAP_BASE + 0x1f400000;
    const u64 code_at = gpu, constants = gpu + 0x1000, output = gpu + 0x2000;
    const u32 blocks = 4, threads = 64, asks = 0xc000, faked = 1024;
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x10000);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    const u64 ops[] = {0x4c98078004470000ull,  /* MOV R0, c[0][0x110] */
                       0x4c98078004570001ull,  /* MOV R1, c[0][0x114] */
                       0x4c98078004870002ull,  /* MOV R2, c[0][0x120]: 0xb000 */
                       0xf0c8000002170003ull,  /* S2R R3, SR_TID.X */
                       0x3848000000270304ull,  /* SHL R4, R3, 2 */
                       0x5c10000000470202ull,  /* IADD R2, R2, R4 */
                       0xf0c8000002570005ull,  /* S2R R5, SR_CTAID.X */
                       0x3848000000870506ull,  /* SHL R6, R5, 8 */
                       0x5c10000000370606ull,  /* IADD R6, R6, R3 */
                       0xef5c000000070206ull,  /* STS [R2], R6 */
                       0xf0a81b8000070000ull,  /* BAR.SYNC 0 */
                       0xef4c000000070208ull,  /* LDS R8, [R2] */
                       0x3848000000870507ull,  /* SHL R7, R5, 8: 64 threads of 4 bytes a block */
                       0x5c10000000770000ull,  /* IADD R0, R0, R7 */
                       0x5c10000000470000ull,  /* IADD R0, R0, R4 */
                       0xeedc200000070008ull,  /* STG.E [R0], R8 */
                       0xe30000000007000full}; /* EXIT */
    u8 code[256] = {0};
    u32 at = 0;
    for (u32 i = 0; i < sizeof ops / sizeof *ops; i++) {
        if (!(at % 4)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    CHECK(at * 8 <= sizeof code);
    CHECK(!gmem_write(code_at, code, sizeof code));
    const u32 descriptor[4] = {(u32)output, (u32)(output >> 32), blocks * threads * 4, 0};
    CHECK(!gmem_write(constants + 0x110, descriptor, sizeof descriptor));
    CHECK(!gmem_write(constants + 0x120, (const u32[1]){0xb000}, 4));
    u32 poison[256];
    for (u32 i = 0; i < blocks * threads; i++) poison[i] = 0xcdcdcdcd;
    CHECK(!gmem_write(output, poison, blocks * threads * 4));
    u32 qmd[64] = {0};
    qmd[QMD_GRID_DIM_X] = blocks;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_SHARED_ALLOC] = asks;
    qmd[QMD_BLOCK_DIM_X] = threads << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)constants;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(constants >> 32) | 0x200u << 15;
    u32 limit = g_vk.props.limits.maxComputeSharedMemorySize;
    u64 before = g_vk_dispatches;
    g_vk.props.limits.maxComputeSharedMemorySize = faked;
    vk_lock();
    DispatchState d;
    mx_dispatch_state(qmd, ch->rcomp, code_at, &d);
    vk_dispatch(&d);
    vk_compute_land();
    vk_unlock();
    g_vk.props.limits.maxComputeSharedMemorySize = limit;
    u32 got[256], wrong = 0, first = 0;
    CHECK(!gmem_read(output, got, sizeof got));
    for (u32 i = 0; i < blocks * threads; i++) {
        u32 want = (i / threads) << 8 | (i % threads);
        if (got[i] != want && !wrong++) first = i;
    }
    int ran = g_vk_dispatches == before + 1;
    if (!ran || wrong)
        fprintf(stderr, "shared memory in a buffer: ran %d, %u of %u words wrong, the first %u holding %08x for %08x\n",
                ran, wrong, blocks * threads, first, got[first], (first / threads) << 8 | (first % threads));
    CHECK(ran && !wrong);
    free(ch);
    CHECK(mem_unmap(cpu, 0x10000) == 0);
    printf(
        "shared memory in a buffer: %u blocks of %u threads asking 48 KiB past a faked 1 KiB read back their words\n",
        blocks, threads);
    return 0;
}
