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

#if !defined(_WIN32)
#include <sys/mman.h>
/* the protection of the mapping at p, as /proc/self/maps gives it ("rw-s", "---s"): 1 when found */
static int urr_prot(const void* p, char out[5]) {
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perm[5];
        if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perm) == 3 && (unsigned long long)(uintptr_t)p >= lo &&
            (unsigned long long)(uintptr_t)p < hi) {
            memcpy(out, perm, 5);
            found = 1;
        }
    }
    fclose(f);
    return found;
}
/* A dma-buf region (udmabuf) keeps the GPU's pages while the CPU's mapping of them is trapped: plan item 8 of
   docs/dev/draw-cost.md watches a render target there in place of a publish at each release. The CPU's mapping of a
   region's window is protected with no access, the GPU writes the region's buffer in 100 submissions, and the
   mapping stays protected through them. Opened again, the CPU reads the GPU's last write, and the device is not lost
   (a lost device is fatal) */
static int check_uma_region_dmabuf_trap(void) {
    const u64 gva = URR_GVA + 0x600000, cpu = URR_CPU + 0x600000;
    VkBuffer r, buf;
    u64 ro, off;
    char before[5] = "", during[5] = "";
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, 0x200000);
    memset(GP(cpu), 0x11, 0x1000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    CHECK(vk_buf_get_strict(gva, 256, &r, &ro)); /* a region over its window, also where imports are dear */
    vk_finish();
    if (!vk_uma_region_held(cpu, 0x1000)) {
        vk_unlock();
        CHECK(mem_unmap(cpu, 0x200000) == 0);
        g_renderer->mem_may_have_changed();
        printf("skip: no dma-buf region holds the window on this device\n");
        return 0;
    }
    CHECK(vk_uma_cover(cpu, 0x1000, &buf, &off) == 1);
    u8* host = GP(cpu);
    CHECK(urr_prot(host, before));
    CHECK(mprotect(host, 0x1000, PROT_NONE) == 0);
    const VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                 .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                 .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                 .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                  .pMemoryBarriers = &mb};
    u32 last = 0;
    for (u32 k = 1; k <= 100; k++) {
        last = 0x01010101u * (k & 0xFF);
        vk_render_end();
        rec_CmdFillBuffer(vk_cmd(), buf, off, 256, last);
        rec_CmdPipelineBarrier2(vk_cmd(), &dep);
        vk_finish();
    }
    CHECK(urr_prot(host, during));
    CHECK(mprotect(host, 0x1000, PROT_READ | PROT_WRITE) == 0);
    vk_unlock();
    u32 differ = 0;
    for (u32 i = 0; i < 256; i++) differ += host[i] != (u8)last;
    const int kept = during[0] == '-' && during[1] == '-';
    if (!kept || differ)
        fprintf(stderr,
                "a trapped region: the mapping was %s before and %s after 100 submissions (want no access), %u of 256 "
                "bytes differ from the GPU's last write (first %02x, want %02x)\n",
                before, during, differ, host[0], (u8)last);
    CHECK(mem_unmap(cpu, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    CHECK(kept && !differ);
    printf("in place: a dma-buf region kept the GPU's writes through 100 submissions with the CPU's mapping trapped "
           "(%s)\n",
           during);
    return 0;
}
/* Plan item 8 of docs/dev/draw-cost.md: a block linear render target over a dma-buf region is watched at a release
   (own_watch_in_place), not published, and its pages stay the region's, which the GPU keeps through the submissions
   after the trap. The CPU's first read publishes it in place in the region (vk_rt_watch_hit), waits for that copy's
   submit, and reads the target's pixels; the pages are still the region's after it */
static int check_uma_region_watch(void) {
    const u64 gva = URR_GVA + 0x800000, cpu = URR_CPU + 0x800000, span = 20ull * 2 * 512 * 16, t = 0x40000;
    const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
    VkBuffer r;
    u64 ro, off;
    if (!vk_uma() || !own_traps()) {
        printf("skip: the watch over a region needs unified memory and traps\n");
        return 0;
    }
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(mem_map(cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, 0x200000);
    memset(GP(cpu), 0x11, 0x200000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    vk_rt_publish_drawn();                       /* the targets the checks before drew */
    CHECK(vk_buf_get_strict(gva, 256, &r, &ro)); /* a region over the window, also where imports are dear */
    vk_finish();
    const int region = vk_uma_region_held(cpu, 0x200000);
    vk_unlock();
    if (!region) {
        CHECK(mem_unmap(cpu, 0x200000) == 0);
        g_renderer->mem_may_have_changed();
        free(ch);
        printf("skip: no dma-buf region holds the window on this device\n");
        return 0;
    }
    const VkRtWatchStats w0 = g_vk_rt_watch;
    rt_clear_rect(ch, gva + t, blue, NULL);
    vk_lock();
    vk_rt_publish_drawn();
    vk_unlock();
    /* watched in place: no publish, the pages still the region's (no pool offset) */
    CHECK(g_vk_rt_watch.arms == w0.arms + 1 && g_vk_rt_watch.by[VK_PUB_IMPORT] == w0.by[VK_PUB_IMPORT]);
    CHECK(g_vk_rt_watch.publishes == w0.publishes && g_vk_rt_watch.hits == w0.hits);
    CHECK(own_watched(cpu + t, span) && own_trapped(cpu + t, span) && mem_pool_offset(cpu + t, &off) != 0);
    /* the GPU goes on with the region's pages: draws to another target and the region's reads, submitted and done (a
       lost device is fatal) */
    for (int k = 0; k < 20; k++) {
        rt_clear_rect(ch, gva + 0x100000, k & 1 ? red : blue, NULL);
        vk_lock();
        CHECK(vk_buf_get_strict(gva, 256, &r, &ro));
        vk_finish();
        vk_unlock();
    }
    vk_lock();
    CHECK(vk_uma_region_held(cpu, 0x200000));
    vk_unlock();
    CHECK(own_watched(cpu + t, span) && g_vk_rt_watch.hits == w0.hits);
    /* this thread holds no lock: its read faults, publishes the target in place and reads its pixels */
    u8 got[64];
    memcpy(got, GP(cpu + t), sizeof got);
    atomic_signal_fence(memory_order_seq_cst);
    u32 differ = 0;
    for (u32 i = 0; i < sizeof got; i += 4)
        differ += got[i] != 0 || got[i + 1] != 0 || got[i + 2] != 255 || got[i + 3] != 255;
    vk_lock();
    const int kept = vk_uma_region_held(cpu, 0x200000);
    vk_unlock();
    const int hit =
        g_vk_rt_watch.hits == w0.hits + 1 && g_vk_rt_watch.by[VK_PUB_CPU_ACCESS] == w0.by[VK_PUB_CPU_ACCESS] + 1;
    if (differ || !hit || !kept || own_trapped(cpu + t, span))
        fprintf(
            stderr,
            "a watch over a region: %u of 16 pixels differ (first %02x %02x %02x %02x, want 00 00 ff ff), %llu hits, "
            "%llu publishes for a CPU access, region kept %d, still trapped %d\n",
            differ, got[0], got[1], got[2], got[3], (unsigned long long)(g_vk_rt_watch.hits - w0.hits),
            (unsigned long long)(g_vk_rt_watch.by[VK_PUB_CPU_ACCESS] - w0.by[VK_PUB_CPU_ACCESS]), kept,
            own_trapped(cpu + t, span));
    CHECK(!differ && hit && kept && !own_trapped(cpu + t, span) && mem_pool_offset(cpu + t, &off) != 0);
    vk_lock();
    vk_rt_before_write(gva, 0x200000, 0); /* the targets go back to memory before it does */
    vk_unlock();
    CHECK(mem_unmap(cpu, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    free(ch);
    printf(
        "in place: a render target over a dma-buf region is watched at a release and published in place at the CPU's "
        "read\n");
    return 0;
}
/* blue pixels of a 320x180 target at guest cpu, as the CPU reads them (a fault, when they are trapped): the count that
   differ of the first 16 */
static u32 urr_blue_differ(u64 cpu, u8 first[4]) {
    u8 got[64];
    memcpy(got, GP(cpu), sizeof got);
    atomic_signal_fence(memory_order_seq_cst);
    memcpy(first, got, 4);
    u32 differ = 0;
    for (u32 i = 0; i < sizeof got; i += 4)
        differ += got[i] != 0 || got[i + 1] != 0 || got[i + 2] != 255 || got[i + 3] != 255;
    return differ;
}
/* a thread that reads the target at its address (urr_reader_at) and keeps how many of its pixels differ, + 1 */
static u64 urr_reader_at;
static _Atomic u32 urr_reader_got;
static void urr_reader(void* unused) {
    (void)unused;
    u8 first[4];
    atomic_store_explicit(&urr_reader_got, urr_blue_differ(urr_reader_at, first) + 1, memory_order_release);
}
/* A dma-buf region lets go while a watch over a render target is on its pages: idle (vk_uma_frame after UMA_IDLE
   presents unused), then dead (a result published GPU-owned over another page of it, which moves that page into the
   pool). Its pages move back to the reservation's own with a copy that a trapped page would fault, under mem.c's lock
   and vk_lock, while the trap thread waited for vk_lock in the landing of another thread's fault there, so it never
   took the copy's: that hung. Here a reader faults on the target first, with vk_lock held. The targets pending there
   are published before the move, and the reader and this thread then read their pixels */
static int check_uma_region_letgo(void) {
    const float blue[4] = {0, 0, 1, 1};
    const u64 span = 20ull * 2 * 512 * 16, t = 0x40000;
    VkBuffer r;
    u64 ro;
    if (!vk_uma() || !own_traps()) {
        printf("skip: the watch over a region needs unified memory and traps\n");
        return 0;
    }
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    for (int dead = 0; dead < 2; dead++) {
        const u64 gva = URR_GVA + 0xA00000 + (u64)dead * 0x200000, cpu = URR_CPU + 0xA00000 + (u64)dead * 0x200000;
        CHECK(mem_map(cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
        gmmu_map(gva, cpu, 0x200000);
        memset(GP(cpu), 0x11, 0x200000);
        g_renderer->mem_may_have_changed();
        vk_lock();
        vk_rt_publish_drawn();
        CHECK(vk_buf_get_strict(gva, 256, &r, &ro));
        vk_finish();
        const int region = vk_uma_region_held(cpu, 0x200000);
        vk_unlock();
        if (!region) {
            CHECK(mem_unmap(cpu, 0x200000) == 0);
            g_renderer->mem_may_have_changed();
            free(ch);
            printf("skip: no dma-buf region holds the window on this device\n");
            return 0;
        }
        rt_clear_rect(ch, gva + t, blue, NULL);
        vk_lock();
        vk_rt_publish_drawn();
        vk_unlock();
        CHECK(own_watched(cpu + t, span));
        VkOwnRes* owner = NULL;
        vk_lock();
        /* the reader's fault first: the trap thread then waits in its landing for vk_lock, which this thread holds */
        const u64 faults = own_faults();
        urr_reader_at = cpu + t;
        atomic_store_explicit(&urr_reader_got, 0, memory_order_release);
        CHECK(plat_thread(urr_reader, NULL, 0) == 0);
        for (int ms = 0; own_faults() == faults; ms++) {
            CHECK(ms < 5000);
            plat_sleep_ns(1000000);
        }
        plat_sleep_ns(20000000);
        if (dead) { /* a page of the region outside the target, GPU-owned: it moves into the pool, the region dies */
            owner = uma_owner(0x1000, 0x42);
            CHECK(owner);
            vk_own_publish_owned(cpu + 0x180000, 0x1000, owner, 0);
            vk_finish();
        } else /* unused for longer than UMA_IDLE presents, at a present that looks for such imports */
            g_vk_present = (g_vk_present / 60 + 12) * 60;
        vk_uma_frame(); /* lets the region go */
        const int gone = !vk_uma_region_held(cpu, 0x200000);
        vk_unlock();
        for (int ms = 0; !atomic_load_explicit(&urr_reader_got, memory_order_acquire); ms++) {
            CHECK(ms < 5000);
            plat_sleep_ns(1000000);
        }
        const u32 reader = atomic_load_explicit(&urr_reader_got, memory_order_acquire) - 1;
        u8 first[4];
        const u32 differ = urr_blue_differ(cpu + t, first) + reader;
        if (differ || !gone)
            fprintf(stderr,
                    "a %s region let go under a watch: gone %d, %u of 16 pixels differ (first %02x %02x %02x "
                    "%02x, want 00 00 ff ff)\n",
                    dead ? "dead" : "idle", gone, differ, first[0], first[1], first[2], first[3]);
        CHECK(gone && !differ);
        if (owner) {
            CHECK(!uma_landed(cpu + 0x180000, 0x1000, 0x42));
            vk_own_res_unref(owner);
        }
        vk_lock();
        vk_rt_before_write(gva, 0x200000, 0);
        vk_unlock();
        CHECK(mem_unmap(cpu, 0x200000) == 0);
        g_renderer->mem_may_have_changed();
    }
    free(ch);
    printf("in place: a region let go, idle and dead, under a watch: the target published first, its pixels read\n");
    return 0;
}
/* A region dies (a page of it moves into the pool under a GPU-owned result) while a watch over a render target is on
   its other pages. A bind meanwhile (here of other memory, as a draw's setup binds) lets no region go and publishes
   nothing: a publish there ended the draw's render pass and waited for the GPU in the middle of its setup. Until the
   next present the region still holds its own pages, and a read in place of them gets the CPU's new bytes; a read of
   the moved page gets that page's new bytes, not the region's old ones. At the present the target is published and
   the region goes, and the CPU reads the target's pixels */
static int check_uma_region_dead_bind(void) {
    const float blue[4] = {0, 0, 1, 1};
    const u64 gva = URR_GVA + 0xE00000, cpu = URR_CPU + 0xE00000, other = URR_GVA + 0x1000000;
    const u64 other_cpu = URR_CPU + 0x1000000, span = 20ull * 2 * 512 * 16, t = 0x40000, moved = 0x180000;
    VkBuffer r, rb;
    u64 ro;
    if (!vk_uma() || !own_traps()) {
        printf("skip: the watch over a region needs unified memory and traps\n");
        return 0;
    }
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(mem_map(cpu, 0x200000, MS_NORMAL, PERM_RW) == 0 && mem_map(other_cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, 0x200000);
    gmmu_map(other, other_cpu, 0x200000);
    memset(GP(cpu), 0x11, 0x200000);
    memset(GP(other_cpu), 0x22, 0x200000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    vk_rt_publish_drawn();
    CHECK(vk_buf_get_strict(gva, 256, &r, &ro));
    vk_finish();
    const int region = vk_uma_region_held(cpu, 0x200000);
    vk_unlock();
    if (!region) {
        CHECK(mem_unmap(cpu, 0x200000) == 0 && mem_unmap(other_cpu, 0x200000) == 0);
        g_renderer->mem_may_have_changed();
        free(ch);
        printf("skip: no dma-buf region holds the window on this device\n");
        return 0;
    }
    rt_clear_rect(ch, gva + t, blue, NULL);
    vk_lock();
    vk_rt_publish_drawn();
    vk_unlock();
    CHECK(own_watched(cpu + t, span));
    /* the region dies: a GPU-owned result over a page outside the target, landed, then the CPU's new bytes there and
       on a page the region still holds */
    vk_lock();
    VkOwnRes* owner = uma_owner(0x1000, 0x42);
    CHECK(owner);
    vk_own_publish_owned(cpu + moved, 0x1000, owner, 0);
    vk_finish();
    vk_unlock();
    CHECK(!uma_landed(cpu + moved, 0x1000, 0x42));
    vk_own_res_unref(owner);
    memset(GP(cpu + moved), 0x99, 0x1000);
    memset(GP(cpu + 0x100000), 0x77, 0x1000);
    /* a bind of other memory: no publish, no submit, the watch kept */
    vk_lock();
    const VkRtWatchStats w0 = g_vk_rt_watch;
    const u64 sub = vk_submit_number();
    CHECK(vk_buf_get_strict(other, 256, &r, &ro));
    const int quiet = vk_submit_number() == sub && g_vk_rt_watch.by[VK_PUB_OTHER] == w0.by[VK_PUB_OTHER] &&
                      own_watched(cpu + t, span);
    /* reads before the present: the region's own page, and the moved one */
    u8* result = vk_readback_buffer(512, &rb);
    CHECK(result);
    CHECK(vk_buf_get_strict(gva + 0x100000, 256, &r, &ro));
    uma_read(r, ro, rb, 0);
    CHECK(vk_buf_get_strict(gva + moved, 256, &r, &ro));
    uma_read(r, ro, rb, 256);
    vk_finish();
    vk_readback_ready();
    u32 differ = 0;
    for (u32 i = 0; i < 256; i++) differ += (result[i] != 0x77) + (result[256 + i] != 0x99);
    const u8 got[2] = {result[0], result[256]};
    vk_unlock();
    if (!quiet || differ)
        fprintf(stderr,
                "a dead region under a watch: the bind of other memory %s; %u of 512 bytes read before the present "
                "differ (first %02x and %02x, want 77 and 99)\n",
                quiet ? "was quiet" : "published, submitted or dropped the watch", differ, got[0], got[1]);
    CHECK(quiet && !differ);
    /* the present: the target published, the region gone, the pixels read */
    vk_lock();
    vk_uma_frame();
    const int gone = !vk_uma_region_held(cpu, 0x200000) && !own_watched(cpu + t, 1); /* the driver's, till its reads */
    vk_unlock();
    u8 first[4];
    const u32 px = urr_blue_differ(cpu + t, first);
    if (!gone || px)
        fprintf(stderr, "a dead region at the present: gone %d, %u of 16 pixels differ (first %02x %02x %02x %02x)\n",
                gone, px, first[0], first[1], first[2], first[3]);
    CHECK(gone && !px && GP(cpu + moved)[5] == 0x99 && GP(cpu + 0x100000)[5] == 0x77);
    vk_lock();
    vk_rt_before_write(gva, 0x200000, 0);
    vk_unlock();
    CHECK(mem_unmap(cpu, 0x200000) == 0 && mem_unmap(other_cpu, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    free(ch);
    printf("in place: a dead region under a watch goes at the present, not in a bind, and reads stay current\n");
    return 0;
}
#endif
