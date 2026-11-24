/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One target per address, where the two paths that name it know different strides (surface.c rt_create_).
   - A 2D copy names a target by its guest address, its size and its format, and nothing of the stride of its single
     layer: blit2d's vk_rt passes no layer size, which becomes the least that holds the pixels, while the draw that
     made the target gave the stride the game laid out. When another shape has parked that target, the copy must take
     it back instead of making a second target at the address. A second one takes the copy's pixels where no draw and
     no texture looks, which is how a minimap lost its terrain every frame.
   - The same target in another format class (sRGB over UNORM) takes the pixels of the one it parks (rt_take_pixels).
     Its single layer's stride is padding, not part of the pixels: one path's stride must not keep the other from
     taking them, or the target starts blank where memory has nothing.
   Lavapipe and a discrete GPU differ in nothing here: this is the renderer's own bookkeeping, no pixel leaves its
   image but for the readback that asks what a target holds */
#define RSTR_W       64u
#define RSTR_H       64u
#define RSTR_BH      4u /* 16 GOBs of 8 rows: a row of 64 RGBA8 pixels is one GOB */
#define RSTR_TILE    (RSTR_BH << 4)
#define RSTR_FMT     0xD5u  /* A8B8G8R8 UNORM */
#define RSTR_FMT_ALT 0xD6u  /* A8B8G8R8 sRGB: another format class of the same bytes */
#define RSTR_STRIDE  0x100000ull /* more than a 64 by 64 layer of that format holds: its padding alone */
#define RSTR_SPAN    0x20000u
#define RSTR_CPU     (HEAP_BASE + 0x3e000000)
#define RSTR_GVA     0xaa0000000ull
#define RSTR_RED     0xFF0000FFu
#define RSTR_GREEN   0xFF00FF00u
/* the 64 by 64 target at gva, its single layer laid out in stride (0: the least the pixels need) */
static Surf* rstr_target(u64 gva, u32 fmt, u64 stride) {
    return vk_rt_array(gva, RSTR_W, RSTR_H, fmt, RSTR_TILE, 0, 1, stride, 0, DS_MSAA_1X1);
}
/* another shape at gva: it takes the address and parks what held it */
static Surf* rstr_other(u64 gva) { return vk_rt_array(gva, 32, 32, RSTR_FMT, RSTR_TILE, 0, 1, RSTR_STRIDE, 0, DS_MSAA_1X1); }
/* every pixel of t cleared to colour, as a draw does: on the GPU, the newest write the target has (vk_lock held) */
static void rstr_clear(Surf* t, u32 colour) {
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue c = {.float32 = {(float)(colour & 255) / 255.0f, (float)(colour >> 8 & 255) / 255.0f,
                                       (float)(colour >> 16 & 255) / 255.0f, (float)(colour >> 24) / 255.0f}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    rec_CmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &all);
    vk_surf_wrote(t);
    vk_rt_drawn(t);
}
static int check_rt_stride(void) {
    CHECK(mem_map(RSTR_CPU, RSTR_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(RSTR_GVA, RSTR_CPU, RSTR_SPAN);
    /* guest bytes no target on the GPU holds: what a target reads that loaded itself from memory, 0x40404040 a pixel */
    memset(GP(RSTR_CPU), 0x40, RSTR_SPAN);
    g_renderer->mem_may_have_changed();

    vk_lock();
    Surf* t = rstr_target(RSTR_GVA, RSTR_FMT, RSTR_STRIDE);
    CHECK(t);
    rstr_clear(t, RSTR_RED);
    Surf* other = rstr_other(RSTR_GVA);
    CHECK(other && other != t && (t->gva >> 63));
    /* the 2D copy's call, which knows no stride: the target the draw made has to come back */
    Surf* back = vk_rt(RSTR_GVA, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 0);
    vk_unlock();
    if (!back || back != t || (back->gva >> 63)) {
        fprintf(stderr,
                "a 2D copy named the 64x64 target at %llx that the draw made, its single layer laid out in stride "
                "%llu: %p came back, not %p, %s\n",
                (unsigned long long)RSTR_GVA, (unsigned long long)RSTR_STRIDE, (void*)back, (void*)t,
                back && back == t ? "and it is left parked" : "and the draw's target is still parked");
        return 1;
    }
    u32 got = ct_texel(t, 0, 0);
    if (got != RSTR_RED) {
        fprintf(stderr,
                "a 2D copy took back the target at %llx that the draw left red: a texel reads %08x, not %08x\n",
                (unsigned long long)RSTR_GVA, got, RSTR_RED);
        return 1;
    }

    /* the same target in another format class, whose single layer the copy's path laid out in the least stride */
    const u64 gva = RSTR_GVA + 0x10000;
    vk_lock();
    Surf* unorm = rstr_target(gva, RSTR_FMT, 0);
    CHECK(unorm);
    rstr_clear(unorm, RSTR_GREEN);
    const u64 least = unorm->layer_size;
    Surf* srgb = rstr_target(gva, RSTR_FMT_ALT, RSTR_STRIDE);
    vk_unlock();
    if (least == RSTR_STRIDE) {
        fprintf(stderr, "the least stride of a 64x64 layer of format %x is %llu: the case has no second stride\n",
                RSTR_FMT, (unsigned long long)least);
        return 1;
    }
    if (!srgb || srgb == unorm || !(unorm->gva >> 63)) {
        fprintf(stderr, "format %x at %llx did not take the address of format %x: %p came back, not a new target, and "
                        "%p is parked %d\n",
                RSTR_FMT_ALT, (unsigned long long)gva, RSTR_FMT, (void*)srgb, (void*)unorm, !!(srgb && unorm->gva >> 63));
        return 1;
    }
    got = ct_texel(srgb, 0, 0);
    if (got != RSTR_GREEN) {
        fprintf(stderr,
                "the target in format %x at %llx took the address of the one in format %x, strides %llu and %llu: a "
                "texel reads %08x, not the %08x the other one held\n",
                RSTR_FMT_ALT, (unsigned long long)gva, RSTR_FMT, (unsigned long long)least,
                (unsigned long long)RSTR_STRIDE, got, RSTR_GREEN);
        return 1;
    }

    CHECK(mem_unmap(RSTR_CPU, RSTR_SPAN) == 0);
    puts("render targets named with only their stride different: one target per address, and the pixels moved over "
         "when another format class takes the address");
    return 0;
}

/* What one target per address must not fuse, and what it must. The 64 by 64 target of a game's stride is the holder of
   its address, then parked by another shape; each other shape asked while it is either stays a target of its own: a
   layer more, a volume, another block height, width, height, format, sample count or pitch. The padding alone fuses:
   the copy's least stride, a greater one (which stays), and no smaller one lowers it */
#define RSTS_CPU (HEAP_BASE + 0x3a000000)
#define RSTS_GVA 0xb80000000ull
static Surf* rsts_other(u64 gva, int n) {
    switch (n) {
    case 0: return vk_rt_array(gva, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 2, 0x8000, 0, DS_MSAA_1X1);
    case 1: return vk_rt_array(gva, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 1, 0, 1, DS_MSAA_1X1);
    case 2: return vk_rt_array(gva, RSTR_W, RSTR_H, RSTR_FMT, 3u << 4, 0, 1, 0, 0, DS_MSAA_1X1);
    case 3: return vk_rt_array(gva, RSTR_W, RSTR_H / 2, RSTR_FMT, RSTR_TILE, 0, 1, 0, 0, DS_MSAA_1X1);
    case 4: return vk_rt_array(gva, RSTR_W / 2, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 1, 0, 0, DS_MSAA_1X1);
    case 5: return rstr_target(gva, RSTR_FMT_ALT, 0);
    case 6: return vk_rt_array(gva, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 1, 0, 0, DS_MSAA_2X1);
    default: return NULL;
    }
}
#define RSTS_OTHERS 7
/* texel (x, y) of layer k of t */
static u32 rsts_texel(Surf* t, u32 x, u32 y, u32 k) {
    VkBuffer buffer;
    vk_lock();
    u8* p = vk_readback_buffer(4, &buffer);
    u32 v = 0xdeadbeefu;
    if (p) {
        VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, k, 1},
                               .imageOffset = {(i32)x, (i32)y, 0},
                               .imageExtent = {1, 1, 1}};
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &c);
        vk_finish();
        vk_readback_ready();
        v = snx_ld_le32(p);
    }
    vk_unlock();
    return v;
}
/* each layer of t cleared, as a draw does: the first to colour, the others to its complement in red and green */
static void rsts_clear_layers(Surf* t, u32 colour) {
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (u32 k = 0; k < t->layers; k++) {
        const u32 c32 = k ? colour ^ 0x0000FFFFu : colour;
        VkClearColorValue c = {.float32 = {(float)(c32 & 255) / 255.0f, (float)(c32 >> 8 & 255) / 255.0f,
                                           (float)(c32 >> 16 & 255) / 255.0f, (float)(c32 >> 24) / 255.0f}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, k, 1};
        rec_CmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &all);
    }
    vk_surf_wrote(t);
    vk_rt_drawn(t);
}
static int check_rt_stride_apart(void) {
    CHECK(mem_map(RSTS_CPU, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(RSTS_GVA, RSTS_CPU, 0x200000);
    memset(GP(RSTS_CPU), 0x40, 0x200000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    Surf* t = rstr_target(RSTS_GVA, RSTR_FMT, RSTR_STRIDE);
    CHECK(t);
    CHECK(vk_rt(RSTS_GVA, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 0) == t && t->layer_size == RSTR_STRIDE);
    CHECK(rstr_target(RSTS_GVA, RSTR_FMT, RSTR_STRIDE + 0x80000) == t && t->layer_size == RSTR_STRIDE + 0x80000);
    CHECK(rstr_target(RSTS_GVA, RSTR_FMT, RSTR_STRIDE) == t && t->layer_size == RSTR_STRIDE + 0x80000);
    /* parked, then named with more padding than it had: it keeps the greater */
    CHECK(rstr_other(RSTS_GVA) != t && (t->gva >> 63));
    CHECK(rstr_target(RSTS_GVA, RSTR_FMT, RSTR_STRIDE + 0x100000) == t && t->layer_size == RSTR_STRIDE + 0x100000);
    for (int n = 0; n < RSTS_OTHERS; n++)
        for (int parked = 0; parked < 2; parked++) {
            const u64 g = RSTS_GVA + 0x40000 + (u64)(n * 2 + parked) * 0x10000;
            Surf* k = rstr_target(g, RSTR_FMT, RSTR_STRIDE);
            CHECK(k);
            if (parked) CHECK(rstr_other(g) != k && (k->gva >> 63));
            Surf* x = rsts_other(g, n);
            if (x == k) { /* none where the device makes no target of that shape */
                fprintf(stderr, "shape %d asked at %llx with the 64x64 target %s: %p is the 64x64 one\n", n,
                        (unsigned long long)g, parked ? "parked" : "holding the address", (void*)x);
                return 1;
            }
        }
    /* a target of more layers, or a volume, is not the 64 by 64 one's, holding the address or parked */
    for (int n = 0; n < 2; n++)
        for (int parked = 0; parked < 2; parked++) {
            const u64 g = RSTS_GVA + 0x180000 + (u64)(n * 2 + parked) * 0x10000;
            Surf* k = rsts_other(g, n);
            CHECK(k);
            if (parked) CHECK(rstr_other(g) != k && (k->gva >> 63));
            CHECK(vk_rt(g, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 0) != k);
        }
    /* pitch linear: the least stride of its pitch fuses, another pitch does not */
    Surf* p = vk_rt_array(RSTS_GVA + 0x1c0000, RSTR_W, RSTR_H, RSTR_FMT, 0x1000u, 256, 1, RSTR_STRIDE, 0, DS_MSAA_1X1);
    CHECK(p && vk_rt(RSTS_GVA + 0x1c0000, RSTR_W, RSTR_H, RSTR_FMT, 0x1000u, 0, 256) == p);
    CHECK(vk_rt(RSTS_GVA + 0x1c0000, RSTR_W, RSTR_H, RSTR_FMT, 0x1000u, 0, 512) != p);
    Surf* q =
        vk_rt_array(RSTS_GVA + 0x1d0000, RSTR_W, RSTR_H, RSTR_FMT, 0x1000u, 256, 1, RSTR_STRIDE, 0, DS_MSAA_1X1);
    CHECK(q && rstr_other(RSTS_GVA + 0x1d0000) != q && (q->gva >> 63));
    CHECK(vk_rt(RSTS_GVA + 0x1d0000, RSTR_W, RSTR_H, RSTR_FMT, 0x1000u, 0, 512) != q);
    /* two layers of another class and another stride: only their first layers are the same memory, so the pixels are
       not taken over and memory is loaded, with the first target's layers as it was published when it was parked */
    Surf* two = vk_rt_array(RSTS_GVA + 0x1e0000, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 2, 0x8000, 0, DS_MSAA_1X1);
    CHECK(two);
    rsts_clear_layers(two, RSTR_GREEN);
    Surf* twice = vk_rt_array(RSTS_GVA + 0x1e0000, RSTR_W, RSTR_H, RSTR_FMT_ALT, RSTR_TILE, 0, 2, 0x10000, 0,
                              DS_MSAA_1X1);
    CHECK(twice && twice != two);
    vk_unlock();
    CHECK(rsts_texel(twice, 0, 0, 0) == RSTR_GREEN);
    CHECK(rsts_texel(twice, 0, 0, 1) == 0x40404040u); /* past the end of the first's two layers */
    CHECK(mem_unmap(RSTS_CPU, 0x200000) == 0);
    puts("render targets apart: the holder is one target, its stride never lowered, and layers, volumes and other "
         "shapes stay targets of their own");
    return 0;
}

/* The CPU writes the target's memory after the draw. Memory is newer than the target then, as a texture over it
   takes memory's bytes: a target named by the copy must not go on with the draw's, whether it held the address or was
   parked. A 2D copy's new target loaded them */
static int rstw_case(int park, u32 at) {
    const u64 gva = RSTS_GVA + 0x180000 + at;
    vk_lock();
    Surf* d = rstr_target(gva, RSTR_FMT, RSTR_STRIDE);
    CHECK(d);
    rstr_clear(d, RSTR_RED);
    if (park) CHECK(rstr_other(gva) != d && (d->gva >> 63));
    vk_finish();
    vk_unlock();
    memset(GP(RSTS_CPU + 0x180000 + at), 0x55, 0x10000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    Surf* back = vk_rt(gva, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 0);
    CHECK(back);
    vk_unlock();
    const u32 got = ct_texel(back, 0, 0);
    if (got != 0x55555555u)
        fprintf(stderr,
                "the CPU wrote 55555555 over a 64x64 target the draw left red, %s, and a 2D copy named it in a smaller "
                "stride: a texel reads %08x\n",
                park ? "parked" : "holding the address", got);
    return got != 0x55555555u;
}
/* Parked, and the target of another format class that holds the address is the newer one: it takes that one's pixels on
   the GPU, whatever the strides, as memory holds them since it was published when it was parked but is not newer than
   its draw */
static int rstw_newer(void) {
    const u64 gva = RSTS_GVA + 0x1c0000;
    VkRtCopyCounts was, now;
    vk_lock();
    Surf* d = rstr_target(gva, RSTR_FMT, RSTR_STRIDE);
    CHECK(d);
    rstr_clear(d, RSTR_RED);
    Surf* newer = rstr_target(gva, RSTR_FMT_ALT, 0);
    CHECK(newer && newer != d && (d->gva >> 63));
    CHECK(vk_surf_flush_size(d)); /* newer took its pixels: it was not published as it was parked */
    rstr_clear(newer, RSTR_GREEN);
    vk_rt_copy_counts(&was);
    Surf* back = vk_rt(gva, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 0);
    vk_rt_copy_counts(&now);
    CHECK(back == d);
    CHECK(now.written[VK_RT_COPY_TAKE] == was.written[VK_RT_COPY_TAKE] + 1);
    vk_unlock();
    const u32 got = ct_texel(back, 0, 0);
    if (got != RSTR_GREEN)
        fprintf(stderr,
                "the 64x64 target of format %x, parked under a newer one of format %x that the draw left green, was "
                "named in a smaller stride: a texel reads %08x, not %08x\n",
                RSTR_FMT, RSTR_FMT_ALT, got, RSTR_GREEN);
    return got != RSTR_GREEN;
}
/* The CPU writes none of it: the draw's pixels stay, held or parked, as nothing is newer than them */
static int rstw_kept(int park, u32 at) {
    const u64 gva = RSTS_GVA + 0x100000 + at;
    vk_lock();
    Surf* d = rstr_target(gva, RSTR_FMT, RSTR_STRIDE);
    CHECK(d);
    rstr_clear(d, RSTR_RED);
    if (park) CHECK(rstr_other(gva) != d && (d->gva >> 63));
    Surf* back = vk_rt(gva, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 0);
    CHECK(back == d);
    vk_unlock();
    const u32 got = rsts_texel(back, 0, 0, 0);
    if (got != RSTR_RED)
        fprintf(stderr,
                "a 64x64 target the draw left red, %s, with no CPU write since, and a 2D copy named it in a smaller "
                "stride: a texel reads %08x\n",
                park ? "parked" : "holding the address", got);
    return got != RSTR_RED;
}
/* Parked, the CPU writes part of its memory: what the draw left elsewhere stays. The target was published as it was
   parked, so memory holds the draw's pixels under the CPU's, and a load of it (the target named in a smaller stride)
   takes both. Without it the load took the stale bytes of memory under the rest of the target: the draw was nowhere
   but in the target, which its parking took from the watch (rt_pages) */
static int rstw_parked(void) {
    const u64 gva = RSTS_GVA + 0x1e0000;
    vk_lock();
    Surf* d = rstr_target(gva, RSTR_FMT, RSTR_STRIDE);
    CHECK(d);
    rstr_clear(d, RSTR_RED);
    CHECK(rstr_other(gva) != d && (d->gva >> 63));
    CHECK(!vk_surf_flush_size(d)); /* published as it was parked, none of its pixels missing from memory */
    vk_finish();
    vk_unlock();
    memset(GP(RSTS_CPU + 0x1e0000), 0x55, 0x1000); /* its first page: 16 columns of the 64, whole block rows */
    g_renderer->mem_may_have_changed();
    vk_lock();
    Surf* back = vk_rt(gva, RSTR_W, RSTR_H, RSTR_FMT, RSTR_TILE, 0, 0);
    CHECK(back);
    vk_unlock();
    const u32 wrote = rsts_texel(back, 0, 0, 0), kept = rsts_texel(back, 32, 0, 0);
    if (wrote != 0x55555555u || kept != RSTR_RED)
        fprintf(stderr,
                "the CPU wrote 55555555 over the first columns of a 64x64 target the draw left red, parked, and it was "
                "named in a smaller stride: a texel there reads %08x, one the CPU wrote not %08x\n",
                wrote, kept);
    return wrote != 0x55555555u || kept != RSTR_RED;
}
static int check_rt_stride_written(void) {
    CHECK(mem_map(RSTS_CPU, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(RSTS_GVA, RSTS_CPU, 0x200000);
    const int bad = rstw_case(0, 0) + rstw_case(1, 0x20000) + rstw_newer() + rstw_parked() +
                    rstw_kept(0, 0) + rstw_kept(1, 0x20000);
    CHECK(mem_unmap(RSTS_CPU, 0x200000) == 0);
    if (bad) return 1;
    puts("a target named in a smaller stride after the CPU wrote its memory: memory's bytes, holding it or parked, the "
         "draw's where the CPU wrote none, and the pixels of a newer one of another format class over it");
    return 0;
}
