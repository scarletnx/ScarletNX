/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Textures whose levels or Z slices a game draws through 2D render targets over their memory (texture.c's
   tex_take_drawn): an RGBA8 volume a slice at a time through RGBA8 targets; a BC4 volume a slice at a time through
   RGBA16UI targets of a texel a block, as a game compresses its noise volumes; and a 2D BC4 texture through one such
   target. Each slice or level holds its target's texel, not memory's zeros, at the guest's size: at a resolution scale
   of 1, and of 2, where each target's image is twice the guest's size and a target of another format than the
   texture's goes through an image of its own format at the guest's size */
#define ST_GPU  0x8a8000000ull
#define ST_CPU  (HEAP_BASE + 0x12c00000)
#define ST_SPAN 0x80000ull

/* bytes of t's level 0 at slice or layer z, w x h of its texels, into out. Under vk_lock */
static int st_read(Surf* t, u32 z, u32 bytes, u8* out) {
    VkBuffer buffer;
    u8* d = vk_readback_buffer(bytes, &buffer);
    int volume = t->type == VK_IMAGE_VIEW_TYPE_3D;
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, volume ? 0 : z, 1},
                              .imageOffset = {0, 0, volume ? (i32)z : 0},
                              .imageExtent = {t->w, t->h, 1}};
    CHECK(d);
    vk_render_end();
    vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, d, bytes);
    return 0;
}
/* a texture of tic0's format, w x h texels (blocks of bw x bw), slices of them deep when volume else 2D, in block
   linear blocks 2 GOBs high at gva, each slice drawn as a target of rt_format, unit bytes a texel (a block's) */
static int st_case(GpuChannel* ch, const char* what, u64 gva, u32 tic0, u32 w, u32 h, u32 bw, int volume, u32 rt_format,
                   u32 unit) {
    const u32 tw = w / bw, th = h / bw, slices = volume ? 2 : 1, bytes = tw * th * unit;
    const u64 slab = bytes; /* a slice of whole GOBs, its rows 64 bytes or more and 16 of them */
    const u32 tic[8] = {tic0,
                        (u32)gva,
                        (u32)(gva >> 32) | 3u << 21,
                        1u << 3,
                        (w - 1) | (volume ? 2u : 1u) << 23,
                        (h - 1) | (slices - 1) << 16,
                        0,
                        0};
    u8 got[2048], want[16];
    for (u32 z = 0; z < slices; z++) rt_clear_format(ch, gva + z * slab, tw, th, 1, (int)z, rt_format);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    CHECK(t && t->w == w && t->h == h);
    for (u32 z = 0; z < slices; z++) {
        Surf* s = vk_rt_find(gva + z * slab);
        CHECK(s && bytes <= sizeof got);
        if (g_vk_scale > 1.0f) CHECK(s->sw == 2 * tw && s->sh == 2 * th);
        /* the target's first texel, which its clear made every texel */
        VkBuffer buffer;
        u8* d = vk_readback_buffer(unit, &buffer);
        VkBufferImageCopy one = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
        CHECK(d);
        vk_render_end();
        vk_surf_use(vk_cmd(), s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &one);
        vk_finish();
        vk_readback_ready();
        memcpy(want, d, unit);
        CHECK(!st_read(t, z, bytes, got));
        for (u32 i = 0; i < bytes; i += unit)
            if (memcmp(got + i, want, unit)) {
                fprintf(stderr,
                        "slices from targets, %s at scale %g: slice %u, texel or block %u is %02x%02x%02x%02x, its "
                        "target's %02x%02x%02x%02x\n",
                        what, (double)g_vk_scale, z, i / unit, got[i], got[i + 1], got[i + 2], got[i + 3], want[0],
                        want[1], want[2], want[3]);
                vk_unlock();
                return 1;
            }
    }
    vk_unlock();
    return 0;
}
static int check_slices_from_targets(void) {
    /* UNORM in each component, red green blue alpha */
    const u32 unorm = 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    const u32 rgba8 = 0x08 | unorm, bc4 = 0x27 | unorm;
    GpuChannel* ch = calloc(1, sizeof *ch);
    float scale = g_vk_scale;
    int bc4_3d = vk_tic_format_3d(bc4, 0).vk == VK_FORMAT_BC4_UNORM_BLOCK,
        bc4_2d = vk_tic_format(bc4, 0).vk == VK_FORMAT_BC4_UNORM_BLOCK;
    CHECK(ch);
    CHECK(mem_map(ST_CPU, ST_SPAN, MS_NORMAL, PERM_RW) == 0);
    memset(GP(ST_CPU), 0, ST_SPAN);
    gmmu_map(ST_GPU, ST_CPU, ST_SPAN);
    for (u32 k = 0; k < 2; k++) { /* each scale's targets at addresses of their own */
        u64 at = ST_GPU + k * 0x30000;
        g_vk_scale = k ? 2.0f : 1.0f;
        CHECK(!st_case(ch, "an RGBA8 volume from RGBA8 targets", at, rgba8, 16, 16, 1, 1, 0xD5, 4));
        if (bc4_3d) CHECK(!st_case(ch, "a BC4 volume from RGBA16UI targets", at + 0x10000, bc4, 64, 64, 4, 1, 0xC9, 8));
        if (bc4_2d)
            CHECK(!st_case(ch, "a BC4 texture from an RGBA16UI target", at + 0x20000, bc4, 64, 64, 4, 0, 0xC9, 8));
    }
    g_vk_scale = scale;
    vk_lock();
    vk_rt_before_write(ST_GPU, ST_SPAN); /* the targets go back to memory before it does */
    vk_unlock();
    CHECK(mem_unmap(ST_CPU, ST_SPAN) == 0);
    free(ch);
    printf("slices from targets, at resolution scales 1 and 2: %s\n",
           bc4_3d && bc4_2d ? "an RGBA8 volume, a BC4 volume and a BC4 texture from RGBA16UI targets each hold their "
                              "targets' texels"
                            : "an RGBA8 volume holds its targets' texels (BC4 images unavailable)");
    return 0;
}
