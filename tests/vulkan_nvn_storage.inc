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
    all_samples(r);
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

/* ------------------------------------------------------------ two global buffers over one memory, in real programs */
#define ALIAS_GCODE   0xc50000000ull /* the programs, the position and constant buffer 0 */
#define ALIAS_CODE    (HEAP_BASE + 0x31800000)
#define ALIAS_GDATA   0xc60000000ull /* the storage: a window of its own, which the runtime never wrote */
#define ALIAS_DATA    (HEAP_BASE + 0x31a00000)
#define ALIAS_CBUF    (ALIAS_GCODE + 0x3000)
#define ALIAS_STORED  (ALIAS_GDATA + 0x1000)
#define ALIAS_MOV_R0  0x4c98078014470000ull /* MOV R0, c[0][0x510]: buffer 0's address, NVN's descriptor */
#define ALIAS_MOV_R1  0x4c98078014570001ull /* MOV R1, c[0][0x514] */
#define ALIAS_MOV_R4  0x4c98078014870004ull /* MOV R4, c[0][0x520]: buffer 1's */
#define ALIAS_MOV_R5  0x4c98078014970005ull /* MOV R5, c[0][0x524] */
#define ALIAS_ALD     0xefd9ff800807ff00ull /* the position in, and out */
#define ALIAS_AST     0xeff1ff800707ff00ull
#define ALIAS_EXIT    0xe30000000007000full

/* a channel that draws one point of the vertex program at ALIAS_GCODE, nothing rasterized: buffer 0 is
   [ALIAS_STORED, +0x100), buffer 1 [ALIAS_STORED - 0x100, +0x200). ordering: NVN's order of storage (method_barrier 6
   and 7 between draws), else Maxwell's */
