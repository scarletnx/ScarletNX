/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_mode's BC7 (vk_astc_bc7): an ASTC texture goes up decoded to RGBA8, which the game samples at once, and the BC7
   encoder makes its BC7 image after the frames' presents, VK_BC7_LATER_BLOCKS blocks a frame at most (texture.c
   vk_tex_later_frame). Whole, the BC7 image takes the RGBA8 one's place, which goes once the frames are done, and its
   blocks are the CPU encoder's (tests/glsl/bc7_cpu.cpp) over the RGBA8 texels, byte for byte, edges, strips cut at the
   frame's budget, layers and 3D slices across frames included. A texture written before its BC7 image is whole starts over on the
   new texels; one written whole after is RGBA8 again until the encoder is done again, and pieces of it more than a
   frame's blocks load whole as RGBA8 too, where fewer go straight to BC7. A texture freed leaves the queue */
#include "bc7_cpu.h"

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
/* level l's texels across, down and deep */
static void al_level(const AlTex* x, u32 l, u32* w, u32* h, u32* d) {
    *w = x->c->w >> l ? x->c->w >> l : 1;
    *h = x->c->h >> l ? x->c->h >> l : 1;
    *d = x->c->d3 ? (x->c->d >> l ? x->c->d >> l : 1) : 1;
}
/* the bytes of level l of a layer: RGBA8 texels or BC7 blocks */
static u64 al_level_bytes(const AlTex* x, u32 l, int bc7) {
    u32 w, h, d;
    al_level(x, l, &w, &h, &d);
    return bc7 ? (u64)((w + 3) / 4) * ((h + 3) / 4) * 16 * d : (u64)w * h * 4 * d;
}
static u32 al_layers(const AlTex* x) { return x->c->d3 ? 1 : x->c->d; }
/* every level of every layer of t's image, read back, level after level, layer after layer; malloc'd, its bytes in
   *size. NULL when it couldn't be */
static u8* al_image(const AlTex* x, Surf* t, u64* size) {
    const int bc7 = t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK;
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
            at += al_level_bytes(x, l, bc7);
        }
    vk_lock();
    u8* back = vk_readback_buffer(at, &buffer);
    u8* out = back ? malloc((size_t)at) : NULL;
    if (out) {
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, n, c);
        vk_finish();
        vk_readback_ready();
        memcpy(out, back, (size_t)at);
    }
    vk_unlock();
    free(c);
    *size = at;
    return out;
}
/* the BC7 blocks the CPU encoder makes of an RGBA8 image read back (al_image), each level of each layer on its own as
   the encoder takes a level: malloc'd, its bytes in *size */
