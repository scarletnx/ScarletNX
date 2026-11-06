/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Measure the indexed-table map's real vk_stage allocation and memcpy path. Mode0 uploads the fixed ten-table
   map, mode1 only active tables. This is CPU preparation time; GPU copies below gate correctness before timing. */
static int bench_indexed_map(int active) {
    CHECK(active == 0 || active == 1);
    static const u32 tables[] = {1, 2, 5, 10};
    const u32 operations = 2000, guard = 64;
    const u32 capacity = SH_INDEXED_TABLES * SNX_TEX_ARRAY;
    const u32 full_bytes = capacity * sizeof(u32);
    u32 map[SH_INDEXED_TABLES * SNX_TEX_ARRAY];
    for (u32 i = 0; i < capacity; i++) map[i] = (i * 0x9e3779b1u) ^ 0x13579bdfu;
    u32 validation = g_vk_validation_messages;
    for (u32 test = 0; test < sizeof tables / sizeof *tables; test++) {
        u32 count = tables[test], bytes = count * SNX_TEX_ARRAY * sizeof(u32);
        u32 upload_bytes = active ? bytes : full_bytes;
        vk_lock();
        VkBuffer staged, readback;
        u64 offset;
        u8* upload = vk_stage(upload_bytes, vk_stage_align(), &staged, &offset);
        CHECK(upload);
        memcpy(upload, map, upload_bytes);
        u8* result = vk_readback_buffer(full_bytes + 2 * guard, &readback);
        CHECK(result);
        VkBuffer initial;
        u64 initial_offset;
        u8* padding = vk_stage(full_bytes + 2 * guard, vk_stage_align(), &initial, &initial_offset);
        CHECK(padding);
        memset(padding, 0xcd, full_bytes + 2 * guard);
        VkCommandBuffer cb = vk_cmd();
        VkBufferCopy initialize = {initial_offset, 0, full_bytes + 2 * guard};
        rec_CmdCopyBuffer(cb, initial, readback, 1, &initialize);
        VkMemoryBarrier2 ordered = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
        VkDependencyInfo barrier = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &ordered};
        rec_CmdPipelineBarrier2(cb, &barrier);
        VkBufferCopy copy = {offset, guard, bytes};
        rec_CmdCopyBuffer(cb, staged, readback, 1, &copy);
        vk_finish();
        vk_readback_ready();
        for (u32 i = 0; i < (full_bytes + 2 * guard) / 4; i++) {
            u32 expected = i >= guard / 4 && i < (guard + bytes) / 4 ? map[i - guard / 4] : 0xcdcdcdcd;
            CHECK(snx_ld_le32(result + i * 4) == expected);
        }
        vk_frame_next();
        u64 elapsed[3] = {0}, staged_bytes[3] = {0};
        for (u32 round = 0; round < 5; round++) {
            /* Prime both frame slots and their ring capacity before measuring. Submission and readback remain
               outside the timed interval, as they are not part of an indexed map's allocation and byte copy. */
            vk_cmd();
            u64 total = 0, begin = plat_ns();
            for (u32 i = 0; i < operations; i++) {
                u8* data = vk_stage(upload_bytes, vk_stage_align(), &staged, &offset);
                CHECK(data);
                memcpy(data, map, upload_bytes);
                total += upload_bytes;
            }
            u64 ns = plat_ns() - begin;
            if (round >= 2) {
                elapsed[round - 2] = ns;
                staged_bytes[round - 2] = total;
            }
            vk_finish();
            vk_frame_next();
        }
        vk_unlock();
        for (u32 i = 0; i < 3; i++)
            for (u32 j = i + 1; j < 3; j++)
                if (elapsed[j] < elapsed[i]) {
                    u64 ns = elapsed[i], uploaded = staged_bytes[i];
                    elapsed[i] = elapsed[j];
                    staged_bytes[i] = staged_bytes[j];
                    elapsed[j] = ns;
                    staged_bytes[j] = uploaded;
                }
        CHECK(elapsed[1] && staged_bytes[1] == (u64)operations * upload_bytes);
        CHECK(g_vk_validation_messages == validation);
        printf("GPU_HOTPATH_BENCH workload=indexed_map_%u_tables operations=%u ns=%llu exact=1 tables=%u bytes=%u "
               "staged_bytes=%llu\n",
               count, operations, (unsigned long long)elapsed[1], count, bytes, (unsigned long long)staged_bytes[1]);
    }
    return 0;
}
