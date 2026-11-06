/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Textures loaded whole that take their changes a block at a time (gpu/vk/texture.c tex_check_pieces, tex_patch): a
   write of some bytes of a big texture loads the blocks of the pages they are on, and every texel or block of every
   level and layer of the image is memory's after it. Each case writes the way a game does (the CPU, through the
   texture's own address or a view of it; a DMA copy; both in one frame), then takes the texture in the next frame and
   compares its image with memory, read in the layout NVN gives the texture, and counts what it loaded in vk_tex_stats'
   bytes. Without write tracking (write_tracking=2) a change is found by hashing and loads the whole texture: the image
   must still be memory's. Each case runs in leaves of the tracking's table of its own. SNX_TEST_TEX_PARTIAL_ONLY=1
   runs them alone */
#define TP_CPU    (HEAP_BASE + 0xA000000)
#define TP_GVA    0x9A0000000ull
#define TP_SPAN   0x1000000ull /* each case's memory: the texture, then a page for its copies' source */
#define TP_VIEW   0x8000000ull /* a view of a case's memory is this far past it */
#define TP_LAYERS 2u

typedef struct {
    const char* name;
    int format;  /* NVN's */
    u32 eb, bw;  /* bytes of a texel or block, and its texels across and down */
    VkFormat vk; /* what the device has to sample it as, or the case is skipped */
    u32 size;    /* level 0's texels across and down */
} TpCase;
/* a case's texture: its description and layout as NVN makes them, its header, where it is, the view of its memory
   (0 without the alias pool: the CPU then writes through the texture's own address) and whether write tracking answers
   for it */
typedef struct {
    const TpCase* c;
    NvnTexture tex;
    NvnTexLayout lay;
    u32 tic[8];
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
    u32 side = x->c->size >> l ? x->c->size >> l : 1, units = (side + x->c->bw - 1) / x->c->bw;
    nvn_tex_level_tiles(&x->tex.d, l, &th, &td);
    return tp_level(x, layer, l) + bl_offset(u * x->c->eb, v, 0, nvn_tex_level_gobs(&x->tex.d, l) * 64, units, th, td);
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
    u8* level = malloc((u64)c->size * c->size * 4);
    int n = 0;
    if (!level) return -1;
    guest_done(x->cpu, x->lay.size);
    for (u32 layer = 0; layer < TP_LAYERS && n >= 0; layer++)
        for (u32 l = 0; l < x->tex.d.levels && n >= 0; l++) {
            u32 side = c->size >> l ? c->size >> l : 1, units = (side + c->bw - 1) / c->bw;
            u64 bytes = (u64)units * units * c->eb;
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
                memcpy(level, image, bytes);
            }
            vk_unlock();
            if (!image) {
                n = -1;
                break;
            }
            for (u32 v = 0; v < units; v++)
                for (u32 u = 0; u < units; u++) {
                    const u8 *in = level + ((u64)v * units + u) * c->eb, *mem = GP(x->cpu + tp_at(x, layer, l, u, v));
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

static int tex_partial(const TpCase* c, u32 index) {
    TpTex x = {0};
    const u32 levels = 32u - (u32)__builtin_clz(c->size); /* down to 1x1 */
    x.c = c;
    x.cpu = TP_CPU + index * TP_SPAN;
    x.gva = TP_GVA + index * TP_SPAN;
    x.tex.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    x.tex.d.format = (u16)c->format;
    x.tex.d.levels = (u8)levels;
    x.tex.d.width = x.tex.d.height = c->size;
    x.tex.d.depth = TP_LAYERS;
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
    CHECK(x.t && x.t->w == c->size && x.t->layers == TP_LAYERS && x.t->levels == levels && x.t->size <= x.lay.size);
    u32 first[4];
    u32 image_has, memory_has;
    CHECK(tp_differ(&x, x.t, first, &image_has, &memory_has) == 0);
    for (int i = 0; i < 3; i++) CHECK(tp_frame(&x) == x.t);
    int failed = 0;
    /* (a) 16 bytes in the second page of a block of 16 GOBs (8 KiB) of level 0 of layer 1: the block, both pages */
    CHECK(tp_at(&x, 1, 0, 300, 200) % 8192 >= 4096);
    tp_cpu_write(&x, tp_at(&x, 1, 0, 300, 200), 16);
    failed |= tp_way(&x, "16 bytes of level 0", 8192);
    /* (b) 32 bytes across levels 3 and 4 of layer 0: a block of each (level 4's blocks are smaller) */
    tp_cpu_write(&x, tp_level(&x, 0, 4) - 16, 32);
    failed |= tp_way(&x, "32 bytes across two levels", 16384);
    /* (c) a byte of the last level of layer 1, in the page of the mip tail's levels: each of them, a block or less */
    tp_cpu_write(&x, tp_level(&x, 1, levels - 1), 1);
    failed |= tp_way(&x, "a byte of the mip tail", 8192);
    /* (d) in one frame, a DMA copy into level 1 of layer 0 and the CPU into level 0 of layer 0 through the view */
    CHECK(tp_dma_write(&x, tp_level(&x, 0, 1) + 0x2000));
    tp_cpu_write(&x, tp_level(&x, 0, 0) + 0x30000, 16);
    failed |= tp_way(&x, "a DMA copy and the CPU in one frame", 16384);
    /* (e) a DMA copy into a page of level 0 of layer 1, then the CPU over 8 of its bytes: the CPU's are memory's */
    CHECK(tp_dma_write(&x, tp_level(&x, 1, 0) + 0x50000));
    tp_cpu_write(&x, tp_level(&x, 1, 0) + 0x50010, 8);
    failed |= tp_way(&x, "a DMA copy then the CPU over it", 8192);
    /* (f) a byte in every fourth page for 260 pages, more than the 256 pieces a check keeps: the two closest are joined
       over the pages between them, the others load a block each, less than half the texture */
    for (u32 k = 0; k < 260; k++) tp_cpu_write(&x, (u64)k * 0x4000 + 0x123, 1);
    failed |= tp_way(&x, "260 pages apart", x.lay.size / 2);
    if (x.view) {
        CHECK(mem_alias_unmap(x.view, TP_SPAN, x.cpu) == 0);
        mem_alias_end(x.cpu, TP_SPAN);
    }
    return failed;
}
static int check_tex_partial(void) {
    const TpCase cases[] = {
        {"RGBA8 1024x1024", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 1024},
        {"BC1 2048x2048", NVN_FORMAT_RGBA_DXT1, 8, 4, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 2048},
    };
    int failed = 0;
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) failed |= tex_partial(&cases[i], i);
    CHECK(!failed);
    puts("texture changes: a write of some bytes of a block linear texture loads the blocks of its pages");
    return 0;
}
