/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Check actual ring selection and GPU-observed bytes. Device-local input must stay on the GPU without landing;
   ordinary CPU input must use the stream's host memory (the upload ring with vk_stream_ring=0) rather than the mapped
   device-memory window. */
static int guest_staging_read(u64 gva, u32 bytes, int gpu, u8 value) {
    VkBuffer expected, staged, readback;
    u64 expected_at, at;
    u8* probe = gpu ? vk_stage(16, vk_stage_align(), &expected, &expected_at)
                    : vk_stream_upload(16, vk_stage_align(), &expected, &expected_at);
    CHECK(probe);
    u8* input = vk_stage_guest_input(gva, bytes, bytes, &staged, &at);
    CHECK(input && staged == expected && at > expected_at);
    u8* result = vk_readback_buffer(bytes, &readback);
    CHECK(result);
    vk_guest_read(gva, bytes, input, staged, at);
    VkBufferCopy copy = {at, 0, bytes};
    rec_CmdCopyBuffer(vk_cmd(), staged, readback, 1, &copy);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < bytes; i++) CHECK(result[i] == value);
    return 0;
}

static int check_guest_staging(void) {
    const u64 cpu = HEAP_BASE + 0x18000000, gva = 0x8e0000000ull, span = 0x200000;
    CHECK(!mem_map(cpu, span * 2, MS_NORMAL, PERM_RW));
    gmmu_map(gva, cpu, span * 2);
    vk_lock();
    vk_finish();
    vk_frame_next();
    memset(GP(cpu), 0x11, 0x1000);
    CHECK(!guest_staging_read(gva + 64, 256, 0, 0x11));

    if (own_traps()) {
        vk_frame_next();
        VkOwnRes* owner = vk_own_res_new(256, VK_OWN_BY_DRAW);
        CHECK(owner && !owned_test_fill(owner->buf, 0, 256, 0x22));
        vk_own_publish_owned(cpu + 64, 256, owner, 0);
        CHECK(own_lookup(cpu + 64, 256, NULL, 0) == 1);
        u64 lands = own_landings();
        CHECK(!guest_staging_read(gva + 64, 256, 1, 0x22));
        CHECK(own_landings() == lands);
        own_acquire(cpu, 0x1000);
        vk_own_res_unref(owner);
    } else
        puts("guest staging: GPU ownership route skipped without memory traps");

    if (vk_uma()) {
        vk_frame_next();
        memset(GP(cpu + span), 0x33, 0x1000);
        VkBuffer output;
        u64 at;
        CHECK(vk_buf_copy_writable(gva + span, 256, &output, &at, 1));
        CHECK(!owned_test_fill(output, at, 256, 0x44));
        vk_buf_writable_commit();
        if (vk_uma_pending(gva + span, 256))
            CHECK(!guest_staging_read(gva + span, 256, 1, 0x44));
        else {
            puts("guest staging: pending in-place route skipped because this allocation was copied");
            vk_finish();
        }
    } else
        puts("guest staging: pending in-place route skipped with copied guest memory");
    vk_unlock();
    CHECK(!mem_unmap(cpu, span * 2));
    puts("Guest input staging selects the CPU/GPU rings and preserves exact bytes");
    return 0;
}

/* The stream's chunks are no frame's (cmd.c's vk_stream_upload): one is handed out again once the submission after
   the one that read its bytes is done, and not before. Two chunks' worth with no submission between them are two
   buffers. After two submissions both are handed out again, and no buffer is made */
static int check_stream_ring(void) {
    if (!g_settings.stream_ring) {
        puts("stream ring: skipped with vk_stream_ring=0");
        return 0;
    }
    const u64 chunk = vk_stage_chunk();
    VkBuffer first, second, again[2];
    u64 off;
    vk_lock();
    vk_finish();
    vk_frame_next();
    const u64 recorded = vk_submit_number();
    CHECK(vk_stream_upload(chunk, 16, &first, &off) && !off);
    CHECK(vk_stream_upload(chunk, 16, &second, &off) && !off && second != first);
    vk_finish(); /* the submission that read them, then the one after it */
    vk_cmd();
    vk_finish();
    CHECK(vk_submit_number() >= recorded + 2);
    const u64 made = vk_ring_made();
    CHECK(vk_stream_upload(chunk, 16, &again[0], &off) && !off);
    CHECK(vk_stream_upload(chunk, 16, &again[1], &off) && !off);
    const int same = (again[0] == first && again[1] == second) || (again[0] == second && again[1] == first);
    const u64 made_after = vk_ring_made();
    vk_finish();
    vk_unlock();
    if (!same || made_after != made) {
        fprintf(stderr,
                "stream ring: two chunks handed out after their work was done are %s the two from before, and %llu "
                "buffers were made for them (none expected)\n",
                same ? "" : "not", (unsigned long long)(made_after - made));
        return 1;
    }
    puts("stream ring: two chunks busy at once are two buffers, and both are handed out again once their work is done");
    return 0;
}
