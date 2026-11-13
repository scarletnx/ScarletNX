/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_mode's BC7 (vk_astc_bc7): an ASTC texture goes up decoded to RGBA8, which the game samples at once, and the BC7
   encoder makes its BC7 image after the frames' presents, VK_BC7_LATER_BLOCKS blocks a frame at most (texture.c
   vk_tex_later_frame). Whole, the BC7 image takes the RGBA8 one's place, which goes once the frames are done, and its
   blocks are those the shader makes of the texture's ASTC blocks, as the CPU runs it (tests/glsl/astc_bc7_cpu.cpp), byte for byte, edges, strips cut at the
   frame's budget, layers and 3D slices across frames included. A texture written before its BC7 image is whole starts over on the
   new texels; one written whole after is RGBA8 again until the encoder is done again, and pieces of it more than a
   frame's blocks load whole as RGBA8 too, where fewer go straight to BC7. A texture written again before the encoder
   could make it BC7 is held Exact, out of the queue, even for its pieces, and is made BC7 again once read unwritten
   for a while. A texture freed leaves the queue */
#include "bc7_cpu.h"
#include "astc_bc7_cpu.h"

#define AL_CPU      (HEAP_BASE + 0x1F000000) /* a case's guest memory, AL_SPAN each */
#define AL_SPAN     0x200000ull
#define AL_GVA      0xC00000000ull /* a case's texture, AL_GVA_STEP each; its Exact twin AL_TWIN past it */
#define AL_GVA_STEP 0x10000000ull
#define AL_TWIN     0x8000000ull

typedef struct {
    const char* name;
    u16 format;  /* NVN's */
    u32 w, h, d; /* d: a 2D array's layers, or a 3D texture's depth */
    int d3;      /* 3D */
} AlCase;
typedef struct {
    const AlCase* c;
    NvnTexture tex;
    NvnTexLayout lay;
    u32 tic[8], twin[8];
    u64 cpu;
    u32 rng;
} AlTex;

/* one frame of the encoder: the blocks it encoded */
static u64 al_step(void) {
    VkLaterStats a, b;
    vk_tex_later_stats(&a);
    vk_lock();
    vk_tex_later_frame();
    vk_unlock();
    vk_tex_later_stats(&b);
    return b.blocks - a.blocks;
}
/* the frames the encoder takes until t's BC7 image is whole (64 at most), each within its blocks */
static u32 al_until_done(Surf* t, int* over) {
    u32 frames = 0;
    *over = 0;
    while (t->bc7.queued && frames < 64) {
        if (al_step() > VK_BC7_LATER_BLOCKS) *over = 1;
        frames++;
    }
    return frames;
}
/* the next frame's texture of a header, as a frame finds it after guest memory may have changed */
static Surf* al_frame(const u32* tic) {
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    return t;
}
/* n frames that read the texture of a header and write nothing: what a write after them is apart from the one before
   (texture.c later_soon) */
static void al_idle(const u32* tic, u32 n) {
    for (u32 i = 0; i < n; i++) al_frame(tic);
}
/* level l's texels across, down and deep */
static void al_level(const AlTex* x, u32 l, u32* w, u32* h, u32* d) {
    *w = x->c->w >> l ? x->c->w >> l : 1;
    *h = x->c->h >> l ? x->c->h >> l : 1;
    *d = x->c->d3 ? (x->c->d >> l ? x->c->d >> l : 1) : 1;
}
/* the bytes of level l of a layer: RGBA8 texels (block 0), or 4x4 blocks of block bytes each (BC7's 16) */
static u64 al_level_bytes(const AlTex* x, u32 l, u32 block) {
    u32 w, h, d;
    al_level(x, l, &w, &h, &d);
    return block ? (u64)((w + 3) / 4) * ((h + 3) / 4) * block * d : (u64)w * h * 4 * d;
}
static u32 al_layers(const AlTex* x) { return x->c->d3 ? 1 : x->c->d; }
/* every level of every layer of t's image, read back, level after level, layer after layer; malloc'd, its bytes in
   *size. NULL when it couldn't be. After an RGBA8 image's texels come the ASTC blocks t keeps for its BC7 image
   (astc.c's vk_astc_keep), as it keeps them: level after level, each a layer's or slice's rows of blocks after
   another's. They are what the BC7 image is made from, so what its blocks are held to (al_expect) */
static u8* al_image(const AlTex* x, Surf* t, u64* size) {
    const int bc7 = t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK;
    /* the bytes of a 4x4 block: BC7's, BC5's 16, BC1's and BC4's 8 (vk_astc_bcn); 0 for RGBA8 texels */
    const u32 block = bc7 || vk_astc_bcn(t->f.vk) == 5 ? 16u : vk_astc_bcn(t->f.vk) ? 8u : 0u;
    const u32 n = al_layers(x) * x->tex.d.levels;
    VkBufferImageCopy* c = calloc(n, sizeof *c);
    VkBuffer buffer;
    u64 at = 0;
    u32 k = 0;
    if (!c) return NULL;
    for (u32 layer = 0; layer < al_layers(x); layer++)
        for (u32 l = 0; l < x->tex.d.levels; l++) {
            u32 w, h, d;
            al_level(x, l, &w, &h, &d);
            c[k++] = (VkBufferImageCopy){.bufferOffset = at,
                                         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                         .imageExtent = {w, h, d}};
            at += al_level_bytes(x, l, block);
        }
    vk_lock();
    const u64 kept = block ? 0 : t->bc7.kept_bytes;
    u8* back = vk_readback_buffer(at + kept, &buffer);
    u8* out = back ? malloc((size_t)(at + kept)) : NULL;
    if (out) {
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, n, c);
        if (kept) {
            /* after the uploads' copies into it and the encoder's reads of it */
            VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
            VkDependencyInfo md = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
            rec_CmdPipelineBarrier2(vk_cmd(), &md);
            const VkBufferCopy r = {0, at, kept};
            rec_CmdCopyBuffer(vk_cmd(), t->bc7.kept, buffer, 1, &r);
        }
        vk_finish();
        vk_readback_ready();
        memcpy(out, back, (size_t)(at + kept));
    }
    vk_unlock();
    free(c);
    *size = at;
    return out;
}
/* the BC7 blocks the shader's build of each level makes, run on the CPU (tests/glsl/astc_bc7_cpu.cpp), of the ASTC
   blocks read back after an RGBA8 image's texels (al_image), footprint bw x bh: each level of each layer on its own,
   as the encoder takes a level. The fast build takes what the ASTC blocks say of each block, so the texels alone do
   not give its blocks. malloc'd, its bytes in *size */
