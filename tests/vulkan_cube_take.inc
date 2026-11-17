/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A cube whose faces are drawn as render targets and whose mip 1 a dispatch writes as a storage image (texture.c
   tex_take_drawn, take_cover), as one game's reflection cubes are made each frame. The
   targets have a taller block height than the cube's level 0, so they span the face's other levels too. The storage
   write is the tail of a dispatch (compute.c): the surface written, published over guest memory, and registered as
   stored. Then the cube is looked up, and level 0 of each face and level 1 of face 0 must hold the colors drawn and
   stored, taken on the GPU, its loads of pieces leaving those out */
#define CT_GVA  0xd80000000ull
#define CT_CPU  (HEAP_BASE + 0x2f800000)
#define CT_SPAN 0x200000ull
#define CT_SIDE 64u
/* a 64x64 RGBA8 target at gva of block height 2^bh GOBs, cleared to rgba (floats of 0 to 1) */
static void ct_clear(GpuChannel* ch, u64 gva, u32 w, u32 h, u32 bh, const float rgba[4]) {
    u32* r = ch->r3d;
    r[0x487] = 1;
    r[0x200] = (u32)(gva >> 32);
    r[0x201] = (u32)gva;
    r[0x202] = w;
    r[0x203] = h;
    r[0x204] = 0xD5;
    r[0x205] = bh << 4;
    r[0x206] = 1;
    r[0x207] = 0;
    memcpy(&r[0x360], rgba, 16);
    engine_clear(ch, 15 << 2);
    vk_lock();
    vk_render_end();
    vk_unlock();
}
/* texel (0, 0) of level l of layer k of t's image */
static u32 ct_texel(Surf* t, u32 l, u32 k) {
    VkBuffer buffer;
    vk_lock();
    u8* p = vk_readback_buffer(4, &buffer);
    u32 v = 0xdeadbeefu;
    if (p) {
        VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, k, 1}, .imageExtent = {1, 1, 1}};
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
typedef struct {
    NvnTexture cube;
    NvnTexLayout lay;
    u32 tic[8];
} CtCube;
static int ct_cube(CtCube* c, u64 gva) {
    *c = (CtCube){0};
    c->cube.d.target = NVN_TEXTURE_TARGET_CUBEMAP;
    c->cube.d.format = NVN_FORMAT_RGBA8;
    c->cube.d.levels = 7;
    c->cube.d.width = c->cube.d.height = CT_SIDE;
    c->cube.d.depth = 6;
    c->cube.gpu = gva;
    nvn_tex_layout(&c->cube.d, &c->lay);
    CHECK(c->lay.size <= 0x40000 && nvn_tic_encode(&c->cube, NULL, 0, 0, c->tic));
    return 0;
}
/* the dispatch's tail for level l of face k of the cube: its storage surface (a 2D view of the level) cleared on the
   GPU to color, published, and registered as stored */
static int ct_store(const CtCube* c, u32 l, u32 k, u32 color) {
    NvnTexture v = {0};
    v.d.target = NVN_TEXTURE_TARGET_2D;
    v.d.format = NVN_FORMAT_RGBA8;
    v.d.levels = 1;
    v.d.width = v.d.height = CT_SIDE >> l;
    v.d.depth = 1;
    v.gpu = c->cube.gpu + (u64)k * c->lay.layer_size + nvn_tex_level_offset(&c->cube.d, l);
    u32 tic[8];
    CHECK(nvn_tic_encode(&v, NULL, 0, 0, tic));
    vk_lock();
    Surf* s = vk_tex_from_tic_storage(tic);
    CHECK(s);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue rgba = {.float32 = {(float)(color & 255) / 255.0f, (float)(color >> 8 & 255) / 255.0f,
                                          (float)(color >> 16 & 255) / 255.0f, (float)(color >> 24) / 255.0f}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    rec_CmdClearColorImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &rgba, 1, &all);
    vk_surf_wrote(s);
    if (!vk_surf_publish(s))
        vk_surf_flush(s, NULL);
    else
        vk_surf_stored(s);
    vk_unlock();
    return 0;
}
static const u32 g_ct_faces[6] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu, 0xffff00ffu, 0xffffff00u};
static const u32 g_ct_mip = 0xff808080u;
/* one frame of the pattern into the cube at gva (fresh guest memory: stale gray), then its lookup: 1 when a texel is
   not the one drawn or stored */
