/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Writable storage buffers: kept on the GPU where the host traps memory, and what a binding costs
   (vk_buf_writable_stats). No extracted shaders or game data are used */

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
/* Where the host traps guest memory, a writable binding of whole pages stays on the GPU: its bytes are GPU-owned from
   the bind on, the first bind staging them once, and the draws write their owner in place. Nothing is copied before
   a draw, read back after it or merged, and a CPU read lands the owner with the stores in it. A CPU write between a
   draw and its fence lands it first: the CPU's bytes and the stores beside them on the same page both stay, and over a
   store the CPU's write, the later one, stays. The range the CPU touched takes the copies from then on, as does every
   binding without traps or with unified memory (whose path is its own). The cases each take a range of 4 pages in a
   64 KB granule of their own (vk_own_cpu_read), the last one through a draw: a fragment program's STG */
#define BUFKEEP_GVA 0x4f8000000ull
#define BUFKEEP_CPU (HEAP_BASE + 0xa000000)
#define BUFKEEP_LEN 0x4000u

/* the bytes of the range at range (an offset in the mapped area) as the CPU reads them: the pattern, but for n words
   {offset in the range, value} */
static int bufkeep_expect(const char* when, u64 range, const u32 (*word)[2], u32 n) {
    static u8 got[BUFKEEP_LEN];
    CHECK(!gmem_read(BUFKEEP_GVA + range, got, sizeof got));
    for (u32 i = 0; i < BUFKEEP_LEN; i += 4) {
        u32 want = bufkeep_pattern(range + i);
        for (u32 k = 0; k < n; k++)
            if (word[k][0] == i) want = word[k][1];
        if (snx_ld_le32(got + i) != want) {
            fprintf(stderr, "storage buffer %s: the word at %#x of the range at %#llx is %08x, expected %08x\n", when,
                    i, (unsigned long long)range, snx_ld_le32(got + i), want);
            return 1;
        }
    }
    return 0;
}
/* what the CPU writes at the range's byte at: the pattern's word with every byte changed */
static u32 bufkeep_cpu(u64 range, u32 at) { return ~bufkeep_pattern(range + at); }
/* a writable binding of the range at range, committed, and stores of n words {offset, value} into it on the GPU */
static int bufkeep_draw(u64 range, const u32 (*word)[2], u32 n, VkBuffer* buf, u64* off) {
    vk_lock();
    CHECK(vk_buf_copy_writable(BUFKEEP_GVA + range, BUFKEEP_LEN, buf, off, 1));
    vk_buf_writable_commit();
    for (u32 k = 0; k < n; k++) CHECK(!bufkeep_store(*buf, *off + word[k][0], word[k][1]));
    vk_unlock();
    return 0;
}
static void bufkeep_finish(void) {
    vk_lock();
    vk_finish();
    vk_unlock();
}
/* a channel drawing a triangle over a 4x4 viewport, four color targets of format 0 and no depth, whose fragment
   program stores c[0][0x524] 16 bytes into the storage buffer of NVN's descriptor at c[0][0x510] */