static u8* al_expect(const AlTex* x, const u8* rgba, u32 bw, u32 bh, u64* size) {
    u64 bytes = 0, texels = 0, to = 0;
    for (u32 l = 0; l < x->tex.d.levels; l++) {
        bytes += al_level_bytes(x, l, 16);
        texels += al_level_bytes(x, l, 0);
    }
    bytes *= al_layers(x);
    const u8* kept = rgba + texels * al_layers(x);
    u8* out = malloc((size_t)bytes);
    if (!out) return NULL;
    for (u32 layer = 0; layer < al_layers(x); layer++) {
        u64 level_at = 0; /* where the level's blocks start in what the texture keeps */
        for (u32 l = 0; l < x->tex.d.levels; l++) {
            u32 w, h, d;
            al_level(x, l, &w, &h, &d);
            const u64 slice = (u64)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
            astc_bc7_cpu(vk_bc7_build(w, h, d), kept + level_at + layer * slice, w, h, d, bw, bh, 0, out + to);
            level_at += slice * (x->c->d3 ? d : al_layers(x));
            to += al_level_bytes(x, l, 16);
        }
    }
    *size = bytes;
    return out;
}
/* 1 where t's BC7 image isn't the CPU encoder's of rgba, said with the first block that differs */
static int al_differ(const AlTex* x, const char* when, Surf* t, const u8* rgba) {
    u64 got_size = 0, want_size = 0, first = 0, differ = 0;
    u8* got = al_image(x, t, &got_size);
    u8* want = al_expect(x, rgba, t->f.bw, t->f.bh, &want_size);
    for (u64 i = 0; got && want && got_size == want_size && i < got_size; i += 16)
        if (memcmp(got + i, want + i, 16) && !differ++) first = i;
    int bad = !got || !want || got_size != want_size || differ;
    if (bad)
        fprintf(
            stderr,
            "ASTC as BC7 between frames, %s, %s: %llu of %llu BC7 blocks differ from the CPU encoder's of its RGBA8 "
            "texels, the first at byte %llu (%llu bytes, %llu wanted)\n",
            x->c->name, when, (unsigned long long)differ, (unsigned long long)(got_size / 16),
            (unsigned long long)first, (unsigned long long)got_size, (unsigned long long)want_size);
    free(got);
    free(want);
    return bad;
}
/* fills the texture's memory: random words, the block mode bits of each block stepped through (astc_vulkan's
   blocks_new), so the blocks are every mode's, error blocks among them */
static void al_fill(AlTex* x) {
    guest_done(x->cpu, x->lay.size);
    for (u64 i = 0; i < x->lay.size / 4; i++) {
        x->rng ^= x->rng << 13, x->rng ^= x->rng >> 17, x->rng ^= x->rng << 5;
        snx_st_le32(GP(x->cpu) + i * 4, i % 4 ? x->rng : (x->rng & ~2047u) | (u32)(i / 4 & 2047));
    }
}
/* case c's header at index's place, its twin's at AL_TWIN past it, both over memory span `span` (AL_SPAN each, 16 of
   them), mapped: 1 when not */
static int al_place(AlTex* x, const AlCase* c, u32 index, u32 span) {
    memset(x, 0, sizeof *x);
    x->c = c;
    x->cpu = AL_CPU + span * AL_SPAN;
    x->rng = 0x2545f491u + index;
    x->tex.d.target = c->d3 ? NVN_TEXTURE_TARGET_3D : c->d > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D;
    x->tex.d.format = c->format;
    u32 most = c->w > c->h ? c->w : c->h;
    if (c->d3 && c->d > most) most = c->d;
    x->tex.d.levels = (u8)(32u - snx_clz32(most));
    x->tex.d.width = c->w;
    x->tex.d.height = c->h;
    x->tex.d.depth = c->d;
    x->tex.gpu = AL_GVA + index * AL_GVA_STEP;
    nvn_tex_layout(&x->tex.d, &x->lay);
    NvnTexture twin = x->tex;
    twin.gpu += AL_TWIN;
    if (x->lay.size > AL_SPAN || !nvn_tic_encode(&x->tex, NULL, 0, 0, x->tic) ||
        !nvn_tic_encode(&twin, NULL, 0, 0, x->twin) || mem_map(x->cpu, AL_SPAN, MS_NORMAL, PERM_RW)) {
        fprintf(stderr, "ASTC as BC7 between frames, %s: %llu bytes, or no header, or no memory\n", c->name,
                (unsigned long long)x->lay.size);
        return 1;
    }
    gmmu_map(x->tex.gpu, x->cpu, AL_SPAN);
    gmmu_map(twin.gpu, x->cpu, AL_SPAN);
    return 0;
}
/* case c's texture, its memory filled, at index's place (al_place), made by astc_mode 2 (BC7) */
static Surf* al_make(AlTex* x, const AlCase* c, u32 index) {
    if (al_place(x, c, index, index)) return NULL;
    al_fill(x);
    vk_lock();
    Surf* t = vk_tex_from_tic(x->tic);
    vk_unlock();
    return t;
}
static void al_free(AlTex* x) {
    gmmu_space_unmap(x->tex.gpu, AL_SPAN);
    gmmu_space_unmap(x->tex.gpu + AL_TWIN, AL_SPAN);
    mem_unmap(x->cpu, AL_SPAN);
}
/* t as it is made: RGBA8 the game samples at once, its texels the Exact twin's (astc_mode 1, over the same memory),
   queued for the encoder, or (hot) held Exact for its writes, out of the queue. Its RGBA8 texels, malloc'd, in *rgba;
   1 when not */
