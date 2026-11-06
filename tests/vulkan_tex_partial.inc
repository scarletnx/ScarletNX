/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Textures loaded whole that take their changes a block at a time (gpu/vk/texture.c tex_check_pieces, tex_patch): a
   write of some bytes of a big texture loads the blocks of the pages they are on (of a pitch linear one, the rows
   they lie in), and every texel or block of every level and layer of the image is memory's after it. Each case writes the way a game does (the CPU, through the
   texture's own address or a view of it; a DMA copy; both in one frame), then takes the texture in the next frame and
   compares its image with memory, read in the layout NVN gives the texture, and counts what it loaded in vk_tex_stats'
   bytes. Without write tracking (write_tracking=2) a change is found by hashing and loads the whole texture: the image
   must still be memory's. Each case runs in leaves of the tracking's table of its own. SNX_TEST_TEX_PARTIAL_ONLY=1
   runs them alone */
#define TP_CPU    (HEAP_BASE + 0xA000000)
#define TP_GVA    0x9A0000000ull
#define TP_SPAN   0x1000000ull /* each case's memory: the texture, then a page for its copies' source */
#define TP_VIEW   0x8000000ull /* a view of a case's memory is this far past it */
#define TP_LAYERS 2u /* of a block linear case; a pitch linear one has one */

typedef struct {
    const char* name;
    int format;  /* NVN's */
    u32 eb, bw;  /* bytes of a texel or block, and its texels across and down */
    VkFormat vk; /* what the device has to sample it as, or the case is skipped */
    u32 w, h;    /* level 0's texels */
    u32 pitch;   /* pitch linear, its rows this many bytes apart, one level; 0: block linear, every level */
} TpCase;
/* a case's texture: its description and layout as NVN makes them, its header, where it is, the view of its memory
   (0 without the alias pool: the CPU then writes through the texture's own address) and whether write tracking answers
   for it */
typedef struct {
    const TpCase* c;
    NvnTexture tex;
    NvnTexLayout lay;
    u32 tic[8], layers;
    u64 cpu, gva, view;
    Surf* t;
    int tracked;
} TpTex;

/* byte offset of level l of layer `layer` in the texture's memory */
static u64 tp_level(const TpTex* x, u32 layer, u32 l) {
    return layer * x->lay.layer_size + nvn_tex_level_offset(&x->tex.d, l);
}
/* the byte offset of texel or block (u, v) of level l of layer `layer` */
static u64 tp_at(const TpTex* x, u32 layer, u32 l, u32 u, u32 v) {
    unsigned th, td;
    u32 h = x->c->h >> l ? x->c->h >> l : 1, rows = (h + x->c->bw - 1) / x->c->bw;
    if (x->c->pitch) return (u64)v * x->c->pitch + (u64)u * x->c->eb;
    nvn_tex_level_tiles(&x->tex.d, l, &th, &td);
    return tp_level(x, layer, l) + bl_offset(u * x->c->eb, v, 0, nvn_tex_level_gobs(&x->tex.d, l) * 64, rows, th, td);
}
/* what comes between two frames' draws of the texture: a submission (a new epoch for the caches' checks) and a
   present */
static Surf* tp_frame(const TpTex* x) {
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    vk_lock();
    Surf* t = vk_tex_from_tic(x->tic);
    vk_unlock();
    return t;
}
/* the texels or blocks of t's image, every level of every layer, that differ from memory's: how many, and the first
   in first (layer, level, u, v) with the first 4 bytes the image and memory have of it. -1 when the image couldn't be
   read */
