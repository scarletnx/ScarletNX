/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* Guest memory that a region of unified memory holds, read by the GPU from the region itself (gpu/vk/owned.c): a
   texture's load binds the region's buffer to its copy or deswizzle, in place of staging that the CPU fills, and a
   read into device memory (the copy engine's) is copied on the GPU from the region. No extracted shaders or game data
   are used. Each check reads the counts of vk_guest_read_counts: the bytes the CPU copied must be none, and the bytes
   bound or copied from the region all of the read */
#define URR_CPU (HEAP_BASE + 0x36000000) /* a window each: the pitch texture, the block linear one, the copy */
#define URR_GVA 0x6a0000000ull
static void urr_counts(u64 c[2][VK_GREAD_N]) {
    vk_lock();
    vk_guest_read_counts(c);
    vk_unlock();
}
/* texels [0, size) of t's first level and layer (rows tight, as its image holds them) read back into out */
static int urr_texels(Surf* t, u32 w, u32 h, u8* out, u64 size) {
    VkBuffer rb;
    u8* data = vk_readback_buffer(size, &rb);
    CHECK(data);
    vk_render_end();
    vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, data, (size_t)size);
    return 0;
}

/* A texture over guest memory a region holds, part of which a draw writes in place in the submission being recorded
   (its storage binding, vk_uma_write, here filled by a copy command): the texture's load in the same submission binds
   the region and reads it after that write, which only the barrier of the read in place (uma.c's order) puts before
   it. A pitch linear texture's rows are copied into its image with no other barrier on the buffer. Its texels must be
   the write's where it wrote and the CPU's elsewhere, and the CPU must copy none of its bytes. Then a block linear
   texture in the next window, which the deswizzle reads from the region: the same counts */