static GpuChannel* bufkeep_channel(u64 address) {
    const u64 positions = address + 0x2000, cbuf = address + 0x3000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xe30000000007000full};
    const u64 store[] = {0x4c98078014470000ull, /* MOV R0, c[0][0x510] */
                         0x4c98078014570001ull, /* MOV R1, c[0][0x514] */
                         0x4c98078014970002ull, /* MOV R2, c[0][0x524] */
                         0xeedc200001070002ull, /* STG.E [R0 + 0x10], R2 */
                         0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    u8 bytes[48];
    GpuChannel* channel = calloc(1, sizeof *channel);
    if (!channel) return NULL;
    channel->id = 127;
    for (u32 i = 0; i < 12; i++) {
        u32 word;
        memcpy(&word, position + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
    /* The fragment stage declares no inputs or color outputs: it only stores through a constant-buffer address. */
    if (gmem_write(positions, bytes, sizeof bytes) || !mesh_program(address, vertex, 3, 0) ||
        !mesh_program(address + 0x1000, store, 5, 0)) {
        free(channel);
        return NULL;
    }
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x1000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27; /* RGBA32F */
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(positions >> 32);
    r[0x702] = (u32)positions;
    r[0x7c0] = (u32)((positions + 47) >> 32);
    r[0x7c1] = (u32)(positions + 47);
    r[0x487] = 0x0fac6884;
    for (u32 i = 0; i < 4; i++) {
        u64 a = address + 0x5000 + i * 0x400;
        r[0x200 + i * 0x10] = (u32)(a >> 32);
        r[0x201 + i * 0x10] = (u32)a;
        r[0x202 + i * 0x10] = 4;
        r[0x203 + i * 0x10] = 4;
        r[0x204 + i * 0x10] = 0; /* no format: nothing is written there */
    }
    r[0x54e] = 0;
    channel->ds.bind.cb_addr[4][0] = cbuf;
    channel->ds.bind.cb_size[4][0] = 0x1000;
    channel->r3d_dirty = ~0u;
    return channel;
}

static int check_storage_buffer_keep(void) {
    const u64 a = 0, b = 0x10000, c = 0x20000, dd = 0x30000, e = 0x40000, f = 0x50000, code = 0x60000;
    const u64 sa = g_vk.props.limits.minStorageBufferOffsetAlignment;
    int uma = vk_uma(), expect_kept = own_traps() && !uma, kept;
    VkBuffer b1, b2, rd;
    u64 o1, o2, rd_at;
    u8 w[4];
    CHECK(mem_map(BUFKEEP_CPU, 0x80000, MS_NORMAL, PERM_RW) == 0); /* past the heap */
    gmmu_map(BUFKEEP_GVA, BUFKEEP_CPU, 0x80000);
    for (u64 i = 0; i < 0x60000; i += 4) snx_st_le32(GP(BUFKEEP_CPU + i), bufkeep_pattern(i));
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    vk_unlock();

    /* the first binding: kept where the host traps memory, the range staged once; then a second one, after the
       fence, reads nothing back and copies nothing, and draws read the owner where it is */
    BufCount was = bufkeep_since((BufCount){0}), d;
    CHECK(!bufkeep_draw(a, (const u32[][2]){{0x10, 0x5eed0001}}, 1, &b1, &o1));
    bufkeep_finish();
    d = bufkeep_since(was);
    kept = own_lookup(BUFKEEP_CPU + a, BUFKEEP_LEN, NULL, 0) == 1;
    if (expect_kept && (!kept || d.copied != BUFKEEP_LEN || d.back || d.merged))
        fprintf(stderr,
                "storage buffer: the host traps memory, but the binding copied %llu bytes before its draw and %llu "
                "back after it, merged %llu results and left the range %s, where it keeps it on the GPU: %u, 0 and 0 "
                "bytes, GPU-owned\n",
                (unsigned long long)d.copied, (unsigned long long)d.back, (unsigned long long)d.merged,
                kept ? "GPU-owned" : "the CPU's", BUFKEEP_LEN);
    CHECK(!expect_kept || (kept && d.copied == BUFKEEP_LEN && !d.back && !d.merged));
    if (!expect_kept) CHECK(!kept);
    if (!expect_kept && !uma) CHECK(d.copied == 2 * BUFKEEP_LEN && d.back == BUFKEEP_LEN && d.merged == 1);
    was = bufkeep_since((BufCount){0});
    CHECK(!bufkeep_draw(a, (const u32[][2]){{0x20, 0x5eed0002}}, 1, &b2, &o2));
    vk_lock();
    CHECK(vk_buf_get_input(BUFKEEP_GVA + a, BUFKEEP_LEN, sa, &rd, &rd_at)); /* the result, where it is */
    CHECK(uma || (rd == b2 && rd_at == o2));
    vk_finish();
    CHECK(vk_buf_get_input(BUFKEEP_GVA + a, BUFKEEP_LEN, sa, &rd, &rd_at));
    if (kept) CHECK(rd == b2 && rd_at == o2); /* its owner, where it is */
    vk_unlock();
    d = bufkeep_since(was);
    if (kept && (d.copied || d.back || d.merged || b2 != b1 || o2 != o1))
        fprintf(stderr,
                "storage buffer kept: its next binding copied %llu bytes before its draw and %llu back after it, and "
                "merged %llu results, %s its owner: 0, 0 and 0 in it expected\n",
                (unsigned long long)d.copied, (unsigned long long)d.back, (unsigned long long)d.merged,
                b2 == b1 && o2 == o1 ? "in" : "not in");
    CHECK(!kept || (!d.copied && !d.back && !d.merged && b2 == b1 && o2 == o1));
    if (kept) CHECK(own_lookup(BUFKEEP_CPU + a, BUFKEEP_LEN, NULL, 0) == 1);
    CHECK(!bufkeep_expect("after two draws", a, (const u32[][2]){{0x10, 0x5eed0001}, {0x20, 0x5eed0002}}, 2));
    CHECK(own_lookup(BUFKEEP_CPU + a, BUFKEEP_LEN, NULL, 0) == 0); /* the read landed it */

    /* the CPU writes between a draw and its fence, in place as a guest thread does, past the runtime's hooks: beside
       the first store on its page, over the second (its write comes last, as on the console once the GPU stored),
       and on another page. All of them stay, and the first store */
    CHECK(!bufkeep_draw(b, (const u32[][2]){{0x10, 0x5eed0003}, {0x18, 0x5eed0004}}, 2, &b1, &o1));
    snx_st_le32(GP(BUFKEEP_CPU + b + 0x14), bufkeep_cpu(b, 0x14));
    snx_st_le32(GP(BUFKEEP_CPU + b + 0x18), bufkeep_cpu(b, 0x18));
    snx_st_le32(GP(BUFKEEP_CPU + b + 0x2000), bufkeep_cpu(b, 0x2000));
    bufkeep_finish();
    CHECK(!bufkeep_expect("written by the CPU before the fence", b,
                          (const u32[][2]){{0x10, 0x5eed0003},
                                           {0x14, bufkeep_cpu(b, 0x14)},
                                           {0x18, bufkeep_cpu(b, 0x18)},
                                           {0x2000, bufkeep_cpu(b, 0x2000)}},
                          4));
    /* the CPU wrote that range lately: its next binding takes the copies, and a CPU write beside its store on the
       same page keeps both */
    was = bufkeep_since((BufCount){0});
    CHECK(!bufkeep_draw(b, (const u32[][2]){{0x30, 0x5eed0005}}, 1, &b1, &o1));
    d = bufkeep_since(was);
    if (!uma && d.copied != 2 * BUFKEEP_LEN)
        fprintf(stderr, "storage buffer the CPU wrote: its binding copied %llu bytes before the draw, expected %u\n",
                (unsigned long long)d.copied, 2 * BUFKEEP_LEN);
    CHECK(uma || d.copied == 2 * BUFKEEP_LEN);
    CHECK(own_lookup(BUFKEEP_CPU + b, BUFKEEP_LEN, NULL, 0) == 0);
    snx_st_le32(GP(BUFKEEP_CPU + b + 0x34), bufkeep_cpu(b, 0x34));
    bufkeep_finish();
    CHECK(!bufkeep_expect("written by the CPU beside the store", b,
                          (const u32[][2]){{0x10, 0x5eed0003},
                                           {0x14, bufkeep_cpu(b, 0x14)},
                                           {0x18, bufkeep_cpu(b, 0x18)},
                                           {0x2000, bufkeep_cpu(b, 0x2000)},
                                           {0x30, 0x5eed0005},
                                           {0x34, bufkeep_cpu(b, 0x34)}},
                          6));

    /* a CPU read before the fence lands the owner: a draw binding the range again before the fence writes it
       elsewhere, and lands its store too */
    CHECK(!bufkeep_draw(c, (const u32[][2]){{0x10, 0x5eed0006}}, 1, &b1, &o1));
    memcpy(w, GP(BUFKEEP_CPU + c + 0x800), 4);
    CHECK(snx_ld_le32(w) == bufkeep_pattern(c + 0x800));
    CHECK(!bufkeep_draw(c, (const u32[][2]){{0x20, 0x5eed0007}}, 1, &b2, &o2));
    bufkeep_finish();
    CHECK(!bufkeep_expect("bound again after a CPU read", c, (const u32[][2]){{0x10, 0x5eed0006}, {0x20, 0x5eed0007}},
                          2));

    /* the owner a fence went past, bound again: a CPU read before the next fence waits for the new store when the
       range is kept, and reads the bytes before it otherwise, the copies landing at the fence */
    CHECK(!bufkeep_draw(dd, (const u32[][2]){{0x10, 0x5eed0008}}, 1, &b1, &o1));
    bufkeep_finish();
    CHECK(!bufkeep_draw(dd, (const u32[][2]){{0x20, 0x5eed0009}}, 1, &b2, &o2));
    memcpy(w, GP(BUFKEEP_CPU + dd + 0x20), 4);
    if (snx_ld_le32(w) != (kept ? 0x5eed0009 : bufkeep_pattern(dd + 0x20)))
        fprintf(stderr, "storage buffer bound again: the CPU read %08x before the fence, expected %08x\n",
                snx_ld_le32(w), kept ? 0x5eed0009 : bufkeep_pattern(dd + 0x20));
    CHECK(snx_ld_le32(w) == (kept ? 0x5eed0009 : bufkeep_pattern(dd + 0x20)));
    bufkeep_finish();
    CHECK(!bufkeep_expect("bound again", dd, (const u32[][2]){{0x10, 0x5eed0008}, {0x20, 0x5eed0009}}, 2));

    /* a result of the frame before, its submission held back (the GPU a frame behind), bound again: written in place,
       copying nothing; a CPU read once the GPU goes on waits for the new store when the range is kept */
    VkSemaphore gate;
    CHECK(!bufkeep_draw(f, (const u32[][2]){{0x10, 0x5eed000a}}, 1, &b1, &o1));
    vk_lock();
    CHECK(!suffix_gate(&gate));
    vk_frame_next();
    vk_unlock();
    was = bufkeep_since((BufCount){0});
    CHECK(!bufkeep_draw(f, (const u32[][2]){{0x20, 0x5eed000b}}, 1, &b2, &o2));
    d = bufkeep_since(was);
    if (kept && (d.copied || d.back || d.merged || b2 != b1 || o2 != o1))
        fprintf(stderr,
                "storage buffer kept, of the frame before: its binding copied %llu bytes before its draw and %llu back "
                "after it, merged %llu results, %s its owner: 0, 0, 0, in it expected\n",
                (unsigned long long)d.copied, (unsigned long long)d.back, (unsigned long long)d.merged,
                b2 == b1 && o2 == o1 ? "in" : "not in");
    CHECK(!kept || (!d.copied && !d.back && !d.merged && b2 == b1 && o2 == o1));
    VkSemaphoreSignalInfo open = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = gate, .value = 1};
    CHECK(VK_OK(vkSignalSemaphore(g_vk.dev, &open)));
    memcpy(w, GP(BUFKEEP_CPU + f + 0x20), 4);
    if (snx_ld_le32(w) != (kept ? 0x5eed000b : bufkeep_pattern(f + 0x20)))
        fprintf(stderr, "storage buffer of the frame before: the CPU read %08x before the fence, expected %08x\n",
                snx_ld_le32(w), kept ? 0x5eed000b : bufkeep_pattern(f + 0x20));
    CHECK(snx_ld_le32(w) == (kept ? 0x5eed000b : bufkeep_pattern(f + 0x20)));
    bufkeep_finish();
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    CHECK(!bufkeep_expect("of the frame before", f, (const u32[][2]){{0x10, 0x5eed000a}, {0x20, 0x5eed000b}}, 2));

    /* a draw's own binding: a fragment program's store, two draws, a fence after each */
    const u64 address = BUFKEEP_GVA + code, cbuf = address + 0x3000, stored = BUFKEEP_GVA + e;
    GpuChannel* channel = bufkeep_channel(address);
    CHECK(channel);
    CHECK(!storage_write_words(cbuf + 0x510, (const u32[3]){(u32)stored, (u32)(stored >> 32), BUFKEEP_LEN}, 3));
    for (u32 i = 0; i < 2; i++) {
        int drawn, skipped;
        CHECK(!storage_write_words(cbuf + 0x524, (const u32[1]){0x5eed0010 + i}, 1));
        channel->r3d_dirty = ~0u;
        was = bufkeep_since((BufCount){0});
        targetless_draw(channel, &drawn, &skipped);
        bufkeep_finish();
        d = bufkeep_since(was);
        if (!drawn || skipped) fprintf(stderr, "storage buffer draw %u: drawn %d, skipped %d\n", i, drawn, skipped);
        CHECK(drawn && !skipped);
        if (kept && (d.copied != (i ? 0 : BUFKEEP_LEN) || d.back || d.merged))
            fprintf(stderr,
                    "storage buffer draw %u: copied %llu bytes before it and %llu back after it, merged %llu results: "
                    "%u, 0 and 0 expected\n",
                    i, (unsigned long long)d.copied, (unsigned long long)d.back, (unsigned long long)d.merged,
                    i ? 0 : BUFKEEP_LEN);
        CHECK(!kept || (d.copied == (i ? 0 : BUFKEEP_LEN) && !d.back && !d.merged));
        CHECK(kept == (own_lookup(BUFKEEP_CPU + e, BUFKEEP_LEN, NULL, 0) == 1));
    }
    free(channel);
    CHECK(!bufkeep_expect("stored by two draws", e, (const u32[][2]){{0x10, 0x5eed0011}}, 1));
    vk_lock();
    vk_finish();
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    printf("writable storage buffers: %s; the CPU's writes before the fence and the stores beside them stay\n",
           kept  ? "kept on the GPU, written in place, nothing copied, read back or merged"
           : uma ? "written in guest memory (unified memory)"
                 : "copied, read back and merged (no traps here)");
    return 0;
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
