/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One vertex per draw writes one word. The storage views overlap, but the written words do not. NVN permits the
   draws to overlap until the explicit execution/data barrier. No extracted shaders or game data are used. */
static int nvn_storage_batch(GpuChannel* channel, u64 cbuf, u64 stored, u64 cpu, u32 draws, u32 stride,
                             u64* elapsed, u64* passes) {
    u32 bytes = draws * stride;
    u8* initial = malloc(bytes);
    CHECK(initial);
    memset(initial, 0xcd, bytes);
    CHECK(!gmem_write(stored, initial, bytes));
    vk_lock();
    vk_finish();
    u64 pass0 = g_vk_passes, draw0 = g_vk_drawn, skipped0 = g_vk_skipped;
    u64 start = plat_ns();
    for (u32 i = 0; i < draws; i++) {
        u64 at = stored + (u64)i * stride;
        u32 desc[3] = {(u32)at, (u32)(at >> 32), bytes - i * stride};
        CHECK(!storage_write_words(cbuf + 0x510, desc, 3));
        DrawCall call = {.topology = 0, .count = 1, .instances = 1};
        vk_render_draw(&channel->ds, &call, &channel->owner);
    }
    vk_unlock();
    /* This is the dependency declared by the front end, not an inferred barrier for every overlapping view. */
    if (g_settings.nvn_storage_ordering) {
        g_renderer->method_barrier(6);
        g_renderer->method_barrier(7);
    }
    vk_lock();
    vk_finish();
    *elapsed = plat_ns() - start;
    *passes = g_vk_passes - pass0;
    CHECK(g_vk_drawn == draw0 + draws && g_vk_skipped == skipped0);
    vk_unlock();
    for (u32 i = 0; i < draws; i++) snx_st_le32(initial + i * stride, 0x5eed);
    CHECK(!memcmp(GP(cpu + 0x10000), initial, bytes));
    free(initial);
    return 0;
}