static u8* al_expect(const AlTex* x, const u8* rgba, u64* size) {
    u64 bytes = 0, at = 0, to = 0;
    for (u32 l = 0; l < x->tex.d.levels; l++) bytes += al_level_bytes(x, l, 1);
    bytes *= al_layers(x);
    u8* out = malloc((size_t)bytes);
    if (!out) return NULL;
    for (u32 layer = 0; layer < al_layers(x); layer++)
        for (u32 l = 0; l < x->tex.d.levels; l++) {
            u32 w, h, d;
            al_level(x, l, &w, &h, &d);
            bc7_encode_cpu_build(vk_bc7_build(w, h, d), rgba + at, w, h, d, out + to); /* the level's build */
            at += al_level_bytes(x, l, 0);
            to += al_level_bytes(x, l, 1);
        }
    *size = bytes;
    return out;
}
/* 1 where t's BC7 image isn't the CPU encoder's of rgba, said with the first block that differs */
static int al_differ(const AlTex* x, const char* when, Surf* t, const u8* rgba) {
    u64 got_size = 0, want_size = 0, first = 0, differ = 0;
    u8* got = al_image(x, t, &got_size);
    u8* want = al_expect(x, rgba, &want_size);
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
/* case c's texture, its memory filled, at index's place: the header, its twin's at AL_TWIN past it, made by astc_mode 2
   (BC7) */
static Surf* al_make(AlTex* x, const AlCase* c, u32 index) {
    memset(x, 0, sizeof *x);
    x->c = c;
    x->cpu = AL_CPU + index * AL_SPAN;
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
        return NULL;
    }
    al_fill(x);
    gmmu_map(x->tex.gpu, x->cpu, AL_SPAN);
    gmmu_map(twin.gpu, x->cpu, AL_SPAN);
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
   queued for the encoder. Its RGBA8 texels, malloc'd, in *rgba; 1 when not */
static int al_shown(AlTex* x, Surf* t, const char* when, u8** rgba) {
    u64 size = 0, twin_size = 0;
    g_settings.astc_mode = 1;
    Surf* twin = al_frame(x->twin);
    g_settings.astc_mode = 2;
    *rgba = t ? al_image(x, t, &size) : NULL;
    u8* exact = twin ? al_image(x, twin, &twin_size) : NULL;
    int ok = t && twin && t->upload_valid && t->f.vk == VK_FORMAT_R8G8B8A8_UNORM && t->bc7_later && t->bc7.queued &&
             twin->f.vk == VK_FORMAT_R8G8B8A8_UNORM && !twin->bc7_later && *rgba && exact && size == twin_size &&
             !memcmp(*rgba, exact, (size_t)size);
    if (!ok)
        fprintf(stderr,
                "ASTC as BC7 between frames, %s, %s: the texture %p (format %d, held as BC7 later %d, queued %d, "
                "loaded %d), its Exact twin %p (format %d), their texels %s\n",
                x->c->name, when, (void*)t, t ? (int)t->f.vk : 0, t ? t->bc7_later : 0, t ? t->bc7.queued : 0,
                t ? t->upload_valid : 0, (void*)twin, twin ? (int)twin->f.vk : 0,
                *rgba && exact && size == twin_size && !memcmp(*rgba, exact, (size_t)size) ? "the same" : "differ");
    free(exact);
    return !ok;
}

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
   three layers. Then written whole: RGBA8 again at once, the new texels, and BC7 again from them */
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
    /* written whole: RGBA8 at once, of the new blocks */
    const VkImage bc7_img = t->img;
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

/* (c) A 512x768 ASTC 8x8 texture written whole after the first of its two frames: the encoder drops what it made and
   starts over on the new texels, two frames again */
static int al_written_between(void) {
    static const AlCase c = {"ASTC 8x8 512x768", NVN_FORMAT_RGBA_ASTC_8x8, 512, 768, 1, 0};
    AlTex x;
    u8* rgba = NULL;
    VkLaterStats a, b;
    int over;
    Surf* t = al_make(&x, &c, 2);
    CHECK(t && !al_shown(&x, t, "made", &rgba));
    free(rgba);
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

/* (e) A 2048x2048 ASTC 12x12 texture made BC7, then written in pieces: an ASTC block goes straight to BC7, its image
   staying BC7; 96 KiB of level 0, under half the texture but 55296 BC7 blocks, more than a frame's, load whole as
   RGBA8 that the encoder makes BC7 again. With write tracking only: without it a check by contents loads it whole */
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
    for (int i = 0; i < 3; i++) CHECK(al_frame(x.tic) == t); /* write tracking's scans take in what was written */
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
    guest_done(x.cpu + 0x10000, 96 << 10);
    for (u32 i = 0; i < 96u << 10; i += 64) GP(x.cpu + 0x10000)[i] ^= 0x5A;
    CHECK(al_frame(x.tic) == t);
    if (t->f.vk != VK_FORMAT_R8G8B8A8_UNORM || !t->bc7.queued) {
        fprintf(stderr,
                "ASTC as BC7 between frames, %s: 96 KiB of it written, its image format %d, queued %d, not RGBA8 "
                "and queued\n",
                c.name, (int)t->f.vk, t->bc7.queued);
        return 1;
    }
    CHECK(al_until_done(t, &over) == 11 && !over && t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK);
    printf("ASTC as BC7 between frames, %s: made BC7 in 11 frames; an ASTC block written went straight to BC7, 96 KiB "
           "loaded whole as RGBA8 and made BC7 again\n",
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
    int failed = al_frames();
    failed |= al_layers_again();
    failed |= al_written_between();
    static const AlCase layers = {"ASTC 4x4 128x128, 40 layers", NVN_FORMAT_RGBA_ASTC_4x4, 128, 128, 40, 0},
                        volume = {"ASTC 4x4 128x128x40 3D", NVN_FORMAT_RGBA_ASTC_4x4, 128, 128, 40, 1};
    failed |= al_slices(&layers, 3);
    failed |= al_slices(&volume, 6);
    failed |= al_pieces();
    failed |= al_freed();
    g_settings.astc_mode = had;
    return failed;
}
