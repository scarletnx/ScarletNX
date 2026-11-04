/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU mirror of guest memory (mirror.c): a range bound into it holds guest memory's bytes; a byte the CPU changes
   since goes in when the range is bound again, and a byte the GPU wrote there and the CPU didn't stays */

/* len bytes of the mirror at cpu, read back once the work recorded so far is done */
static int mirror_read(u64 cpu, u32 len, u8* out) {
    VkBuffer back;
    u64 at;
    VmaAllocation mem;
    u8* p = vk_stage_back(len, &back, &at, &mem);
    CHECK(p);
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vk_render_end();
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    VkBufferCopy copy = {cpu, at, len};
    rec_CmdCopyBuffer(vk_cmd(), vk_mirror_buffer(), back, 1, &copy);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    vk_finish();
    CHECK(VK_OK(vmaInvalidateAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE)));
    memcpy(out, p, len);
    return 0;
}
/* the mirror's bytes against guest memory's, but [from, from + n), which hold value */
static int mirror_expect(const u8* got, const u8* guest, u32 len, u32 from, u32 n, u8 value) {
    for (u32 i = 0; i < len; i++) {
        u8 want = i >= from && i < from + n ? value : guest[i];
        if (got[i] != want) {
            fprintf(stderr, "vulkan_headless: mirror: byte %#x is %#x, not %#x\n", i, got[i], want);
            return 1;
        }
    }
    return 0;
}

/* a program's write of [cpu, cpu + n) of the mirror, value in its bytes and their bits in the mask, as its stores
   would leave them (logged 0: the bytes alone, which no store of a logging program leaves) */
static int mirror_write(u64 cpu, u32 n, u8 value, int logged) {
    CHECK(!owned_test_fill(vk_mirror_buffer(), cpu, n, value));
    if (!logged) return 0;
    u64 w0 = cpu / 32, w1 = (cpu + n + 31) / 32;
    VkBuffer src;
    u64 at;
    u32* m = (u32*)(void*)vk_stage_upload((w1 - w0) * 4, 16, &src, &at);
    CHECK(m);
    memset(m, 0, (w1 - w0) * 4);
    for (u64 b = cpu; b < cpu + n; b++) m[b / 32 - w0] |= 1u << (b % 32);
    VkBufferCopy copy = {at, w0 * 4, (w1 - w0) * 4};
    rec_CmdCopyBuffer(vk_cmd(), src, vk_mirror_mask(), 1, &copy);
    return 0;
}

static int check_mirror(void) {
    const u64 cpu = HEAP_BASE + 0x7c0000, gva = 0xa600000, len = 0x3000;
    static u8 got[0x3000];
    if (!vk_mirror_on()) {
        puts("The GPU mirror is off on this device: not checked");
        return 0;
    }
    gmmu_map(gva, cpu, len);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    for (u32 i = 0; i < len; i++) GP(cpu)[i] = (u8)(i * 7 + 3);
    vk_ww_new_epoch();
    VkBuffer buf;
    u64 off;
    /* bound: guest memory's bytes, at the guest address */
    CHECK(vk_mirror_slice(gva, len, &buf, &off));
    CHECK(buf == vk_mirror_buffer() && off == cpu);
    CHECK(!mirror_read(cpu, len, got));
    CHECK(!mirror_expect(got, GP(cpu), len, 0, 0, 0));
    /* the GPU writes 16 bytes at 0x100 (as a program's stores would); the CPU writes the byte after them and one
       on another page: bound again, the CPU's bytes go in and the GPU's stay */
    CHECK(!owned_test_fill(vk_mirror_buffer(), cpu + 0x100, 16, 0x77));
    vk_finish();
    GP(cpu)[0x110] = 0x5a;
    GP(cpu)[0x2003] = 0xa5;
    vk_ww_new_epoch();
    CHECK(vk_mirror_slice(gva, len, &buf, &off));
    CHECK(!mirror_read(cpu, len, got));
    CHECK(!mirror_expect(got, GP(cpu), len, 0x100, 16, 0x77));
    /* a range over the same guest memory at another GPU address is the same bytes */
    gmmu_map(gva + 0x100000, cpu + 0x1000, 0x1000);
    CHECK(vk_mirror_slice(gva + 0x100000, 0x1000, &buf, &off));
    CHECK(off == cpu + 0x1000);

    /* written by a program: pending, then at the submit its logged bytes land in guest memory, and no others (the 8
       bytes at 0x400, changed without being logged), and the byte the CPU wrote beside them meanwhile stays */
    memcpy(got, GP(cpu), len);
    CHECK(!mirror_write(cpu + 0x200, 24, 0x66, 1));
    CHECK(!mirror_write(cpu + 0x400, 8, 0x99, 0));
    vk_mirror_wrote(cpu, len);
    CHECK(vk_mirror_pending(gva, len) && !vk_mirror_pending(gva + 0x10000, 0x100));
    GP(cpu)[0x219] = 0x11;
    got[0x219] = 0x11;
    vk_finish();
    CHECK(!vk_mirror_pending(gva, len));
    for (u32 i = 0; i < len; i++) {
        u8 want = i >= 0x200 && i < 0x218 ? 0x66 : got[i];
        if (GP(cpu)[i] != want) {
            fprintf(stderr, "vulkan_headless: mirror: landed byte %#x is %#x, not %#x\n", i, GP(cpu)[i], want);
            CHECK(0);
        }
    }
    /* bound again: what landed is in the shadow, so nothing is copied over the program's bytes */
    vk_ww_new_epoch();
    CHECK(vk_mirror_slice(gva, len, &buf, &off));
    {
        static u8 m[0x3000];
        CHECK(!mirror_read(cpu, len, m));
        CHECK(m[0x200] == 0x66 && m[0x217] == 0x66 && m[0x219] == 0x11 && m[0x400] == 0x99);
    }

    /* more pages written than the harvest's room (64 at first): those past it land by a pass of their own */
    {
        const u64 c2 = HEAP_BASE + 0x5000000, g2 = 0xa700000, l2 = 0x100000; /* past the heap */
        CHECK(mem_map(c2, l2, MS_NORMAL, PERM_RW) == 0);
        gmmu_map(g2, c2, l2);
        for (u32 i = 0; i < l2; i++) GP(c2)[i] = (u8)(i * 5);
        vk_ww_new_epoch();
        CHECK(vk_mirror_slice(g2, l2, &buf, &off));
        for (u32 pg = 0; pg < 100; pg++) CHECK(!mirror_write(c2 + pg * 4096 + pg, 1, 0xe0 | (pg & 15), 1));
        vk_mirror_wrote(c2, l2);
        u64 gr, hv, pages, landed, overflows0, overflows;
        u32 room;
        vk_mirror_stats(&gr, &hv, &pages, &landed, &overflows0, &room);
        vk_finish();
        vk_mirror_stats(&gr, &hv, &pages, &landed, &overflows, &room);
        CHECK(overflows == overflows0 + 1 && room >= 128); /* past its room once, and more room from then on */
        for (u32 i = 0; i < l2; i++) {
            u32 pg = i / 4096;
            u8 want = pg < 100 && i % 4096 == pg ? (u8)(0xe0 | (pg & 15)) : (u8)(i * 5);
            if (GP(c2)[i] != want) {
                fprintf(stderr, "vulkan_headless: mirror: landed byte %#x of the 100 pages is %#x, not %#x\n", i,
                        GP(c2)[i], want);
                CHECK(0);
            }
        }
    }
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    puts("The GPU mirror takes the bytes the CPU changed and keeps those the GPU wrote; the bytes its programs logged "
         "land, and no others");
    return 0;
}