static int al_shown_as(AlTex* x, Surf* t, const char* when, u8** rgba, int hot) {
    u64 size = 0, twin_size = 0;
    g_settings.astc_mode = 1;
    Surf* twin = al_frame(x->twin);
    g_settings.astc_mode = 2;
    *rgba = t ? al_image(x, t, &size) : NULL;
    u8* exact = twin ? al_image(x, twin, &twin_size) : NULL;
    int ok = t && twin && t->upload_valid && t->f.vk == VK_FORMAT_R8G8B8A8_UNORM && t->bc7_later &&
             t->bc7.queued == !hot && t->bc7_hot == hot && twin->f.vk == VK_FORMAT_R8G8B8A8_UNORM && !twin->bc7_later &&
             *rgba && exact && size == twin_size && !memcmp(*rgba, exact, (size_t)size);
    if (!ok)
        fprintf(stderr,
                "ASTC as BC7 between frames, %s, %s: the texture %p (format %d, held as BC7 later %d, queued %d, held "
                "Exact for its writes %d, not %d, loaded %d), its Exact twin %p (format %d), their texels %s\n",
                x->c->name, when, (void*)t, t ? (int)t->f.vk : 0, t ? t->bc7_later : 0, t ? t->bc7.queued : 0,
                t ? t->bc7_hot : 0, hot, t ? t->upload_valid : 0, (void*)twin, twin ? (int)twin->f.vk : 0,
                *rgba && exact && size == twin_size && !memcmp(*rgba, exact, (size_t)size) ? "the same" : "differ");
    free(exact);
    return !ok;
}
static int al_shown(AlTex* x, Surf* t, const char* when, u8** rgba) { return al_shown_as(x, t, when, rgba, 0); }

/* (a) A 1024x768 ASTC 8x8 texture, 65539 BC7 blocks in 11 levels: frame 1 encodes 128 of level 0's 192 block rows,
   frame 2 the other 64 and levels 1 to 7 to the block, frame 3 the last three; the RGBA8 image stays until then, and
   goes with the frames after the swap */
static int al_frames(void) {
    static const AlCase c = {"ASTC 8x8 1024x768", NVN_FORMAT_RGBA_ASTC_8x8, 1024, 768, 1, 0};
    AlTex x;
    u8* rgba = NULL;
    VkLaterStats a, b;
    int over;
    Surf* t = al_make(&x, &c, 0);
    CHECK(t && !al_shown(&x, t, "made", &rgba));
    const VkImage rgba_img = t->img;
    const u64 rgba_bytes = t->bytes;
    vk_tex_later_stats(&a);
    const u64 first = al_step(), second = al_step();
    CHECK(t->bc7.queued && t->img == rgba_img && t->f.vk == VK_FORMAT_R8G8B8A8_UNORM);
    const u64 deferred = vk_deferred_bytes();
    const u64 third = al_step();
    const u64 swapped_deferred = vk_deferred_bytes();
    vk_tex_later_stats(&b);
    if (first != VK_BC7_LATER_BLOCKS || second != VK_BC7_LATER_BLOCKS || third != 3 || t->bc7.queued) {
        fprintf(stderr,
                "ASTC as BC7 between frames, %s: frames of %llu, %llu and %llu blocks, %s, not %u, %u and 3, whole\n",
                c.name, (unsigned long long)first, (unsigned long long)second, (unsigned long long)third,
                t->bc7.queued ? "queued still" : "whole", VK_BC7_LATER_BLOCKS, VK_BC7_LATER_BLOCKS);
        return 1;
    }
    (void)over;
    /* the BC7 image in the RGBA8 one's place, the RGBA8 one's memory deferred, then freed with the frames */
    CHECK(t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK && t->img != rgba_img && !t->bc7.img && t->bytes < rgba_bytes);
    CHECK(swapped_deferred >= deferred + rgba_bytes);
    CHECK(b.made == a.made + 1 && b.freed - a.freed == rgba_bytes - t->bytes);
    vk_lock();
    vk_finish();
    for (int i = 0; i < VK_FRAMES; i++) {
        vk_frame_next();
        vk_cmd();
    }
    vk_unlock();
    CHECK(vk_deferred_bytes() + rgba_bytes <= swapped_deferred);
    int failed = al_differ(&x, "whole", t, rgba);
    if (!failed)
        printf("ASTC as BC7 between frames, %s: RGBA8 at once, BC7 in 3 frames of %llu, %llu and %llu blocks, %llu "
               "KiB of RGBA8 freed after them, the CPU encoder's blocks\n",
               c.name, (unsigned long long)first, (unsigned long long)second, (unsigned long long)third,
               (unsigned long long)((rgba_bytes - t->bytes) >> 10));
    free(rgba);
    al_free(&x);
    return failed;
}

/* (b) A 2D array of 3 layers of 100x60 ASTC 6x5 in 7 levels, sides on no block: a frame, a dispatch a level for its
   three layers. Then written whole frames later: RGBA8 again at once, the new texels, and BC7 again from them */
static int al_layers_again(void) {
    static const AlCase c = {"ASTC 6x5 100x60, 3 layers", NVN_FORMAT_RGBA_ASTC_6x5, 100, 60, 3, 0};
    AlTex x;
    u8* rgba = NULL;
    int over;
    Surf* t = al_make(&x, &c, 1);
    CHECK(t && !al_shown(&x, t, "made", &rgba));
    const u64 dispatches = vk_astc_dispatches();
    const u32 frames = al_until_done(t, &over);
    const u64 by = vk_astc_dispatches() - dispatches;
    if (frames != 1 || over || by != x.tex.d.levels) {
        fprintf(stderr, "ASTC as BC7 between frames, %s: %u frames, %llu encoder dispatches, not 1 and %u\n", c.name,
                frames, (unsigned long long)by, x.tex.d.levels);
        return 1;
    }
    int failed = al_differ(&x, "whole", t, rgba);
    free(rgba);
    /* written whole past the frame its encode takes and the one after (else it is held Exact, (g)): RGBA8 at once, of
       the new blocks */
    const VkImage bc7_img = t->img;
    al_idle(x.tic, 4);
    al_fill(&x);
    CHECK(al_frame(x.tic) == t && t->img != bc7_img);
    failed |= al_shown(&x, t, "written whole after", &rgba);
    CHECK(al_until_done(t, &over) == 1 && !over && t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK);
    failed |= al_differ(&x, "written whole after, whole again", t, rgba);
    if (!failed)
        printf("ASTC as BC7 between frames, %s: one frame, an encoder dispatch a level; written whole after, RGBA8 at "
               "once and BC7 again of the new blocks\n",
               c.name);
    free(rgba);
    al_free(&x);
    return failed;
}

