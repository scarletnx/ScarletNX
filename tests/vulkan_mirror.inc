/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU mirror of guest memory (mirror.c): a range bound into it holds guest memory's bytes; a byte the CPU changes
   since goes in when the range is bound again, and a byte the GPU wrote there and the CPU didn't stays */

/* len bytes of src at from, read back once the work recorded so far is done */
static int buffer_read(VkBuffer src, u64 from, u32 len, u8* out) {
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
    VkBufferCopy copy = {from, at, len};
    rec_CmdCopyBuffer(vk_cmd(), src, back, 1, &copy);
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
/* len bytes of the mirror at cpu */
static int mirror_read(u64 cpu, u32 len, u8* out) { return buffer_read(vk_mirror_buffer(), cpu, len, out); }
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

/* A dispatch's write log into the mirror's mask (vk_mirror_fold): two buffers of one dispatch, the first at a byte
   that is not the first of a mask word, the second over it. Only the windows flagged are taken, the bits past a
   buffer's length go, and the bits of the two buffers meet in the mask */
static int check_mirror_fold(u64 cpu, u64 gva) {
    const u64 a = cpu + 0x1005, b = a + 8; /* a: bit 5 of its mask word */
    const u32 alen = 0x1800, blen = 0x1ff0; /* b inside the 3 pages mapped */
    static u8 want[0x2100]; /* a byte for each guest byte from cpu + 0x1000 */
    static u32 words[2 * SH_WLOG_PART];
    memset(want, 0, sizeof want);
    memset(words, 0, sizeof words);
    /* the bits of buffer p for its bytes [from, to), the window flagged or not */
    u32* part[2] = {words, words + SH_WLOG_PART};
    const struct {
        u32 p, from, to, flag;
    } runs[] = {
        {0, 0x10, 0x30, 1},     /* across a mask word of the log and two of the mirror's */
        {0, 0x17f0, 0x1810, 1}, /* past a's length from 0x1800: those go */
        {1, 0x0, 0x4, 1},       /* b's first bytes, a's 8 to 12 */
        {1, 0x1100, 0x1104, 0}, /* window 1 of b not flagged: not taken */
    };
    for (u32 r = 0; r < sizeof runs / sizeof *runs; r++) {
        u32* flags = part[runs[r].p];
        for (u32 x = runs[r].from; x < runs[r].to; x++) {
            flags[SH_WPAGE_SLOT + x / 32] |= 1u << (x % 32);
            if (runs[r].flag) flags[x / 4096] = 1;
            u64 at = (runs[r].p ? b : a) + x;
            u32 len = runs[r].p ? blen : alen;
            if (runs[r].flag && x < len) want[at - (cpu + 0x1000)] = 1;
        }
    }
    VkWriteLog* log = vk_wlog_get(2);
    CHECK(log && vk_wlog_size(log) == sizeof words);
    VkBuffer src;
    u64 at;
    u8* p = vk_stage_upload(sizeof words, 16, &src, &at);
    CHECK(p);
    memcpy(p, words, sizeof words);
    vk_finish(); /* the mask clear of what came before */
    vk_render_end();
    VkBufferCopy copy = {at, 0, sizeof words};
    rec_CmdCopyBuffer(vk_cmd(), src, vk_wlog_buffer(log), 1, &copy);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    VkMirFold f[2] = {{a, alen, 0}, {b, blen, 1}};
    vk_mirror_fold(vk_cmd(), vk_wlog_buffer(log), vk_wlog_size(log), f, 2);
    CHECK(vk_mirror_pending(gva + 0x1005, alen));
    /* the mask, read before the submit's harvest clears it */
    static u32 mask[0x2100 / 32];
    CHECK(!buffer_read(vk_mirror_mask(), (cpu + 0x1000) / 8, sizeof mask, (u8*)mask));
    for (u32 i = 0; i < sizeof want; i++)
        if ((mask[i / 32] >> (i % 32) & 1) != want[i]) {
            fprintf(stderr, "vulkan_headless: mirror fold: the bit of byte %#x is %u, not %u\n", i,
                    mask[i / 32] >> (i % 32) & 1, want[i]);
            return 1;
        }
    vk_wlog_unref(log);
    /* the mirror's bytes there are guest memory's: landing the bits changes nothing */
    vk_mirror_publish();
    vk_mirror_mem_need(gva + 0x1000, 0x2000);
    own_acquire(cpu + 0x1000, 0x2000);
    CHECK(!vk_mirror_pending(gva + 0x1000, 0x2000));
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
    {
        static u8 before_landing[0x3000];
        memcpy(before_landing, GP(cpu), len);
        CHECK(!mirror_read(cpu, len, got)); /* its finish takes the program's bytes too */
        CHECK(!mirror_expect(got, before_landing, len, 0x100, 16, 0x77));
        /* the CPU's read, as gmem_read makes it: what is pending first, then what the GPU owns lands */
        vk_mirror_mem_need(gva, len);
        own_acquire(cpu, len);
    }
    /* a range over the same guest memory at another GPU address is the same bytes */
    gmmu_map(gva + 0x100000, cpu + 0x1000, 0x1000);
    CHECK(vk_mirror_slice(gva + 0x100000, 0x1000, &buf, &off));
    CHECK(off == cpu + 0x1000);

    /* written by a program: pending, then once its submit is done the page it wrote stays the mirror's, GPU-owned,
       with the byte the CPU wrote beside the program's meanwhile taken into it; nothing lands until the CPU touches the
       page (where the host traps guest memory), then the page lands whole. The bytes the program didn't log are guest
       memory's either way (the 8 bytes at 0x400, changed without being logged, which no program's stores leave: the
       page landed before, so they were taken from guest memory into the mirror) */
    memcpy(got, GP(cpu), len);
    CHECK(!mirror_write(cpu + 0x200, 24, 0x66, 1));
    CHECK(!mirror_write(cpu + 0x400, 8, 0x99, 0));
    vk_mirror_wrote(cpu, len);
    CHECK(vk_mirror_pending(gva, len) && !vk_mirror_pending(gva + 0x10000, 0x100));
    GP(cpu)[0x219] = 0x11;
    got[0x219] = 0x11;
    u64 lands = own_landings();
    vk_finish();
    /* taken in once its submit is done, but not published inside the landing, which can run in a draw's setup:
       pending until a point where nothing is being recorded */
    {
        OwnRange o0[2];
        CHECK(!own_traps() || (vk_mirror_pending(gva, len) && !own_lookup(cpu, len, o0, 2)));
    }
    vk_mirror_publish();
    CHECK(!vk_mirror_pending(gva, len));
    if (own_traps()) {
        OwnRange o[4];
        CHECK(own_lookup(cpu, len, o, 4) == 1 && o[0].addr == cpu && o[0].size == 0x1000 &&
              ((const VkOwnRes*)o[0].res)->buf == vk_mirror_buffer());
        CHECK(own_landings() == lands); /* not landed: the CPU hasn't touched it */
        /* bound again while owned: the mirror keeps the program's bytes, guest memory under it isn't copied in */
        vk_ww_new_epoch();
        CHECK(vk_mirror_slice(gva, len, &buf, &off));
        {
            static u8 m[0x3000];
            CHECK(!mirror_read(cpu, 0x1000, m));
            for (u32 i = 0; i < 0x1000; i++) {
                u8 want = i >= 0x200 && i < 0x218 ? 0x66 : got[i];
                if (m[i] != want) {
                    fprintf(stderr, "vulkan_headless: mirror: owned byte %#x is %#x, not %#x\n", i, m[i], want);
                    CHECK(0);
                }
            }
        }
        own_acquire(cpu, len); /* the CPU's touch: it lands */
        CHECK(own_landings() > lands && !own_lookup(cpu, len, o, 4));
    }
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
        vk_mirror_mem_need(g2, l2); /* those the output took are the mirror's until the CPU touches them */
        own_acquire(c2, l2);
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
    /* a draw's constants over a program's pending write, at an address a uniform buffer doesn't bind at: copied on the
       GPU from the mirror into memory it binds at, the program's bytes, without waiting for the GPU */
    {
        static u8 u[20], guest[20];
        CHECK(!mirror_write(cpu + 0x208, 8, 0x3c, 1));
        vk_mirror_wrote(cpu + 0x208, 8);
        u64 finishes = g_vk_finishes;
        CHECK(vk_buf_get_uniform(gva + 0x204, 20, &buf, &off));
        CHECK(g_vk_finishes == finishes);
        CHECK(buf != vk_mirror_buffer() && off % vk_stage_align() == 0);
        memcpy(guest, GP(cpu + 0x204), 20);
        CHECK(!buffer_read(buf, off, 20, u));
        CHECK(!mirror_expect(u, guest, 20, 4, 8, 0x3c));
    }
    CHECK(!check_mirror_fold(cpu, gva));
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    puts("The GPU mirror takes the bytes the CPU changed and keeps those the GPU wrote; the bytes its programs logged "
         "land, and no others; a draw reads pending bytes at any address without a wait");
    return 0;
}
