/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Logged storage results (buffer.c's land_logged, writelog.c): a result whose writers log the bytes they write lands
   those bytes, and no others, with no original bytes kept. The writers here are copies on the GPU standing in for a
   compute program's stores: each copies its value into the result and its bits into the write log, as
   sm86_translate.c's mark_written has a store do */

/* part 0 of log: the bytes [from, from + n) of a binding written, as the GPU would log them (a bit a byte) */
static int logged_mark(VkWriteLog* log, u32 from, u32 n) {
    static u32 masks[SH_WLOG_WORDS];
    memset(masks, 0, sizeof masks);
    for (u32 b = from; b < from + n; b++) masks[b / 32] |= 1u << (b % 32);
    /* the words set, a run at a time, copied from staging into the log */
    for (u32 i = 0; i < SH_WLOG_WORDS;) {
        if (!masks[i]) {
            i++;
            continue;
        }
        u32 j = i;
        while (j < SH_WLOG_WORDS && masks[j]) j++;
        VkBuffer src;
        u64 at;
        u8* data = vk_stage_upload((u64)(j - i) * 4, 16, &src, &at);
        CHECK(data);
        memcpy(data, masks + i, (size_t)(j - i) * 4);
        VkBufferCopy copy = {at, (u64)i * 4, (u64)(j - i) * 4};
        rec_CmdCopyBuffer(vk_cmd(), src, vk_wlog_buffer(log), 1, &copy);
        i = j;
    }
    return 0;
}
/* a logged writer of [gva, gva + len): bound, committed, its value copied into [from, from + n) of the binding and
   those bytes logged; logged_n bytes from from are logged, which may be fewer than it changes */
static int logged_write(u64 gva, u32 len, u32 from, u32 n, u32 logged_n, u8 value, VkBuffer* out, u64* at) {
    VkWriteLog* log = vk_wlog_get(1);
    CHECK(log);
    CHECK(vk_buf_copy_writable_logged(gva, len, out, at, 1, log, 0));
    vk_buf_writable_commit();
    if (n) CHECK(!owned_test_fill(*out, *at + from, n, value));
    if (logged_n) CHECK(!logged_mark(log, from, logged_n));
    vk_buf_log_retire(log);
    return 0;
}
static void logged_count(void* user, u32 from, u32 to) {
    *(u32*)user += to - from;
}
static int logged_expect(const u8* mem, const u8* before, u32 len, const u32* from, const u32* n, const u8* value,
                         u32 runs) {
    for (u32 i = 0; i < len; i++) {
        u8 want = before[i];
        for (u32 k = 0; k < runs; k++)
            if (i >= from[k] && i < from[k] + n[k]) want = value[k];
        if (mem[i] != want) {
            fprintf(stderr, "vulkan_headless: logged results: byte %#x is %#x, not %#x\n", i, mem[i], want);
            return 1;
        }
    }
    return 0;
}

static int check_storage_logged(void) {
    const u64 cpu = HEAP_BASE + 0x760000, gva = 0xa400000, a = gva + 0x100, alen = 0x1000;
    static u8 before[0x3000];
    if (vk_uma()) return 0; /* written in place there (uma.c): nothing lands */
    gmmu_map(gva, cpu, 0x3000);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    VkBuffer out, out2;
    u64 at, at2;
    VkSemaphore gate;

    /* the bytes logged land and no others: the result changes [0x10, 0x50) of its binding, its log gives [0x10,
       0x40) and four bytes at 0x800 it wrote with the value they had. Meanwhile the CPU writes a byte at 0x900, which
       nothing logged, and one at 0x800: the GPU's store lands last */
    for (u32 i = 0; i < 0x3000; i++) GP(cpu)[i] = (u8)(i * 7 + 1);
    memcpy(before, GP(cpu), sizeof before);
    CHECK(!logged_write(a, alen, 0x10, 0x40, 0x30, 0x22, &out, &at));
    {
        VkWriteLog* log = vk_wlog_get(1); /* the four bytes at 0x800, by a second writer of the same draw */
        CHECK(log);
        CHECK(vk_buf_copy_writable_logged(a, alen, &out2, &at2, 1, log, 0));
        vk_buf_writable_commit();
        CHECK(out2 == out && at2 == at);
        CHECK(!logged_mark(log, 0x800, 4));
        vk_buf_log_retire(log);
    }
    GP(cpu)[0x100 + 0x900] = 0x77;
    GP(cpu)[0x100 + 0x801] = 0x99;
    vk_finish();
    {
        const u32 from[2] = {0x110, 0xa00}, n[2] = {0x30, 1};
        const u8 value[2] = {0x22, 0x77};
        CHECK(!logged_expect(GP(cpu), before, 0x3000, from, n, value, 2));
    }

    /* two logged writers in two submits, the second writing a suffix of the first's result in place: both land */
    for (u32 i = 0; i < 0x3000; i++) GP(cpu)[i] = (u8)(i * 5 + 3);
    memcpy(before, GP(cpu), sizeof before);
    CHECK(!logged_write(a, alen, 0, 0x10, 0x10, 0x33, &out, &at));
    CHECK(!suffix_gate(&gate));
    CHECK(!logged_write(a + 0x40, alen - 0x40, 0x20, 0x10, 0x10, 0x44, &out2, &at2));
    if (!(0x40 % g_vk.props.limits.minStorageBufferOffsetAlignment)) CHECK(out2 == out && at2 == at + 0x40);
    CHECK(!suffix_open(gate));
    {
        const u32 from[2] = {0x100, 0x160}, n[2] = {0x10, 0x10};
        const u8 value[2] = {0x33, 0x44};
        CHECK(!logged_expect(GP(cpu), before, 0x3000, from, n, value, 2));
    }

    /* a writer that logs nothing over a logged result that is pending: not in place, as the logged one has no
       original bytes to compare with, but composed over it in a result of its own, which lands after it */
    for (u32 i = 0; i < 0x3000; i++) GP(cpu)[i] = (u8)(i * 3 + 5);
    memcpy(before, GP(cpu), sizeof before);
    CHECK(!logged_write(a, alen, 0, 0x10, 0x10, 0x55, &out, &at));
    CHECK(!suffix_gate(&gate));
    CHECK(vk_buf_copy_writable(a, alen, &out2, &at2, 1));
    CHECK(!(out2 == out && at2 == at));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out2, at2 + 0x20, 0x10, 0x66));
    CHECK(!suffix_open(gate));
    {
        const u32 from[2] = {0x100, 0x120}, n[2] = {0x10, 0x10};
        const u8 value[2] = {0x55, 0x66};
        CHECK(!logged_expect(GP(cpu), before, 0x3000, from, n, value, 2));
    }

    /* and joining one being set up, in the same draw: the result takes its original bytes then and compares, so the
       bytes the logging writer changed land too */
    for (u32 i = 0; i < 0x3000; i++) GP(cpu)[i] = (u8)(i * 11 + 7);
    memcpy(before, GP(cpu), sizeof before);
    {
        VkWriteLog* log = vk_wlog_get(1);
        CHECK(log);
        CHECK(vk_buf_copy_writable_logged(a, alen, &out, &at, 1, log, 0));
        CHECK(vk_buf_copy_writable(a, alen, &out2, &at2, 1));
        CHECK(out2 == out && at2 == at);
        vk_buf_writable_commit();
        CHECK(!owned_test_fill(out, at + 0x30, 0x10, 0x88));
        CHECK(!logged_mark(log, 0x30, 8)); /* fewer than it changed: compared, all 16 land */
        vk_buf_log_retire(log);
    }
    vk_finish();
    {
        const u32 from[1] = {0x130}, n[1] = {0x10};
        const u8 value[1] = {0x88};
        CHECK(!logged_expect(GP(cpu), before, 0x3000, from, n, value, 1));
    }

    /* the logs went back to the pool clear: the next one has no window written */
    {
        VkWriteLog* log = vk_wlog_get(1);
        CHECK(log);
        vk_finish();
        u32 bytes = 0;
        CHECK(vk_wlog_runs(log, 0, SH_WLOG_SLOT, logged_count, &bytes) == 0 && bytes == 0);
        vk_wlog_put(log);
    }
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    puts("Logged storage results land the bytes their writers logged and no others, with no original bytes kept");
    return 0;
}