/* (c) A 512x768 ASTC 8x8 texture whose encoder started late (behind other textures, as after a load) written whole
   after the first of its two frames: the encoder drops what it made and starts over on the new texels, two frames
   again. Written so long after its load, it is not held Exact */
static int al_written_between(void) {
    static const AlCase c = {"ASTC 8x8 512x768", NVN_FORMAT_RGBA_ASTC_8x8, 512, 768, 1, 0};
    AlTex x;
    u8* rgba = NULL;
    VkLaterStats a, b;
    int over;
    Surf* t = al_make(&x, &c, 2);
    CHECK(t && !al_shown(&x, t, "made", &rgba));
    free(rgba);
    al_idle(x.tic, 4);
    CHECK(al_step() == VK_BC7_LATER_BLOCKS && t->bc7.queued && t->bc7.img);
    al_fill(&x);
    CHECK(al_frame(x.tic) == t);
    int failed = al_shown(&x, t, "written between two frames", &rgba);
    vk_tex_later_stats(&a);
    const u32 frames = al_until_done(t, &over);
    vk_tex_later_stats(&b);
    if (frames != 2 || over || b.restarted != a.restarted + 1) {
        fprintf(stderr,
                "ASTC as BC7 between frames, %s, written between two frames: %u frames, %llu encodes started over, not "
                "2 and 1\n",
                c.name, frames, (unsigned long long)(b.restarted - a.restarted));
        failed = 1;
    }
    failed |= al_differ(&x, "written between two frames, whole", t, rgba);
    if (!failed)
        printf("ASTC as BC7 between frames, %s: written after its first frame, the encoder started over on the new "
               "texels and took two frames again\n",
               c.name);
    free(rgba);
    al_free(&x);
    return failed;
}

/* (d) Slices past a frame's blocks, each strip at its own: a 2D array of 40 layers of 128x128 ASTC 4x4 (32 layers of
   level 0 in frame 1, the other 8 and the levels after in frame 2), and a 3D texture of that size where the device
   makes 3D BC7 images (its depth slices so) */
static int al_slices(const AlCase* c, u32 index) {
    AlTex x;
    u8* rgba = NULL;
    int over;
    Surf* t = al_make(&x, c, index);
    CHECK(t);
    if (!t->bc7_later) {
        printf("ASTC as BC7 between frames, %s: the device makes no such BC7 image, skipped\n", c->name);
        al_free(&x);
        return 0;
    }
    CHECK(!al_shown(&x, t, "made", &rgba));
    const u64 first = al_step();
    const u32 frames = al_until_done(t, &over);
    if (first != VK_BC7_LATER_BLOCKS || frames != 1 || over || t->f.vk != VK_FORMAT_BC7_UNORM_BLOCK) {
        fprintf(stderr, "ASTC as BC7 between frames, %s: a frame of %llu blocks, then %u more, not %u and 1\n", c->name,
                (unsigned long long)first, frames, VK_BC7_LATER_BLOCKS);
        return 1;
    }
    int failed = al_differ(&x, "whole", t, rgba);
    if (!failed)
        printf("ASTC as BC7 between frames, %s: 32 slices of level 0 in the first frame, the rest in the second, each "
               "where it lies\n",
               c->name);
    free(rgba);
    al_free(&x);
    return failed;
}

/* (e) A 2048x2048 ASTC 12x12 texture made BC7, then written in pieces, each more than the 12 frames its encode takes
   and one after the write before (else it is held Exact, (h)): an ASTC block goes straight to BC7, its image staying
   BC7; 96 KiB of level 0, under half the texture but 55296 BC7 blocks, more than a frame's, load whole as RGBA8 that
   the encoder makes BC7 again. With write tracking only: without it a check by contents loads it whole */
static int al_pieces(void) {
    static const AlCase c = {"ASTC 12x12 2048x2048", NVN_FORMAT_RGBA_ASTC_12x12, 2048, 2048, 1, 0};
    AlTex x;
    u8* rgba = NULL;
    int over;
    Surf* t = al_make(&x, &c, 4);
    CHECK(t && !al_shown(&x, t, "made", &rgba));
    free(rgba);
    vk_lock();
    const int tracked = vk_written_since(GP(x.cpu), x.lay.size, 0) >= 0;
    vk_unlock();
    CHECK(al_until_done(t, &over) == 11 && !over && t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK);
    al_idle(x.tic, 13); /* write tracking's scans take in what was written too */
    if (!tracked) {
        printf("ASTC as BC7 between frames, %s: no write tracking, pieces skipped\n", c.name);
        al_free(&x);
        return 0;
    }
    const VkImage bc7_img = t->img;
    guest_done(x.cpu + 0x10000, 16);
    GP(x.cpu + 0x10000)[3] ^= 0x5A;
    CHECK(al_frame(x.tic) == t);
    if (t->img != bc7_img || t->f.vk != VK_FORMAT_BC7_UNORM_BLOCK || t->bc7.queued) {
        fprintf(stderr, "ASTC as BC7 between frames, %s: an ASTC block written, its image format %d, queued %d\n",
                c.name, (int)t->f.vk, t->bc7.queued);
        return 1;
    }
    al_idle(x.tic, 13);
    guest_done(x.cpu + 0x10000, 96 << 10);
    for (u32 i = 0; i < 96u << 10; i += 64) GP(x.cpu + 0x10000)[i] ^= 0x5A;
    CHECK(al_frame(x.tic) == t);
    if (t->f.vk != VK_FORMAT_R8G8B8A8_UNORM || !t->bc7.queued || t->bc7_hot) {
        fprintf(stderr,
                "ASTC as BC7 between frames, %s: 96 KiB of it written, its image format %d, queued %d, held Exact %d, "
                "not RGBA8, queued and not held\n",
                c.name, (int)t->f.vk, t->bc7.queued, t->bc7_hot);
        return 1;
    }
    CHECK(al_until_done(t, &over) == 11 && !over && t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK);
    printf("ASTC as BC7 between frames, %s: made BC7 in 11 frames; an ASTC block written went straight to BC7, 96 KiB "
           "loaded whole as RGBA8 and made BC7 again\n",
           c.name);
    al_free(&x);
    return 0;
}