static int tp_differ(const TpTex* x, Surf* t, u32 first[4], u32* image_has, u32* memory_has) {
    const TpCase* c = x->c;
    u8* level = malloc((u64)c->w * c->h * 4);
    int n = 0;
    if (!level) return -1;
    guest_done(x->cpu, x->lay.size);
    for (u32 layer = 0; layer < x->layers && n >= 0; layer++)
        for (u32 l = 0; l < x->tex.d.levels && n >= 0; l++) {
            u32 w = c->w >> l ? c->w >> l : 1, h = c->h >> l ? c->h >> l : 1;
            u32 across = (w + c->bw - 1) / c->bw, rows = (h + c->bw - 1) / c->bw;
            u64 bytes = (u64)across * rows * c->eb;
            VkBuffer buffer;
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                      .imageExtent = {w, h, 1}};
            vk_lock();
            u8* image = vk_readback_buffer(bytes, &buffer);
            if (image) {
                vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
                rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
                vk_finish();
                vk_readback_ready();
                memcpy(level, image, bytes);
            }
            vk_unlock();
            if (!image) {
                n = -1;
                break;
            }
            for (u32 v = 0; v < rows; v++)
                for (u32 u = 0; u < across; u++) {
                    const u8 *in = level + ((u64)v * across + u) * c->eb, *mem = GP(x->cpu + tp_at(x, layer, l, u, v));
                    if (!memcmp(in, mem, c->eb)) continue;
                    if (!n++) {
                        first[0] = layer, first[1] = l, first[2] = u, first[3] = v;
                        *image_has = snx_ld_le32(in);
                        *memory_has = snx_ld_le32(mem);
                    }
                }
        }
    free(level);
    return n;
}
/* the next frame's texture after a write: the one made, all of it memory's, and with write tracking no more than most
   bytes loaded, of some blocks alone. 1 when not, said with the first texel or block that differs */
static int tp_way(TpTex* x, const char* way, u64 most) {
    u64 loads, bytes, loads2, bytes2, partial, partial_bytes, partial2, partial_bytes2;
    u32 first[4] = {0}, image_has = 0, memory_has = 0;
    vk_tex_stats(&loads, &bytes);
    vk_tex_partial_stats(&partial, &partial_bytes);
    Surf* now = tp_frame(x);
    vk_tex_stats(&loads2, &bytes2);
    vk_tex_partial_stats(&partial2, &partial_bytes2);
    int differ = now ? tp_differ(x, now, first, &image_has, &memory_has) : -1;
    u64 loaded = bytes2 - bytes;
    int some = loaded <= most && partial2 > partial && partial_bytes2 - partial_bytes == loaded;
    if (now == x->t && differ == 0 && (!x->tracked || some)) {
        printf("texture changes %s, %s: all of it as memory has it, %llu bytes loaded again%s\n", x->c->name, way,
               (unsigned long long)loaded, x->tracked ? ", the blocks of the pages written" : " (no write tracking)");
        return 0;
    }
    fprintf(stderr,
            "texture changes %s, %s: the same texture %d, %llu bytes loaded again in %llu loads, %llu of them some "
            "blocks alone (%llu bytes; at most %llu wanted of a %llu-byte texture); %d texels or blocks of the image "
            "differ from memory\n",
            x->c->name, way, now == x->t, (unsigned long long)loaded, (unsigned long long)(loads2 - loads),
            (unsigned long long)(partial2 - partial), (unsigned long long)(partial_bytes2 - partial_bytes),
            (unsigned long long)most, (unsigned long long)x->lay.size, differ);
    if (differ > 0)
        fprintf(stderr, "  the first, (%u, %u) of level %u of layer %u: image %08x, memory %08x\n", first[2], first[3],
                first[1], first[0], image_has, memory_has);
    return 1;
}
/* n bytes at off of the texture's memory changed by the CPU (each byte xored with 0x5A), through the view when there
   is one */
static void tp_cpu_write(const TpTex* x, u64 off, u32 n) {
    u8* p = GP((x->view ? x->view : x->cpu) + off);
    guest_done(x->cpu + off, n); /* the GPU's writes there first, as the guest's fence has them */
    for (u32 i = 0; i < n; i++) p[i] ^= 0x5A;
}
/* 64 bytes at off of the texture's memory written by a DMA copy from the case's source page, as they are */
static int tp_dma_write(const TpTex* x, u64 off) {
    GpuDmaOp o = {.width = 64, .line_in = 64, .line_out = 64, .lines = 1};
    o.src = pitch_side(x->gva + TP_SPAN - 0x1000, 0);
    o.dst = pitch_side(x->gva + off, 0);
    return g_renderer->dma_copy(&o);
}

/* the writes of a block linear case: in a block's second page, across two levels, in the mip tail, a DMA copy and the
   CPU in one frame, the CPU over a DMA copy's bytes, and more pages than a check keeps */
