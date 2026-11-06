/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Writable storage buffers: what a binding costs (vk_buf_writable_stats). No extracted shaders or game data are used */

/* the word at byte at of the buffer as the CPU writes it */
static u32 bufkeep_pattern(u64 at) { return 0xb0f00000u ^ (u32)(at >> 2); }
/* the GPU writes value at buf + off after everything recorded before it, as a draw's store would */
static int bufkeep_store(VkBuffer buf, u64 off, u32 value) {
    VkBuffer src;
    u64 at;
    u8* data = vk_stage_upload(4, 16, &src, &at);
    CHECK(data);
    snx_st_le32(data, value);
    vk_render_end();
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    VkBufferCopy copy = {at, off, 4};
    rec_CmdCopyBuffer(vk_cmd(), src, buf, 1, &copy);
    return 0;
}
/* the writable bindings' costs since was */
typedef struct {
    u64 copied, back, merged;
} BufCount;
static BufCount bufkeep_since(BufCount was) {
    BufCount now;
    vk_buf_writable_stats(&now.copied, &now.back, &now.merged);
    return (BufCount){now.copied - was.copied, now.back - was.back, now.merged - was.merged};
}
/* round i's word in the bench's buffer: each round a word of its own */
static u64 bufkeep_at(u32 i) { return (u64)(i & 63) * 0x10000 + (u64)i * 4; }

/* --bench-storage-buffer: a writable binding of a 4 MB storage buffer, a word of it written on the GPU as a draw's
   store, and the frame's fence (vk_finish), rounds times, each round's word read by the CPU at the end. Times the
   rounds, and counts the bytes copied for the bindings before their stores and back after them, and the results
   merged */
static int bench_storage_buffer(void) {
    const u64 gva = 0x4fc000000ull, cpu = HEAP_BASE + 0xb000000, size = 4u << 20;
    const u32 rounds = 64;
    VkBuffer buf;
    u64 off;
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0); /* past the heap */
    gmmu_map(gva, cpu, size);
    for (u64 i = 0; i < size; i += 4) snx_st_le32(GP(cpu + i), bufkeep_pattern(i));
    vk_lock();
    vk_finish();
    /* round 0 outside the time: the rings grown */
    CHECK(vk_buf_copy_writable(gva, size, &buf, &off, 1));
    vk_buf_writable_commit();
    CHECK(!bufkeep_store(buf, off + bufkeep_at(0), 0x5eed0000));
    vk_finish();
    BufCount was = bufkeep_since((BufCount){0});
    u64 ns = 0;
    for (u32 i = 1; i <= rounds; i++) {
        u64 t = plat_ns();
        CHECK(vk_buf_copy_writable(gva, size, &buf, &off, 1));
        vk_buf_writable_commit();
        CHECK(!bufkeep_store(buf, off + bufkeep_at(i), 0x5eed0000 + i));
        vk_finish();
        ns += plat_ns() - t;
    }
    BufCount d = bufkeep_since(was);
    vk_unlock();
    u8 word[4];
    for (u32 i = 0; i <= rounds; i++) {
        CHECK(!gmem_read(gva + bufkeep_at(i), word, 4));
        if (snx_ld_le32(word) != 0x5eed0000 + i)
            fprintf(stderr, "storage buffer bench: round %u's word holds %08x, expected %08x\n", i, snx_ld_le32(word),
                    0x5eed0000 + i);
        CHECK(snx_ld_le32(word) == 0x5eed0000 + i);
    }
    CHECK(!gmem_read(gva + 8, word, 4));
    CHECK(snx_ld_le32(word) == bufkeep_pattern(8));
    printf("GPU_HOTPATH_BENCH workload=storage_buffer_draws operations=%u ns=%llu exact=1 bytes=%llu copied_bytes=%llu "
           "read_back_bytes=%llu merged=%llu\n",
           rounds, (unsigned long long)ns, (unsigned long long)size, (unsigned long long)d.copied,
           (unsigned long long)d.back, (unsigned long long)d.merged);
    return 0;
}