/* (g) A 512x512 ASTC 8x8 texture, whose encode takes a frame, written whole again and again: 3 frames after the write
   before it stays queued; 2 frames after, the frame its encode takes and the one after, it is held Exact for its
   writes, RGBA8 the Exact twin's, out of the queue, and stays so for a write the next frame. Read 29 frames unwritten it
   is still held; at 30 it is queued again, made BC7 in a frame, the CPU encoder's blocks of its last texels */
static int al_hot(void) {
    static const AlCase c = {"ASTC 8x8 512x512, written often", NVN_FORMAT_RGBA_ASTC_8x8, 512, 512, 1, 0};
    AlTex x;
    u8* rgba = NULL;
    VkLaterStats a, b;
    int over, failed = 0;
    Surf* t = al_make(&x, &c, 7);
    CHECK(t && !al_shown(&x, t, "made", &rgba));
    free(rgba);
    vk_tex_later_stats(&a);
    al_idle(x.tic, 1);
    al_fill(&x);
    CHECK(al_frame(x.tic) == t);
    failed |= al_shown_as(&x, t, "written 3 frames after its load", &rgba, 0);
    free(rgba);
    al_fill(&x); /* al_shown_as's frame was one */
    CHECK(al_frame(x.tic) == t);
    failed |= al_shown_as(&x, t, "written 2 frames after the write before", &rgba, 1);
    free(rgba);
    CHECK(al_step() == 0);
    al_fill(&x);
    CHECK(al_frame(x.tic) == t);
    failed |= al_shown_as(&x, t, "written the next frame", &rgba, 1);
    vk_tex_later_stats(&b);
    if (b.went_hot != a.went_hot + 1 || b.hot != a.hot + 1) {
        fprintf(stderr, "ASTC as BC7 between frames, %s: %llu went held Exact, %u held now, not 1 and %u\n", c.name,
                (unsigned long long)(b.went_hot - a.went_hot), b.hot, a.hot + 1);
        failed = 1;
    }
    /* al_shown_as's frame read only its twin: 28 frames that read it, 29 after its last write, then the 30th */
    al_idle(x.tic, 28);
    if (!t->bc7_hot || t->bc7.queued) {
        fprintf(stderr, "ASTC as BC7 between frames, %s: 29 frames unwritten, held Exact %d, queued %d, not 1 and 0\n",
                c.name, t->bc7_hot, t->bc7.queued);
        failed = 1;
    }
    al_idle(x.tic, 1);
    vk_tex_later_stats(&b);
    if (t->bc7_hot || !t->bc7.queued || b.cooled != a.cooled + 1 || b.hot != a.hot) {
        fprintf(stderr,
                "ASTC as BC7 between frames, %s: 30 frames unwritten, held Exact %d, queued %d, %llu made BC7 again, "
                "%u held, not 0, 1, 1 and %u\n",
                c.name, t->bc7_hot, t->bc7.queued, (unsigned long long)(b.cooled - a.cooled), b.hot, a.hot);
        failed = 1;
    }
    CHECK(al_until_done(t, &over) == 1 && !over && t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK);
    failed |= al_differ(&x, "read unwritten, made BC7 again", t, rgba);
    if (!failed)
        printf("ASTC as BC7 between frames, %s: written 3 frames apart stays queued, 2 apart held Exact out of the "
               "queue, read 30 frames unwritten made BC7 again\n",
               c.name);
    free(rgba);
    al_free(&x);
    return failed;
}

/* (h) The same texture made BC7, then written in pieces: an ASTC block 3 frames after its last write goes straight to
   BC7; another the next frame loads it whole as RGBA8, held Exact for its writes, out of the queue: pieces of a texture
   written often are never encoded where the game waits. With write tracking only */
static int al_hot_pieces(void) {
    static const AlCase c = {"ASTC 8x8 512x512, pieces written often", NVN_FORMAT_RGBA_ASTC_8x8, 512, 512, 1, 0};
    AlTex x;
    u8* rgba = NULL;
    int over;
    Surf* t = al_make(&x, &c, 8);
    CHECK(t && !al_shown(&x, t, "made", &rgba));
    free(rgba);
    vk_lock();
    const int tracked = vk_written_since(GP(x.cpu), x.lay.size, 0) >= 0;
    vk_unlock();
    CHECK(al_until_done(t, &over) == 1 && !over && t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK);
    al_idle(x.tic, 3);
    if (!tracked) {
        printf("ASTC as BC7 between frames, %s: no write tracking, skipped\n", c.name);
        al_free(&x);
        return 0;
    }
    const VkImage bc7_img = t->img;
    guest_done(x.cpu + 0x1000, 16);
    GP(x.cpu + 0x1000)[3] ^= 0x5A;
    CHECK(al_frame(x.tic) == t);
    if (t->img != bc7_img || t->f.vk != VK_FORMAT_BC7_UNORM_BLOCK || t->bc7_hot || t->bc7.queued) {
        fprintf(stderr,
                "ASTC as BC7 between frames, %s: an ASTC block written 3 frames after, its image format %d, held "
                "Exact %d, queued %d, not BC7, 0 and 0\n",
                c.name, (int)t->f.vk, t->bc7_hot, t->bc7.queued);
        return 1;
    }
    guest_done(x.cpu + 0x2000, 16);
    GP(x.cpu + 0x2000)[3] ^= 0x5A;
    CHECK(al_frame(x.tic) == t);
    if (t->f.vk != VK_FORMAT_R8G8B8A8_UNORM || !t->bc7_hot || t->bc7.queued) {
        fprintf(stderr,
                "ASTC as BC7 between frames, %s: an ASTC block written the next frame, its image format %d, held "
                "Exact %d, queued %d, not RGBA8, 1 and 0\n",
                c.name, (int)t->f.vk, t->bc7_hot, t->bc7.queued);
        return 1;
    }
    printf("ASTC as BC7 between frames, %s: a block 3 frames after its last write went straight to BC7, another the "
           "next frame loaded it whole as RGBA8, held Exact\n",
           c.name);
    al_free(&x);
    return 0;
}