static int tp_block_linear(TpTex* x) {
    const u32 levels = x->tex.d.levels;
    int failed = 0;
    /* (a) 16 bytes in the second page of a block of 16 GOBs (8 KiB) of level 0 of layer 1: the block, both pages */
    CHECK(tp_at(x, 1, 0, 300, 200) % 8192 >= 4096);
    tp_cpu_write(x, tp_at(x, 1, 0, 300, 200), 16);
    failed |= tp_way(x, "16 bytes of level 0", 8192);
    /* (b) 32 bytes across levels 3 and 4 of layer 0: a block of each (level 4's blocks are smaller) */
    tp_cpu_write(x, tp_level(x, 0, 4) - 16, 32);
    failed |= tp_way(x, "32 bytes across two levels", 16384);
    /* (c) a byte of the last level of layer 1, in the page of the mip tail's levels: each of them, a block or less */
    tp_cpu_write(x, tp_level(x, 1, levels - 1), 1);
    failed |= tp_way(x, "a byte of the mip tail", 8192);
    /* (d) in one frame, a DMA copy into level 1 of layer 0 and the CPU into level 0 of layer 0 through the view */
    CHECK(tp_dma_write(x, tp_level(x, 0, 1) + 0x2000));
    tp_cpu_write(x, tp_level(x, 0, 0) + 0x30000, 16);
    failed |= tp_way(x, "a DMA copy and the CPU in one frame", 16384);
    /* (e) a DMA copy into a page of level 0 of layer 1, then the CPU over 8 of its bytes: the CPU's are memory's */
    CHECK(tp_dma_write(x, tp_level(x, 1, 0) + 0x50000));
    tp_cpu_write(x, tp_level(x, 1, 0) + 0x50010, 8);
    failed |= tp_way(x, "a DMA copy then the CPU over it", 8192);
    /* (f) a byte in every fourth page for 260 pages, more than the 256 pieces a check keeps: the two closest are joined
       over the pages between them, the others load a block each, less than half the texture */
    for (u32 k = 0; k < 260; k++) tp_cpu_write(x, (u64)k * 0x4000 + 0x123, 1);
    failed |= tp_way(x, "260 pages apart", x->lay.size / 2);
    return failed;
}
/* the writes of a pitch linear case: a 64x64 region (a row's 256 bytes in one page, or in a page that rows a page cuts
   share), a DMA copy and the CPU in one frame, and a byte in each of 300 rows */
