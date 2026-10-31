/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A completed storage result shares a page with an owned range. Acquiring the
   owner must not re-enter frame writeback while that owner is marked landing. */
static int check_owned_readback(void) {
    const u64 cpu = HEAP_BASE + 0x700000, gva = 0xa000000;
    if (!own_traps() || g_vk.uma) return 0; /* with unified memory a result is published in place, owned by nothing */
    gmmu_map(gva, cpu, 0x1000);
    vk_lock();
    vk_finish();
    for (u32 fresh = 0; fresh < 2; fresh++) {
        VkBuffer output;
        u64 offset;
        VkOwnRes* owner = vk_own_res_new(256);
        VkSemaphore gate;
        VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                          .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
        CHECK(owner && VK_OK(vkCreateSemaphore(g_vk.dev, &ci, NULL, &gate)));
        memset(GP(cpu), 0x11, 0x1000);
        CHECK(vk_buf_copy_writable(gva, 256, &output, &offset));
        VkBuffer upload;
        u64 at;
        u8* data = vk_stage_upload(512, 16, &upload, &at);
        CHECK(data);
        memset(data, 0x22, 256);
        memset(data + 256, 0x33, 256);
        VkBufferCopy copy = {at, offset, 256};
        rec_CmdCopyBuffer(vk_cmd(), upload, output, 1, &copy);
        vk_buf_writable_commit();
        copy = (VkBufferCopy){at + 256, 0, 256};
        rec_CmdCopyBuffer(vk_cmd(), upload, owner->buf, 1, &copy);
        /* Hold the submission until submit's automatic completed-frame scan
           has returned. The storage result is then complete but still pending. */
        vk_submit_after(gate, 1, 2);
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                        .semaphore = gate, .value = 1};
        CHECK(VK_OK(vkSignalSemaphore(g_vk.dev, &signal)));
        vk_wait_submit(vk_last_submit());
        vk_own_publish(cpu + 256, 256, owner, 0);
        CHECK(own_lookup(cpu + 256, 256, NULL, 0) == 1);
        if (fresh) vk_frame_next();
        own_acquire(cpu + 256, 256);
        CHECK(own_lookup(cpu + 256, 256, NULL, 0) == 0);
        for (u32 i = 256; i < 512; i++) CHECK(GP(cpu)[i] == 0x33);
        vk_finish();
        for (u32 i = 0; i < 256; i++) CHECK(GP(cpu)[i] == 0x22);
        for (u32 i = 512; i < 4096; i++) CHECK(GP(cpu)[i] == 0x11);
        vk_own_res_unref(owner);
        vkDestroySemaphore(g_vk.dev, gate, NULL);
    }
    vk_unlock();
    puts("Owned readback preserves adjacent pending results in active and fresh frames");
    return 0;
}

static int owned_test_fill(VkBuffer dst, u64 off, u32 len, u8 value) {
    VkBuffer src;
    u64 at;
    u8* data = vk_stage_upload(len, 16, &src, &at);
    CHECK(data);
    memset(data, value, len);
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    VkBufferCopy copy = {at, off, len};
    rec_CmdCopyBuffer(vk_cmd(), src, dst, 1, &copy);
    return 0;
}

/* Binding a GPU-owned input for shader stores must not wait for the CPU baseline.
   The baseline must still distinguish untouched bytes and a later CPU edit. */
static int check_owned_writable(void) {
    const u64 cpu = HEAP_BASE + 0x710000, gva = 0xa100000;
    if (!own_traps() || g_vk.uma) return 0; /* as check_owned_readback */
    gmmu_map(gva, cpu, 0x1000);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    for (u32 edit = 0; edit < 2; edit++) {
        memset(GP(cpu), 0x11, 0x1000);
        VkOwnRes* owner = vk_own_res_new(256);
        CHECK(owner);
        CHECK(!owned_test_fill(owner->buf, 0, 256, 0x22));
        vk_own_publish(cpu + 64, 256, owner, 0);
        CHECK(own_lookup(cpu + 64, 256, NULL, 0) == 1);
        u64 lands = own_landings(), submits = vk_submit_number();
        VkBuffer output;
        u64 at;
        CHECK(vk_buf_copy_writable(gva, 0x1000, &output, &at));
        CHECK(own_landings() == lands && vk_submit_number() == submits);
        vk_buf_writable_commit();
        /* An unchanged second binding reuses the pending buffer without reading
           the GPU baseline or landing the original owner. */
        if (vk_written_since(gmem_host(gva, 0x1000), 0x1000, vk_ww_now()) == 0) {
            VkBuffer reused;
            u64 reused_at;
            CHECK(vk_buf_copy_writable(gva, 0x1000, &reused, &reused_at));
            CHECK(reused == output && reused_at == at);
            CHECK(own_landings() == lands && vk_submit_number() == submits);
        }
        /* A draw changes one owned word and one CPU-backed word. */
        CHECK(!owned_test_fill(output, at, 4, 0x33));
        CHECK(!owned_test_fill(output, at + 64, 8, 0x44));
        if (edit) {
            own_acquire(cpu + 68, 4);
            memset(GP(cpu + 68), 0x55, 4);
        }
        vk_finish();
        own_acquire(cpu, 0x1000);
        for (u32 i = 0; i < 0x1000; i++) {
            u8 expected = i < 4 ? 0x33 : i >= 64 && i < 72 ? 0x44 : i >= 64 && i < 320 ? 0x22 : 0x11;
            if (edit && i >= 68 && i < 72) expected = 0x55;
            CHECK(GP(cpu)[i] == expected);
        }
        vk_own_res_unref(owner);
        vk_frame_next();
    }
    /* Sparse GPU mappings: CPU bytes, a hole, then a separate owned host page. */
    gmmu_map(gva + 0x2000, cpu + 0x4000, 0x1000);
    memset(GP(cpu), 0x11, 0x1000);
    memset(GP(cpu + 0x4000), 0x66, 0x1000);
    VkOwnRes* owner = vk_own_res_new(256);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 256, 0x22));
    vk_own_publish(cpu + 0x4000, 256, owner, 0);
    u64 lands = own_landings(), submits = vk_submit_number(), at;
    VkBuffer output;
    CHECK(vk_buf_copy_writable(gva, 0x3000, &output, &at));
    CHECK(own_landings() == lands && vk_submit_number() == submits);
    vk_buf_writable_commit();
    CHECK(!owned_test_fill(output, at + 0x1000, 4, 0x44));
    CHECK(!owned_test_fill(output, at + 0x2008, 4, 0x33));
    vk_finish();
    own_acquire(cpu + 0x4000, 0x1000);
    for (u32 i = 0; i < 0x1000; i++) {
        CHECK(GP(cpu)[i] == 0x11);
        CHECK(GP(cpu + 0x4000)[i] == (i >= 8 && i < 12 ? 0x33 : i < 256 ? 0x22 : 0x66));
    }
    vk_own_res_unref(owner);
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    puts("GPU-owned writable inputs avoid draw-time landing and preserve later CPU edits");
    return 0;
}
