/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Storage results over one guest range across submits, held pending by a gate on the first submit: a later submit's
   draw writes a suffix of a result in place, a read of the whole range takes the newest result where it is though a
   neighbour's result shares its last page, and a composed result isn't moved past a later result that took its
   bytes (it would land over them) */
static int suffix_gate(VkSemaphore* gate) {
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    CHECK(VK_OK(vkCreateSemaphore(g_vk.dev, &ci, NULL, gate)));
    vk_submit_after(*gate, 1, 2);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    return 0;
}
static int suffix_open(VkSemaphore gate) {
    VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = gate, .value = 1};
    CHECK(VK_OK(vkSignalSemaphore(g_vk.dev, &signal)));
    vk_finish();
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    return 0;
}
/* guest memory as it was (before) with the runs [at, at + 16) of each fill */
static int suffix_expect(const u8* mem, const u8* before, u32 len, const u32* at, const u8* value, u32 n) {
    for (u32 i = 0; i < len; i++) {
        u8 want = before[i];
        for (u32 k = 0; k < n; k++)
            if (i >= at[k] && i < at[k] + 16) want = value[k];
        if (mem[i] != want) {
            fprintf(stderr, "vulkan_headless: byte %#x is %#x, not %#x\n", i, mem[i], want);
            return 1;
        }
    }
    return 0;
}

static int check_storage_suffix(void) {
    const u64 cpu = HEAP_BASE + 0x720000, gva = 0xa200000, a = gva + 0x100, alen = 0x1000;
    const u64 sa = g_vk.props.limits.minStorageBufferOffsetAlignment;
    static u8 before[0x3000];
    gmmu_map(gva, cpu, 0x3000);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    for (u32 i = 0; i < 0x3000; i++) GP(cpu)[i] = (u8)(i * 7 + 1);
    memcpy(before, GP(cpu), sizeof before);

    /* a range's result, in a submit held back */
    VkBuffer out_a, out_s, out_b, rd;
    u64 at_a, at_s, at_b, rd_at;
    VkSemaphore gate;
    CHECK(vk_buf_copy_writable(a, alen, &out_a, &at_a));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out_a, at_a, 16, 0x22));
    CHECK(!suffix_gate(&gate));
    /* the next submit writes a suffix of it: the same buffer, landing after this submit */
    u64 submit = vk_submit_number();
    CHECK(vk_buf_copy_writable(a + 0x40, alen - 0x40, &out_s, &at_s));
    CHECK(vk_submit_number() == submit);
    if (!(0x40 % sa)) CHECK(out_s == out_a && at_s == at_a + 0x40);
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out_s, at_s, 16, 0x33));
    /* a neighbour's result after it, on its last page */
    CHECK(vk_buf_copy_writable(a + alen, 0x100, &out_b, &at_b));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out_b, at_b, 16, 0x44));
    /* the whole range, read: the result covering it is bound where it is, the neighbour gives it no byte */
    CHECK(vk_buf_get_input(a, alen, sa, &rd, &rd_at));
    CHECK(rd == out_a && rd_at == at_a);
    CHECK(vk_submit_number() == submit);
    CHECK(!suffix_open(gate));
    const u32 at[3] = {0x100, 0x140, 0x1100};
    const u8 value[3] = {0x22, 0x33, 0x44};
    CHECK(!suffix_expect(GP(cpu), before, 0x3000, at, value, 3));

    /* A composed result whose bytes a later result took stays where it was recorded */
    for (u32 i = 0; i < 0x3000; i++) GP(cpu)[i] = (u8)(i * 5 + 3);
    memcpy(before, GP(cpu), sizeof before);
    VkBuffer out_m0, out_m, out_r, out_n;
    u64 at_m0, at_m, at_r, at_n;
    CHECK(vk_buf_copy_writable(a + 0x40, alen - 0x40, &out_m0, &at_m0));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out_m0, at_m0, 16, 0x11));
    CHECK(!suffix_gate(&gate));
    /* the whole range over the suffix's result: composed */
    CHECK(vk_buf_copy_writable(a, alen, &out_m, &at_m));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out_m, at_m, 16, 0x22));
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    /* a range over its first bytes and the ones before: composed over it, a new value in those first bytes */
    CHECK(vk_buf_copy_writable(a - 0x10, 0x20, &out_r, &at_r));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out_r, at_r + 0x10, 16, 0x55));
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    /* a suffix of the composed result again: written into a new result, which lands after both */
    CHECK(vk_buf_copy_writable(a + 0x80, alen - 0x80, &out_n, &at_n));
    CHECK(!(out_n == out_m && at_n == at_m + 0x80));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out_n, at_n, 16, 0x66));
    CHECK(!suffix_open(gate));
    const u32 at2[3] = {0x100, 0x140, 0x180};
    const u8 value2[3] = {0x55, 0x11, 0x66};
    CHECK(!suffix_expect(GP(cpu), before, 0x3000, at2, value2, 3));
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    puts("Storage results are written again in place from a later submit, and read where they are");
    return 0;
}