static int tp_pitch_linear(TpTex* x) {
    const u32 gp = x->c->pitch, rowb = x->c->w * x->c->eb;
    int failed = 0;
    /* (a) a 64x64 region at (1100, 50): of each row its page's texels, or where a page cuts rows, the rows whole (each
       row twice at most) */
    for (u32 v = 50; v < 114; v++) tp_cpu_write(x, tp_at(x, 0, 0, 1100, v), 256);
    failed |= tp_way(x, "a 64x64 region", gp % 4096 ? 130ull * rowb : 64ull * 4096);
    /* (b) in one frame, a DMA copy into row 300 and the CPU into row 400 through the view: two rows each at most */
    CHECK(tp_dma_write(x, tp_at(x, 0, 0, 30, 300)));
    tp_cpu_write(x, tp_at(x, 0, 0, 1500, 400), 16);
    failed |= tp_way(x, "a DMA copy and the CPU in one frame", 4ull * rowb);
    /* (c) a byte in each of 300 rows, more than the 256 pieces a check keeps: still less than half the texture */
    for (u32 v = 100; v < 400; v++) tp_cpu_write(x, tp_at(x, 0, 0, 8, v), 1);
    failed |= tp_way(x, "a byte in each of 300 rows", x->lay.size / 2);
    return failed;
}
static int tex_partial(const TpCase* c, u32 index) {
    TpTex x = {0};
    const u32 big = c->w > c->h ? c->w : c->h;
    x.c = c;
    x.cpu = TP_CPU + index * TP_SPAN;
    x.gva = TP_GVA + index * TP_SPAN;
    x.layers = c->pitch ? 1 : TP_LAYERS;
    x.tex.d.target = c->pitch ? NVN_TEXTURE_TARGET_2D : NVN_TEXTURE_TARGET_2D_ARRAY;
    x.tex.d.format = (u16)c->format;
    x.tex.d.levels = (u8)(c->pitch ? 1 : 32u - snx_clz32(big)); /* down to 1x1 */
    x.tex.d.width = c->w;
    x.tex.d.height = c->h;
    x.tex.d.depth = x.layers;
    x.tex.d.flags = c->pitch ? NVN_TEXTURE_FLAGS_LINEAR : 0;
    x.tex.d.stride = c->pitch;
    x.tex.gpu = x.gva;
    nvn_tex_layout(&x.tex.d, &x.lay);
    CHECK(x.lay.size + 0x1000 <= TP_SPAN && nvn_tic_encode(&x.tex, NULL, 0, 0, x.tic));
    if (vk_tic_format(x.tic[0], 0).vk != c->vk) {
        printf("texture changes %s: the GPU has no such images, skipped\n", c->name);
        return 0;
    }
    CHECK(mem_map(x.cpu, TP_SPAN, MS_NORMAL, PERM_RW) == 0);
    for (u64 i = 0; i < TP_SPAN; i++) GP(x.cpu)[i] = (u8)(i * 29 + (i >> 12) * 71 + 3);
    gmmu_map(x.gva, x.cpu, TP_SPAN);
    if (mem_pool_active() && !mem_alias_begin(x.cpu, TP_SPAN)) {
        CHECK(mem_alias_map(x.cpu + TP_VIEW, x.cpu, TP_SPAN, MS_NORMAL, PERM_RW) == 0);
        x.view = x.cpu + TP_VIEW;
    } else
        printf("texture changes %s: no alias pool, the CPU writes through the texture's own address\n", c->name);
    vk_lock();
    x.tracked = vk_written_since(GP(x.cpu), x.lay.size, 0) >= 0;
    x.t = vk_tex_from_tic(x.tic);
    vk_unlock();
    CHECK(x.t && x.t->w == c->w && x.t->h == c->h && x.t->layers == x.layers && x.t->levels == x.tex.d.levels &&
          x.t->bl == !c->pitch && x.t->size <= x.lay.size);
    u32 first[4];
    u32 image_has, memory_has;
    CHECK(tp_differ(&x, x.t, first, &image_has, &memory_has) == 0);
    for (int i = 0; i < 3; i++) CHECK(tp_frame(&x) == x.t);
    int failed = c->pitch ? tp_pitch_linear(&x) : tp_block_linear(&x);
    if (x.view) {
        CHECK(mem_alias_unmap(x.view, TP_SPAN, x.cpu) == 0);
        mem_alias_end(x.cpu, TP_SPAN);
    }
    return failed;
}
/* An ASTC texture decoded on the GPU (held as BC7 with astc_bc=1, else RGBA8) takes its changes as the runs of blocks
   of their pages, decoded (and encoded) again: after each write its image is the one a whole upload of the same bytes
   makes, byte for byte (a texture of its own over the same memory at another GPU address, made then), and only those
   runs were loaded. The writes: an ASTC block astride BC7 blocks, a DMA copy, the mip tail */
typedef struct {
    const char* name;
    int format; /* NVN's */
    u32 bw;     /* the footprint, square */
    u32 size;   /* level 0's texels across and down */
    int bc7;    /* astc_bc */
} TaCase;
/* every level of every layer of t's image, read back: the bytes, malloc'd (the caller frees them), and how many */
static u8* ta_image(const TpTex* x, Surf* t, u64* size) {
    const int bc7 = t->f.vk == VK_FORMAT_BC7_UNORM_BLOCK || t->f.vk == VK_FORMAT_BC7_SRGB_BLOCK;
    u64 total = 0, at = 0;
    for (u32 l = 0; l < x->tex.d.levels; l++) {
        u32 side = x->c->w >> l ? x->c->w >> l : 1;
        total += (bc7 ? (u64)((side + 3) / 4) * ((side + 3) / 4) * 16 : (u64)side * side * 4) * x->layers;
    }
    u8* all = malloc(total);
    if (!all) return NULL;
    for (u32 layer = 0; layer < x->layers; layer++)
        for (u32 l = 0; l < x->tex.d.levels; l++) {
            u32 side = x->c->w >> l ? x->c->w >> l : 1;
            u64 bytes = bc7 ? (u64)((side + 3) / 4) * ((side + 3) / 4) * 16 : (u64)side * side * 4;
            VkBuffer buffer;
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                      .imageExtent = {side, side, 1}};
            vk_lock();
            u8* image = vk_readback_buffer(bytes, &buffer);
            if (image) {
                vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
                rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
                vk_finish();
                vk_readback_ready();
                memcpy(all + at, image, bytes);
            }
            vk_unlock();
            if (!image) {
                free(all);
                return NULL;
            }
            at += bytes;
        }
    *size = total;
    return all;
}
/* the GPU address of the whole upload a step compares with: mapped with the texture's, as a mapping made later
   stamps every GPU page that shows the same memory, the texture's too, which a check then loads whole */
