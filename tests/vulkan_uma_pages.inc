/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* The page model of guest memory read in place (gpu/vk/uma.c). A region's table gives each page a blk bit where newer
   bytes of it are elsewhere: the producers of such bytes set it, and the fast path (vk_uma_read) leaves a range with
   such a page to the buffer cache's chain. Each case draws the green of its color from a constant buffer's
   c[0][0x644], read in place in a region, in a window of guest memory of its own. Between two draws that make the
   region and read it, the case puts newer bytes of the word elsewhere, or changes the region:
   - a pending graphics result: a storage write over the word that can't be in place (its range is not one run of
     guest memory), so a GfxOut holds the zeros (buffer.c's g_out_pages);
   - a queued release's payload over the word (release.c's g_rpages);
   - the runtime's own write of 0.0 there (front);
   - a region over its window and the next one, made by a range across their edge, which takes over the region the
     draws read (a merge), then the guest CPU's write of 0.0;
   - the window's pages given back and mapped again (mem.c's pages_replaced), then 0.0 written.
   The draw must give the pixel each case expects; the fast path must leave the word to the chain where newer bytes
   are elsewhere, and bind it with no other check where they are not. The cross-check of
   draw_state_check=1 must find no difference. Elsewhere on unified memory a pending compute result is never made (a
   dispatch's outputs are vk_buf_copy_writable's there, compute.c's deferred), and the mirror is off. A storage image
   not written back (image_out.c's g_img_pages) counts its pages in g_pend_any as the GfxOut's do */
#define UPG_CPU (HEAP_BASE + 0x28000000) /* two windows for each draw of each case */
#define UPG_GVA 0x5e0000000ull           /* 16 MiB of GPU addresses for each */
enum { UPG_GFXOUT, UPG_RELEASE, UPG_FRONT, UPG_MERGE, UPG_REPLACED, UPG_N };
static const char* const upg_name[UPG_N] = {"a pending graphics result", "a queued release's payload",
                                            "the runtime's write", "a region merged over it",
                                            "its pages given back and mapped again"};
/* the pixel the draw of channel gives, its target read back: what came before it is in the same submission */
static int upg_pixel(GpuChannel* channel, u8 px[4]) {
    u64 drawn = g_vk_drawn;
    vk_lock();
    rt_clear(channel, UMA_CB_TARGET, 4, 4, 0, 0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    Surf* surface = vk_rt_find(UMA_CB_TARGET);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(4, &buffer);
    CHECK(surface && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {1, 1, 0}, .imageExtent = {1, 1, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(px, data, 4);
    vk_unlock();
    return 0;
}
/* what case which puts over c[0][0x644] of the window at cpu (GPU address gva) after the draws that read it, or how it
   changes the region; *want the pixel then, *blk 1 when its bytes are elsewhere (the fast path must not take them), 0
   when the fast path must read it, 2 when the region it read went (the chain makes another) */
static int upg_produce(int which, u64 cpu, u64 gva, const u8** want, int* blk) {
    static const u8 red[4] = {255, 0, 0, 255};
    const float zero = 0.0f;
    VkBuffer w, b;
    u64 wo, bo;
    *want = red;
    *blk = 1;
    switch (which) {
        case UPG_GFXOUT: /* zeros over [0x600, 0x1100), whose second page is elsewhere (upg_case): refused in place */
            vk_lock();
            CHECK(vk_buf_copy_writable(gva + 0x600, 0xb00, &w, &wo, 1));
            CHECK(!owned_test_fill(w, wo, 0xb00, 0));
            vk_buf_writable_commit();
            CHECK(vk_buf_pending(gva + 0x600, 0xb00)); /* a result to land, not a write in place */
            vk_unlock();
            return 0;
        case UPG_RELEASE: /* queued before the draw, which reads it: the chain copies the word with it */
            vk_release_write(gva + 0x644, &zero, 4);
            return 0;
        case UPG_FRONT: CHECK(!gmem_write(gva + 0x644, &zero, 4)); return 0;
        case UPG_MERGE: /* a range across the edge of its window and the next: one region of both */
            vk_lock();
            CHECK(vk_buf_get_input(gva + 0x200000 - 0x100, 0x200, 4, &b, &bo));
            CHECK(vk_uma_held(cpu, 0x400000));
            vk_unlock();
            memcpy(GP(cpu) + 0x644, &zero, 4); /* the guest CPU's */
            g_renderer->mem_may_have_changed();
            *blk = 0;
            return 0;
        case UPG_REPLACED:
            CHECK(mem_unmap(cpu, 0x200000) == 0 && mem_map(cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
            g_renderer->mem_may_have_changed();
            memset(GP(cpu), 0, 0x1000); /* 0.0 at 0x644 */
            *blk = 2;                   /* the old region went: the chain makes another */
            return 0;
    }
    return 1;
}
/* case which's draw: its pixel, whether a lookup of the constant buffer as the draw makes it was bound by the fast path
   before the case changed anything (warm) and after it (fast), and whether it was left to the chain over a blk page
   (refused). The draw's vertices may be read in place too: the lookups are
   made alone, between the draws */
typedef struct {
    u8 px[4];
    const u8* want;
    int blk, warm, fast, refused;
} UpgDraw;
/* a lookup of the constant buffer at gva as cb_get makes it: whether the fast path bound it, and whether it left it to
   the chain over a blk page */
static int upg_probe(u64 gva, int* fast, int* refused) {
    VkBuffer b;
    u64 o, h0, b0, h1, b1;
    vk_lock();
    vk_uma_fast_counts(&h0, &b0);
    CHECK(vk_buf_get_uniform(gva, 0x650, &b, &o));
    vk_uma_fast_counts(&h1, &b1);
    vk_unlock();
    *fast = h1 == h0 + 1;
    *refused = b1 == b0 + 1;
    return 0;
}
static int upg_case(int which, UpgDraw* d) {
    const float one = 1.0f;
    const u64 at = (u64)which, cpu = UPG_CPU + at * 0x400000, gva = UPG_GVA + at * 0x1000000;
    u8 px[4];
    int ignored;
    GpuChannel* channel = uma_constants_draw(cpu, gva);
    CHECK(channel);
    /* the next window too, for the merge; the GfxOut's second page elsewhere, before the draws (a mapping change makes
       the fast path resolve the range again) */
    CHECK(mem_map(cpu + 0x200000, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva + 0x200000, cpu + 0x200000, 0x200000);
    if (which == UPG_GFXOUT) gmmu_map(gva + 0x1000, cpu + 0x100000, 0x1000);
    memcpy(GP(cpu) + 0x644, &one, 4);
    g_renderer->mem_may_have_changed();
    CHECK(!upg_pixel(channel, px)); /* the region made, through the chain */
    CHECK(!upg_pixel(channel, px)); /* read in place again */
    CHECK(!upg_probe(gva, &d->warm, &ignored));
    CHECK(!upg_produce(which, cpu, gva, &d->want, &d->blk));
    CHECK(!upg_probe(gva, &d->fast, &d->refused));
    CHECK(!upg_pixel(channel, d->px));
    if (which == UPG_RELEASE) {
        vk_release_submit();
        vk_lock();
        vk_release_drain();
        vk_finish();
        vk_unlock();
    }
    CHECK(!uma_constants_done(channel, cpu));
    CHECK(mem_unmap(cpu + 0x200000, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    return 0;
}
static int check_uma_pages_producers(void) {
    const int copies = g_settings.guest_buffers_copied;
    int bad = 0;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    /* the dma-buf way, whose regions the fast path reads in place under uma_copies=0, else in place everywhere */
    const int dmabuf = vk_uma_can_dmabuf();
    vk_lock();
    if (dmabuf) CHECK(vk_uma_migrate(2));
    vk_unlock();
    if (!dmabuf) g_settings.guest_buffers_copied = 1;
    const u64 diffs = vk_uma_check_diffs();
    for (int which = 0; which < UPG_N; which++) {
        UpgDraw d;
        CHECK(!upg_case(which, &d));
        const int right = !memcmp(d.px, d.want, 4);
        /* the fast path: it read the word before, and over newer bytes elsewhere left it to the chain, else read it
           again (a merged or new region resolved) */
        const int fast = d.warm && (d.blk == 1 ? d.refused && !d.fast : d.blk == 0 ? d.fast : !d.refused);
        if (!right || !fast) {
            fprintf(stderr,
                    "page model, %s: drew %u,%u,%u,%u, expected %u,%u,%u,%u; the fast path read it before %d, after "
                    "%d, left it to the chain %d\n",
                    upg_name[which], d.px[0], d.px[1], d.px[2], d.px[3], d.want[0], d.want[1], d.want[2], d.want[3],
                    d.warm, d.fast, d.refused);
            bad = 1;
        }
    }
    vk_lock();
    if (dmabuf) CHECK(vk_uma_migrate(0));
    vk_finish();
    vk_unlock();
    g_settings.guest_buffers_copied = copies;
    if (vk_uma_check_diffs() != diffs)
        fprintf(stderr, "page model: the cross-check found %llu differences\n",
                (unsigned long long)(vk_uma_check_diffs() - diffs));
    CHECK(!bad && vk_uma_check_diffs() == diffs);
    printf("in place, the page model: a read with newer bytes elsewhere (a pending result, a queued release, the "
           "runtime's write) takes the chain, a merged or new region is read again (%s)\n",
           dmabuf ? "dma-buf regions" : "imports");
    return 0;
}

/* The page model: a write in place is no result added (vk_buf_adds, g_pend_any.adds), which every later draw's caches
   looked again at; it moves only its own count (vk_uma_writes) and pends until its submit is done, for the GPU's and
   the CPU's readers alike */
#define UPG_W  (HEAP_BASE + 0x2d000000)
#define UPG_GW 0x5f0000000ull
static int check_uma_pages_counts(void) {
    VkBuffer w;
    u64 wo;
    u8 bytes[16];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UPG_W, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UPG_GW, UPG_W, 0x200000);
    memset(GP(UPG_W), 0x41, 0x2000);
    vk_lock();
    CHECK(vk_buf_copy_writable(UPG_GW + 0x100, 256, &w, &wo, 1)); /* the region made, the write in place */
    const int inplace = !vk_buf_pending(UPG_GW + 0x100, 256);
    const u64 adds = vk_buf_adds(), any = g_pend_any.adds;
    const u64 writes = vk_uma_writes();
    CHECK(!owned_test_fill(w, wo, 256, 0x42));
    vk_buf_writable_commit();
    const int counted = vk_buf_adds() != adds || g_pend_any.adds != any;
    const int moved = vk_uma_writes() == writes + 1;
    const int pending = vk_uma_pending(UPG_GW + 0x100, 256) && pend_hit(&g_pend_any, UPG_GW + 0x1ff, 1) &&
                        !vk_uma_pending(UPG_GW + 0x2000, 0x100); /* by page */
    const u64 after = vk_uma_writes();
    vk_unlock();
    CHECK(!gmem_read(UPG_GW + 0x100, bytes, sizeof bytes)); /* the runtime's CPU read waits for the write */
    for (u32 i = 0; i < sizeof bytes; i++) CHECK(bytes[i] == 0x42);
    vk_lock();
    vk_finish();
    const int landed = !vk_uma_pending(UPG_GW + 0x100, 256) && vk_uma_writes() == after;
    vk_unlock();
    CHECK(mem_unmap(UPG_W, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    if (!inplace || counted || !moved || !pending || !landed)
        fprintf(stderr,
                "page model counts: in place %d, counted as a result %d, its own count moved %d, pending %d, landed "
                "%d\n",
                inplace, counted, moved, pending, landed);
    CHECK(inplace && !counted && moved && pending && landed);
    printf("in place, the page model: a write in place moves its own count, not the results added, and pends until "
           "its submit is done\n");
    return 0;
}

/* A region of whole windows is the rule, but not where a window's pages are not all mapped (or movable, the migrate
   way): imp_get then imports the range's own pages. Only 128 KiB are mapped here, across the edge of two windows, and
   a dispatch's read of [0x1ff000, 0x201000) in place makes such a region. The runtime then writes a word of the first
   window beside it (front_write), and the region's read is bound again. A page table of whole windows over such a
   region wrote past its end and below its start (a race on the Deck crashed in rg_stamp): such a region has none, and
   its pages use the tables of before. The bytes read in place are checked, in each way the device has */
#define UPG_P  (HEAP_BASE + 0x2e000000)
#define UPG_GP 0x5f8000000ull
static int upg_partial(int way) {
    const u32 word = 0x22222222u;
    VkBuffer b, rb;
    u64 o;
    CHECK(mem_map(UPG_P + 0x1f0000, 0x20000, MS_NORMAL, PERM_RW) == 0); /* no window whole */
    gmmu_map(UPG_GP, UPG_P + 0x1f0000, 0x20000);
    memset(GP(UPG_P + 0x1f0000), 0x21, 0x20000);
    vk_lock();
    if (way) CHECK(vk_uma_migrate(way));
    CHECK(vk_buf_get_strict(UPG_GP + 0xf000, 0x2000, &b, &o)); /* in place: a region of its own two pages */
    const int held = vk_uma_held(UPG_P + 0x1ff000, 0x2000) && !vk_uma_held(UPG_P + 0x1f0000, 0x1000);
    vk_finish();
    vk_unlock();
    CHECK(!gmem_write(UPG_GP + 0x100, &word, 4)); /* the runtime's, in the first window, beside the region */
    memset(GP(UPG_P + 0x1ff000), 0x23, 0x2000);   /* the guest CPU's, in the region */
    g_renderer->mem_may_have_changed();
    vk_lock();
    u8* data = vk_readback_buffer(0x2000, &rb);
    CHECK(data && vk_buf_get_strict(UPG_GP + 0xf000, 0x2000, &b, &o));
    VkBufferCopy c = {o, 0, 0x2000};
    rec_CmdCopyBuffer(vk_cmd(), b, rb, 1, &c);
    vk_finish();
    vk_readback_ready();
    u32 wrong = 0;
    for (u32 i = 0; i < 0x2000; i++) wrong += data[i] != 0x23;
    if (way) CHECK(vk_uma_migrate(0));
    vk_finish();
    vk_unlock();
    const int kept = snx_ld_le32(GP(UPG_P + 0x1f0100)) == word;
    CHECK(mem_unmap(UPG_P + 0x1f0000, 0x20000) == 0);
    g_renderer->mem_may_have_changed();
    if (!held || wrong || !kept)
        fprintf(stderr,
                "page model, a region of part of a window (way %d): held %d, %u bytes read wrong, the word %d\n", way,
                held, wrong, kept);
    CHECK(held && !wrong && kept);
    return 0;
}
static int check_uma_pages_partial(void) {
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(!upg_partial(0));
    if (vk_uma_can_dmabuf()) CHECK(!upg_partial(2));
    printf("in place, the page model: a region of part of a window, which the runtime writes beside, keeps no page "
           "table and reads right (%s)\n",
           vk_uma_can_dmabuf() ? "imports and dma-bufs" : "imports");
    return 0;
}
