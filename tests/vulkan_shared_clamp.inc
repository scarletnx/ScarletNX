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
   on the real one) runs when its accesses stay below what the device gives, and is left out, counted under its own
   reason, when an address may go past it */
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
    if (ran_below != 1 || ran_unbounded != 0 || skipped_after != skipped + 1 ||
        !strstr(why, "shared memory past the GPU's"))
        fprintf(stderr,
                "shared memory clamp: %llu and %llu dispatches recorded, %llu skipped before and %llu after%s\n",
                (unsigned long long)ran_below, (unsigned long long)ran_unbounded, (unsigned long long)skipped,
                (unsigned long long)skipped_after, why);
    CHECK(ran_below == 1);
    CHECK(ran_unbounded == 0);
    CHECK(skipped_after == skipped + 1 && strstr(why, "shared memory past the GPU's"));
    puts("shared memory clamp: a launch past the device's size runs when its accesses fit, and is left out when not");
    return 0;
}