#define TA_STEPS 3
static u64 ta_ref(const TpTex* x, u32 step) { return x->gva + 0x1000000000ull + (u64)step * TP_SPAN; }
/* the next frame's texture after a write: the one made, its image a whole upload's of the same bytes (a texture made
   now over them at another GPU address, the step's own), and with write tracking no more than most bytes of guest
   memory decoded again, of some runs alone. 1 when not, said with the first byte that differs */
static int ta_way(TpTex* x, const char* way, u32 step, u64 most) {
    u64 loads, bytes, loads2, bytes2, partial, partial_bytes, partial2, partial_bytes2, size = 0, ref_size = 0;
    vk_tex_stats(&loads, &bytes);
    vk_tex_partial_stats(&partial, &partial_bytes);
    Surf* now = tp_frame(x);
    vk_tex_stats(&loads2, &bytes2);
    vk_tex_partial_stats(&partial2, &partial_bytes2);
    NvnTexture ref = x->tex;
    u32 tic[8];
    ref.gpu = ta_ref(x, step);
    CHECK(step < TA_STEPS && nvn_tic_encode(&ref, NULL, 0, 0, tic));
    vk_lock();
    Surf* whole = vk_tex_from_tic(tic);
    vk_unlock();
    u8* got = now ? ta_image(x, now, &size) : NULL;
    u8* want = whole ? ta_image(x, whole, &ref_size) : NULL;
    u64 differ = 0, first = 0;
    for (u64 i = 0; got && want && i < size && size == ref_size; i++)
        if (got[i] != want[i] && !differ++) first = i;
    u64 loaded = bytes2 - bytes;
    int some = loaded <= most && partial2 > partial && partial_bytes2 - partial_bytes == loaded;
    int same = got && want && size == ref_size && !differ;
    if (now == x->t && whole && whole != now && same && (!x->tracked || some)) {
        printf("texture changes %s, %s: its image a whole upload's, %llu bytes of memory decoded again%s\n",
               x->c->name, way, (unsigned long long)loaded, x->tracked ? ", the runs of the pages written" : "");
        free(got);
        free(want);
        return 0;
    }
    fprintf(stderr,
            "texture changes %s, %s: the same texture %d, a whole upload %d, %llu bytes decoded again in %llu loads, "
            "%llu of them some runs alone (at most %llu wanted); %llu of %llu bytes of the image differ from the whole "
            "upload's, the first at %llu: %02x, the whole upload's %02x\n",
            x->c->name, way, now == x->t, whole && whole != now, (unsigned long long)loaded,
            (unsigned long long)(loads2 - loads), (unsigned long long)(partial2 - partial), (unsigned long long)most,
            (unsigned long long)differ, (unsigned long long)size, (unsigned long long)first,
            got && first < size ? got[first] : 0, want && first < ref_size ? want[first] : 0);
    free(got);
    free(want);
    return 1;
}
static int tex_partial_astc(const TaCase* a, u32 index) {
    TpCase c = {a->name, a->format, 16, a->bw, VK_FORMAT_UNDEFINED, a->size, a->size, 0};
    TpTex x = {0};
    const int had = g_settings.astc_bc;
    u32 rng = 0x9e3779b9u + index;
    x.c = &c;
    x.cpu = TP_CPU + index * TP_SPAN;
    x.gva = TP_GVA + index * TP_SPAN;
    x.layers = TP_LAYERS;
    x.tex.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    x.tex.d.format = (u16)a->format;
    x.tex.d.levels = (u8)(32u - snx_clz32(a->size));
    x.tex.d.width = x.tex.d.height = a->size;
    x.tex.d.depth = TP_LAYERS;
    x.tex.gpu = x.gva;
    nvn_tex_layout(&x.tex.d, &x.lay);
    CHECK(x.lay.size + 0x1000 <= TP_SPAN && nvn_tic_encode(&x.tex, NULL, 0, 0, x.tic));
    g_settings.astc_bc = a->bc7;
    VkFmt f = vk_tic_format(x.tic[0], 0);
    int bc7 = f.vk == VK_FORMAT_BC7_UNORM_BLOCK;
    if (f.conv != VKC_ASTC || bc7 != a->bc7) {
        printf("texture changes %s: the GPU %s, skipped\n", a->name,
               f.conv != VKC_ASTC ? "samples ASTC as it is" : "holds no ASTC texture as BC7");
        g_settings.astc_bc = had;
        return 0;
    }
    CHECK(mem_map(x.cpu, TP_SPAN, MS_NORMAL, PERM_RW) == 0);
    /* blocks of every mode, as random words with their mode bits stepped through (astc_vulkan's blocks_new) */
    for (u64 i = 0; i < TP_SPAN / 4; i++) {
        rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
        snx_st_le32(GP(x.cpu) + i * 4, i % 4 ? rng : (rng & ~2047u) | (u32)(i / 4 & 2047));
    }
    gmmu_map(x.gva, x.cpu, TP_SPAN);
    for (u32 step = 0; step < TA_STEPS; step++) gmmu_map(ta_ref(&x, step), x.cpu, TP_SPAN);
    vk_lock();
    x.tracked = vk_written_since(GP(x.cpu), x.lay.size, 0) >= 0;
    x.t = vk_tex_from_tic(x.tic);
    vk_unlock();
    CHECK(x.t && x.t->f.conv == VKC_ASTC && x.t->levels == x.tex.d.levels && x.t->layers == TP_LAYERS);
    for (int i = 0; i < 3; i++) CHECK(tp_frame(&x) == x.t);
    int failed = 0;
    /* (a) ASTC block 5 of row 7 of level 0 of layer 1, whose texels are astride BC7 blocks where the footprint isn't
       a multiple of 4: its run of blocks, of a block of 16 GOBs at most */
    tp_cpu_write(&x, tp_at(&x, 1, 0, 5, 7), 16);
    failed |= ta_way(&x, "an ASTC block astride BC7 blocks", 0, 8192);
    /* (b) a DMA copy of 64 bytes (4 ASTC blocks) into level 1 of layer 0 */
    CHECK(tp_dma_write(&x, tp_level(&x, 0, 1) + 0x400));
    failed |= ta_way(&x, "a DMA copy", 1, 8192);
    /* (c) a byte of the last level of layer 0, in the page of the mip tail's levels */
    tp_cpu_write(&x, tp_level(&x, 0, x.tex.d.levels - 1), 1);
    failed |= ta_way(&x, "a byte of the mip tail", 2, 8192);
    g_settings.astc_bc = had;
    return failed;
}
static int check_tex_partial(void) {
    const TpCase cases[] = {
        {"RGBA8 1024x1024", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 1024, 1024, 0},
        {"BC1 2048x2048", NVN_FORMAT_RGBA_DXT1, 8, 4, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 2048, 2048, 0},
        {"RGBA8 4096x512 pitch linear, 4 pages a row", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 4096, 512,
         16384},
        {"RGBA8 2000x1200 pitch linear, rows pages cut", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 2000, 1200,
         8448},
    };
    const TaCase astc[] = {
        {"ASTC 6x6 510x510 as BC7", NVN_FORMAT_RGBA_ASTC_6x6, 6, 510, 1},
        {"ASTC 5x5 500x500 as BC7", NVN_FORMAT_RGBA_ASTC_5x5, 5, 500, 1},
        {"ASTC 4x4 512x512 as BC7", NVN_FORMAT_RGBA_ASTC_4x4, 4, 512, 1},
        {"ASTC 6x6 510x510 as RGBA8", NVN_FORMAT_RGBA_ASTC_6x6, 6, 510, 0},
    };
    const u32 n = sizeof cases / sizeof *cases;
    int failed = 0;
    for (u32 i = 0; i < n; i++) failed |= tex_partial(&cases[i], i);
    for (u32 i = 0; i < sizeof astc / sizeof *astc; i++) failed |= tex_partial_astc(&astc[i], n + i);
    CHECK(!failed);
    puts("texture changes: a write of some bytes of a texture loads the blocks of its pages, or the rows they lie in; an "
         "ASTC texture's are decoded as a whole upload decodes them");
    return 0;
}