static int ct_frame(GpuChannel* ch, const CtCube* c, u32 bump) {
    unsigned th = 0, td = 0;
    nvn_tex_level_tiles(&c->cube.d, 0, &th, &td);
    for (u32 k = 0; k < 6; k++) {
        const u32 col = g_ct_faces[k] ^ bump;
        const float rgba[4] = {(float)(col & 255) / 255.0f, (float)(col >> 8 & 255) / 255.0f,
                               (float)(col >> 16 & 255) / 255.0f, (float)(col >> 24) / 255.0f};
        /* a block height one step taller than the level's: the target spans the face's other levels */
        ct_clear(ch, c->cube.gpu + (u64)k * c->lay.layer_size, CT_SIDE, CT_SIDE, th + 1, rgba);
    }
    CHECK(!ct_store(c, 1, 0, g_ct_mip ^ bump));
    vk_lock();
    Surf* t = vk_tex_from_tic(c->tic);
    vk_unlock();
    CHECK(t);
    int bad = 0;
    for (u32 k = 0; k < 6; k++) {
        u32 got = ct_texel(t, 0, k);
        if (got != (g_ct_faces[k] ^ bump)) {
            fprintf(stderr, "cube take: face %u level 0 holds %08x, drawn %08x\n", k, got, g_ct_faces[k] ^ bump);
            bad = 1;
        }
    }
    u32 got = ct_texel(t, 1, 0);
    if (got != (g_ct_mip ^ bump)) {
        fprintf(stderr, "cube take: face 0 level 1 holds %08x, stored %08x\n", got, g_ct_mip ^ bump);
        bad = 1;
    }
    return bad;
}
static int check_cube_take(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    ch->id = 145;
    CHECK(mem_map(CT_CPU, CT_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(CT_GVA, CT_CPU, CT_SPAN);
    int failed = 0;
    CtCube c;
    CHECK(!ct_cube(&c, CT_GVA));
    memset(GP(CT_CPU), 0x40, (size_t)c.lay.size);
    u64 before[8], after[8];
    vk_tex_take_counts(before);
    for (u32 f = 0; f < 3; f++) {
        g_renderer->mem_may_have_changed();
        g_vk_present++;
        failed |= ct_frame(ch, &c, f * 0x00010101u);
    }
    vk_tex_take_counts(after);
    printf("cube take: %llu targets and %llu storage surfaces taken, %llu KB of pieces left out, %llu not taken with "
           "newer memory\n",
           (unsigned long long)(after[0] - before[0]), (unsigned long long)(after[1] - before[1]),
           (unsigned long long)((after[7] - before[7]) >> 10), (unsigned long long)(after[5] - before[5]));
    if (after[1] == before[1] || after[6] == before[6]) {
        fprintf(stderr, "cube take: no storage surface was taken, or no piece left out\n");
        failed = 1;
    }
    free(ch);
    if (!failed) puts("PASS cube take: faces drawn and a mip stored are in the cube, from memory and on the GPU");
    return failed;
}
/* The race's pattern (SNX_TEST_NVN_CASE=cube_take_frames): each frame the 6 faces drawn, then every level from 1 of
   every face stored as a dispatch stores it, then the cube looked up, 8 frames. After the first frame the cube must
   load no pieces from memory: it holds what the targets and the stored surfaces give on the GPU. Each frame's loads
   of pieces, their bytes and the takes are printed, and a stored level and a drawn face checked */
static int check_cube_take_frames(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    ch->id = 147;
    CHECK(mem_map(CT_CPU, CT_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(CT_GVA, CT_CPU, CT_SPAN);
    CtCube c;
    CHECK(!ct_cube(&c, CT_GVA + 0x100000));
    memset(GP(CT_CPU + 0x100000), 0x40, (size_t)c.lay.size);
    unsigned th = 0, td = 0;
    nvn_tex_level_tiles(&c.cube.d, 0, &th, &td);
    int failed = 0;
    for (u32 f = 0; f < 8; f++) {
        g_renderer->mem_may_have_changed();
        g_vk_present++;
        u64 loads0, bytes0, loads1, bytes1, up0, upb0, up1, upb1, t0[8], t1[8];
        vk_tex_partial_stats(&loads0, &bytes0);
        vk_tex_stats(&up0, &upb0);
        vk_tex_take_counts(t0);
        for (u32 k = 0; k < 6; k++) {
            const u32 col = g_ct_faces[k] ^ f;
            const float rgba[4] = {(float)(col & 255) / 255.0f, (float)(col >> 8 & 255) / 255.0f,
                                   (float)(col >> 16 & 255) / 255.0f, (float)(col >> 24) / 255.0f};
            ct_clear(ch, c.cube.gpu + (u64)k * c.lay.layer_size, CT_SIDE, CT_SIDE, th, rgba);
        }
        for (u32 k = 0; k < 6; k++)
            for (u32 l = 1; l < c.cube.d.levels; l++) CHECK(!ct_store(&c, l, k, (g_ct_mip ^ f) + l));
        vk_lock();
        Surf* t = vk_tex_from_tic(c.tic);
        vk_unlock();
        CHECK(t);
        vk_tex_partial_stats(&loads1, &bytes1);
        vk_tex_stats(&up1, &upb1);
        vk_tex_take_counts(t1);
        const u32 top = ct_texel(t, c.cube.d.levels - 1, 5), face = ct_texel(t, 0, 3);
        printf("cube take frames, frame %u: %llu loads (%llu of pieces, %llu KB), %llu targets and %llu stored taken, "
               "%llu refused with newer memory, %llu at no level\n",
               f, (unsigned long long)(up1 - up0), (unsigned long long)(loads1 - loads0),
               (unsigned long long)((upb1 - upb0) >> 10), (unsigned long long)(t1[0] - t0[0]),
               (unsigned long long)(t1[1] - t0[1]), (unsigned long long)(t1[5] - t0[5]),
               (unsigned long long)(t1[4] - t0[4]));
        if (top != (g_ct_mip ^ f) + c.cube.d.levels - 1 || face != (g_ct_faces[3] ^ f)) {
            fprintf(stderr,
                    "cube take frames, frame %u: face 5's last level %08x (stored %08x), face 3 %08x (drawn "
                    "%08x)\n",
                    f, top, (g_ct_mip ^ f) + c.cube.d.levels - 1, face, g_ct_faces[3] ^ f);
            failed = 1;
        }
        if (f && (loads1 != loads0 || up1 != up0 || t1[5] != t0[5])) {
            fprintf(stderr, "cube take frames, frame %u: the cube loaded from memory, or refused a take\n", f);
            failed = 1;
        }
    }
    free(ch);
    if (!failed) puts("PASS cube take frames: after the first frame the cube loads nothing from memory");
    return failed;
}