/* (f) A texture freed while the encoder makes its BC7 image (the textures unused go, as the 20 second sweep frees
   them): out of the queue, and its BC7 image with it */
static int al_freed(void) {
    static const AlCase c = {"ASTC 8x8 1024x768, freed", NVN_FORMAT_RGBA_ASTC_8x8, 1024, 768, 1, 0};
    AlTex x;
    VkLaterStats s;
    Surf* t = al_make(&x, &c, 5);
    CHECK(t && t->bc7.queued);
    CHECK(al_step() == VK_BC7_LATER_BLOCKS && t->bc7.queued && t->bc7.img);
    const u64 deferred = vk_deferred_bytes(), bc7_bytes = t->bc7.bytes;
    g_vk_present += 2;
    vk_lock();
    vk_surf_reclaim(VK_LRU_TEX, g_vk_present, ~0ull, VK_RECLAIM_LOSSLESS);
    vk_unlock();
    vk_tex_later_stats(&s);
    if (s.waiting || vk_deferred_bytes() < deferred + bc7_bytes) {
        fprintf(stderr,
                "ASTC as BC7 between frames, %s: %u textures still queued, %llu bytes deferred more, not 0 and at "
                "least the BC7 image's %llu\n",
                c.name, s.waiting, (unsigned long long)(vk_deferred_bytes() - deferred), (unsigned long long)bc7_bytes);
        return 1;
    }
    CHECK(al_step() == 0);
    printf("ASTC as BC7 between frames, %s: freed while it was made, out of the queue with its BC7 image\n", c.name);
    al_free(&x);
    return 0;
}

/* (i) The encoder's fast build is timed on the GPU for astc_quick_mtexels (astc.c bc7_time_begin), its transcodes of a
   megatexel or more alone. A 1024x1024 ASTC 8x8 texture held as BC7 at load (g_vk.astc_at_load, astc_mode=3's) adds,
   once its submission is done, its first level's texels, one dispatch of a megatexel, and some time within the wall
   time, where the graphics queue writes timestamps (nothing where it writes none); its smaller levels add nothing. The
   size of a very large level follows the sums, and is the setting's own with vk_astc_quick_measure(0) */
static int al_timed(void) {
    static const AlCase c = {"ASTC 8x8 1024x1024 at load, timed", NVN_FORMAT_RGBA_ASTC_8x8, 1024, 1024, 1, 0};
    const int at_load = g_vk.astc_at_load;
    AlTex x;
    u64 ns0, t0, ns1, t1;
    later_drain(); /* nothing of the checks before encodes beside it */
    vk_lock();
    vk_finish();
    vk_unlock();
    vk_astc_quick_texels(&ns0, &t0);
    const u64 wall0 = plat_ns();
    g_vk.astc_at_load = 1;
    Surf* t = al_make(&x, &c, 9);
    g_vk.astc_at_load = at_load;
    CHECK(t && t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK && !t->bc7_later);
    vk_lock();
    vk_finish(); /* its submission done: its timestamps read */
    vk_unlock();
    const u64 wall = plat_ns() - wall0; /* the GPU's time of the dispatch fits in it */
    const u64 size = vk_astc_quick_texels(&ns1, &t1), want = g_vk.gfx_timestamp_bits ? 1ull << 20 : 0;
    if (t1 - t0 != want || (want && (ns1 <= ns0 || ns1 - ns0 > wall)) ||
        size != snx_astc_quick_texels(g_settings.astc_quick_mtexels, ns1, t1)) {
        fprintf(stderr,
                "ASTC as BC7 at load, %s: %llu texels timed in %llu ns, not %llu%s; very large from %llu texels where "
                "the sums give %llu\n",
                c.name, (unsigned long long)(t1 - t0), (unsigned long long)(ns1 - ns0), (unsigned long long)want,
                want ? " in some time within the wall time" : " (no timestamps)", (unsigned long long)size,
                (unsigned long long)snx_astc_quick_texels(g_settings.astc_quick_mtexels, ns1, t1));
        return 1;
    }
    const int had = vk_astc_quick_measure(0);
    const u64 as_set = vk_astc_quick_texels(&ns1, &t1);
    CHECK(had && !vk_astc_quick_measure(had));
    CHECK(as_set == snx_astc_quick_texels(g_settings.astc_quick_mtexels, 0, 0));
    printf("ASTC as BC7 at load, %s: a level of a megatexel timed in %.2f ms of the GPU, %.2f ns a texel, its smaller "
           "levels not; very large levels from %.1f megatexels (astc_quick_mtexels=%d)\n",
           c.name, (double)(ns1 - ns0) / 1e6, want ? (double)(ns1 - ns0) / (double)want : 0.0, (double)size / (1 << 20),
           g_settings.astc_quick_mtexels);
    al_free(&x);
    return 0;
}

/* (h) The BC7 cache (astc.c over bc7_cache.c): the levels the encoder makes go into it once their frames are done, and
   a second texture of the same bytes, the header's twin at another address, takes them from it in its first frame
   with no encode: whole at once, its blocks the CPU encoder's of its RGBA8 texels */
/* A BC7 cache file in this run's own folder (g_cachedir, main's snx_temp_dir): every headless variant opens one, a few
   at a time under ctest -j, and a file of the tests' shared working folder is in use by the other run, which then
   runs without a cache. al_cache_open refuses a path outside that folder, naming both */