static int bench_nvn_storage(int ordering, u32 draws, u32 rounds) {
    int saved_ordering = g_settings.nvn_storage_ordering;
    g_settings.nvn_storage_ordering = ordering;
    const u64 address = 0x4a0000000ull, cpu = HEAP_BASE + 0x780000;
    const u64 positions = address + 0x2000, cbuf = address + 0x3000, stored = address + 0x10000;
    const u64 vertex[] = {0x4c98078014470000ull, 0x4c98078014570001ull, storage_mov(2, 0x5eed),
                          0xeedc200000070002ull, 0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull,
                          0xe30000000007000full};
    const u64 fragment[] = {0xe30000000007000full};
    const float position[4] = {0, 0, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 124;
    u64 align = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (align < 16) align = 16;
    CHECK(align <= 256 && draws && draws <= 512 && rounds && rounds <= 20);
    gmmu_map(address, cpu, 0x80000);
    CHECK(!gmem_write(positions, position, sizeof position));
    CHECK(mesh_program(address, vertex, 7, 0));
    CHECK(mesh_program(address + 0x1000, fragment, 1, 1));
    g_renderer->method_barrier(5); /* The preceding case can leave a different program at this address. */
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32); r[0x583] = (u32)address;
    r[0x850] = 1; r[0x851] = 0x1000; r[0x64b] = 1;
    r[0x286] = 0x6420; r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 0;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(positions >> 32); r[0x702] = (u32)positions;
    r[0x7c0] = (u32)((positions + 15) >> 32); r[0x7c1] = (u32)(positions + 15);
    r[0x487] = 0x0fac6884;
    r[0x54e] = 0;
    channel->ds.bind.cb_addr[0][0] = cbuf;
    channel->ds.bind.cb_size[0][0] = 0x1000;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    channel->ds.owner = (ordering ? 0x80000000u : 0) | channel->id;
    u32 validation = g_vk_validation_messages;
    u64 elapsed, passes;
    for (u32 i = 0; i < 2; i++) CHECK(!nvn_storage_batch(channel, cbuf, stored, cpu, draws, (u32)align, &elapsed, &passes));
    u64 ns = 0, npass = 0;
    for (u32 i = 0; i < rounds; i++) {
        CHECK(!nvn_storage_batch(channel, cbuf, stored, cpu, draws, (u32)align, &elapsed, &passes));
        ns += elapsed; npass += passes;
    }
    if (ordering) {
        /* A later draw reads a result that is still queued on the GPU. No host wait can supply the dependency. */
        u32 desc[3] = {(u32)stored, (u32)(stored >> 32), 32};
        CHECK(!storage_write_words(stored, (const u32[8]){0}, 8));
        CHECK(!storage_write_words(cbuf + 0x510, desc, 3));
        DrawCall call = {.topology = 0, .count = 1, .instances = 1};
        vk_lock();
        vk_render_draw(&channel->ds, &call, &channel->owner);
        vk_unlock();
        g_renderer->method_barrier(6);
        g_renderer->method_barrier(7);
        u64 reader[7];
        memcpy(reader, vertex, sizeof reader);
        reader[2] = 0xeed4200000070002ull; /* LDG.E R2, [R0] */
        reader[3] = 0xeedc200000470002ull; /* STG.E [R0 + 4], R2 */
        CHECK(mesh_program(address, reader, 7, 0));
        g_renderer->method_barrier(5);
        vk_lock();
        vk_render_draw(&channel->ds, &call, &channel->owner);
        vk_finish();
        vk_unlock();
        for (u32 i = 0; i < 8; i++) CHECK(snx_ld_le32(GP(cpu + 0x10000 + 4 * i)) == (i < 2 ? 0x5eed : 0));
        /* A read of a pending output followed by an in-place overwrite also needs execution order, even when
           the earlier writer is already visible. Capture the old word before the overwrite without a host wait. */
        vk_lock();
        VkBuffer output, input, snapshot;
        u64 at, input_at;
        CHECK(vk_buf_copy_writable(stored, 32, &output, &at, 0));
        CHECK(!owned_test_fill(output, at, 4, 0x22));
        vk_buf_writable_commit();
        vk_unlock();
        g_renderer->method_barrier(6);
        vk_lock();
        CHECK(vk_buf_get_input(stored, 32, g_vk.props.limits.minStorageBufferOffsetAlignment, &input, &input_at));
        CHECK(vk_buf_order_pending(0) && !vk_buf_order_pending(1));
        u8* old_word = vk_readback_buffer(4, &snapshot);
        CHECK(old_word);
        /* get_input supplies graphics visibility. This test's transfer reader needs its own COPY scope. */
        VkMemoryBarrier2 visible = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo visibility = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .memoryBarrierCount = 1, .pMemoryBarriers = &visible};
        rec_CmdPipelineBarrier2(vk_cmd(), &visibility);
        VkBufferCopy copy = {input_at, 0, 4};
        rec_CmdCopyBuffer(vk_cmd(), input, snapshot, 1, &copy);
        vk_unlock();
        g_renderer->method_barrier(6);
        /* Use a real shader overwrite. The fill helper adds its own barrier and would hide a missing WAR order. */
        u64 overwrite[7];
        memcpy(overwrite, vertex, sizeof overwrite);
        overwrite[2] = storage_mov(2, 0x33333333);
        CHECK(mesh_program(address, overwrite, 7, 0));
        g_renderer->method_barrier(5);
        vk_lock();
        vk_render_draw(&channel->ds, &call, &channel->owner);
        vk_finish();
        vk_readback_ready();
        CHECK(snx_ld_le32(old_word) == 0x22222222 && snx_ld_le32(GP(cpu + 0x10000)) == 0x33333333);
        vk_unlock();
    }
    CHECK(g_vk_validation_messages == validation);
    printf("NVN_DRAW_BENCH ordering=%d descriptors=%d record=%d draws=%u rounds=%u ns=%llu passes=%llu exact=1\n",
           ordering, g_vk.has_descriptor_buffers != 0, g_settings.vulkan_recording_thread, draws, rounds,
           (unsigned long long)ns, (unsigned long long)npass);
    g_settings.nvn_storage_ordering = saved_ordering;
    free(channel);
    return 0;
}
