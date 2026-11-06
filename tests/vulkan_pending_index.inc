/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pending storage results looked up by the pages they cover (buffer.c's index of g_out): with 257 results pending, a
   read of one result's range, a read beside it on its page, the page-level question, a read through another GPU
   address of its pages, a read over a hundred results and a mapping change over a result's unmapped page each have
   only the results on their pages compared (vk_buf_pending_inspected), and get what comparing every result gave: the
   result itself, guest memory, a composition, the landing. Every result then lands its bytes and no others */
static int check_pending_index(void) {
    const u64 gva = 0x6d000000ull, alias = 0x6e000000ull, cpu = HEAP_BASE + 0x200000;
    const u64 ugva = 0x6f000000ull, ucpu = HEAP_BASE + 0x300000;
    enum { RESULTS = 256, HALF = 2048, K = 100, MANY = 100 };
    VkBuffer out[RESULTS];
    u64 out_at[RESULTS];
    if (vk_uma()) {
        puts("Pending storage results by page: none to look up with unified memory (bindings write guest memory)");
        return 0;
    }
    gmmu_map(gva, cpu, RESULTS * 4096ull);
    gmmu_map(alias, cpu, RESULTS * 4096ull);
    gmmu_map(ugva, ucpu, 4096); /* the GPU page after it stays unmapped */
    memset(GP(cpu), 0x11, RESULTS * 4096);
    memset(GP(ucpu), 0x11, 0x2000);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    for (u32 i = 0; i < RESULTS; i++) {
        CHECK(vk_buf_copy_writable(gva + i * 4096ull, HALF, &out[i], &out_at[i], 1));
        vk_buf_writable_commit();
        CHECK(!owned_test_fill(out[i], out_at[i], HALF, (u8)(0x80 + (i & 0x7f))));
    }
    VkBuffer uout, buf;
    u64 uat, at;
    CHECK(vk_buf_copy_writable(ugva, 0x2000, &uout, &uat, 1));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(uout, uat, 0x2000, 0x5a));

    /* one result's range, older than the newest: that result, where it is */
    u64 seen = vk_buf_pending_inspected();
    CHECK(vk_buf_get_input(gva + K * 4096ull, HALF, 1, &buf, &at));
    CHECK(buf == out[K] && at == out_at[K]);
    u64 one = vk_buf_pending_inspected() - seen;
    /* the rest of its page: no byte of a result there, guest memory (two searches, one result each: the source's,
       then the read of guest memory's own before it reads) */
    seen = vk_buf_pending_inspected();
    CHECK(vk_buf_get_input(gva + K * 4096ull + HALF, HALF, 1, &buf, &at));
    CHECK(!(buf == out[K] && at == out_at[K]));
    u64 beside = vk_buf_pending_inspected() - seen;
    /* asked by page, as draws ask before reading: pending */
    seen = vk_buf_pending_inspected();
    CHECK(vk_buf_pending(gva + K * 4096ull + HALF, 16));
    u64 page = vk_buf_pending_inspected() - seen;
    /* through the other GPU address of its pages: composed from it (a search, then the composition's own) */
    seen = vk_buf_pending_inspected();
    CHECK(vk_buf_get_input(alias + K * 4096ull, HALF, 1, &buf, &at));
    CHECK(!(buf == out[K] && at == out_at[K]));
    u64 other = vk_buf_pending_inspected() - seen;
    /* over the first hundred results' pages: those hundred, composed (a search, then the composition's own) */
    seen = vk_buf_pending_inspected();
    CHECK(vk_buf_get_input(gva, MANY * 4096ull, 1, &buf, &at));
    u64 many = vk_buf_pending_inspected() - seen;
    /* a mapping change over the unmapped page of the last result: no guest page there, found by its GPU pages, and
       landed with everything recorded */
    seen = vk_buf_pending_inspected();
    vk_buf_mem_need_map(ugva + 0x1000, 0x1000);
    u64 unmapped = vk_buf_pending_inspected() - seen;
    CHECK(!vk_buf_pending_any());
    printf("pending storage results compared: %llu for one result's range, %llu beside it, %llu by page, %llu through "
           "another address, %llu over %u results' pages, %llu over an unmapped page, of %u pending\n",
           (unsigned long long)one, (unsigned long long)beside, (unsigned long long)page, (unsigned long long)other,
           (unsigned long long)many, MANY, (unsigned long long)unmapped, RESULTS + 1);
    CHECK(one == 1 && beside == 2 && page == 1 && other == 2 && many == 2 * MANY && unmapped == 1);
    vk_finish();
    vk_unlock();
    for (u32 i = 0; i < RESULTS; i++)
        for (u32 j = 0; j < 4096; j++)
            if (GP(cpu)[i * 4096 + j] != (j < HALF ? (u8)(0x80 + (i & 0x7f)) : 0x11)) {
                fprintf(stderr, "vulkan_headless: result %u's page, byte %#x: %#x\n", i, j, GP(cpu)[i * 4096 + j]);
                return 1;
            }
    for (u32 j = 0; j < 0x2000; j++)
        if (GP(ucpu)[j] != (j < 0x1000 ? 0x5a : 0x11)) {
            fprintf(stderr, "vulkan_headless: the partly unmapped result's memory, byte %#x: %#x\n", j, GP(ucpu)[j]);
            return 1;
        }
    CHECK(g_vk_validation_messages == validation);
    puts("Pending storage results are looked up by the pages they cover");
    return 0;
}