static void al_cache_path(char* path, size_t size, const char* name) { snprintf(path, size, "%s%s", g_cachedir, name); }
static int al_cache_in_run(const char* path) {
    size_t n = strlen(g_cachedir);
    return n && !strncmp(path, g_cachedir, n) && path[n];
}
static int al_cache_open(const char* path) {
    if (!al_cache_in_run(path)) {
        fprintf(stderr, "vulkan_headless: the BC7 cache %s is outside this run's folder %s\n", path, g_cachedir);
        return 1;
    }
    remove(path);
    vk_astc_cache_open(path);
    return 0;
}
/* the guard itself: a bare name (the working folder's), another folder and the folder alone are refused */
static int al_cache_guard(void) {
    char path[sizeof g_cachedir + 32];
    al_cache_path(path, sizeof path, "guard.vkbc7");
    CHECK(al_cache_in_run(path));
    CHECK(!al_cache_in_run("guard.vkbc7") && !al_cache_in_run("/guard.vkbc7") && !al_cache_in_run(g_cachedir));
    printf("ASTC as BC7 between frames: BC7 cache files only in the run's folder %s\n", g_cachedir);
    return 0;
}
static int al_cached(void) {
    static const AlCase c = {"ASTC 8x8 256x256, cached", NVN_FORMAT_RGBA_ASTC_8x8, 256, 256, 1, 0};
    AlTex x;
    u64 levels, hits, hits2, bytes, rgba8, reads, stored, stored2, restarts, file, size = 0;
    int over;
    char cache[sizeof g_cachedir + 32];
    al_cache_path(cache, sizeof cache, "vulkan_headless_test.vkbc7");
    CHECK(!al_cache_open(cache));
    vk_bc7_cache_stats(&reads, &stored, &restarts, &file);
    Surf* t = al_make(&x, &c, 7);
    CHECK(t && t->bc7.queued && t->bc7.keyed == (1u << x.tex.d.levels) - 1);
    al_until_done(t, &over);
    vk_lock();
    vk_finish(); /* the levels read back land in the cache */
    vk_unlock();
    vk_bc7_cache_stats(&reads, &stored2, &restarts, &file);
    vk_astc_stats(&levels, &hits, &bytes, &rgba8);
    const u64 encodes = vk_astc_dispatches();
    Surf* twin = al_frame(x.twin);
    u8* rgba = twin ? al_image(&x, twin, &size) : NULL;
    const u64 blocks = al_step();
    vk_astc_stats(&levels, &hits2, &bytes, &rgba8);
    const int whole = twin && !twin->bc7.queued && twin->f.vk == VK_FORMAT_BC7_UNORM_BLOCK;
    int failed = !t->bc7.queued && stored2 - stored == x.tex.d.levels && twin && twin != t && rgba && whole &&
                         hits2 - hits == x.tex.d.levels && vk_astc_dispatches() == encodes
                     ? al_differ(&x, "from the cache", twin, rgba)
                     : 1;
    if (failed)
        fprintf(stderr,
                "ASTC as BC7 between frames, %s: %llu levels stored of %u; the twin %p whole %d after a frame of %llu "
                "blocks, %llu levels from the cache, %llu encoder dispatches\n",
                c.name, (unsigned long long)(stored2 - stored), x.tex.d.levels, (void*)twin, whole,
                (unsigned long long)blocks, (unsigned long long)(hits2 - hits),
                (unsigned long long)(vk_astc_dispatches() - encodes));
    else
        printf("ASTC as BC7 between frames, %s: %u levels stored, and the same bytes elsewhere taken from the cache "
               "in one frame, with no encode\n",
               c.name, x.tex.d.levels);
    free(rgba);
    vk_bc7_cache_close();
    remove(cache);
    al_free(&x);
    return failed;
}

/* (j) astc_mode's Auto follows the GPU's memory (gpu/texture/astc_bc.h, device.c): Exact while the caches have room,
   so a texture loads RGBA8 and stays out of the encoder's queue. A trim, with the budget lowered to the caches' use (as
   vk_budget_mb lowers it), turns it to BC7: the next texture is queued for the encoder as it loads, and the game's BC7
   cache opens at the next present, which keeps BC7 with no more room than the trim left. The first, loaded Exact with
   its memory unchanged since, is marked at the switch and read again from memory at its first use of a later present
   (vk_tex_later_exact): queued then, its blocks kept, and made BC7 by the encoder's frames, the RGBA8 texels it showed
   before; the switch's band takes at least the three quarters of its RGBA8 bytes that encode frees. A fourth, loaded
   Exact too but whose memory changed before the switch, stays Exact: its check loads it again first, whether the
   switch marked it or not (write tracking may not have taken that write in yet). A present with the budget back, its room under the
   trim's start past what the trim left, turns it to Exact again, and the third texture loads RGBA8 */
