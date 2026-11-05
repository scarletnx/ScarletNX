/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pending storage lookups against real GPU buffers. All outputs remain pending while timed, then every byte is
   checked after GPU completion. Recent and old ranges distinguish the tail shortcut from the full search. */
static int bench_storage_lookup(void) {
    const u64 gva = 0x6c000000ull, cpu = HEAP_BASE + 0x700000;
    const u32 results = 64, bytes = 0x20000, operations = 20000;
    CHECK(!vk_uma()); /* --vk_memory=1: this test measures copied pending results, not host-memory imports. */
    gmmu_map(gva, cpu, 0x80000);
    memset(GP(cpu), 0xcd, 0x80000);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    VkBuffer first = VK_NULL_HANDLE, last = VK_NULL_HANDLE;
    u64 first_at = 0, last_at = 0;
    for (u32 i = 0; i < results; i++) {
        VkBuffer buffer;
        u64 at;
        CHECK(vk_buf_copy_writable(gva + i * 4096ull, 4096, &buffer, &at, 1));
        vk_buf_writable_commit();
        CHECK(!owned_test_fill(buffer, at, 4096, 0x20 + i));
        if (!i) first = buffer, first_at = at;
    }
    const u64 recent = gva + 0x40000;
    CHECK(vk_buf_copy_writable(recent, bytes, &last, &last_at, 1));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(last, last_at, bytes, 0xa5));
    u64 elapsed[2];
    for (u32 old = 0; old < 2; old++) {
        u64 addr = old ? gva : recent;
        u32 len = old ? 4096 : bytes;
        VkBuffer want = old ? first : last, buffer;
        u64 want_at = old ? first_at : last_at, at;
        for (u32 i = 0; i < 64; i++) {
            CHECK(vk_buf_get_input(addr, len, 1, &buffer, &at));
            CHECK(buffer == want && at == want_at);
        }
        u64 begin = plat_ns();
        for (u32 i = 0; i < operations; i++) {
            CHECK(vk_buf_get_input(addr, len, 1, &buffer, &at));
            CHECK(buffer == want && at == want_at);
        }
        elapsed[old] = plat_ns() - begin;
    }
    /* A suffix also takes the newest buffer at its own offset. The neighbour and physical-alias cases run in the
       ordinary storage fixtures, which use this same lookup and check newest-writer ordering. */
    VkBuffer suffix;
    u64 suffix_at;
    CHECK(vk_buf_get_input(recent + 256, bytes - 256, 1, &suffix, &suffix_at));
    CHECK(suffix == last && suffix_at == last_at + 256);
    vk_finish();
    vk_unlock();
    for (u32 i = 0; i < results; i++)
        for (u32 j = 0; j < 4096; j++) CHECK(GP(cpu)[i * 4096 + j] == 0x20 + i);
    for (u32 j = 0; j < bytes; j++) CHECK(GP(cpu + 0x40000)[j] == 0xa5);
    for (u32 j = 0x60000; j < 0x80000; j++) CHECK(GP(cpu)[j] == 0xcd);
    CHECK(g_vk_validation_messages == validation);
    for (u32 old = 0; old < 2; old++)
        printf("GPU_HOTPATH_BENCH workload=storage_lookup_%s operations=%u ns=%llu exact=1 results=%u bytes=%u\n",
               old ? "old" : "recent", operations, (unsigned long long)elapsed[old], results + 1, old ? 4096 : bytes);
    return 0;
}
