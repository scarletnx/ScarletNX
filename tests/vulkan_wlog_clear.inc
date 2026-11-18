/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A write log in host memory is clear again when it goes back to its pool (writelog.c vk_wlog_unref): each window a
   store flagged, and its byte mask. The unref clears the windows that vk_wlog_runs found
   flagged, and reads the flags past the windows it read by their coarse words. A flag there comes from a store past
   the range its reader reads (write_log.c clamps the window, and does not bound it by the binding), or from a part no
   reader read. Here the GPU sets the flags and their coarse words (a fill of 4 bytes each), as a dispatch's stores
   do. The next user of the log, which reads all 16 MiB of each part, must find no window of the user before. The
   unref reads only the flags of the groups of 64 with a coarse word that the reads left */
typedef struct {
    u32 n;
    u32 run[8][2];
} WlogRuns;
static void wlog_run_note(void* user, u32 from, u32 to) {
    WlogRuns* r = user;
    if (r->n < 8) {
        r->run[r->n][0] = from;
        r->run[r->n][1] = to;
    }
    r->n++;
}
/* window w of part p flagged, with bits in mask word k (bytes w * 4096 + k * 32 up) */
static void wlog_flag(VkWriteLog* l, u32 p, u32 w, u32 k, u32 bits) {
    const u64 base = (u64)p * SH_WLOG_PART * 4;
    rec_CmdFillBuffer(vk_cmd(), vk_wlog_buffer(l), base + 4ull * w, 4, 1);
    rec_CmdFillBuffer(vk_cmd(), vk_wlog_buffer(l), base + 4ull * (SH_WPAGE_SLOT * (1ull + SH_WLOG_MASK) + w / 64), 4, 1);
    rec_CmdFillBuffer(vk_cmd(), vk_wlog_buffer(l), base + 4ull * (SH_WPAGE_SLOT + (u64)w * SH_WLOG_MASK + k), 4, bits);
}
static int check_wlog_clear(void) {
    vk_lock();
    vk_finish();
    vk_render_end();
    VkWriteLog* l = vk_wlog_get(2);
    CHECK(l && !vk_wlog_on_gpu(l));
    /* part 0: windows 0, 100 and 3000 in the range its reader reads (3001 windows), 4000 past it; part 1: unread */
    wlog_flag(l, 0, 0, 0, 0xF);
    wlog_flag(l, 0, 100, 1, 0xFF);
    wlog_flag(l, 0, 3000, 127, 0x80000000u);
    wlog_flag(l, 0, 4000, 2, 0x3);
    wlog_flag(l, 1, 5, 0, 0x1);
    VkMemoryBarrier2 host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                             .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                             .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                             .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                            .pMemoryBarriers = &host};
    rec_CmdPipelineBarrier2(vk_cmd(), &dep);
    vk_finish();
    WlogRuns r = {0};
    const u32 seen = vk_wlog_runs(l, 0, 3001u * 4096, wlog_run_note, &r);
    int failed = seen != 3 || r.n != 3 || r.run[0][0] != 0 || r.run[0][1] != 4 || r.run[1][0] != 100u * 4096 + 32 ||
                 r.run[1][1] != 100u * 4096 + 40 || r.run[2][0] != 3000u * 4096 + 4095 ||
                 r.run[2][1] != 3000u * 4096 + 4096;
    if (failed)
        fprintf(stderr, "write log clear: the first reader found %u windows and %u runs, where 3 of each were set\n",
                seen, r.n);
    u64 unrefs0, scanned0, cleared0, unrefs, scanned, cleared;
    vk_wlog_clear_stats(&unrefs0, &scanned0, &cleared0);
    vk_wlog_unref(l);
    vk_wlog_clear_stats(&unrefs, &scanned, &cleared);
    /* the groups past the 3001 windows read: 2944 to 3007 and 3968 to 4031 of part 0, and 0 to 63 of part 1 */
    failed |= unrefs != unrefs0 + 1 || cleared != cleared0 + 5 || scanned != scanned0 + 3 * 64;
    if (cleared != cleared0 + 5 || scanned != scanned0 + 3 * 64)
        fprintf(stderr, "write log clear: the unref cleared %llu windows, where 5 were flagged, and read %llu flags, "
                        "where 192 were in groups with a store\n",
                (unsigned long long)(cleared - cleared0), (unsigned long long)(scanned - scanned0));
    /* the pool gives the same log again, newest first: every window of both parts is clear */
    VkWriteLog* again = vk_wlog_get(2);
    CHECK(again == l);
    for (u32 p = 0; p < 2; p++) {
        r = (WlogRuns){0};
        const u32 left = vk_wlog_runs(again, p, SH_WPAGE_SLOT * 4096u, wlog_run_note, &r);
        if (left || r.n) {
            fprintf(stderr, "write log clear: part %u of the log given again has %u windows flagged (%u runs, the "
                            "first at %u)\n", p, left, r.n, r.n ? r.run[0][0] : 0);
            failed = 1;
        }
    }
    vk_wlog_unref(again);
    vk_unlock();
    CHECK(!failed);
    printf("write logs: a log in host memory comes back from its pool clear, the windows its reader read and those "
           "past them\n");
    return 0;
}