static int al_auto(void) {
    static const AlCase c = {"ASTC 8x8 256x256, Auto", NVN_FORMAT_RGBA_ASTC_8x8, 256, 256, 1, 0};
    const SnxAstcAuto had = g_vk.astc_auto;
    AlTex xa, xb, xc, xe;
    u64 usage, budget, reads, stored, restarts, file0, file, size = 0;
    g_settings.astc_mode = 0;
    g_vk.astc_auto = (SnxAstcAuto){0, 0, 0};
    vk_bc7_cache_stats(&reads, &stored, &restarts, &file0);
    Surf* a = al_make(&xa, &c, 10);
    CHECK(a && a->f.vk == VK_FORMAT_R8G8B8A8_UNORM && !a->bc7_later && !vk_astc_bc7());
    u64 shown_size = 0;
    u8* shown = al_image(&xa, a, &shown_size); /* its RGBA8 texels, no blocks kept */
    const u64 a_bytes = a->bytes;
    CHECK(shown);
    Surf* e = al_make(&xe, &c, 13);
    CHECK(e && e->f.vk == VK_FORMAT_R8G8B8A8_UNORM && !e->bc7_later);
    al_fill(&xe); /* its memory written since it loaded, as the game writes it, and nothing loaded it again yet */
    g_renderer->mem_may_have_changed();
    g_vk_present++; /* a trim of its own present: none has run dry in it */
    vk_mem_use(&usage, &budget);
    const u64 cap = vk_mem_budget_cap(usage);
    vk_lock();
    vk_mem_trim(0, 0);
    vk_unlock();
    const SnxAstcAuto trimmed = g_vk.astc_auto;
    const int a_marked = a->bc7_reread && !a->bc7_later && !a->bc7.queued && trimmed.band >= a_bytes - a_bytes / 4,
              e_marked = e->bc7_reread;
    Surf* b = al_make(&xb, &c, 11);
    vk_lock();
    vk_astc_auto_present();
    vk_unlock();
    const int kept = g_vk.astc_auto.bc7;
    const u64 band = g_vk.astc_auto.band;
    vk_bc7_cache_stats(&reads, &stored, &restarts, &file);
    g_renderer->mem_may_have_changed();
    g_vk_present++; /* the first use of the two since the switch */
    vk_lock();
    const int a_again = vk_tex_from_tic(xa.tic) == a && a->bc7_later && a->bc7.queued && a->bc7.kept &&
                        !a->bc7_reread && a->f.vk == VK_FORMAT_R8G8B8A8_UNORM,
              e_exact = vk_tex_from_tic(xe.tic) == e && !e->bc7_later && !e->bc7.queued && !e->bc7.kept &&
                        e->f.vk == VK_FORMAT_R8G8B8A8_UNORM;
    vk_unlock();
    /* read again: the same RGBA8 texels, and now the blocks it keeps after them, which the encoder makes BC7 */
    u8* rgba = a_again ? al_image(&xa, a, &size) : NULL;
    const int same = rgba && size == shown_size && a->bc7.kept_bytes && !memcmp(rgba, shown, shown_size);
    vk_mem_budget_cap(cap);
    vk_lock();
    vk_astc_auto_present();
    vk_unlock();
    const SnxAstcAuto back = g_vk.astc_auto;
    Surf* d = al_make(&xc, &c, 12);
    int failed = !trimmed.bc7 || trimmed.switches != 1 || !b || !b->bc7_later || !b->bc7.queued || !a_marked ||
                 !a_again || !same || !e_exact || !kept || (!file0 && !file) || back.bc7 || back.switches != 2 ||
                 back.band != band || !d || d->bc7_later || d->f.vk != VK_FORMAT_R8G8B8A8_UNORM;
    if (failed)
        fprintf(
            stderr,
            "astc_mode's Auto: after a trim under a budget of %llu MB, BC7 %d after %llu switches (a band of %llu "
            "MB), the first texture marked %d and read again at its next use %d (its texels the same %d), the one whose memory changed "
            "marked %d and Exact at its next use %d, the next held as BC7 later %d, queued %d, kept at a present "
            "%d, the BC7 cache %llu bytes; with the budget back BC7 %d after %llu switches, the third texture held "
            "as BC7 later %d\n",
            (unsigned long long)(usage >> 20), trimmed.bc7, (unsigned long long)trimmed.switches,
            (unsigned long long)(trimmed.band >> 20), a_marked, a_again, same, e_marked, e_exact, b ? b->bc7_later : -1,
            b ? b->bc7.queued : -1, kept, (unsigned long long)file, back.bc7, (unsigned long long)back.switches,
            d ? d->bc7_later : -1);
    later_drain();
    if (!failed && a->f.vk != VK_FORMAT_BC7_UNORM_BLOCK) {
        fprintf(stderr, "astc_mode's Auto: the texture loaded RGBA8 before the trim is format %d after the encoder\n",
                (int)a->f.vk);
        failed = 1;
    }
    if (!failed) failed = al_differ(&xa, "loaded Exact, read again and made BC7 once Auto turned", a, rgba);
    if (!failed)
        printf("astc_mode's Auto: RGBA8 with room; a trim under a budget of %llu MB made the next texture BC7, and the "
               "one loaded before it too once read again, between frames, while one whose memory changed stayed RGBA8 "
               "(the trim left %llu MB under its start); the budget back made the third RGBA8 again\n",
               (unsigned long long)(usage >> 20), (unsigned long long)(trimmed.band >> 20));
    free(rgba);
    free(shown);
    if (!file0) vk_bc7_cache_close(); /* the game's, which the present opened: the checks after have none */
    g_vk.astc_auto = had;
    g_settings.astc_mode = 2;
    al_free(&xa);
    al_free(&xb);
    al_free(&xc);
    al_free(&xe);
    return failed;
}

static int check_astc_later(void) {
    const int had = g_settings.astc_mode;
    u32 tic[8];
    NvnTexture probe = {0};
    probe.d.target = NVN_TEXTURE_TARGET_2D;
    probe.d.format = NVN_FORMAT_RGBA_ASTC_8x8;
    probe.d.levels = 1;
    probe.d.width = probe.d.height = probe.d.depth = 64;
    probe.d.depth = 1;
    probe.gpu = AL_GVA;
    g_settings.astc_mode = 2;
    CHECK(nvn_tic_encode(&probe, NULL, 0, 0, tic));
    const VkFmt f = vk_tic_format(tic[0], 0);
    if (f.conv != VKC_ASTC || f.vk != VK_FORMAT_BC7_UNORM_BLOCK) {
        printf("ASTC as BC7 between frames: the GPU %s, skipped\n",
               f.conv != VKC_ASTC ? "samples ASTC as it is" : "holds no ASTC texture as BC7");
        g_settings.astc_mode = had;
        return 0;
    }
    later_drain(); /* what the checks before left to the encoder */
    int failed = al_cache_guard();
    failed |= al_frames();
    failed |= al_layers_again();
    failed |= al_written_between();
    static const AlCase layers = {"ASTC 4x4 128x128, 40 layers", NVN_FORMAT_RGBA_ASTC_4x4, 128, 128, 40, 0},
                        volume = {"ASTC 4x4 128x128x40 3D", NVN_FORMAT_RGBA_ASTC_4x4, 128, 128, 40, 1};
    failed |= al_slices(&layers, 3);
    failed |= al_slices(&volume, 6);
    failed |= al_pieces();
    failed |= al_hot();
    failed |= al_hot_pieces();
    failed |= al_freed();
    failed |= al_timed();
    failed |= al_cached();
    failed |= al_auto();
    g_settings.astc_mode = had;
    return failed;
}
