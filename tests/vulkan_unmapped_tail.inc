/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A storage result whose range ends on an unmapped GPU page (1872 bytes at page offset 0xa00, 336 of them past the
   mapped page), bound again by a later draw, writable and then read: neither the guest nor the GPU changed a byte of
   it, so neither binding lands it (copyable_span: a span on a page the result doesn't map counts as unchanged). Its
   bytes land on the mapped page at the end, and none past them */
static int check_unmapped_tail(void) {
    const u64 gva = 0x71000000ull, cpu = HEAP_BASE + 0x13000000, off = 0xa00, len = 1872;
    if (vk_uma()) {
        puts("Storage result over an unmapped page: none to land with unified memory (bindings write guest memory)");
        return 0;
    }
    CHECK(!mem_map(cpu, 0x1000, MS_NORMAL, PERM_RW));
    gmmu_map(gva, cpu, 4096); /* the GPU page after it stays unmapped */
    memset(GP(cpu), 0x11, 4096);
    vk_lock();
    vk_finish();
    VkBuffer out, buf;
    u64 at, bat;
    CHECK(vk_buf_copy_writable(gva + off, len, &out, &at, 1));
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(out, at, len, 0x5a));
    u64 finishes = g_vk_finishes;
    CHECK(vk_buf_copy_writable(gva + off, len, &buf, &bat, 1));
    vk_buf_writable_commit();
    CHECK(vk_buf_get_input(gva + off, len, 1, &buf, &bat));
    CHECK(g_vk_finishes == finishes);
    vk_finish();
    vk_unlock();
    for (u32 j = 0; j < 4096; j++) CHECK(GP(cpu)[j] == (j >= off ? 0x5a : 0x11));
    puts("A storage result over an unmapped page is bound again with no landing");
    return 0;
}