static int check_uma_texture_region(void) {
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    const u32 w = 1024, h = 512, pitch = w * 4; /* 2 MiB: the window, whole */
    const u64 size = (u64)pitch * h, wrote = 0x80000, wrote_n = 0x100000;
    const u64 bl_gva = URR_GVA + 0x200000, bl_cpu = URR_CPU + 0x200000;
    const u32 tic[8] = {
        rgba8, (u32)URR_GVA, (u32)(URR_GVA >> 32) | 2u << 21, pitch >> 5, (w - 1) | 1u << 23, (h - 1) | 1u << 31, 0, 0};
    /* 64x64 block linear, blocks one GOB high: 64 rows of 256 bytes, 16 KiB */
    const u32 bl_tic[8] = {rgba8, (u32)bl_gva, (u32)(bl_gva >> 32) | 3u << 21, 0, 63u | 1u << 23, 63u | 1u << 31, 0, 0};
    u64 c0[2][VK_GREAD_N], c1[2][VK_GREAD_N], c2[2][VK_GREAD_N];
    VkBuffer r, wb;
    u64 ro, wo;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    u8* texels = malloc((size_t)size);
    CHECK(texels);
    CHECK(mem_map(URR_CPU, 0x400000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(URR_GVA, URR_CPU, 0x400000);
    memset(GP(URR_CPU), 0x11, (size_t)size);
    for (u32 i = 0; i < 0x4000; i++) GP(bl_cpu)[i] = (u8)(i * 7 + 3);
    g_renderer->mem_may_have_changed();
    vk_lock();
    CHECK(vk_buf_get_input(URR_GVA, 256, 4, &r, &ro)); /* a draw reads it in place: a region over each window */
    CHECK(vk_buf_get_input(bl_gva, 256, 4, &r, &ro));
    const int held = vk_uma_held(URR_CPU, size) && vk_uma_held(bl_cpu, 0x4000);
    vk_finish(); /* no write in place pending from here */
    vk_render_end();
    CHECK(vk_uma_write(URR_GVA + wrote, wrote_n, 4, 1, &wb, &wo)); /* a draw's storage binding, written in place */
    rec_CmdFillBuffer(vk_cmd(), wb, wo, wrote_n, 0x22222222u);
    vk_uma_writes_commit();
    const int pending = vk_uma_pending(URR_GVA, size);
    vk_unlock();
    urr_counts(c0);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    CHECK(t && !t->bl && t->size == size);
    vk_unlock();
    urr_counts(c1);
    vk_lock();
    CHECK(!urr_texels(t, w, h, texels, size));
    vk_unlock();
    u64 differ = 0, first = ~0ull;
    for (u64 i = 0; i < size; i++)
        if (texels[i] != (i >= wrote && i < wrote + wrote_n ? 0x22 : 0x11)) {
            if (!differ) first = i;
            differ++;
        }
    const u64 cpu = c1[0][VK_GREAD_CPU] - c0[0][VK_GREAD_CPU], bound = c1[0][VK_GREAD_BOUND] - c0[0][VK_GREAD_BOUND];
    /* the block linear one: its deswizzle reads the region, with nothing written in place */
    vk_lock();
    Surf* b = vk_tex_from_tic(bl_tic);
    CHECK(b && b->bl);
    vk_unlock();
    urr_counts(c2);
    vk_lock();
    CHECK(!urr_texels(b, 64, 64, texels, 0x4000));
    vk_unlock();
    u64 bl_differ = 0;
    for (u32 y = 0; y < 64; y++)
        for (u32 x = 0; x < 256; x++) bl_differ += texels[y * 256 + x] != GP(bl_cpu)[bl_offset(x, y, 0, 256, 64, 0, 0)];
    const u64 bl_cpu_bytes = c2[0][VK_GREAD_CPU] - c1[0][VK_GREAD_CPU],
              bl_bound = c2[0][VK_GREAD_BOUND] - c1[0][VK_GREAD_BOUND];
    /* order (uma.c): a write over this submission's write gets a barrier where the front end gives none (ordered 1),
       and where it gives its own (ordered 0) only after it asked for one; its full barrier orders all before it. The
       first write of a submission gets one */
    u64 nb[6];
    vk_lock();
    vk_finish();
    vk_render_end();
    const u64 b0 = vk_uma_barrier_count();
    for (int k = 0; k < 6; k++) {
        if (k == 2) vk_uma_order_request();
        if (k == 4) vk_uma_ordered();
        CHECK(vk_uma_write(URR_GVA + wrote, wrote_n, 4, k == 3, &wb, &wo));
        vk_uma_writes_commit();
        nb[k] = vk_uma_barrier_count() - b0;
    }
    vk_finish();
    vk_unlock();
    /* the first, the one asked for, the conservative one; none after the full barrier or with none asked */
    const u64 want[6] = {1, 1, 2, 3, 3, 3};
    int order_bad = 0;
    for (int k = 0; k < 6; k++) order_bad |= nb[k] != want[k];
    if (order_bad)
        fprintf(stderr, "order of writes in place: barriers %llu %llu %llu %llu %llu %llu, want 1 1 2 3 3 3\n",
                (unsigned long long)nb[0], (unsigned long long)nb[1], (unsigned long long)nb[2],
                (unsigned long long)nb[3], (unsigned long long)nb[4], (unsigned long long)nb[5]);
    free(texels);
    CHECK(mem_unmap(URR_CPU, 0x400000) == 0);
    g_renderer->mem_may_have_changed();
    if (!held || !pending || differ || cpu || bound != size || bl_differ || bl_cpu_bytes || bl_bound != 0x4000)
        fprintf(
            stderr,
            "texture over a region: held %d, a write in place pending %d; pitch linear: %llu of %llu bytes not "
            "the write's or the CPU's (the first at %llx: %02x), %llu bytes copied by the CPU, %llu bound in place; "
            "block linear: %llu texels' bytes wrong, %llu bytes copied by the CPU, %llu bound in place\n",
            held, pending, (unsigned long long)differ, (unsigned long long)size, (unsigned long long)first,
            differ ? texels[0] : 0, (unsigned long long)cpu, (unsigned long long)bound, (unsigned long long)bl_differ,
            (unsigned long long)bl_cpu_bytes, (unsigned long long)bl_bound);
    CHECK(held && pending && !differ && !cpu && bound == size);
    CHECK(!bl_differ && !bl_cpu_bytes && bl_bound == 0x4000);
    CHECK(!order_bad);
    printf("in place: a texture's load reads a region where it is, after a write in place in the same submission; "
           "writes in place take the front end's storage barriers\n");
    return 0;
}

/* Guest bytes a region holds, read into device memory (owned.c's guest_read): the read the copy engine and the 2D
   engine take for a destination's bytes and for a source under a render target, and dispatches for their constants.
   Each run is copied on the GPU from the region's buffer, in stream order (vk_guest_read_device) and in the
   submission's upload command buffer (vk_guest_read_ahead), and the CPU copies none of its bytes */
static int check_uma_dma_region(void) {
    const u64 src = URR_GVA + 0x400000, src_cpu = URR_CPU + 0x400000;
    u64 c0[2][VK_GREAD_N], c1[2][VK_GREAD_N], at;
    u8 bytes[2][256];
    VkBuffer r, dev, rb;
    u64 ro;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(src_cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(src, src_cpu, 0x200000);
    for (u32 i = 0; i < 0x1000; i++) GP(src_cpu)[i] = (u8)(i * 5 + 1);
    g_renderer->mem_may_have_changed();
    vk_lock();
    CHECK(vk_buf_get_input(src, 256, 4, &r, &ro)); /* a draw reads it in place: a region over its window */
    const int held = vk_uma_held(src_cpu + 0x100, 256);
    vk_finish();
    vk_guest_read_counts(c0);
    vk_render_end();
    CHECK(vk_stage_device(512, 16, &dev, &at));
    CHECK(vk_guest_read_device(src + 0x100, 256, dev, at));
    const int ahead = vk_guest_read_ahead(src + 0x300, 256, dev, at + 256); /* nothing written in this submission */
    vk_guest_read_counts(c1);
    u8* data = vk_readback_buffer(512, &rb);
    CHECK(data);
    VkBufferCopy back = {at, 0, 512};
    vk_render_end();
    rec_CmdCopyBuffer(vk_cmd(), dev, rb, 1, &back);
    vk_finish();
    vk_readback_ready();
    memcpy(bytes, data, sizeof bytes);
    vk_unlock();
    u32 differ = 0;
    for (u32 i = 0; i < 256; i++) {
        differ += bytes[0][i] != GP(src_cpu)[0x100 + i];
        differ += bytes[1][i] != GP(src_cpu)[0x300 + i];
    }
    const u64 cpu = c1[1][VK_GREAD_CPU] - c0[1][VK_GREAD_CPU],
              regions = c1[1][VK_GREAD_REGIONS] - c0[1][VK_GREAD_REGIONS];
    CHECK(mem_unmap(src_cpu, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    if (!held || !ahead || differ || cpu || regions != 512)
        fprintf(stderr,
                "device read from a region: held %d, read ahead %d, %u of 512 bytes differ (first %02x, want %02x), "
                "%llu bytes copied by the CPU, %llu copied from the region\n",
                held, ahead, differ, bytes[0][0], (u8)(0x100 * 5 + 1), (unsigned long long)cpu,
                (unsigned long long)regions);
    CHECK(held && ahead && !differ && !cpu && regions == 512);
    printf("in place: a read into device memory copies a region's bytes from the region on the GPU\n");
    return 0;
}