static int alias_channel(GpuChannel* channel, int ordering) {
    const u64 positions = ALIAS_GCODE + 0x2000;
    const u64 fragment[] = {ALIAS_EXIT};
    const float position[4] = {0, 0, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    const u32 desc[8] = {(u32)ALIAS_STORED,         (u32)(ALIAS_STORED >> 32), 0x100, 0,
                         (u32)(ALIAS_STORED - 0x100), (u32)(ALIAS_STORED >> 32), 0x200, 0};
    channel->id = 125;
    CHECK(mem_map(ALIAS_CODE, 0x200000, MS_NORMAL, PERM_RW) == 0);
    CHECK(mem_map(ALIAS_DATA, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(ALIAS_GCODE, ALIAS_CODE, 0x200000);
    gmmu_map(ALIAS_GDATA, ALIAS_DATA, 0x200000);
    CHECK(!gmem_write(positions, position, sizeof position));
    CHECK(mesh_program(ALIAS_GCODE + 0x1000, fragment, 1, 1));
    CHECK(!storage_write_words(ALIAS_CBUF + 0x510, desc, 8));
    u32* r = channel->r3d;
    r[0x582] = (u32)(ALIAS_GCODE >> 32);
    r[0x583] = (u32)ALIAS_GCODE;
    r[0x850] = 1;
    r[0x851] = 0x1000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 0;
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(positions >> 32);
    r[0x702] = (u32)positions;
    r[0x7c0] = (u32)((positions + 15) >> 32);
    r[0x7c1] = (u32)(positions + 15);
    r[0x487] = 0x0fac6884;
    r[0x54e] = 0;
    channel->ds.bind.cb_addr[0][0] = ALIAS_CBUF;
    channel->ds.bind.cb_size[0][0] = 0x1000;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    channel->ds.owner = (ordering ? 0x80000000u : 0) | channel->id;
    return 0;
}
static int alias_channel_end(void) {
    CHECK(mem_unmap(ALIAS_CODE, 0x200000) == 0 && mem_unmap(ALIAS_DATA, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    return 0;
}
/* one draw of program as the vertex program, recorded; then NVN's barriers when ordering */
static int alias_program_draw(GpuChannel* channel, const u64* program, u32 n, int ordering) {
    CHECK(mesh_program(ALIAS_GCODE, program, n, 0));
    g_renderer->method_barrier(5); /* the program at this address is another one now */
    const u64 drawn = g_vk_drawn;
    DrawCall call = {.topology = 0, .count = 1, .instances = 1};
    vk_lock();
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_unlock();
    CHECK(g_vk_drawn == drawn + 1);
    if (ordering) {
        g_renderer->method_barrier(6);
        g_renderer->method_barrier(7);
    }
    return 0;
}
/* the storage's first words as guest memory has them once the GPU is done, n of them into words */
static void alias_words(u32* words, u32 n) {
    vk_lock();
    vk_finish();
    vk_unlock();
    for (u32 i = 0; i < n; i++) words[i] = snx_ld_le32(GP(ALIAS_DATA + 0x1000 + 4 * i));
}

/* One program stores 0x5eed through buffer 0, loads that word through buffer 1, and stores what it loaded through
   buffer 0 again, a word on: in place (each buffer at its own offset in guest memory) and in the atlas (both one
   buffer of copied pages), the load sees the store. The translator declares the two buffers Aliased */
static int check_alias_shader(void) {
    const u64 program[] = {ALIAS_MOV_R0, ALIAS_MOV_R1, storage_mov(2, 0x5eed),
                           0xeedc200000070002ull, /* STG.E [R0], R2 */
                           ALIAS_MOV_R4, ALIAS_MOV_R5,
                           0xeed4200010070402ull, /* LDG.E R2, [R4 + 0x100]: buffer 1's view of [R0] */
                           0xeedc200000470002ull, /* STG.E [R0 + 4], R2 */
                           ALIAS_ALD, ALIAS_AST, ALIAS_EXIT};
    const int was = g_settings.guest_buffers_copied;
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel && !alias_channel(channel, 0));
    int ok[2] = {1, 1}, way[2] = {0, 0};
    u32 words[2][2] = {{0}};
    for (int inplace = 1; inplace >= 0; inplace--) {
        if (inplace && !vk_uma()) {
            printf("skip: in place, guest memory is copied on this device (vk_memory)\n");
            continue;
        }
        g_settings.guest_buffers_copied = inplace ? was : 2; /* uma_copies=2 binds no aliasing draw in place */
        memset(GP(ALIAS_DATA), 0xcd, 0x2000);
        const AliasCounts before = alias_counts();
        CHECK(!alias_program_draw(channel, program, sizeof program / sizeof *program, 0));
        const AliasCounts after = alias_counts();
        way[inplace] = inplace ? after.inplace == before.inplace + 1 : after.atlas == before.atlas + 1;
        alias_words(words[inplace], 2);
        ok[inplace] = way[inplace] && words[inplace][0] == 0x5eed && words[inplace][1] == 0x5eed;
    }
    g_settings.guest_buffers_copied = was;
    CHECK(!alias_channel_end());
    free(channel);
    if (!ok[0] || !ok[1])
        fprintf(stderr,
                "aliased global buffers: in place %d (words %08x %08x), atlas %d (words %08x %08x), want 0x5eed in "
                "both\n",
                way[1], words[1][0], words[1][1], way[0], words[0][0], words[0][1]);
    CHECK(ok[0] && ok[1]);
    printf("aliased global buffers: a load through one sees a store through the other, in place and in the atlas\n");
    return 0;
}

/* The order of aliasing draws written in place: draw A stores 0x5eed in place (one buffer), draw B is the aliasing one
   that loads A's word through buffer 1 and stores it a word on through buffer 0, and draw C loads B's word and stores
   it a word further. uma.c's order puts a barrier between each, by page, in Maxwell's order and in NVN's (its barriers
   between the draws): the last word is A's, with no new message of the validation layer (its sync check sees a
   missing barrier) */
static int check_uma_alias_order(void) {
    const u64 a[] = {ALIAS_MOV_R0, ALIAS_MOV_R1, storage_mov(2, 0x5eed), 0xeedc200000070002ull, /* STG.E [R0], R2 */
                     ALIAS_ALD,    ALIAS_AST,    ALIAS_EXIT};
    const u64 b[] = {ALIAS_MOV_R0, ALIAS_MOV_R1, ALIAS_MOV_R4, ALIAS_MOV_R5,
                     0xeed4200010070402ull, /* LDG.E R2, [R4 + 0x100] */
                     0xeedc200000470002ull, /* STG.E [R0 + 4], R2 */
                     ALIAS_ALD,    ALIAS_AST,    ALIAS_EXIT};
    const u64 c[] = {ALIAS_MOV_R0, ALIAS_MOV_R1,
                     0xeed4200000470002ull, /* LDG.E R2, [R0 + 4] */
                     0xeedc200000870002ull, /* STG.E [R0 + 8], R2 */
                     ALIAS_ALD,    ALIAS_AST,    ALIAS_EXIT};
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    const int was_ordering = g_settings.nvn_storage_ordering;
    int ok[2] = {0, 0};
    u32 words[2][3] = {{0}}, messages[2] = {0};
    for (int ordering = 0; ordering < 2; ordering++) {
        GpuChannel* channel = calloc(1, sizeof *channel);
        CHECK(channel);
        g_settings.nvn_storage_ordering = ordering;
        CHECK(!alias_channel(channel, ordering));
        memset(GP(ALIAS_DATA), 0xcd, 0x2000);
        const u32 validation = g_vk_validation_messages;
        const AliasCounts before = alias_counts();
        CHECK(!alias_program_draw(channel, a, sizeof a / sizeof *a, ordering));
        CHECK(!alias_program_draw(channel, b, sizeof b / sizeof *b, ordering));
        CHECK(!alias_program_draw(channel, c, sizeof c / sizeof *c, ordering));
        const AliasCounts after = alias_counts();
        alias_words(words[ordering], 3);
        messages[ordering] = g_vk_validation_messages - validation;
        ok[ordering] = after.inplace == before.inplace + 1 && after.atlas == before.atlas &&
                       words[ordering][0] == 0x5eed && words[ordering][1] == 0x5eed && words[ordering][2] == 0x5eed &&
                       !messages[ordering];
        CHECK(!alias_channel_end());
        free(channel);
    }
    g_settings.nvn_storage_ordering = was_ordering;
    for (int ordering = 0; ordering < 2; ordering++)
        if (!ok[ordering])
            fprintf(stderr, "aliasing draws in order (%s's): words %08x %08x %08x, want 0x5eed, %u validation "
                    "messages\n", ordering ? "NVN" : "Maxwell", words[ordering][0], words[ordering][1],
                    words[ordering][2], messages[ordering]);
    CHECK(ok[0] && ok[1]);
    printf("in place: a draw before an aliasing draw and one after it run in order, in Maxwell's order and NVN's\n");
    return 0;
}
