/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Textures from the guest's texture headers (TIC, 32 bytes each), and samplers from its sampler headers (TSC). A
   texture whose address holds a render target is that render target (a copy of its top-left corner when the header
   is smaller). Others are loaded from guest memory, staged as they are and deswizzled and converted on the GPU
   (vk/blocklinear.c, vk/texconv.c, vk/astc.c), and checked for changes now and then: often at first, rarely once
   they have stayed the same. */
#include "nx/gpu/vk/descriptor_objects.h"
#include "common/utilities/bits.h"
#include "nx/gpu/texture/descriptor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/tracy.h"

/* the block height and depth (log2 GOBs) of a mip level of a block-linear texture. The hardware shrinks them for
   small levels: the height from the base level on, the depth only from level 1 on and by one step a level
   (Ryujinx's SizeCalculator) */
static void bl_level_blocks(u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 level, u32* lbh, u32* lbd) {
    u32 lh = h >> level ? h >> level : 1, gy = ((lh + f->bh - 1) / f->bh + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    for (u32 l = 1; l <= level; l++) {
        u32 ld = d >> l ? d >> l : 1;
        if (bd > 0 && ld <= (1u << (bd - 1))) bd--;
    }
    *lbh = bh;
    *lbd = bd;
}
/* whether a sparse texture's block-linear level lwb bytes wide, lh rows (of texels or blocks) high and ld slices deep
   is at least a tile in every direction: its tile 2^tile_w GOBs across (the header's tile width), 2^bh high and 2^bd
   deep (the header's block, not the level's shrunk one). The hardware pads such a level's rows to whole tiles */
static int bl_level_tiled(u32 lwb, u32 lh, u32 ld, u32 bh, u32 bd, u32 tile_w) {
    return tile_w && lwb >= 64u << tile_w && lh >= 8u << bh && ld >= 1u << bd;
}
/* the GOBs across that level's rows: its bytes in whole GOBs, or in whole tiles */
static u32 bl_level_gobs_x(u32 lwb, u32 lh, u32 ld, u32 bh, u32 bd, u32 tile_w) {
    u32 g = (lwb + 63) / 64, n = 1u << tile_w;
    return bl_level_tiled(lwb, lh, ld, bh, bd, tile_w) ? (g + n - 1) / n * n : g;
}
/* bytes of one mip level of a block-linear texture; lbh and lbd (may be NULL) get its block height and depth */
static u64 bl_level_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 tile_w, u32 level, u32* lbh,
                         u32* lbd) {
    u32 lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1, ld = d >> level ? d >> level : 1;
    u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
    u32 gx = bl_level_gobs_x(tw * f->bpp, th, ld, bh, bd, tile_w), gy = (th + 7) / 8, b, bdd;
    bl_level_blocks(h, d, f, bh, bd, level, &b, &bdd);
    if (lbh) *lbh = b;
    if (lbd) *lbd = bdd;
    u64 gys = ((gy + (1u << b) - 1) >> b) << b;
    u64 lds = ((ld + (1u << bdd) - 1) >> bdd) << bdd;
    return (u64)gx * gys * lds * 512;
}
/* bytes from one layer of a block-linear texture to the next: all its levels, aligned to a block of GOBs that is
   shrunk for a small base level, or to a sparse texture's whole tile (Ryujinx's AlignLayerSize) */
static u64 bl_layer_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 tile_w, u32 levels) {
    /* the sizes asked last, by shape: every lookup of a texture asks this, and it costs divisions by the block size
       over each level, the levels' block sizes each a loop of its own */
    static struct {
        u32 w, h, d, bh, bd, tile_w, levels, fbw, fbh, fbpp;
        u64 sz;
    } memo[256];
    u32 m = (u32)(((((u64)w * 0x9E3779B1u) ^ h) * 0x85EBCA77u ^
                   ((u64)d << 20 | (u64)levels << 14 | tile_w << 12 | bh << 8 | bd << 4) ^ (u64)f->bpp << 24 ^
                   (u64)f->bw << 28 ^ (u64)f->bh << 32) *
                      0x9E3779B97F4A7C15ull >>
                  56);
    if (memo[m].w == w && memo[m].h == h && memo[m].d == d && memo[m].bh == bh && memo[m].bd == bd &&
        memo[m].tile_w == tile_w && memo[m].levels == levels && memo[m].fbw == f->bw && memo[m].fbh == f->bh &&
        memo[m].fbpp == f->bpp && w)
        return memo[m].sz;
    u32 w0 = w, h0 = h, d0 = d, bh0 = bh, bd0 = bd;
    u64 sz = 0;
    for (u32 l = 0; l < levels; l++) sz += bl_level_size(w, h, d, f, bh, bd, tile_w, l, NULL, NULL);
    u32 gy = ((h + f->bh - 1) / f->bh + 7) / 8;
    if (!tile_w) {
        while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
        while (bd > 0 && d <= (1u << (bd - 1))) bd--;
    }
    u64 al = 512ull << (tile_w + bh + bd);
    sz = (sz + al - 1) / al * al;
    memo[m].w = w0, memo[m].h = h0, memo[m].d = d0, memo[m].bh = bh0, memo[m].bd = bd0, memo[m].tile_w = tile_w;
    memo[m].levels = levels, memo[m].fbw = f->bw, memo[m].fbh = f->bh, memo[m].fbpp = f->bpp, memo[m].sz = sz;
    return sz;
}

/* the guest bytes a move of a level reads (lsz bytes at gva) staged as they are (what the GPU holds of them copied on
   the GPU, vk_guest_read), zero past the level's end: buf and off hold the layout's first byte the move reads
   (vk_bl_range) */
static int stage_bl(const VkBlMove* m, u64 gva, u64 lsz, VkBuffer* buf, u64* off) {
    u64 lo, hi;
    vk_bl_range(m, &lo, &hi);
    u64 n = lo >= lsz ? 0 : hi <= lsz ? hi - lo : lsz - lo;
    u8* p = vk_stage_guest_input(gva + lo, n, hi - lo, buf, off);
    if (!p) return 0;
    vk_guest_read(gva + lo, n, p, *buf, *off);
    memset(p + n, 0, hi - lo - n);
    return 1;
}

/* one level of one layer in guest memory, for vk_astc_upload to ask for its block rows; or with layers, that level of
   each layer, a layer's layer_size bytes after the one before (vk_astc_upload_layers, whose z counts layers) */
typedef struct {
    Surf* t;
    u64 gva;
    u64 lsz;
    u32 rowb, th, lbh, lbd, gobs_x;
    int layers;
} LevelSource;
/* The part of an ASTC level's BC7 key (vk_bc7_level_key) that its guest bytes make: the words that lay its blocks out
   in guest memory, and those bytes as the CPU has them, of each of nlayers layers. 0 when the GPU holds any of them
   (they are not here to hash: an owner's pages hold another's bytes until they land) or memory maps them not all. What
   is pending over them lands first, as a change check's hash has it, which can submit the frame's command buffer */
static int astc_content(const LevelSource* l, u32 nlayers, u64 content[2]) {
    const Surf* t = l->t;
    const u32 layout[8] = {(u32)t->bl, t->bl ? 0 : t->pitch, l->rowb, l->th, l->lbh, l->lbd, l->gobs_x, nlayers};
    content[0] = content[1] = 0;
    vk_bc7_content(content, layout, sizeof layout);
    for (u32 k = 0; k < nlayers; k++) {
        const u64 gva = l->gva + (u64)k * t->layer_size;
        for (u64 o = 0; o < l->lsz;) {
            u64 cpu = 0, n = gmmu_run(gva + o, l->lsz - o, &cpu);
            if (!n || (own_any() && own_lookup(cpu, n, NULL, 0))) return 0;
            gmem_need(gva + o, n);
            vk_bc7_content(content, GP(cpu), n);
            o += n;
        }
    }
    return 1;
}
/* A guest read while staging lands GPU results, and landing can submit the frame's command buffer: the upload's
   commands then belong to the one recording after it, with the image's transfer use established there again */
static VkCommandBuffer upload_cb(Surf* t, VkCommandBuffer cb) {
    VkCommandBuffer now = vk_cmd();
    if (now == cb) return cb;
    vk_surf_use(now, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return now;
}
/* block linear blocks staged as they are and deswizzled on the GPU into the scratch buffer; pitch linear ones staged
   a row at a time */
static int astc_rows(void* ctx, VkCommandBuffer* pcb, u32 z, u32 nz, u32 y, u32 n, VkBuffer* buf, u64* off) {
    LevelSource* l = ctx;
    /* each layer's level a block linear surface of its own: a move each, into the buffer one after another */
    if (l->t->bl && l->layers) {
        VkBlMove m = {l->rowb, l->th, l->lbh, l->lbd, y, n, 0, 1, l->rowb, l->gobs_x};
        u64 cap, slice = (u64)l->rowb * n;
        VkBuffer* staged = malloc(nz * (sizeof *staged + sizeof(u64)));
        u64* at = (u64*)(staged + nz);
        *buf = vk_bl_scratch(slice * nz, &cap);
        *off = 0;
        int ok = staged && *buf;
        for (u32 k = 0; ok && k < nz; k++)
            ok = stage_bl(&m, l->gva + (u64)(z + k) * l->t->layer_size, l->lsz, &staged[k], &at[k]);
        if (ok) {
            VkCommandBuffer cb = *pcb = upload_cb(l->t, *pcb);
            vk_bl_barrier(cb, 1);
            for (u32 k = 0; ok && k < nz; k++) ok = vk_bl_move(cb, &m, 0, staged[k], at[k], *buf, k * slice);
            vk_bl_barrier(cb, 0);
        }
        free(staged);
        return ok;
    }
    if (l->t->bl) {
        VkBlMove m = {l->rowb, l->th, l->lbh, l->lbd, y, n, z, nz, l->rowb, l->gobs_x}; /* 16 byte blocks: whole runs */
        VkBuffer staged;
        u64 at;
        u64 cap;
        *buf = vk_bl_scratch((u64)l->rowb * n * nz, &cap);
        *off = 0;
        if (!*buf || !stage_bl(&m, l->gva, l->lsz, &staged, &at)) return 0;
        VkCommandBuffer cb = *pcb = upload_cb(l->t, *pcb);
        vk_bl_barrier(cb, 1);
        int ok = vk_bl_move(cb, &m, 0, staged, at, *buf, 0);
        vk_bl_barrier(cb, 0);
        return ok;
    }
    /* a slice's rows after the slice before's, a layer layer_size bytes after the one before */
    u64 stride = l->layers ? l->t->layer_size : (u64)l->th * l->t->pitch;
    u64 first = (u64)z * stride + (u64)y * l->t->pitch;
    u64 end = (u64)(z + nz - 1) * stride + (u64)(y + n - 1) * l->t->pitch + l->rowb;
    u8* p = vk_stage_guest_input(l->gva + first, end - first, (u64)l->rowb * n * nz, buf, off);
    if (!p) return 0;
    for (u32 k = 0; k < nz; k++)
        for (u32 r = 0; r < n; r++) {
            u64 at = (u64)(z + k) * stride + (u64)(y + r) * l->t->pitch;
            u64 to = ((u64)k * n + r) * l->rowb;
            vk_guest_read(l->gva + at, l->rowb, p + to, *buf, *off + to);
        }
    *pcb = upload_cb(l->t, *pcb);
    return 1;
}

/* A texture's upload, recorded in batches that share their barriers: a batch's block linear strips are deswizzled
   into the scratch buffer one after another, then the strips of a format the host can't take as it is are converted
   there too, then every strip is copied into the image. The CPU only copies guest memory into staging, in whole
   ranges: GOBs and rows are never taken apart on it */
typedef struct {
    VkBuffer in;
    u64 in_at, out_at;
    u32 in_pitch, w, rows, slices;
} UpConv;
typedef struct {
    VkCommandBuffer cb;
    Surf* t;
    VkBuffer scratch;
    u64 cap, used;
    int open;        /* the barrier before the batch's dispatches is recorded */
    u32 moves;       /* deswizzles recorded in the batch */
    UpConv conv[32]; /* conversions, recorded once the moves they read are done */
    u32 nconv;
    VkBufferImageCopy reg[64];
    VkBuffer src[64]; /* each copy's buffer: the scratch buffer, or staging */
    u32 nreg;
} Upload;
/* the buffer a batch records into now: staging can submit, which ends the one the batch was opened on */
static VkCommandBuffer up_cb(Upload* u) {
    VkCommandBuffer cb = upload_cb(u->t, u->cb);
    if (cb != u->cb) {
        u->cb = cb;
        u->open = 0; /* the batch's barriers ended with the buffer that held them */
    }
    return cb;
}
static void up_open(Upload* u) {
    up_cb(u);
    if (!u->open) vk_bl_barrier(u->cb, 1);
    u->open = 1;
}
static int up_flush(Upload* u) {
    int ok = 1;
    up_cb(u);
    if (u->moves) vk_bl_barrier(u->cb, 0);
    if (u->nconv) {
        up_open(u);
        for (u32 i = 0; i < u->nconv && ok; i++)
            ok = vk_tc_convert(u->cb, &u->t->f, u->conv[i].w, u->conv[i].rows, u->conv[i].slices, u->conv[i].in,
                               u->conv[i].in_at, u->conv[i].in_pitch, u->scratch, u->conv[i].out_at);
        vk_bl_barrier(u->cb, 0);
    }
    for (u32 i = 0, j; i < u->nreg; i = j) {
        for (j = i + 1; j < u->nreg && u->src[j] == u->src[i];) j++;
        rec_CmdCopyBufferToImage(u->cb, u->src[i], u->t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, j - i, u->reg + i);
    }
    u->open = 0;
    u->moves = u->nconv = u->nreg = 0;
    u->used = 0;
    return ok;
}
/* room in the batch for a strip: size bytes of the scratch buffer, two copies and a conversion. After the batch's
   other pieces the strip's lose less than 64 bytes to alignment (its rows start on bl_lin_align, 48 at most, and their
   conversion on 16); in an empty batch none, its first piece at 0 and a conversion right after rows, whose pitch is a
   multiple of 16 (vk_bl_move takes no other). The batch is flushed first when they don't fit, and the scratch buffer
   grown to size when even an empty one wouldn't hold them, so the strip's pieces stay in one batch and one buffer: no
   more, as the buffer stays as big as the most asked of it. 0 when memory ran out */
static int up_reserve(Upload* u, u64 size) {
    if ((!size || u->used + 64 + size <= u->cap) && u->nreg + 2 <= sizeof u->reg / sizeof u->reg[0] &&
        u->nconv < sizeof u->conv / sizeof u->conv[0])
        return 1;
    if (!up_flush(u)) return 0;
    return !size || size <= u->cap || (u->scratch = vk_bl_scratch(size, &u->cap)) != VK_NULL_HANDLE;
}
/* size bytes of the scratch buffer at a multiple of align, within what up_reserve made room for */
static u64 up_space(Upload* u, u64 size, u32 align) {
    u64 at = (u->used + align - 1) / align * align;
    u->used = at + size;
    return at;
}
/* the least common multiple of 16 and a texel's (or block's) bytes */
static u32 bl_lin_align(u32 bpp) {
    u32 a = 16;
    while (a % bpp) a += 16;
    return a;
}

/* one level of one layer, what tex_upload's strips of it share */
typedef struct {
    u32 level, layer, lw, lh, ld, rowb, th;
    u32 conv;     /* converted on the GPU (vk_tc_converts) */
    VkTcShape tc; /* the conversion's shape */
    u32 pitch;    /* the linear rows' pitch: deswizzled, or repacked from a pitch linear level */
    u32 gobs_x;   /* a block linear level's GOBs across (bl_level_gobs_x) */
    u32 x, cols;  /* a run of blocks of a row (tex_patch): its first texel column and texels across; 0 and 0: the row */
} UpLevel;
/* units rows y to y + n (rows of texels, or of blocks) of slices z to z + nz, in linear rows in_pitch bytes apart at
   in_at in in (staging, or the scratch buffer once the batch's moves are done), converted when they need to be and
   copied into the image. The caller has reserved the room (up_reserve) */
static int up_rows(Upload* u, const UpLevel* lv, VkBuffer in, u64 in_at, u32 in_pitch, u32 z, u32 nz, u32 y, u32 n) {
    const VkFmt* f = &u->t->f;
    u32 top = y * f->bh, height = lv->lh - top < n * f->bh ? lv->lh - top : n * f->bh, cols = lv->lw - lv->x;
    if (lv->cols && lv->cols < cols) cols = lv->cols;
    VkBufferImageCopy r = {
        .bufferRowLength = in_pitch / f->bpp * f->bw,
        .bufferImageHeight = n * f->bh,
        .imageSubresource = {f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, lv->level, lv->layer, 1},
        .imageOffset = {(i32)lv->x, (i32)top, (i32)z},
        .imageExtent = {cols, height, nz}};
    if (!lv->conv) {
        r.bufferOffset = in_at;
        u->src[u->nreg] = in;
        u->reg[u->nreg++] = r;
        return 1;
    }
    u64 out = up_space(u, vk_tc_out_size(&lv->tc, n, nz), 16);
    u->conv[u->nconv++] = (UpConv){in, in_at, out, in_pitch, lv->lw, n, nz};
    r.bufferOffset = out;
    r.bufferRowLength = lv->tc.out_texels;
    u->src[u->nreg] = u->scratch;
    u->reg[u->nreg++] = r;
    if (lv->tc.stencil) {
        r.bufferOffset = out + (u64)lv->tc.out_texels * n * nz * 4;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        u->src[u->nreg] = u->scratch;
        u->reg[u->nreg++] = r;
    }
    return 1;
}
/* a block linear strip: move m of bytes staged at staged_at deswizzled into the scratch buffer, then up_rows. Its
   rows land at row y, slice z of the level (the move's own rows and slices are a gathered layout's for a strip of
   GOB rows) */
static int up_move(Upload* u, const UpLevel* lv, const VkBlMove* m, VkBuffer staged, u64 staged_at, u32 z, u32 y) {
    u64 lin = (u64)m->slices * m->rows * m->lin_pitch;
    if (!up_reserve(u, lin + (lv->conv ? vk_tc_out_size(&lv->tc, m->rows, m->slices) : 0))) return 0;
    u64 at = up_space(u, lin, bl_lin_align(u->t->f.bpp));
    up_open(u);
    if (!vk_bl_move(u->cb, m, 0, staged, staged_at, u->scratch, at)) return 0;
    u->moves++;
    return up_rows(u, lv, u->scratch, at, m->lin_pitch, z, m->slices, y, m->rows);
}

/* a block linear level: its guest bytes staged as they are and deswizzled on the GPU. The whole level at once when
   it fits in band; else strips of whole rows of blocks of 2^lbd slices, contiguous in memory; else, when one of those
   is too big, strips of 2^k GOB rows of a slice, each block's piece of them (contiguous in it) staged after the last
   one's: the layout of a surface of one row of blocks 2^k GOBs high */
static int level_upload_bl(Upload* u, const UpLevel* lv, u64 gva, u64 lsz, u32 lbh, u32 lbd, u64 band) {
    u32 rowb = lv->rowb, th = lv->th, ld = lv->ld, pitch = lv->pitch;
    u32 gobs_x = (rowb + 63) >> 6, nz = ld < (1u << lbd) ? ld : 1u << lbd;
    /* a sparse texture's rows of whole tiles are wider than its texels' GOBs */
    u64 block = 512ull << (lbh + lbd), block_row = (u64)lv->gobs_x * block;
    /* scratch bytes a row of units takes: deswizzled, and converted */
    u64 per_row = pitch + (lv->conv ? vk_tc_out_size(&lv->tc, 1, 1) : 0);
    VkBuffer staged;
    u64 staged_at;
    if (per_row * th * ld <= band && lsz <= band) {
        VkBlMove m = {rowb, th, lbh, lbd, 0, th, 0, ld, pitch, lv->gobs_x};
        return stage_bl(&m, gva, lsz, &staged, &staged_at) && up_move(u, lv, &m, staged, staged_at, 0, 0);
    }
    u64 rows_cost = per_row * (8u << lbh) * nz;
    if (rows_cost <= band && block_row <= band) {
        u64 k = band / (rows_cost > block_row ? rows_cost : block_row);
        u32 n = (u32)k * (8u << lbh);
        for (u32 z = 0; z < ld; z += nz)
            for (u32 y = 0; y < th; y += n) {
                u32 rows = th - y < n ? th - y : n, slices = ld - z < nz ? ld - z : nz;
                VkBlMove m = {rowb, th, lbh, lbd, y, rows, z, slices, pitch, lv->gobs_x};
                if (!stage_bl(&m, gva, lsz, &staged, &staged_at) || !up_move(u, lv, &m, staged, staged_at, z, y))
                    return 0;
            }
        return 1;
    }
    u32 k = lbh;
    while (k && (per_row * (8u << k) > band || (u64)gobs_x * (512u << k) > band)) k--;
    if (per_row * 8 > band || (u64)gobs_x * 512 > band) {
        LOG_LIMIT(1, "vulkan: a %ux%u texture level's rows exceed the staging memory (left out)\n", lv->lw, lv->lh);
        return 0;
    }
    u64 rows_blocks = ((((u64)th + 7) >> 3) + (1u << lbh) - 1) >> lbh, piece = 512ull << k;
    for (u32 z = 0; z < ld; z++)
        for (u32 gy = 0; gy < (th + 7) >> 3; gy += 1u << k) {
            u64 base = (z >> lbd) * rows_blocks * block_row + (gy >> lbh) * block_row +
                       ((((u64)(z & ((1u << lbd) - 1)) << lbh) + (gy & ((1u << lbh) - 1))) << 9);
            u64 end = base + (gobs_x - 1) * block + piece;
            u64 have = base >= lsz ? 0 : end <= lsz ? end - base : lsz - base;
            u8* p = vk_stage_guest_input(gva + base, have, gobs_x * piece, &staged, &staged_at);
            if (!p) return 0;
            for (u32 gx = 0; gx < gobs_x; gx++) {
                u64 o = base + gx * block, n = o >= lsz ? 0 : o + piece <= lsz ? piece : lsz - o;
                vk_guest_read(gva + o, n, p + gx * piece, staged, staged_at + gx * piece);
                memset(p + gx * piece + n, 0, piece - n);
            }
            u32 y = gy << 3;
            VkBlMove m = {rowb, 8u << k, k, 0, 0, th - y < (8u << k) ? th - y : 8u << k, 0, 1, pitch};
            if (!up_move(u, lv, &m, staged, staged_at, z, y)) return 0;
        }
    return 1;
}

/* a pitch linear level: its rows staged as they are, whole slices or strips of rows of a slice, when the guest's
   pitch suits the copies and the conversion; else each row into rows of lv->pitch */
static int level_upload_pitch(Upload* u, const UpLevel* lv, u64 gva, u64 lsz, u64 band) {
    const Surf* t = u->t;
    u32 th = lv->th, ld = lv->ld, gp = t->pitch;
    int as_is = gp % (lv->conv ? lv->tc.in_align : 4) == 0 && gp % t->f.bpp == 0;
    u32 pitch = as_is ? gp : lv->pitch;
    u64 out_row = lv->conv ? vk_tc_out_size(&lv->tc, 1, 1) : 0, per_row = pitch > out_row ? pitch : out_row;
    u32 n = per_row * th * ld <= band ? th : (u32)(band / per_row ? band / per_row : 1);
    u32 nz = n == th ? ld : 1;
    if (n > th) n = th;
    for (u32 z = 0; z < ld; z += nz)
        for (u32 y = 0; y < th; y += n) {
            u32 rows = th - y < n ? th - y : n, slices = ld - z < nz ? ld - z : nz;
            VkBuffer staged;
            u64 at, first = ((u64)z * th + y) * gp, size = (u64)rows * slices * pitch;
            u64 end = ((u64)(z + slices - 1) * th + y + rows - 1) * gp + lv->rowb;
            u64 source_bytes = as_is ? first >= lsz ? 0 : first + size <= lsz ? size : lsz - first : end - first;
            u8* p = vk_stage_guest_input(gva + first, source_bytes, size, &staged, &at);
            if (!p) return 0;
            if (as_is) { /* the rows, slices after another, are one range of guest memory */
                u64 have = first >= lsz ? 0 : first + size <= lsz ? size : lsz - first;
                vk_guest_read(gva + first, have, p, staged, at);
                memset(p + have, 0, size - have);
            } else
                for (u32 k = 0; k < slices; k++)
                    for (u32 r = 0; r < rows; r++) {
                        u64 o = ((u64)(z + k) * th + y + r) * gp;
                        u64 to = ((u64)k * rows + r) * pitch;
                        memset(p + to, 0, pitch);
                        vk_guest_read(gva + o, lv->rowb, p + to, staged, at + to);
                    }
            if (!up_reserve(u, lv->conv ? vk_tc_out_size(&lv->tc, rows, slices) : 0) ||
                !up_rows(u, lv, staged, at, pitch, z, slices, y, rows))
                return 0;
        }
    return 1;
}

/* the most staging one piece of a level takes: a bigger level is staged in strips, as no allocation may be bigger
   than g_vk.alloc_cap (memory.c) */
#define TEX_BAND (64ull << 20)

/* for the stats line: every load and its bytes, and those of some blocks alone (tex_patch) */
static struct {
    u64 uploads, bytes, partial, partial_bytes;
    u64 pipelines, pipeline_ns;
} g_tex_loads;
void vk_texture_pipeline_made(u64 ns) {
    g_tex_loads.pipelines++;
    g_tex_loads.pipeline_ns += ns;
}
void vk_texture_pipeline_stats(u64* count, u64* ns) {
    *count = g_tex_loads.pipelines;
    *ns = g_tex_loads.pipeline_ns;
}
void vk_tex_stats(u64* uploads, u64* bytes) {
    *uploads = g_tex_loads.uploads;
    *bytes = g_tex_loads.bytes;
}
void vk_tex_partial_stats(u64* loads, u64* bytes) {
    *loads = g_tex_loads.partial;
    *bytes = g_tex_loads.partial_bytes;
}
/* A texture whose image has memory only where it is mapped (resident_image): its pages, as vk_image_resident takes
   them, are the bricks of each layer, then its mip tails */
static u32 resident_pages(const Surf* t) { return t->layers * t->bricks + t->tails; }
/* the bytes of a layer's bricks, and of all its levels */
static u64 resident_bricks_end(const Surf* t) { return (u64)t->bricks * t->brick_bytes; }
static u64 resident_levels_end(const Surf* t) { return t->size - (u64)(t->layers - 1) * t->layer_size; }
/* the page byte o of such a texture lies in: a brick, or the mip tail of its layer's levels from tail_first on; -1 in
   the padding after a layer's levels. *end gets where that page's bytes there end */
static i64 resident_page(const Surf* t, u64 o, u64* end) {
    const u64 layer = o / t->layer_size, in = o % t->layer_size, base = layer * t->layer_size;
    if (in < resident_bricks_end(t)) {
        *end = base + (in / t->brick_bytes + 1) * t->brick_bytes;
        return (i64)(layer * t->bricks + in / t->brick_bytes);
    }
    if (in < resident_levels_end(t)) {
        *end = base + resident_levels_end(t);
        return (i64)((u64)t->layers * t->bricks + (t->tails == 1 ? 0 : layer));
    }
    *end = base + t->layer_size;
    return -1;
}
/* A level whose device blocks are lower than its blocks of GOBs (an ASTC texture decoded: its 64 KiB tile in 8x8
   blocks is 512 texels high, an RGBA8 device block 128) stacks them down to one block of GOBs, which is one range of
   the guest's layout with the blocks of GOBs beside it: its brick_bytes units are that range's shares, and a stack's
   device blocks are bound together where memory maps any of its bytes. 1 where the level is whole stacks: a 2D ASTC
   texture, rows a whole number of GOBs that divides its block of GOBs, its device blocks down a whole number of stacks */
static int resident_stacks(const Surf* t, u32 rows, u32 lbh, u32 down) {
    const u32 unit = 8u << lbh;
    return t->f.conv == VKC_ASTC && t->type != VK_IMAGE_VIEW_TYPE_3D && rows && rows % 8 == 0 && rows < unit &&
           unit % rows == 0 && down % (unit / rows) == 0;
}
/* the device blocks level l of such a texture stacks (resident_stacks), 1 where its blocks of GOBs are as high */
static u32 resident_stack(const Surf* t, u32 l) {
    u32 lbh, lbd, rows = t->grain.height / t->f.bh;
    bl_level_blocks(t->h, 1, &t->f, t->bh, t->bd, l, &lbh, &lbd);
    return rows && rows < 8u << lbh ? (8u << lbh) / rows : 1;
}
/* want, the units of such a texture in the guest's order (resident_page), as the device's blocks (dev, the order
   vk_image_resident takes), or back (to_guest): a stack's blocks the most of its units, and its units the most of its
   blocks. 0 when no level stacks, both orders the same */
static int resident_order(const Surf* t, u8* want, u8* dev, int to_guest) {
    const VkExtent3D g = t->grain;
    u32 l, at = 0;
    for (l = 0; l < t->tail_first && resident_stack(t, l) == 1; l++) {}
    if (l == t->tail_first) return 0;
    for (u32 layer = 0; layer < t->layers; layer++)
        for (l = 0; l < t->tail_first; l++) {
            const u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, k = resident_stack(t, l);
            const u32 bx = (lw + g.width - 1) / g.width, by = (lh + g.height - 1) / g.height;
            for (u32 u = 0; u < bx * by / k; u++) {
                const u32 x = u % bx, row = u / bx * k;
                u8 v = 0;
                for (u32 j = 0; j < k; j++) {
                    const u8 w = to_guest ? dev[at + (row + j) * bx + x] : want[at + u * k + j];
                    v = w > v ? w : v;
                }
                for (u32 j = 0; j < k; j++) {
                    if (to_guest)
                        want[at + u * k + j] = v;
                    else
                        dev[at + (row + j) * bx + x] = v;
                }
            }
            at += bx * by;
        }
    for (u32 i = t->layers * t->bricks; i < resident_pages(t); i++) {
        if (to_guest)
            want[i] = dev[i];
        else
            dev[i] = want[i];
    }
    return 1;
}
/* the pages of such a texture that memory maps any byte of */
typedef struct {
    const Surf* t;
    u8* want;
} Mapped;
static void page_mapped(void* ctx, u64 gva, u64 cpu, u64 n) {
    Mapped* m = ctx;
    u64 end;
    (void)cpu;
    for (u64 o = gva - m->t->gva; o < gva + n - m->t->gva; o = end) {
        i64 k = resident_page(m->t, o, &end);
        if (k >= 0) m->want[k] = 1;
    }
}
/* such a texture's pages bound where memory maps any of their bytes and released where it maps none
   (vk_image_resident: what isn't bound reads as zeros, as the holes do), the ones bound set in want (resident_pages of
   them, zeroed; 2 where the memory is new). 1 when they all are */
static int resident_bind_mapped(Surf* t, u8* want) {
    Mapped m = {t, want};
    u32 n = 0, pages = resident_pages(t);
    u64 bytes;
    gmmu_pieces(t->gva, t->size, page_mapped, &m);
    u8* dev = malloc(pages);
    if (!dev) return 0;
    const int stacked = resident_order(t, want, dev, 0);
    if (!stacked) memcpy(dev, want, pages);
    for (u32 i = 0; i < pages; i++) n += dev[i];
    /* room made under the budget, a mip tail counted as a brick */
    if ((u64)n * t->brick_bytes > t->bytes) vk_mem_trim((u64)n * t->brick_bytes - t->bytes, 0);
    int ok = vk_image_resident(t->img, dev, pages, &bytes);
    if (!stacked || !resident_order(t, want, dev, 1)) memcpy(want, dev, pages);
    free(dev);
    vk_surf_bytes(t, bytes);
    return ok;
}
static int tex_patch_range(Upload* u, u64 a, u64 b);
/* such a texture's upload: its pages bound where it is mapped (resident_bind_mapped), then each bound brick staged as
   it is and deswizzled on the GPU as a surface of one row of blocks, into its place in its level and layer (an ASTC
   one's blocks decoded there, as its pieces are), and the levels of each bound mip tail as a texture's pieces are
   (tex_patch_range) */
static int resident_upload(Surf* t) {
    const VkFmt* f = &t->f;
    const VkExtent3D g = t->grain;
    const u32 pages = resident_pages(t), d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1,
              across = g.width / f->bw * f->bpp;
    u8* want = calloc(pages, 1);
    if (!want) return 0;
    int ok = resident_bind_mapped(t, want);
    Upload u = {.cb = vk_cmd(), .t = t};
    vk_surf_use(u.cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    u32 i = 0;
    for (u32 layer = 0; layer < t->layers && ok; layer++)
        for (u32 l = 0; l < t->tail_first && ok; l++) {
            const u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, ld = d3 >> l ? d3 >> l : 1;
            u32 lbh, lbd;
            bl_level_blocks(t->h, d3, f, t->bh, t->bd, l, &lbh, &lbd);
            for (u32 z = 0; z < ld && ok; z += g.depth)
                for (u32 y = 0; y < lh && ok; y += g.height)
                    for (u32 x = 0; x < lw && ok; x += g.width, i++) {
                        if (!want[i]) continue;
                        const u64 from = layer * t->layer_size + (u64)(i - layer * t->bricks) * t->brick_bytes;
                        if (f->conv == VKC_ASTC) {
                            ok = tex_patch_range(&u, from, from + t->brick_bytes);
                            continue;
                        }
                        VkBlMove mv = {across, 8u << lbh, lbh, lbd, 0, 8u << lbh, 0, 1u << lbd, across};
                        const u64 lin = (u64)mv.slices * mv.rows * mv.lin_pitch;
                        u64 staged_at, at;
                        VkBuffer staged;
                        ok = stage_bl(&mv, t->gva + from, t->brick_bytes, &staged, &staged_at) && up_reserve(&u, lin);
                        if (!ok) break;
                        at = up_space(&u, lin, bl_lin_align(f->bpp));
                        up_open(&u);
                        if (!(ok = vk_bl_move(u.cb, &mv, 0, staged, staged_at, u.scratch, at))) break;
                        u.moves++;
                        u.src[u.nreg] = u.scratch;
                        u.reg[u.nreg++] = (VkBufferImageCopy){
                            .bufferOffset = at,
                            .bufferRowLength = g.width,
                            .bufferImageHeight = g.height,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                            .imageOffset = {(i32)x, (i32)y, (i32)z},
                            .imageExtent = {lw - x < g.width ? lw - x : g.width, lh - y < g.height ? lh - y : g.height,
                                            ld - z < g.depth ? ld - z : g.depth}};
                        g_tex_loads.bytes += t->brick_bytes;
                    }
        }
    for (u32 k = 0; k < t->tails && ok; k++)
        if (want[t->layers * t->bricks + k])
            for (u32 layer = 0; layer < t->layers && ok; layer++)
                if (t->tails == 1 || layer == k)
                    ok = tex_patch_range(&u, layer * t->layer_size + resident_bricks_end(t),
                                         layer * t->layer_size + resident_levels_end(t));
    ok = up_flush(&u) && ok;
    if (!ok)
        LOG_LIMIT(1, "vulkan: out of memory or staging loading a %ux%ux%u texture's bricks\n", t->w, t->h,
                  t->layers * d3);
    free(want);
    return ok;
}
/* ------------------------------------------------------------ ASTC held as BC7, made between frames */
/* An ASTC texture held as BC7 (vk_astc_bc7) goes up decoded to RGBA8, which the game samples at once, and the BC7
   encoder makes its BC7 image from those texels after the frames' presents, VK_BC7_LATER_BLOCKS blocks a frame at most:
   uploading straight to BC7 held the game for the encoder, 50 to 90 ms when two arrays came in one frame, a second and
   more before the arrays went up a level at a time. Once whole, the BC7 image takes the RGBA8 one's place, which goes
   with its views when the frames that sample them are done. A write of the texture before then (a whole upload or
   pieces, which give it a new wgen) makes the encoder drop what it made and start over behind the others; one whole
   after it is loaded as RGBA8 again, and pieces of it go straight to BC7 while they are a frame's blocks or fewer.
   A texture the game writes more often than the encoder makes its BC7 image is held Exact instead (later_wrote) until
   it goes unwritten a while (later_cool). A texture whose memory is bound by tiles (resident_image), or that has holes
   in its mapping, is made BC7 and loaded straight to it */
static int is_bc7(VkFormat f) { return f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK; }
/* the RGBA8 format a BC7 one is decoded to before it is encoded, and back: the same sRGB */
static VkFormat later_rgba8(VkFormat bc7) {
    return bc7 == VK_FORMAT_BC7_SRGB_BLOCK ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}
static VkFormat later_bc7(VkFormat rgba8) {
    return rgba8 == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
}
/* t's image in format fmt: what tex_create makes it (vf has to live until it is made), storage where fmt is sampled
   so too (*storage) */
static void tex_info(const Surf* t, VkFormat fmt, VkImageCreateInfo* ii, VkViewFormats* vf, int* storage) {
    VkFormatProperties fp;
    const int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    *ii = (VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                              .format = fmt,
                              .extent = {t->w, t->h, is3d ? t->depth3d : 1},
                              .mipLevels = t->levels,
                              .arrayLayers = t->layers,
                              .samples = vk_surf_samples(t),
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       (t->msaa ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
    *storage = !t->msaa && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    if (*storage) ii->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!is3d && t->layers >= 6 && t->w == t->h && !t->msaa) ii->flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    /* a multisampled one is loaded by drawing its samples from the guest's layout of them, through a view of their
       raw bytes (gpu/vk/msaa.c) */
    vk_view_formats(ii, vf, t->msaa ? vk_raw_format(t->f.bpp) : VK_FORMAT_UNDEFINED);
}
/* the textures whose BC7 images the encoder makes, in the order they were loaded, and what it did; the textures held
   Exact for their writes now */
static struct {
    Surf** q;
    u32 n, cap, hot;
    VkLaterStats stats;
} g_later;
/* the BC7 image made so far for t goes once the frames that wrote it are done */
static void later_drop_image(Surf* t) {
    if (t->bc7.img) {
        vk_defer_image(t->bc7.img, t->bc7.mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
        vk_defer_bytes(t->bc7.bytes);
    }
    t->bc7.img = VK_NULL_HANDLE;
    t->bc7.mem = VK_NULL_HANDLE;
    t->bc7.bytes = 0;
    t->bc7.level = t->bc7.slice = t->bc7.row = 0;
}
/* t's RGBA8 image holds what its BC7 one has to, as of its wgen: queued, the encoder starting from its first block */
static void later_queue(Surf* t) {
    /* a queued one starts over on its new wgen */
    if (!t->bc7_later || t->bc7_hot || is_bc7(t->f.vk) || t->bc7.queued) return;
    if (g_later.n == g_later.cap) {
        u32 cap = g_later.cap ? g_later.cap * 2 : 256;
        Surf** q = realloc(g_later.q, (size_t)cap * sizeof *q);
        if (!q) {
            LOG_LIMIT(
                4,
                "vulkan: no memory to queue a %ux%ux%u ASTC texture for the BC7 encoder: it stays RGBA8, %llu MB, "
                "until it is loaded again\n",
                t->w, t->h, t->layers * t->depth3d, (unsigned long long)(t->bytes >> 20));
            return;
        }
        g_later.q = q;
        g_later.cap = cap;
    }
    g_later.q[g_later.n++] = t;
    t->bc7.queued = 1;
    t->bc7.wgen = t->wgen;
    t->bc7.level = t->bc7.slice = t->bc7.row = 0;
}
/* the queue's first texture to its end */
static void later_to_back(void) {
    Surf* t = g_later.q[0];
    memmove(g_later.q, g_later.q + 1, (size_t)(g_later.n - 1) * sizeof *g_later.q);
    g_later.q[g_later.n - 1] = t;
}
/* s out of the queue, and the BC7 image being made for it with it */
static void later_unqueue(Surf* s) {
    if (!s->bc7.queued) return;
    u32 i = 0;
    while (i < g_later.n && g_later.q[i] != s) i++;
    if (i < g_later.n) {
        memmove(g_later.q + i, g_later.q + i + 1, (size_t)(g_later.n - i - 1) * sizeof *g_later.q);
        g_later.n--;
    }
    later_drop_image(s);
    s->bc7.queued = 0;
}
void vk_tex_later_forget(Surf* s) {
    later_unqueue(s);
    if (s->bc7_hot) g_later.hot--;
    s->bc7_hot = 0;
}
/* the frames the encoder takes to make t's BC7 image, VK_BC7_LATER_BLOCKS blocks a frame */
static u32 later_frames(const Surf* t) {
    const int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    u64 blocks = 0;
    for (u32 l = 0; l < t->levels; l++) {
        const u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
        const u32 slices = is3d ? (t->depth3d >> l ? t->depth3d >> l : 1) : t->layers;
        blocks += (u64)((lw + 3) / 4) * ((lh + 3) / 4) * slices;
    }
    return (u32)((blocks + VK_BC7_LATER_BLOCKS - 1) / VK_BC7_LATER_BLOCKS);
}
/* written now, t would be written again before the encoder could make its BC7 image and the game sample that a frame:
   no more frames since its last write than the encoder takes, plus one. A write a frame is one a frame too many for
   any texture, and one of 32768 blocks or more takes as many frames as it has of them */
static int later_soon(const Surf* t) { return t->bc7_wrote && g_vk_present + 1 - t->bc7_wrote <= later_frames(t) + 1; }
/* t, an ASTC texture held as BC7, written (a whole load or pieces, a new wgen): written again soon, it is held Exact,
   out of the queue: the encoder's work would be dropped at its next write, and pieces of a BC7 image are encoded where
   the game waits for them */
static void later_wrote(Surf* t) {
    if (!t->bc7_later) return;
    if (!t->bc7_hot && later_soon(t)) {
        t->bc7_hot = 1;
        g_later.hot++;
        g_later.stats.went_hot++;
        later_unqueue(t);
    }
    t->bc7_wrote = g_vk_present + 1;
}
/* t used: held Exact for its writes and unwritten since for four times the frames its encode takes, plus one, and a
   second at 30 fps at least, it is read more than written: queued to be made BC7 again */
static void later_cool(Surf* t) {
    if (!t->bc7_hot) return;
    const u32 quiet = 4 * (later_frames(t) + 1);
    if (g_vk_present + 1 - t->bc7_wrote < (quiet > 30 ? quiet : 30)) return;
    t->bc7_hot = 0;
    g_later.hot--;
    g_later.stats.cooled++;
    later_queue(t);
}
void vk_tex_later_stats(VkLaterStats* s) {
    *s = g_later.stats;
    s->hot = g_later.hot;
    s->waiting = g_later.n;
    s->waiting_bytes = 0;
    for (u32 i = 0; i < g_later.n; i++) s->waiting_bytes += g_later.q[i]->bytes;
}
/* t's image as img (memory mem, bytes bytes) of format fmt, in UNDEFINED or (copied) TRANSFER_DST_OPTIMAL: the old one
   and its views go once the frames that sample them are done */
static void tex_image_swap(Surf* t, VkImage img, VmaAllocation mem, u64 bytes, VkFormat fmt, int copied) {
    for (int i = 0; i < t->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, t->sv[i].v, VK_NULL_HANDLE);
    t->nsv = 0;
    vk_defer_image(t->img, t->mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_defer_bytes(t->bytes);
    t->img = img;
    t->mem = mem;
    vk_surf_bytes(t, bytes);
    t->f.vk = fmt;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
    t->storage_capable = !t->msaa && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    t->layout = copied ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    t->stage = copied ? VK_PIPELINE_STAGE_2_COPY_BIT : 0;
    t->access = copied ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0;
}
/* t, held as BC7 and loaded whole again: an RGBA8 image in place of its BC7 one, which the load fills and the encoder
   makes BC7 again (later_queue). 0 when no memory was found for it: the texture is left out until a check finds some,
   as any image that finds none */
static int later_rgba8_again(Surf* t) {
    VkImageCreateInfo ii;
    VkViewFormats vf;
    VkImage img;
    VmaAllocation mem;
    u64 bytes;
    int storage;
    const VkFormat rgba8 = later_rgba8(t->f.vk);
    tex_info(t, rgba8, &ii, &vf, &storage);
    if (!vk_cache_image_create(&ii, &img, &mem, &bytes)) {
        LOG_LIMIT(4,
                  "vulkan: no memory for the RGBA8 image (%llu MB) a %ux%ux%u ASTC texture held as BC7 is loaded again "
                  "into: it is left out until a check finds some\n",
                  (unsigned long long)(bytes >> 20), t->w, t->h, t->layers * t->depth3d);
        return 0;
    }
    tex_image_swap(t, img, mem, bytes, rgba8, 0);
    return 1;
}
/* encodes t's BC7 image on from where the encoder got, up to left blocks, its first strip whatever its size: the RGBA8
   image a copy's source and back in the layout it was sampled in. The blocks it took */
static u32 later_encode(VkCommandBuffer cb, Surf* t, u32 left) {
    const int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    const u64 cap = vk_bc7_strip_texels();
    const VkImageLayout was = t->layout;
    const VkPipelineStageFlags2 stage = t->stage;
    const VkAccessFlags2 access = t->access;
    const int first = !t->bc7.level && !t->bc7.slice && !t->bc7.row;
    u32 used = 0;
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    /* the BC7 image's copies after the last frame's into it (from nothing when the encoder starts on it) */
    vk_barrier(cb, t->bc7.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
               first ? 0 : VK_ACCESS_2_TRANSFER_WRITE_BIT,
               first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    while (t->bc7.level < t->levels && (!used || used < left)) {
        const u32 l = t->bc7.level, lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
        const u32 slices = is3d ? (t->depth3d >> l ? t->depth3d >> l : 1) : t->layers;
        const u32 cx = (lw + 3) / 4, cy = (lh + 3) / 4;
        const u64 room = left > used ? left - used : 1, slice_blocks = (u64)cx * cy, row_texels = (u64)lw * 4;
        const int keyed = l < VK_TEX_MAX_LEVELS && (t->bc7.keyed >> l & 1);
        /* a level the BC7 cache has: copied from it, a sixteenth of the encoder's cost a block counted */
        if (keyed && !t->bc7.slice && !t->bc7.row &&
            vk_bc7_cache_level(cb, t->bc7.img, t->bc7.key[l], l, lw, lh, slices, is3d)) {
            used += (u32)((slice_blocks * slices + 15) / 16);
            t->bc7.level++;
            continue;
        }
        u32 nz = 1, n;
        if (!t->bc7.row && slice_blocks <= room && row_texels * cy <= cap) {
            /* whole slices, as many as the blocks left and the scratch take */
            u64 k = room / slice_blocks, by_cap = cap / (row_texels * cy);
            if (k > by_cap) k = by_cap;
            nz = slices - t->bc7.slice < k ? slices - t->bc7.slice : (u32)k;
            n = cy;
        } else {
            /* rows of blocks of one slice: one at least */
            u64 k = room / cx, by_cap = cap / row_texels;
            if (k > by_cap) k = by_cap;
            if (!k) k = 1;
            n = cy - t->bc7.row < k ? cy - t->bc7.row : (u32)k;
        }
        const u32 y = t->bc7.row * 4, rows = lh - y < n * 4 ? lh - y : n * 4;
        if (!vk_bc7_encode_image(cb, t->img, t->bc7.img, l, lw, lh, slices, is3d, t->bc7.slice, nz, y, rows)) break;
        used += cx * n * nz;
        if (n == cy)
            t->bc7.slice += nz;
        else if ((t->bc7.row += n) == cy) {
            t->bc7.row = 0;
            t->bc7.slice++;
        }
        if (t->bc7.slice == slices) {
            /* the level whole: its blocks read back for the cache, where it has room for them now */
            if (keyed) vk_bc7_capture_level(cb, t->bc7.img, t->bc7.key[l], l, lw, lh, slices, is3d);
            t->bc7.slice = 0;
            t->bc7.level++;
        }
    }
    if (was == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || was == VK_IMAGE_LAYOUT_GENERAL)
        vk_surf_use(cb, t, was, stage, access);
    return used;
}
void vk_tex_later_frame(void) {
    if (!g_later.n || !vk_cmd_ready()) return;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    u32 left = VK_BC7_LATER_BLOCKS;
    SNX_ZONE(z, "vk BC7 later");
    /* each texture is put back once at most (skipped) for a failed load, and once for a write */
    for (u32 skipped = 0; left && g_later.n && skipped < g_later.n;) {
        Surf* t = g_later.q[0];
        if (t->bc7.wgen != t->wgen) {
            /* written since the encoder started: what it made goes, and the texture waits behind the others */
            if (t->bc7.img) g_later.stats.restarted++;
            later_drop_image(t);
            t->bc7.wgen = t->wgen;
            later_to_back();
            continue;
        }
        if (!t->upload_valid) { /* its upload is tried again first: the RGBA8 image isn't whole */
            later_to_back();
            skipped++;
            continue;
        }
        if (!t->bc7.img) {
            VkImageCreateInfo ii;
            VkViewFormats vf;
            int storage;
            tex_info(t, later_bc7(t->f.vk), &ii, &vf, &storage);
            vk_surf_touch(t); /* used now: the trim that makes room for the image leaves t, and t first in the queue */
            if (!vk_cache_image_create(&ii, &t->bc7.img, &t->bc7.mem, &t->bc7.bytes)) {
                LOG_LIMIT(4,
                          "vulkan: no memory for the BC7 image (%llu MB) of a %ux%ux%u ASTC texture: it stays RGBA8, "
                          "%llu MB, and the encoder tries again in the next frame\n",
                          (unsigned long long)(t->bc7.bytes >> 20), t->w, t->h, t->layers * t->depth3d,
                          (unsigned long long)(t->bytes >> 20));
                t->bc7.img = VK_NULL_HANDLE;
                t->bc7.mem = VK_NULL_HANDLE;
                t->bc7.bytes = 0;
                later_to_back();
                break;
            }
        }
        if (!cb) {
            vk_render_end(); /* copies and dispatches happen outside rendering passes */
            cb = vk_cmd();
        }
        u32 used = later_encode(cb, t, left);
        g_later.stats.blocks += used;
        left = used < left ? left - used : 0;
        if (t->bc7.level < t->levels) {
            if (!used) break; /* no descriptor set left: the next frame goes on */
            continue;
        }
        /* whole: in the RGBA8 image's place, in the layout that one was sampled in */
        const VkImageLayout was = t->layout;
        const VkPipelineStageFlags2 stage = t->stage;
        const VkAccessFlags2 access = t->access;
        g_later.stats.made++;
        g_later.stats.freed += t->bytes > t->bc7.bytes ? t->bytes - t->bc7.bytes : 0;
        tex_image_swap(t, t->bc7.img, t->bc7.mem, t->bc7.bytes, later_bc7(t->f.vk), 1);
        if (was == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || was == VK_IMAGE_LAYOUT_GENERAL)
            vk_surf_use(cb, t, was, stage, access);
        memset(&t->bc7, 0, sizeof t->bc7);
        memmove(g_later.q, g_later.q + 1, (size_t)(g_later.n - 1) * sizeof *g_later.q);
        g_later.n--;
    }
    if (cb) vk_submit_recorded(); /* its own submission, after the present's */
}

/* 1 when every subresource was copied; 0 when memory or staging ran out, and upload_valid keeps it for a retry */
static int tex_upload_(Surf* t);
/* a texture's upload as a profiler zone (SNX_TRACY builds) */
static int tex_upload(Surf* t) {
    SNX_ZONE_V(z, "texture upload", t->size);
    int r = tex_upload_(t);
    SNX_ZONE_END(z);
    return r;
}
static int tex_upload_(Surf* t) {
    const VkFmt* f = &t->f;
    SNX_ZONE_T(z_up, "texture upload", "%ux%u", t->w, t->h);
    SNX_ZONE_VALUE(z_up, t->size);
    g_tex_loads.uploads++;
    if (!t->bricks) g_tex_loads.bytes += t->size; /* a brick at a time for the others */
    if (t->upload_valid || !t->bc7_wrote) later_wrote(t); /* the first load, or a write: not a failed one tried again */
    t->upload_valid = 0;
    t->wgen = vk_surf_gen(); /* even a new texture at a reused address invalidates an old target's copy */
    t->took_drawn = 0;       /* memory's own texels again, until tex_take_drawn copies targets in */
    vk_surf_relist(t);
    vk_render_end();                                     /* copies and clears happen outside rendering passes */
    if (t->msaa) return t->upload_valid = vk_ms_load(t); /* its samples drawn from the guest's layout of them */
    if (t->bricks) return t->upload_valid = resident_upload(t);
    /* held as BC7 and whole: RGBA8 again, made BC7 again between frames */
    if (t->bc7_later && is_bc7(f->vk) && !later_rgba8_again(t)) return t->upload_valid = 0;
    t->bc7.keyed = 0; /* the BC7 cache's keys of the bytes loaded now, below */
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    u32 layers = t->layers, d3 = is3d ? t->depth3d : 1;
    /* a strip's scratch rows keep the buffer that big (vk_bl_scratch): VK_PART_SHARE of the cap at most */
    u64 band = g_vk.alloc_cap / VK_PART_SHARE < TEX_BAND ? g_vk.alloc_cap / VK_PART_SHARE : TEX_BAND;
    int complete = 1;
    Upload u = {.cb = cb, .t = t};
    /* an ASTC array a level at a time, every layer of it in one pass (vk_astc_upload_layers) */
    const int astc_layers = f->conv == VKC_ASTC && !is3d && layers > 1;
    for (u32 layer = 0; layer < (astc_layers ? 1 : layers) && complete; layer++) {
        u64 at = layer * t->layer_size;
        for (u32 l = 0; l < t->levels && complete; l++) {
            u32 lw = t->w >> l, lh = t->h >> l, ld = d3 >> l;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            if (!ld) ld = 1;
            u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
            u32 rowb = tw * f->bpp, lbh = 0, lbd = 0;
            u64 lsz = t->bl ? bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, t->tile_w, l, &lbh, &lbd)
                            : (u64)t->pitch * th * ld;
            u32 gobs_x = bl_level_gobs_x(rowb, th, ld, t->bh, t->bd, t->tile_w);
            if (f->conv == VKC_ASTC) { /* decoded on the GPU from its block rows, deswizzled on the GPU */
                LevelSource ls = {t, t->gva + at, lsz, rowb, th, lbh, lbd, gobs_x, astc_layers};
                /* the BC7 cache's key of a level held as BC7, straight (vk_astc_upload takes it) or between frames
                   (kept for the encoder there), hashed before the upload stages the same bytes */
                const u32 nlayers = astc_layers ? layers : 1, slices = astc_layers ? layers : ld;
                const int straight = is_bc7(f->vk), later = t->bc7_later && !straight;
                u64 content[2];
                const int keyed = (straight || later) && l < VK_TEX_MAX_LEVELS && vk_astc_cached(lw, lh, slices) &&
                                  astc_content(&ls, nlayers, content);
                if (keyed && later) {
                    const VkFormat bc7 = later_bc7(f->vk);
                    vk_bc7_level_key(content, lw, lh, is3d ? ld : 1, nlayers, f->bw, f->bh,
                                     bc7 == VK_FORMAT_BC7_SRGB_BLOCK, vk_bc7_build(lw, lh, is3d ? ld : 1),
                                     t->bc7.key[l]);
                    t->bc7.keyed |= 1u << l;
                }
                const u64* key_content = keyed && straight ? content : NULL;
                complete = up_flush(&u); /* after the hash, which can submit: the batch's buffer is the one recording */
                complete =
                    complete &&
                    (astc_layers ? vk_astc_upload_layers(u.cb, t, lw, lh, l, 0, layers, astc_rows, &ls, key_content)
                                 : vk_astc_upload(u.cb, t, 0, 0, lw, lh, ld, l, layer, astc_rows, &ls, key_content));
                u.scratch = VK_NULL_HANDLE; /* astc_rows may have grown the scratch buffer: asked for again */
                u.cap = 0;
            } else {
                UpLevel lv = {l, layer, lw, lh, ld, rowb, th, (u32)vk_tc_converts(f)};
                u32 align = bl_lin_align(f->bpp);
                if (lv.conv) {
                    lv.tc = vk_tc_shape(f, lw);
                    if (lv.tc.in_align > align) align = lv.tc.in_align;
                }
                lv.pitch = (rowb + align - 1) / align * align;
                lv.gobs_x = gobs_x;
                complete = t->bl ? level_upload_bl(&u, &lv, t->gva + at, lsz, lbh, lbd, band)
                                 : level_upload_pitch(&u, &lv, t->gva + at, lsz, band);
            }
            if (!complete)
                LOG_LIMIT(1, "vulkan: out of memory or staging uploading a %ux%u texture level (left out)\n", lw, lh);
            at += lsz;
        }
    }
    complete = up_flush(&u) && complete;
    if (complete) later_queue(t);
    return t->upload_valid = complete;
}

/* change checks: a hash of the whole texture, 4 KiB at a time, the same whichever way its bytes are reached (the
   sampled check for big long-stable textures is vk_sample_hash) */
#define MEM_HASH_PIECE 4096u
static u64 g_tex_hashes; /* texture memory hashed to learn whether it changed, whole or sampled (vk_tex_hashes) */
u64 vk_tex_hashes(void) { return g_tex_hashes; }
static u64 tex_sample_hash(u64 gva, u64 size) {
    g_tex_hashes++;
    return vk_sample_hash(gva, size);
}
static u64 mem_hash(u64 gva, u64 size) {
    u64 piece[MEM_HASH_PIECE / 8];
    u64 h = 1469598103934665603ull;
    g_tex_hashes++;
    /* bytes the GPU holds are trapped: they hash as their owner (vk_guest_peek), so the texture is read in pieces.
       Every byte counts: the CPU rewrites part of a page (a row of a texture, half a GOB's rows) */
    u8* p = own_any() ? NULL : gmem_ptr(gva, size);
    u64 tag;
    /* all of it one GPU-owned range (an engine's result: a video's picture from VIC, a copy's): its owner's identity
       there, the pattern vk_guest_peek writes over each piece, not that pattern hashed byte by byte, 0.7 ms a frame
       for a video's 8 MB picture */
    if (!p && vk_guest_owned_whole(gva, size, &tag)) return snx_fnv_step(h, tag);
    for (u64 o = 0; o < size; o += MEM_HASH_PIECE) {
        u64 n = size - o < MEM_HASH_PIECE ? size - o : MEM_HASH_PIECE;
        if (!p) vk_guest_peek(gva + o, piece, n);
        h = snx_fnv_step(h, vk_hash(p ? p + o : (const u8*)piece, n));
    }
    return h;
}
/* the hash of t's memory: of all of it, or for one with holes in its mapping loaded a piece at a time, of each run the
   GPU's page table maps of it with where it lies, as its holes read as zeros (a 2D sparse texture can be hundreds of
   megabytes of them) and a run mapped or unmapped changes the runs. The runs are taken RUNS_BATCH at a time under the
   page table's index lock, and hashed after it is free: mem_hash takes pend_lock and may land results, and a thread
   that holds pend_lock waits for the index lock (guest_covered in renderer.c). Hashed inside gmmu_pieces, the GPU
   thread and one game's UnmapPhysicalMemory each held the lock the other waited for, with vk_lock held too, and the
   game stopped before its title screen */
#define RUNS_BATCH 64
typedef struct {
    u64 next; /* where the next batch starts: the end of the last run taken */
    u32 n;
    struct {
        u64 gva, n;
    } run[RUNS_BATCH];
} RunsTaken;
static void runs_take(void* ctx, u64 gva, u64 cpu, u64 n) {
    RunsTaken* r = ctx;
    (void)cpu;
    if (r->n == RUNS_BATCH) return;
    r->run[r->n].gva = gva;
    r->run[r->n].n = n;
    r->n++;
    r->next = gva + n;
}
static u64 tex_hash(const Surf* t) {
    u64 h = 1469598103934665603ull, at = t->gva, end = t->gva + t->size;
    if (!t->piecewise) return mem_hash(t->gva, t->size);
    while (at < end) {
        RunsTaken r = {0};
        gmmu_pieces(at, end - at, runs_take, &r);
        for (u32 i = 0; i < r.n; i++)
            h = snx_fnv_step(snx_fnv_step(h, r.run[i].gva - t->gva), mem_hash(r.run[i].gva, r.run[i].n));
        if (r.n < RUNS_BATCH) break;
        at = r.next;
    }
    return h;
}
/* A linear texture over memory a shader wrote in place, the write not done yet: its bytes are copied from the
   import on the GPU, after the write, as hashing them would wait for the submit and all the work before it. One game's
   exposure histogram, a 64x1 texture one compute program fills and the next reads, held the GPU thread for 20 ms of each
   30 ms frame with the GPU idle behind it. 1 if it was copied. The hash then matches nothing: the next check that finds
   the write done loads the memory again */
static int tex_copy_pending(Surf* t) {
    const VkFmt* f = &t->f;
    VkBuffer ib;
    u64 cpu = 0, io;
    if (!vk_uma_pending(t->gva, t->size) || t->bl || t->bricks || t->piecewise || t->msaa || t->is_rt || f->depth ||
        f->conv || vk_tc_converts(f) || f->bw != 1 || f->bh != 1 || !f->bpp || t->levels != 1 || t->layers != 1 ||
        t->type == VK_IMAGE_VIEW_TYPE_3D || t->pitch % f->bpp || t->pitch < t->w * f->bpp)
        return 0;
    vk_uma_gpu_reads(1);        /* the in-place writes are copied below, not waited for */
    gmem_need(t->gva, t->size); /* anything else pending there lands first */
    vk_uma_gpu_reads(0);
    vk_render_end(); /* copies happen outside rendering passes */
    if (gmmu_run(t->gva, t->size, &cpu) != t->size || !vk_uma_copy_pending(cpu, t->size, &ib, &io) || io % f->bpp)
        return 0;
    VkCommandBuffer cb = vk_cmd();
    VkBufferImageCopy r = {.bufferOffset = io,
                           .bufferRowLength = t->pitch / f->bpp,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {t->w, t->h, 1}};
    vk_all_barrier(cb); /* the writer's, in stream order */
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    rec_CmdCopyBufferToImage(cb, ib, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    t->wgen = vk_surf_gen();
    t->took_drawn = 0;
    vk_surf_relist(t);
    t->upload_valid = 1;
    t->hash = ~t->hash;
    return 1;
}
/* the texture's memory hashed again, and loaded again when that changed: 1 if it did */
static int tex_rehash(Surf* t) {
    t->gw = gpu_mem_generation();
    if (tex_copy_pending(t)) return 1;
    u64 hh = tex_hash(t);
    if (hh == t->hash && t->upload_valid) return 0;
    if (tex_upload(t)) t->hash = hh; /* an incomplete upload keeps the old hash, so the next check retries it */
    return 1;
}

int vk_tex_dma_rows(u64 gva, u32 wb, u32 bh, u32 ox, u32 oy, u32 line_bytes, u32 rows, VkBuffer buf, u64 rows_at,
                    u32 rows_pitch, Surf** hit, int max) {
    int n = 0;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    for (Surf* t = vk_surf_first(gva); t && n < max; t = t->next) {
        const VkFmt* f = &t->f;
        if (t->is_rt || t->gva != gva || !t->upload_valid || !t->bl || t->bh != bh || t->bd || t->msaa ||
            t->type == VK_IMAGE_VIEW_TYPE_3D || f->depth || f->conv || vk_tc_converts(f) || !f->bpp || !f->bw ||
            !f->bh || ox % f->bpp || line_bytes % f->bpp || rows_pitch % f->bpp || (rows_at % f->bpp) || (rows_at & 3))
            continue;
        u32 tw = (t->w + f->bw - 1) / f->bw, th = (t->h + f->bh - 1) / f->bh;
        /* the same layout: as many GOBs a row (their addresses don't depend on the height), the rows within level 0
           of a texture with more of them after it */
        if (bl_level_gobs_x(tw * f->bpp, th, 1, t->bh, t->bd, t->tile_w) != ((u64)wb + 63) >> 6 || oy >= th ||
            ((t->levels > 1 || t->layers > 1) && oy + rows > th))
            continue;
        /* what an engine wrote since its last check would be lost */
        if (gpu_mem_dirty_since(t->gw, t->gva, t->size)) continue;
        u32 x = ox / f->bpp * f->bw, y = oy * f->bh, w = line_bytes / f->bpp * f->bw, h = rows * f->bh;
        if (x >= t->w) continue;
        if (w > t->w - x) w = t->w - x;
        if (h > t->h - y) h = t->h - y;
        VkBufferImageCopy r = {.bufferOffset = rows_at,
                               .bufferRowLength = rows_pitch / f->bpp * f->bw,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageOffset = {(i32)x, (i32)y, 0},
                               .imageExtent = {w, h, 1}};
        if (!cb) cb = vk_cmd();
        vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        rec_CmdCopyBufferToImage(cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        hit[n++] = t;
    }
    return n;
}
void vk_tex_dma_seen(Surf** hit, int n) {
    for (int i = 0; i < n; i++) hit[i]->gw = gpu_mem_generation();
}

void vk_tex_note_writeback(Surf* t) {
    t->hash = tex_hash(t);
    t->gw = gpu_mem_generation();
    t->wseen = vk_ww_now();
    t->checked = g_vk_present;
    t->stable = 0;
}

static int format_samples(VkFormat f) {
    static VkFormat ok[64], bad[64];
    static int nok, nbad;
    for (int i = 0; i < nok; i++)
        if (ok[i] == f) return 1;
    for (int i = 0; i < nbad; i++)
        if (bad[i] == f) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    int r = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    if (r && nok < 64) ok[nok++] = f;
    if (!r && nbad < 64) {
        bad[nbad++] = f;
        WARN("vulkan: the GPU can't sample format %d (textures in it aren't drawn)\n", (int)f);
    }
    return r;
}

static int tex_can_piece(const Surf* t);
/* t's format in a log line: an ASTC footprint and the format it is held in, decoded, else Vulkan's */
static const char* tex_format_name(const Surf* t, char* s, size_t n) {
    const VkFmt* f = &t->f;
    if (f->conv == VKC_ASTC)
        snprintf(s, n, "ASTC %ux%u as %s", f->bw, f->bh,
                 f->vk == VK_FORMAT_BC7_UNORM_BLOCK || f->vk == VK_FORMAT_BC7_SRGB_BLOCK ? "BC7" : "RGBA8");
    else
        snprintf(s, n, "format %d", (int)f->vk);
    return s;
}
/* level l of t's layout: the GOBs across its rows, and the block of GOBs it is laid out in (log2 high and deep) */
static u32 level_layout(const Surf* t, u32 l, u32* lbh, u32* lbd) {
    const VkFmt* f = &t->f;
    const u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1, lw = t->w >> l ? t->w >> l : 1,
              lh = t->h >> l ? t->h >> l : 1, ld = d3 >> l ? d3 >> l : 1;
    bl_level_blocks(t->h, d3, f, t->bh, t->bd, l, lbh, lbd);
    return bl_level_gobs_x((lw + f->bw - 1) / f->bw * f->bpp, (lh + f->bh - 1) / f->bh, ld, t->bh, t->bd, t->tile_w);
}
/* t's image with memory only where it is mapped (vk_image_create_resident), when the device binds it in bricks that
   are each one range of the guest's layout: whole blocks of GOBs side by side in one row of them, as a sparse
   texture's 64 KiB tile is (nvn_tex_layout), or a stack of them is (resident_stacks). Each level before the device's
   mip tail is then rows of whole bricks, the n-th brick_bytes of a layer the device's n-th block of it (or the n-th
   share of a stack's range, resident_order), and the levels from the mip tail on the rest of the layer. tex_upload binds a brick or a mip tail where memory maps any of its bytes. An ASTC one's tile is the
   device's block where it is held as BC7 and its footprint is 4x4: 64x64 blocks of 16 bytes either way */
static int resident_image(Surf* t, const VkImageCreateInfo* ii) {
    const VkFmt* f = &t->f;
    const u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1;
    VkResidentShape s;
    char name[32], in[64];
    if (!tex_can_piece(t)) return 0;
    if (!vk_resident_shape(ii, &s)) {
        if (g_vk.resident &&
            (ii->imageType == VK_IMAGE_TYPE_3D ? g_vk.feat.sparseResidencyImage3D : g_vk.feat.sparseResidencyImage2D))
            LOG_LIMIT(4,
                      "vulkan: a %ux%ux%u texture of %u levels in %s with holes in its mapping is made whole: the "
                      "device has no sparse images of format %d that size\n",
                      t->w, t->h, t->layers * d3, t->levels, tex_format_name(t, name, sizeof name), (int)f->vk);
        return 0;
    }
    const VkExtent3D g = s.block;
    const u32 across = g.width % f->bw ? 0 : g.width / f->bw * f->bpp, rows = g.height % f->bh ? 0 : g.height / f->bh;
    const u64 brick_bytes = (u64)across / 64 * (rows / 8) * g.depth * 512;
    u32 l = 0, lbh, lbd, bricks = 0;
    for (; l < s.tail_first; l++) {
        const u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, ld = d3 >> l ? d3 >> l : 1;
        const u32 bx = (lw + g.width - 1) / g.width, gobs = level_layout(t, l, &lbh, &lbd);
        if (!across || across % 64 || g.depth != 1u << lbd || gobs != bx * (across / 64)) break;
        if (rows != 8u << lbh && !resident_stacks(t, rows, lbh, (lh + g.height - 1) / g.height)) break;
        bricks += bx * ((lh + g.height - 1) / g.height) * ((ld + g.depth - 1) / g.depth);
    }
    if (!s.tail_first || l < s.tail_first || (t->layers > 1 && t->layer_size % brick_bytes)) {
        /* the level that isn't whole bricks, else the last one before the mip tail */
        const u32 at = l < s.tail_first || !l ? l : l - 1, lw = t->w >> at ? t->w >> at : 1,
                  gobs = level_layout(t, at, &lbh, &lbd);
        /* the device's block in the layout's terms */
        if (!across || !rows)
            snprintf(in, sizeof in, "not whole %ux%u blocks", f->bw, f->bh);
        else if (across % 64 || rows % 8)
            snprintf(in, sizeof in, "%u bytes across and %u rows high in its layout", across, rows);
        else
            snprintf(in, sizeof in, "%u GOBs across and %u high in its layout", across / 64, rows / 8);
        LOG_LIMIT(4,
                  "vulkan: a %ux%ux%u texture of %u levels in %s with holes in its mapping is made whole: the device "
                  "binds blocks of %ux%ux%u texels, %s, its mip tail from level %u; the layout's level %u has blocks "
                  "%u GOBs high and %u deep, %u GOBs a row (%u in whole blocks), its layers %llu bytes apart\n",
                  t->w, t->h, t->layers * d3, t->levels, tex_format_name(t, name, sizeof name), g.width, g.height,
                  g.depth, in, s.tail_first, at, 1u << lbh, 1u << lbd, gobs,
                  (lw + g.width - 1) / g.width * (across / 64), (unsigned long long)t->layer_size);
        return 0;
    }
    if (!vk_image_create_resident(ii, &s, &t->img)) return 0;
    t->grain = g;
    t->brick_bytes = (u32)brick_bytes;
    t->bricks = bricks;
    t->tail_first = s.tail_first;
    t->tails = s.tails;
    t->bytes = 0;
    return 1;
}

/* a texture of w by h pixels, with the samples of the sample layout msaa (DS_MSAA_, canonical). holes: its memory has
   holes in its mapping */
static Surf* tex_create(u64 gva, u32 w, u32 h, u32 layers, u32 d3, u32 levels, VkImageViewType type, VkFmt f, u32 fkey,
                        u32 bl, u32 bh, u32 bd, u32 tile_w, u32 pitch, u64 layer_size, u32 msaa, int holes) {
    /* a multisampled one is loaded by drawing its samples from the guest's layout of them, through a view of their
       raw bytes (gpu/vk/msaa.c) */
    VkFormat raw = msaa ? vk_raw_format(f.bpp) : VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (msaa ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    if (msaa && (f.depth || f.conv || !raw ||
                 !vk_ms_supported(raw, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, msaa) ||
                 !vk_ms_supported(f.vk, usage, raw != f.vk ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, msaa))) {
        LOG_LIMIT(4, "vulkan: a %u-sample texture of format %d can't be loaded from memory (not drawn)\n",
                  ds_msaa_samples(msaa), (int)f.vk);
        return NULL;
    }
    Surf* t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->msaa = (u8)msaa;
    t->ms_raw = msaa != 0;
    t->gva = gva;
    t->w = t->sw = w;
    t->h = t->sh = h;
    t->layers = layers;
    t->depth3d = d3;
    t->levels = levels;
    t->type = type;
    t->f = f;
    t->fkey = fkey;
    t->bl = bl;
    t->bh = bh;
    t->bd = bd;
    t->tile_w = tile_w;
    t->pitch = pitch;
    t->layer_size = layer_size;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ii;
    VkViewFormats vf;
    tex_info(t, f.vk, &ii, &vf, &t->storage_capable);
    if (holes && resident_image(t, &ii)) {
        vk_surf_add(t);
        return t;
    }
    /* held as BC7: RGBA8 until the encoder has made its BC7 image between frames (vk_tex_later_frame), but one with
       holes, whose RGBA8 image would take memory for its holes as well */
    if (f.conv == VKC_ASTC && is_bc7(f.vk) && !holes && g_settings.astc_mode != 3) {
        t->bc7_later = 1;
        t->f.vk = later_rgba8(f.vk);
        tex_info(t, t->f.vk, &ii, &vf, &t->storage_capable);
    }
    /* room is made for it under the budget: the caller holds only surfaces it used this present, which stay */
    if (!vk_cache_image_create(&ii, &t->img, &t->mem, &t->bytes)) {
        free(t);
        return NULL;
    }
    vk_surf_add(t);
    return t;
}

/* a render target read through a smaller header or a feedback draw: a copy refreshed when the target is drawn to. One
   copy per size, in a list through next: one draw can hold copies of a target at two sizes (two headers, or two
   stages), so a copy isn't replaced while a draw may hold it. Copies not read for a second go; their views and image
   when the frame being recorded is done, as descriptors may still name them */
/* the target's top-left w x h copied out, in the target's format, or in as: another color format of the same texel
   size, the target's bytes read as that format (a bit copy, which vkCmdCopyImage makes between such formats) */
static Surf* rt_crop(Surf* rt, u32 w, u32 h, const VkFmt* as) {
    Surf* c = NULL;
    VkFmt f = as ? *as : rt->f;
    for (Surf** pp = &rt->crop; *pp;) {
        Surf* x = *pp;
        if (x->w == w && x->h == h && x->f.vk == f.vk)
            c = x;
        else if ((i32)(g_vk_present - (u32)x->last_used) > 60) {
            *pp = x->next;
            for (int i = 0; i < x->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, x->sv[i].v, VK_NULL_HANDLE);
            vk_defer_image(x->img, x->mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
            vk_defer_bytes(x->bytes);
            free(x);
            continue;
        }
        pp = &x->next;
    }
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) return rt;
        c->gva = rt->gva;
        c->w = w;
        c->h = h;
        c->sw = vk_sc(w) < rt->sw ? vk_sc(w) : rt->sw; /* the same region of the scaled target */
        c->sh = vk_sc(h) < rt->sh ? vk_sc(h) : rt->sh;
        c->layers = rt->layers;
        c->levels = c->depth3d = 1;
        c->f = f;
        c->fkey = rt->fkey;
        c->type = rt->type;
        c->is_rt = 1;       /* sampled like one; not in the table, so never found as one */
        c->msaa = rt->msaa; /* a multisampled target's copy has its samples, which a copy takes as they are */
        c->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (c->msaa &&
            !vk_ms_supported(f.vk, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, c->msaa)) {
            free(c);
            return rt;
        }
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = f.vk,
                                .extent = {c->sw, c->sh, 1},
                                .mipLevels = 1,
                                .arrayLayers = rt->layers,
                                .samples = vk_surf_samples(c),
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkViewFormats vf;
        vk_view_formats(&ii, &vf, VK_FORMAT_UNDEFINED);
        if (!vk_cache_image_create(&ii, &c->img, &c->mem, &c->bytes)) { /* rt was used now: it stays */
            free(c);
            return rt;
        }
        c->crop_gen = ~0u;
        c->next = rt->crop;
        rt->crop = c;
        if (w != rt->w || h != rt->h)
            LOG_LIMIT(10, "vulkan: %ux%u texture reads the %ux%u render target at %llx: only that region is used\n", w,
                      h, rt->w, rt->h, (unsigned long long)rt->gva);
    }
    c->last_used = g_vk_present;
    if (c->crop_gen != rt->wgen) {
        VkCommandBuffer cb = vk_cmd();
        VkImageCopy r[2];
        vk_surf_use(cb, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        vk_surf_use(cb, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        /* VkImageCopy takes one aspect per region: depth and stencil are two (a view samples one of them) */
        VkImageAspectFlags a = rt->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        memset(r, 0, sizeof r);
        r[0].srcSubresource.aspectMask = a;
        r[0].srcSubresource.layerCount = rt->layers;
        r[0].dstSubresource.aspectMask = a;
        r[0].dstSubresource.layerCount = rt->layers;
        r[0].extent.width = c->sw;
        r[0].extent.height = c->sh;
        r[0].extent.depth = 1;
        r[1] = r[0];
        r[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        r[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        rec_CmdCopyImage(cb, rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rt->f.stencil ? 2 : 1, r);
        c->crop_gen = rt->wgen;
    }
    return c;
}

Surf* vk_rt_snapshot(Surf* rt) {
    if (!rt || rt->layout == VK_IMAGE_LAYOUT_UNDEFINED) return rt;
    return rt_crop(rt, rt->w, rt->h, NULL);
}

/* texture header type -> what the image is created as */
static VkImageViewType tic_view_type(u32 type, u32 d) {
    switch (type) {
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return VK_IMAGE_VIEW_TYPE_CUBE;
        case 8: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case 4:
        case 5: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case 1:
        case 7: return d > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
    return VK_IMAGE_VIEW_TYPE_2D; /* 1D textures are 2D images one texel high */
}

/* ---- textures loaded a piece at a time

   A virtual pool's texture is mapped a 64 KiB tile at a time, a few of its thousands of tiles (one game's light volumes
   are 477 MB with 384 KiB mapped). Loaded whole, such a texture had all its bytes hashed and staged, holes and all, when
   it was made and again whenever a tile was mapped. Instead its image is cleared, then the blocks its mapped runs lie
   in are loaded, and later only the blocks of the pages the GPU's records (a mapping, an engine's write) and write
   tracking (the CPU's) name. A texture loaded whole takes its changes the same way while write tracking answers for
   its memory: a write of a page of an 8192x8192 texture loads the blocks of that page, not its 256 MiB. A run of
   blocks of one row of blocks is the layout of a surface that row high and that run wide, which the GPU deswizzles
   into its place in the image */
#define TEX_PIECES 256
/* bytes of a texture to load, in order, those that meet joined. Past TEX_PIECES of them the two closest are joined
   over the bytes between them, which loads more than changed and never less */
typedef struct {
    struct {
        u64 at, n;
    } piece[TEX_PIECES + 1]; /* the one added before two are joined */
    u32 count;
    int all; /* the whole texture */
} TexPieces;
static void pieces_add(TexPieces* p, u64 at, u64 n) {
    u32 i = p->count, j;
    if (!n || p->all) return;
    while (i && p->piece[i - 1].at > at) i--;
    if (i && p->piece[i - 1].at + p->piece[i - 1].n >= at) { /* the one before reaches it */
        i--;
        n = at + n - p->piece[i].at;
        at = p->piece[i].at;
        if (p->piece[i].n > n) n = p->piece[i].n;
    } else {
        memmove(p->piece + i + 1, p->piece + i, (p->count - i) * sizeof *p->piece);
        p->count++;
    }
    for (j = i + 1; j < p->count && p->piece[j].at <= at + n; j++) /* the ones after it reaches */
        if (p->piece[j].at + p->piece[j].n > at + n) n = p->piece[j].at + p->piece[j].n - at;
    p->piece[i].at = at, p->piece[i].n = n;
    memmove(p->piece + i + 1, p->piece + j, (p->count - j) * sizeof *p->piece);
    p->count -= j - i - 1;
    if (p->count > TEX_PIECES) { /* the two with the fewest bytes between them */
        u32 k = 0;
        for (j = 1; j + 1 < p->count; j++)
            if (p->piece[j + 1].at - p->piece[j].at - p->piece[j].n <
                p->piece[k + 1].at - p->piece[k].at - p->piece[k].n)
                k = j;
        p->piece[k].n = p->piece[k + 1].at + p->piece[k + 1].n - p->piece[k].at;
        memmove(p->piece + k + 1, p->piece + k + 2, (p->count - k - 2) * sizeof *p->piece);
        p->count--;
    }
}

/* page k of a texture with memory only where it is mapped (resident_page), all of it, in each layer it has bytes in */
static void resident_page_add(const Surf* t, u32 k, TexPieces* p) {
    if (k < t->layers * t->bricks) {
        pieces_add(p, (u64)(k / t->bricks) * t->layer_size + (u64)(k % t->bricks) * t->brick_bytes, t->brick_bytes);
        return;
    }
    for (u32 layer = 0; layer < t->layers; layer++)
        if (t->tails == 1 || k - t->layers * t->bricks == layer)
            pieces_add(p, layer * t->layer_size + resident_bricks_end(t),
                       resident_levels_end(t) - resident_bricks_end(t));
}

/* a texture's memory where it isn't one host run, over the runs the GPU's page table maps of it (gmmu_pieces, which
   clips them to it): first the bytes they cover, then what vk_written_since says of them, -1 once a run can't tell.
   With p, the runs (counting) or their pages written since (asking) go in it */
typedef struct {
    u64 mapped, gva, off; /* off: the texture's byte where the run being asked starts */
    u32 since;
    int ask, written;
    TexPieces* p;
} TexRuns;
static void tex_written_piece(void* ctx, u64 at, u64 n) {
    TexRuns* r = ctx;
    pieces_add(r->p, r->off + at, n);
}
static void tex_run(void* ctx, u64 gva, u64 cpu, u64 n) {
    TexRuns* r = ctx;
    if (!r->ask) {
        r->mapped += n;
        if (r->p) pieces_add(r->p, gva - r->gva, n);
        return;
    }
    if (r->written < 0) return;
    int w = vk_written_since(GP(cpu), n, r->since);
    r->written = w < 0 ? -1 : r->written | w;
    r->off = gva - r->gva;
    if (w > 0 && r->p) vk_written_runs(GP(cpu), n, r->since, tex_written_piece, r);
}
/* vk_written_since for the texture's memory: its one host run, or each run the GPU's page table maps of it, with holes
   in its mapping (a virtual pool's, mapped a 64 KiB tile at a time, whose tiles the CPU writes in place) or without
   (several pieces of guest memory): the runs' answers are as good as one run's, and save hashing it. -1 where write
   tracking is off. With p, the pages written go in it */
static int tex_written_since(const Surf* t, u64 gva, TexPieces* p) {
    u8* host = gmem_host(gva, t->size);
    TexRuns r = {0, gva, 0, t->wseen, 1, 0, p};
    if (host) {
        int w = vk_written_since(host, t->size, t->wseen);
        if (w > 0 && p) vk_written_runs(host, t->size, t->wseen, tex_written_piece, &r);
        return w;
    }
    gmmu_pieces(gva, t->size, tex_run, &r);
    return r.written;
}
static void tex_dirty_piece(void* ctx, u64 at, u64 n) {
    TexRuns* r = ctx;
    r->mapped += n;
    pieces_add(r->p, at - r->gva, n);
}
/* loaded a piece at a time: one sample, in a format the GPU takes as it is, block linear with texels or blocks whole
   in a GOB's 64-byte rows, or pitch linear with texels or blocks of 1 to 16 bytes, a power of two (a row staged
   anywhere in staging is where a copy can read it). Or ASTC, decoded (and encoded as BC7) a run of blocks at a time
   (level_patch_astc): block linear and 2D, as a volume's ASTC levels are decoded whole */
static int tex_can_piece(const Surf* t) {
    const VkFmt* f = &t->f;
    if (f->conv == VKC_ASTC) return t->bl && !t->msaa && t->type != VK_IMAGE_VIEW_TYPE_3D;
    return !t->msaa && f->conv == VKC_NONE && !vk_tc_converts(f) && !f->depth && f->bpp &&
           (t->bl ? 64 % f->bpp == 0 : 16 % f->bpp == 0);
}

/* the blocks of one level of one layer (lsz bytes at gva) that its bytes [a, b) lie in, loaded a run of blocks of one
   row of blocks at a time */
static int level_patch_bl(Upload* u, const UpLevel* lv, u64 gva, u64 lsz, u32 lbh, u32 lbd, u64 a, u64 b) {
    const VkFmt* f = &u->t->f;
    u32 gobs_x = lv->gobs_x, align = bl_lin_align(f->bpp); /* a sparse texture's rows of whole tiles: padded */
    u64 block = 512ull << (lbh + lbd), rows_blocks = ((((u64)lv->th + 7) >> 3) + (1u << lbh) - 1) >> lbh;
    u64 k = a / block, end = (b + block - 1) / block;
    if (end > lsz / block) end = lsz / block;
    while (k < end) {
        u64 row = k / gobs_x, e = (row + 1) * gobs_x < end ? (row + 1) * gobs_x : end;
        u32 x0 = (u32)(k % gobs_x), y = (u32)(row % rows_blocks) << (3 + lbh), z = (u32)(row / rows_blocks) << lbd;
        if (y < lv->th && z < lv->ld && x0 * 64 < lv->rowb) { /* else the layout's padding past the level's texels */
            u32 rowb = lv->rowb - x0 * 64 < (e - k) * 64 ? lv->rowb - x0 * 64 : (u32)(e - k) * 64;
            VkBlMove m = {rowb,
                          8u << lbh,
                          lbh,
                          lbd,
                          0,
                          lv->th - y < 8u << lbh ? lv->th - y : 8u << lbh,
                          0,
                          lv->ld - z < 1u << lbd ? lv->ld - z : 1u << lbd,
                          (rowb + align - 1) / align * align};
            UpLevel run = *lv;
            VkBuffer staged;
            u64 at;
            run.x = x0 * (64 / f->bpp) * f->bw;
            run.cols = rowb / f->bpp * f->bw;
            if (!stage_bl(&m, gva + k * block, lsz - k * block, &staged, &at) ||
                !up_move(u, &run, &m, staged, at, z, y))
                return 0;
            g_tex_loads.bytes += (e - k) * block;
        }
        k = e;
    }
    return 1;
}
/* rows r0 to r1 of a layer of a pitch linear texture (of texels, or of blocks; its slices' rows after one another),
   bytes x0 to x1 of each */
typedef struct {
    u32 layer, x0, x1;
    u64 r0, r1;
} PitchRect;
/* bytes [a, b) of a layer of a pitch linear texture as its rows: of one row, the texels or blocks they cover; of rows
   they run across, all of each row. 0 when they are none of its texels (a row's pitch past them, or past its rows) */
static int pitch_rect(const Surf* t, u32 layer, u64 a, u64 b, PitchRect* r) {
    const VkFmt* f = &t->f;
    u32 gp = t->pitch, rowb = (t->w + f->bw - 1) / f->bw * f->bpp;
    u64 rows = (u64)((t->h + f->bh - 1) / f->bh) * (t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1);
    if (b > rows * gp) b = rows * gp;
    if (a >= b) return 0;
    r->layer = layer;
    r->r0 = a / gp;
    r->r1 = (b - 1) / gp + 1;
    r->x0 = 0;
    r->x1 = rowb;
    if (r->r1 - r->r0 == 1) {
        r->x0 = (u32)(a % gp) / f->bpp * f->bpp;
        r->x1 = ((u32)((b - 1) % gp) / f->bpp + 1) * f->bpp;
        if (r->x0 >= rowb) return 0;
        if (r->x1 > rowb) r->x1 = rowb;
    }
    return 1;
}
/* r loaded again: its rows staged as they are when they are whole (pitch and all), else their bytes a row at a time,
   in strips of a slice that staging takes */
static int pitch_patch(Upload* u, const PitchRect* r) {
    const Surf* t = u->t;
    const VkFmt* f = &t->f;
    u32 gp = t->pitch, th = (t->h + f->bh - 1) / f->bh, rowb = (t->w + f->bw - 1) / f->bw * f->bpp, w = r->x1 - r->x0;
    u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1;
    int as_is = r->x0 == 0 && w == rowb && gp % f->bpp == 0;
    u32 in_pitch = as_is ? gp : w;
    u64 band = g_vk.alloc_cap < TEX_BAND ? g_vk.alloc_cap : TEX_BAND, lsz = (u64)gp * th * d3;
    u64 gva = t->gva + (u64)r->layer * t->layer_size;
    UpLevel lv = {0, r->layer, t->w, t->h, d3, rowb, th, 0};
    lv.x = r->x0 / f->bpp * f->bw;
    lv.cols = w / f->bpp * f->bw;
    for (u64 row = r->r0; row < r->r1;) {
        u32 z = (u32)(row / th), y = (u32)(row % th);
        u64 n = r->r1 - row < th - y ? r->r1 - row : th - y, first = row * gp + r->x0;
        if (n * in_pitch > band) n = band / in_pitch ? band / in_pitch : 1;
        VkBuffer staged;
        u64 at;
        u8* p = vk_stage_guest_input(gva + first, (n - 1) * gp + (as_is ? gp : w), n * in_pitch, &staged, &at);
        if (!p) return 0;
        if (as_is) { /* the rows are one range of guest memory */
            u64 size = n * in_pitch, have = first + size <= lsz ? size : lsz - first;
            vk_guest_read(gva + first, have, p, staged, at);
            memset(p + have, 0, size - have);
        } else
            for (u64 k = 0; k < n; k++) vk_guest_read(gva + first + k * gp, w, p + k * w, staged, at + k * w);
        if (!up_reserve(u, 0) || !up_rows(u, &lv, staged, at, in_pitch, z, 1, y, (u32)n)) return 0;
        g_tex_loads.bytes += n * w;
        row += n;
    }
    return 1;
}
/* a pitch linear texture's pieces p loaded again, as the rows they lie in (pitch_rect): rows after one another, or
   the same rows, that cover the same texels go as one, so a region of a big texture's rows written a page a row is
   one copy, and rows two pieces share (pages that cut rows) load once */
static int tex_patch_pitch(Upload* u, const TexPieces* p) {
    const Surf* t = u->t;
    PitchRect run = {0}, next;
    int have = 0;
    for (u32 i = 0; i < p->count; i++)
        for (u64 a = p->piece[i].at, b = a + p->piece[i].n; a < b;) {
            u32 layer = (u32)(a / t->layer_size);
            u64 lo = (u64)layer * t->layer_size, hi = lo + t->layer_size;
            if (layer >= t->layers) break;
            if (pitch_rect(t, layer, a - lo, (b < hi ? b : hi) - lo, &next)) {
                if (have && run.layer == next.layer && run.x0 == next.x0 && run.x1 == next.x1 && next.r0 <= run.r1) {
                    if (next.r1 > run.r1) run.r1 = next.r1;
                } else {
                    if (have && !pitch_patch(u, &run)) return 0;
                    run = next;
                    have = 1;
                }
            }
            a = hi;
        }
    return !have || pitch_patch(u, &run);
}
/* the ASTC blocks of one level of one layer (lsz bytes at gva) that its bytes [a, b) lie in, decoded again a run of
   blocks of one row of blocks at a time, as level_patch_bl loads them: the layout of a surface that row high and that
   run wide, which astc_rows deswizzles. A run starts on a GOB, 4 ASTC blocks across, and is 8 << lbh block rows high
   (or ends at the level's edge), so it holds whole the 4x4 blocks a texture held as BC7 (astc_mode=0) encodes from its
   texels, whatever the footprint: 4 and 8 blocks are whole multiples of lcm(4, bw) / bw and lcm(4, bh) / bh, which
   are 1, 2 or 4. Each BC7 block then takes the texels it takes in the whole level's upload, its neighbours' too */
static int level_patch_astc(Upload* u, const UpLevel* lv, u64 gva, u64 lsz, u32 lbh, u32 lbd, u64 a, u64 b) {
    Surf* t = u->t;
    const VkFmt* f = &t->f;
    u32 gobs_x = lv->gobs_x;
    u64 block = 512ull << (lbh + lbd), rows_blocks = ((((u64)lv->th + 7) >> 3) + (1u << lbh) - 1) >> lbh;
    u64 k = a / block, end = (b + block - 1) / block;
    if (end > lsz / block) end = lsz / block;
    while (k < end) {
        u64 row = k / gobs_x, e = (row + 1) * gobs_x < end ? (row + 1) * gobs_x : end;
        u32 x0 = (u32)(k % gobs_x), y = (u32)(row % rows_blocks) << (3 + lbh), z = (u32)(row / rows_blocks) << lbd;
        if (y < lv->th && z < lv->ld && x0 * 64 < lv->rowb) { /* else the layout's padding past the level's blocks */
            u32 rowb = lv->rowb - x0 * 64 < (e - k) * 64 ? lv->rowb - x0 * 64 : (u32)(e - k) * 64;
            u32 tx = x0 * 4 * f->bw, ty = y * f->bh, rows = lv->th - y < 8u << lbh ? lv->th - y : 8u << lbh;
            u32 w = rowb / 16 * f->bw, h = rows * f->bh;
            LevelSource ls = {t, gva + k * block, lsz - k * block, rowb, 8u << lbh, lbh, lbd, 0};
            if (w > lv->lw - tx) w = lv->lw - tx;
            if (h > lv->lh - ty) h = lv->lh - ty;
            if (!up_flush(u) || !vk_astc_upload(u->cb, t, tx, ty, w, h, 1, lv->level, lv->layer, astc_rows, &ls, NULL))
                return 0;
            u->scratch = VK_NULL_HANDLE; /* astc_rows may have grown the scratch buffer: asked for again */
            u->cap = 0;
            g_tex_loads.bytes += (e - k) * block;
        }
        k = e;
    }
    return 1;
}
/* bytes [a, b) of the texture loaded again: the blocks of each level of each layer they lie in */
static int tex_patch_range(Upload* u, u64 a, u64 b) {
    Surf* t = u->t;
    const VkFmt* f = &t->f;
    u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1;
    for (u32 layer = 0; layer < t->layers; layer++) {
        u64 at = layer * t->layer_size;
        for (u32 l = 0; l < t->levels && at < b; l++) {
            u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, ld = d3 >> l ? d3 >> l : 1, lbh, lbd;
            u64 lsz = bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, t->tile_w, l, &lbh, &lbd);
            if (a < at + lsz) {
                UpLevel lv = {.level = l,
                              .layer = layer,
                              .lw = lw,
                              .lh = lh,
                              .ld = ld,
                              .rowb = (lw + f->bw - 1) / f->bw * f->bpp,
                              .th = (lh + f->bh - 1) / f->bh};
                lv.gobs_x = bl_level_gobs_x(lv.rowb, lv.th, ld, t->bh, t->bd, t->tile_w);
                if (!(f->conv == VKC_ASTC ? level_patch_astc : level_patch_bl)(
                        u, &lv, t->gva + at, lsz, lbh, lbd, a > at ? a - at : 0, b - at < lsz ? b - at : lsz))
                    return 0;
            }
            at += lsz;
        }
    }
    return 1;
}
/* pieces of sum bytes of t, an ASTC texture whose BC7 image the encoder made: more BC7 blocks than it encodes in a
   frame (vk_tex_later_frame), which would hold the game as long, or of a texture written soon again (later_soon),
   which is held Exact from now on, load whole as RGBA8 */
static int later_pieces_whole(const Surf* t, u64 sum) {
    return t->bc7_later && is_bc7(t->f.vk) &&
           (t->bc7_hot || later_soon(t) || sum / 16 * t->f.bw * t->f.bh / 16 > VK_BC7_LATER_BLOCKS);
}
/* the pieces p of t loaded again into its image; all of it when they are more than half of it (a whole upload reads
   at most twice their bytes, in fewer moves), when render targets' pixels were copied into it (tex_take_drawn),
   which a whole upload takes again, or when they are too many BC7 blocks to encode at once (later_pieces_whole). A
   texture loaded whole whose hash is still its image's (no blocks loaded alone since) is hashed for that, as a change
   found by its hash is: its pages written with the bytes they had load nothing. 1 when the image has them */
static int tex_patch(Surf* t, const TexPieces* p) {
    u64 sum = 0, bytes = g_tex_loads.bytes;
    for (u32 i = 0; i < p->count; i++) sum += p->piece[i].n;
    if (p->all || t->took_drawn || sum > t->size / 2 || later_pieces_whole(t, sum)) {
        if (t->piecewise || !t->hash) return tex_upload(t);
        tex_rehash(t);
        return t->upload_valid;
    }
    if (!sum) return 1;
    vk_render_end(); /* copies, and a bind's submission, happen outside rendering passes */
    TexPieces bound;
    if (t->bricks) {
        /* memory for the bricks and mip tails mapped now, before their bytes go in, and only the pieces' parts in
           those: the others read as zeros (a tile unmapped). One given memory now is loaded whole: its texels are
           undefined, and its bytes the pieces miss are zeros or what memory maps there */
        const u32 pages = resident_pages(t);
        u8* want = calloc(pages, 1);
        int ok = want && resident_bind_mapped(t, want);
        bound.count = 0, bound.all = 0;
        for (u32 i = 0; i < p->count && ok; i++) {
            u64 end = p->piece[i].at + p->piece[i].n, e;
            for (u64 o = p->piece[i].at; o < end; o = e) {
                i64 k = resident_page(t, o, &e);
                if (k >= 0 && want[k] == 1) pieces_add(&bound, o, (e < end ? e : end) - o);
            }
        }
        for (u32 k = 0; k < pages && ok; k++)
            if (want[k] == 2) resident_page_add(t, k, &bound);
        free(want);
        if (!ok) {
            LOG_LIMIT(1,
                      "vulkan: no memory to bind a %ux%ux%u texture's mapped bricks: it loads again in the next "
                      "frame\n",
                      t->w, t->h, t->layers * t->depth3d);
            return t->upload_valid = 0;
        }
        if (bound.all) return tex_upload(t);
        p = &bound;
        sum = 0;
        for (u32 i = 0; i < p->count; i++) sum += p->piece[i].n;
        if (!sum) return 1;
    }
    SNX_ZONE_V(z, "texture pieces", sum);
    g_tex_loads.uploads++;
    later_wrote(t);
    t->wgen = vk_surf_gen(); /* an image that changed, as an upload's */
    t->hash = 0;             /* no longer the image's: a check by contents loads all of it */
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    Upload u = {.cb = cb, .t = t};
    int ok = 1;
    if (!t->bl) ok = tex_patch_pitch(&u, p);
    for (u32 i = 0; i < p->count && ok && t->bl; i++)
        ok = tex_patch_range(&u, p->piece[i].at, p->piece[i].at + p->piece[i].n);
    ok = up_flush(&u) && ok;
    t->bc7.keyed = 0;       /* its levels' bytes changed: no longer the cache's keys */
    if (ok) later_queue(t); /* an RGBA8 one the encoder starts on again */
    g_tex_loads.partial++;
    g_tex_loads.partial_bytes += g_tex_loads.bytes - bytes;
    if (!ok) {
        LOG_LIMIT(1,
                  "vulkan: out of memory or staging loading %llu KB of a %ux%ux%u texture: it loads again in the next "
                  "frame\n",
                  (unsigned long long)(sum >> 10), t->w, t->h, t->depth3d);
        t->upload_valid = 0;
    }
    SNX_ZONE_END(z);
    return ok;
}
/* zeros copied from staging into every level and layer of t's image (compressed images have no clear), at most this
   many of them at once */
#define TEX_ZEROS (4ull << 20)
static int tex_zero(Surf* t) {
    const VkFmt* f = &t->f;
    u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1, n = 0;
    u64 zeros = t->size < TEX_ZEROS ? t->size : TEX_ZEROS, off;
    VkBuffer buf;
    VkBufferImageCopy r[64];
    u8* p = vk_stage_upload(zeros, vk_stage_align(), &buf, &off);
    if (!p) return 0;
    memset(p, 0, zeros);
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (u32 layer = 0; layer < t->layers; layer++)
        for (u32 l = 0; l < t->levels; l++) {
            u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, ld = d3 >> l ? d3 >> l : 1;
            u32 th = (lh + f->bh - 1) / f->bh;
            u64 row = (u64)(lw + f->bw - 1) / f->bw * f->bpp, slice = row * th;
            u32 rows = slice <= zeros ? th : (u32)(zeros / row), slices = slice <= zeros ? (u32)(zeros / slice) : 1;
            for (u32 z = 0; z < ld; z += slices)
                for (u32 y = 0; y < th; y += rows) {
                    r[n++] = (VkBufferImageCopy){
                        .bufferOffset = off,
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                        .imageOffset = {0, (i32)(y * f->bh), (i32)z},
                        .imageExtent = {lw, lh - y * f->bh < rows * f->bh ? lh - y * f->bh : rows * f->bh,
                                        ld - z < slices ? ld - z : slices}};
                    if (n == sizeof r / sizeof *r) {
                        rec_CmdCopyBufferToImage(cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, r);
                        n = 0;
                    }
                }
        }
    if (n) rec_CmdCopyBufferToImage(cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, r);
    vk_all_barrier(cb); /* the blocks loaded next go over the zeros */
    return 1;
}
/* t, a texture with holes in its mapping: its image cleared, then the blocks of its mapped runs loaded; loaded whole
   when they are more than half of it. 1 when the image has them */
static int tex_load_pieces(Surf* t) {
    TexPieces p;
    TexRuns r = {0, t->gva, 0, 0, 0, 0, &p};
    u64 sum = 0;
    p.count = 0, p.all = 0;
    gmmu_pieces(t->gva, t->size, tex_run, &r);
    for (u32 i = 0; i < p.count; i++) sum += p.piece[i].n;
    if (p.all || sum > t->size / 2) return tex_upload(t);
    t->upload_valid = 0;
    t->wgen = vk_surf_gen(); /* even a new texture at a reused address invalidates an old target's copy */
    t->took_drawn = 0;
    vk_surf_relist(t);
    if (!t->bricks && !tex_zero(t)) return 0; /* a resident image's unbound bricks read as zeros already */
    t->upload_valid = 1;
    return tex_patch(t, &p);
}
/* p's bytes that are t's: a layer's levels from where the layer starts, each layer_size from the last. A texture of a
   base level on, as a shader binds one level of a mip chain, has the whole chain's layer as its stride, and the bytes
   between its layers are the chain's other levels. Writes there (each other level written through a view of its own,
   surface.c publish_own) aren't the texture's, and loaded all of it when they were more than half its span */
static void pieces_own(const Surf* t, TexPieces* p) {
    u64 between = (u64)(t->layers - 1) * t->layer_size;
    if (p->all || !p->count || t->layers < 2 || t->size <= between || t->size - between >= t->layer_size) return;
    u64 own = t->size - between;
    TexPieces q;
    q.count = 0, q.all = 0;
    for (u32 i = 0; i < p->count; i++) {
        u64 at = p->piece[i].at, end = at + p->piece[i].n;
        for (u64 k = at / t->layer_size; k < t->layers && k * t->layer_size < end; k++) {
            u64 lo = k * t->layer_size, hi = lo + own;
            if (lo < at) lo = at;
            if (hi > end) hi = end;
            if (lo < hi) pieces_add(&q, lo, hi - lo);
        }
    }
    *p = q;
}
/* p without the pieces that t's last publish as a storage image still owns, not written since (vk_guest_owned_by):
   memory there holds t's own texels. The GPU's records are of pages, and the views of a cube's last levels share
   theirs: each view's publish marked the others' texels written, and each loaded them again every frame */
static void pieces_published(const Surf* t, u64 gva, TexPieces* p) {
    u32 n = 0;
    if (p->all || !t->pub_serial) return;
    for (u32 i = 0; i < p->count; i++)
        if (!vk_guest_owned_by(gva + p->piece[i].at, p->piece[i].n, t->pub_serial)) p->piece[n++] = p->piece[i];
    p->count = n;
}
/* the checks of a texture loaded a piece at a time (tex_can_piece; with holes, or loaded whole): the pages the GPU's
   records and write tracking name since the last ones, loaded once both were asked, so the image holds what either
   found. 0 when write tracking can't tell: checked by contents (an engine's write after that check in the same epoch
   is loaded by its pages, and leaves the contents' check a hash that differs, so the next one loads it whole) */
static int tex_check_pieces(Surf* t, u64 gva) {
    TexPieces p;
    TexRuns r = {0, gva, 0, 0, 0, 0, &p};
    u64 gw = gpu_mem_generation(); /* before the records are read: what they miss after it, the next check finds */
    u32 ww_epoch = vk_ww_epoch();  /* before the check */
    p.count = 0, p.all = 0;        /* each bind checks: the 4 KiB of pieces are written before they are read */
    if (t->checked != g_vk_present || t->ww_epoch != ww_epoch) {
        int wr = tex_written_since(t, gva, &p);
        if (wr < 0) return 0;
        t->ww_epoch = ww_epoch;
        t->checked = g_vk_present;
        if (wr) t->wseen = vk_ww_now(); /* as tex_check's: the clock after the scans it took in */
    }
    if (gpu_mem_dirty_since(t->gw, gva, t->size)) {
        gpu_mem_dirty_runs(t->gw, gva, t->size, tex_dirty_piece, &r);
        if (!r.mapped) p.all = 1; /* found through the guest's addresses alone (SNX_ALIAS_CHECK) */
    }
    t->gw = gw;
    pieces_own(t, &p);
    pieces_published(t, gva, &p);
    if (p.count || p.all) {
        t->stable = 0;
        tex_patch(t, &p);
    }
    return 1;
}

static u32 g_bud_frame;
static i64 g_bud;
/* a texture that stayed the same over its last 40 frames' checks: the schedule below checks it by samples */
static int tex_settled(const Surf* t) { return t->stable >= 40 && t->size >= 16384; }
/* a whole-texture check's outcome (tex_rehash's) in its stability: a change starts it over, an unchanged texture
   counts once a frame (count: its first check in this one). Settling, it takes its samples' hash */
static void tex_settle(Surf* t, u64 gva, int changed, int count) {
    if (changed)
        t->stable = 0;
    else if (count && t->stable < 100000)
        t->stable++;
    if (count && t->stable >= 39 && t->size >= 16384) {
        t->shash = tex_sample_hash(gva, t->size);
        t->tfull = g_vk_present;
    }
}
/* t, a texture found for a header: still as guest memory has it (re-uploaded where it changed), or NULL when its
   upload failed. gva is its address */
static Surf* tex_check(Surf* t, u64 gva) {
    vk_surf_touch(t);
    later_cool(t);
    /* an upload that ran out of memory or staging (here, or a re-upload below) is tried again once a frame. A
       texture is only returned whole */
    if (!t->upload_valid) {
        if (t->checked != g_vk_present) {
            t->checked = g_vk_present;
            if (t->piecewise) {
                t->gw = gpu_mem_generation();
                tex_load_pieces(t);
            } else
                tex_rehash(t);
        }
        if (!t->upload_valid) return NULL;
    }
    int untracked = 0; /* tex_check_pieces asked write tracking, which can't tell: not asked again below */
    if (t->piecewise || tex_can_piece(t)) {
        if (tex_check_pieces(t, gva)) return t->upload_valid ? t : NULL;
        untracked = 1;
    }
    /* the GPU front end wrote its memory (DMA, I2M, 2D, compute results): checked now, whatever the schedule
       below says, as writes within a frame have to show in the same frame. Not written: it still matches as of
       now, so the next check asks only about later writes */
    int fresh = 0;
    if (gpu_mem_dirty_since(t->gw, gva, t->size)) {
        fresh = 1;
        if (tex_rehash(t)) t->stable = 0;
    } else
        t->gw = gpu_mem_generation();
    /* the CPU's writes, once a frame and again after each submission or stream wait that can make new ones the
       stream's to see (vk_ww_epoch): write tracking proves the memory unchanged without hashing its texels */
    u32 ww_epoch = vk_ww_epoch(); /* before the check */
    if (t->checked != g_vk_present || t->ww_epoch != ww_epoch) {
        int wr = untracked ? -1 : tex_written_since(t, gva, NULL);
        u32 wnow = vk_ww_now(); /* after the check: the pages it took in are marked with this clock at most */
        int epoch = t->ww_epoch != ww_epoch;
        t->ww_epoch = ww_epoch;
        if (wr >= 0) {
            t->checked = g_vk_present;
            /* Written: hashed again after the check took in the scans that found the writes, so the new contents hold
               what their marks (the clock as it is now, at most) stand for. A rehash for the front end's writes came
               before those scans, and is done again. Unchanged, the texture keeps its baseline: the clock before the
               check plus one is what the next scans taken in are marked with, and the texture took their writes as
               seen (one game's glyph cache missed the glyphs its CPU drew after a check that marked nothing) */
            if (wr) {
                t->wseen = wnow;
                tex_rehash(t);
            }
            return t->upload_valid ? t : NULL;
        }
        /* without it, hashed whole once in each epoch it is used in until it settles: what the CPU wrote before a
           submission or the end of a stream wait reaches the next sample, as with write tracking. The frame's schedule
           below alone misses that (the write and the sample come between two presents, and a headless run presents
           nothing). Expiring every texture at the cache invalidation methods did it before, and isn't enough either:
           edenprobe rewrites texels with only the sampler and header caches invalidated after, never the texture data
           cache. A settled texture is left to the schedule: epochs start at every submission, and hashing every
           texture in each took half the GPU thread (FreeBSD) */
        if (epoch && !tex_settled(t)) {
            int first = t->checked != g_vk_present;
            t->checked = g_vk_present;
            if (!fresh) tex_settle(t, gva, tex_rehash(t), first);
            return t->upload_valid ? t : NULL;
        }
    }
    /* without it: often at first, backing off while the texture stays the same, within a per-frame budget */
    if (t->checked != g_vk_present && (i32)(g_vk_present - t->next_check) >= 0) {
        if (g_bud_frame != g_vk_present) {
            g_bud_frame = g_vk_present;
            g_bud = 24 << 20;
        }
        if (tex_settled(t) && (i32)(g_vk_present - t->tfull) < 120) {
            t->checked = g_vk_present;
            if (tex_sample_hash(gva, t->size) == t->shash) {
                t->next_check = g_vk_present + 20 + ((u32)(gva >> 9) & 7);
                return t->upload_valid ? t : NULL;
            }
            t->stable = 0;
        }
        if (g_bud > 0 || t->stable < 4) {
            t->checked = g_vk_present;
            g_bud -= (i64)(t->size >> (t->size > 65536 ? 1 : 0));
            tex_settle(t, gva, tex_rehash(t), 1);
            t->next_check = g_vk_present + (t->stable < 4 ? 4 : t->stable < 12 ? 8 : 20) + ((u32)(gva >> 9) & 3);
        }
    }
    return t->upload_valid ? t : NULL;
}
/* the dimension of the texture a header describes, as ShaderInfo.tex_query_hint keys a TXQ's texture on it: 1 + its
   ShTex type (0 1D, 1 2D, 2 3D, 3 cube), + 4 for an array (a 2D header of several slices is one, as tic_view_type
   binds it) */
u8 vk_tic_query_hint(const u32* tic) {
    TicFields tf;
    tic_decode(tic, &tf);
    switch (tf.type) {
        case 0: return 1;         /* 1D */
        case 2: return 1 + 2;     /* 3D */
        case 3: return 1 + 3;     /* cube */
        case 4: return 1 + 4;     /* 1D array */
        case 5: return 1 + 1 + 4; /* 2D array */
        case 8: return 1 + 3 + 4; /* cube array */
        case 1:
        case 7: return tf.depth > 1 ? 1 + 1 + 4 : 1 + 1; /* 2D, and 2D of several slices */
    }
    return 1 + 1; /* a buffer: queried as 2D, as before */
}
static Surf* tex_from_tic(const u32* tic, int raw, int storage) {
    TicFields tf;
    tic_decode(tic, &tf);
    u64 gva = tf.address;
    u32 hv = tf.header;
    int bl = hv == 3 || hv == 4;
    u32 w = tf.width, h = tf.height, d = tf.depth;
    u32 type = tf.type;
    int srgb = (int)tf.srgb;
    u32 glevels = tf.levels;
    u32 bh = tf.block_h, bd = tf.block_d, tile_w = bl ? tf.tile_w : 0, pitch = bl ? 0 : tf.pitch;
    if (!gva) return NULL;
    if (hv == 0 || type == 6) {
        /* Buffer TICs describe a linear row of texels, bound as a 2D image the shader's 1D fetch reads at
           (x mod row, x / row): one row, or rows of 2^SH_TEXEL_ROW_SHIFT texels after another in memory for a longer
           buffer (UE4's vertex factories keep a mesh's texture coordinates and colors in such buffers, of tens of
           thousands of texels) */
        if (hv != 0 || type != 6) return NULL;
        w = tf.buffer_width;
        h = d = 1;
        if (w > (1u << SH_TEXEL_ROW_SHIFT)) {
            h = (w + (1u << SH_TEXEL_ROW_SHIFT) - 1) >> SH_TEXEL_ROW_SHIFT;
            w = 1u << SH_TEXEL_ROW_SHIFT;
        }
        glevels = 1;
        bl = 0;
        bh = bd = tile_w = 0;
    }
    /* a multisampled texture (2D, 2D array, 2D without levels) is its pixels' size; guest memory holds its samples, a
       block of them for each pixel (gpu/vk/msaa.c), one level */
    u32 msaa = type == 1 || type == 5 || type == 7 ? ds_msaa_canonical(tf.msaa) : DS_MSAA_1X1;
    if (msaa) glevels = 1;
    if (type == 0 || type == 4) h = 1;
    if (type == 3)
        d = 6;
    else if (type == 8)
        d *= 6;
    VkFmt f = vk_tic_format(tic[0], srgb);
    if (raw) {
        /* SULD.D.32 takes texel coordinates and reads a texel's 32 bits, whatever the header's components are (one
           game's post-processing reads its A2B10G10R10 targets and R32 masks this way): any color format of 4-byte
           texels stored as they are, viewed as R32_UINT */
        if (f.bpp != 4 || f.bw != 1 || f.conv || f.depth) return NULL;
        f.vk = VK_FORMAT_R32_UINT;
        f.is_int = 1;
        f.sint = 0;
    }
    if (type == 6 && f.bpp && w <= UINT32_MAX / f.bpp) pitch = w * f.bpp;
    if (!f.bpp || !w || w > 16384 || h > 16384 || d > 2048) return NULL;
    u32 gw = w << ds_msaa_log2_x(msaa), gh = h << ds_msaa_log2_y(msaa); /* the guest's texels: samples */
    if (!bl && pitch < (gw + f.bw - 1) / f.bw * f.bpp) {
        LOG_LIMIT(1, "vulkan: a pitch-linear texture's pitch (%u bytes) is shorter than its rows (not drawn)\n", pitch);
        return NULL;
    }
    VkImageViewType vt = tic_view_type(type, d);
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE && d != 6) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    u32 layers = vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : d, d3 = vt == VK_IMAGE_VIEW_TYPE_3D ? d : 1;
    if (vt == VK_IMAGE_VIEW_TYPE_3D && !raw) f = vk_tic_format_3d(tic[0], srgb);
    /* bytes from one layer to the next: all the levels the header has, whichever of them it shows */
    u64 stride =
        bl ? bl_layer_size(gw, gh, d3, &f, bh, bd, tile_w, glevels) : (u64)pitch * ((gh + f.bh - 1) / f.bh) * d3;
    /* A texture upload reads guest memory. Complete any compute storage writes that overlap it first. */
    if (g_cs_any) vk_compute_mem_need(gva, stride * layers);
    /* the levels the shader sees, base level to max level (word 7), as a texture of their own the way Ryujinx makes
       it: the base level's address, size and blocks. Pitch-linear textures have one level */
    u32 levels = bl ? glevels : 1;
    u32 base = tf.base_level, maxl = tf.max_level;
    if (bl && base < glevels && (base || maxl + 1 != glevels)) {
        for (u32 l = 0; l < base; l++) gva += bl_level_size(w, h, d3, &f, bh, bd, tile_w, l, NULL, NULL);
        /* the tile width pads the levels at least a tile in size: none from a base level smaller than that on. At
           one that big the block isn't shrunk, so the levels the view shows are padded as the texture's are */
        u32 base_w = ((w >> base ? w >> base : 1) + f.bw - 1) / f.bw,
            base_h = ((h >> base ? h >> base : 1) + f.bh - 1) / f.bh;
        if (!bl_level_tiled(base_w * f.bpp, base_h, d3 >> base ? d3 >> base : 1, bh, bd, tile_w)) tile_w = 0;
        bl_level_blocks(h, d3, &f, bh, bd, base, &bh, &bd);
        w = w >> base ? w >> base : 1;
        h = h >> base ? h >> base : 1;
        d3 = d3 >> base ? d3 >> base : 1;
        levels = maxl >= base ? maxl - base + 1 : 1;
        if (levels > glevels - base) levels = glevels - base;
    }
    /* no more levels than the size has (Vulkan requires it) */
    u32 maxdim = w > h ? w : h, maxlev = 1;
    if (d3 > maxdim) maxdim = d3;
    while (maxdim >> maxlev) maxlev++;
    if (levels > maxlev) levels = maxlev;
    /* a render target at that address: read it on the GPU, unless its contents are undefined (never drawn, or memory
       it can't load was written over it): then memory has them */
    Surf* rt = type == 6 ? NULL : vk_rt_find(gva);
    /* a texture of several levels isn't the target (its other levels are other targets' memory, a mip chain drawn level
       by level): loaded from memory */
    if (rt && rt->layout != VK_IMAGE_LAYOUT_UNDEFINED && f.bw == 1 && (!f.conv || f.depth) && type != 2 && rt->w >= w &&
        rt->h >= h && (d == 1 || type == 1 || type == 7) && levels == 1 && rt->msaa == msaa) {
        /* a header of another texel size covers other memory than the target's pixels (depth is read through its
           own aspect, whatever the host format's size) */
        if (f.bpp == rt->f.bpp || f.depth || rt->f.depth) {
            /* A smaller header is a corner of the target only where its pixels lie at the same bytes (dynamic
               resolution). One laid out otherwise is another texture in memory the target once had, which it doesn't
               read, and which neither keeps the target alive nor loads it again. One game's 256x1 ramp, at the address
               of a 2048x4096 depth target gone by, took that target's corner, and its sky stayed white until it was
               started again */
            if (!rt->msaa && (rt->w != w || rt->h != h) && !vk_rt_same_layout(rt, w, h, bl, bh, pitch)) {
                LOG_LIMIT(
                    30,
                    "vulkan: a %ux%u texture (%u bytes a texel, depth %d) at the address of the %ux%u render target "
                    "at %llx (%u bytes a pixel, depth %d) isn't laid out as it is: loaded from memory\n",
                    w, h, f.bpp, f.depth, rt->w, rt->h, (unsigned long long)gva, rt->f.bpp, rt->f.depth);
                goto texture_from_memory;
            }
            vk_rt_note_read(rt);
            if (rt->layout == VK_IMAGE_LAYOUT_UNDEFINED) goto texture_from_memory;
            /* Crops are sampling snapshots, without storage usage or a guest layout for publishing writes: a
               storage image is the target itself when its pixels lie where the target's do, a smaller header of the
               same rows too (dynamic resolution writes a frame's corner of it, which its samples' crops then take
               up), else a surface with the descriptor's own guest layout. A scaled target's pixels aren't where the
               program's coordinates write */
            if (storage) {
                if (!vk_rt_same_layout(rt, w, h, bl, bh, pitch) || rt->sw != rt->w || rt->sh != rt->h ||
                    rt->layers != layers || rt->depth3d != d3 || rt->f.vk != f.vk || !rt->storage_capable)
                    goto texture_from_memory;
                return rt;
            }
            /* the same bytes in another color format (an RGBA8 target read as R32_Uint): sampling the target itself
               would convert its texels, so a copy in the texture's format holds them as they are. sRGB and linear
               twins are views of one image and need no copy */
            if (!f.depth && !rt->f.depth && vk_srgb_as(f.vk, 0) != vk_srgb_as(rt->f.vk, 0))
                return rt_crop(rt, w, h, &f);
            return rt->w == w && rt->h == h ? rt : rt_crop(rt, w, h, NULL);
        }
        LOG_LIMIT(1,
                  "vulkan: a texture with %u-byte texels reads the render target at %llx, which has %u-byte pixels: it "
                  "is loaded from memory\n",
                  f.bpp, (unsigned long long)gva, rt->f.bpp);
    }
texture_from_memory:
    /* loaded from memory, with what render targets drew into its layers and levels copied in after (tex_take_drawn) */
    /* what the device can make */
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    int cube = vt != VK_IMAGE_VIEW_TYPE_3D && layers >= 6 && w == h; /* created cube compatible */
    if (vt == VK_IMAGE_VIEW_TYPE_3D
            ? w > lim->maxImageDimension3D || h > lim->maxImageDimension3D || d3 > lim->maxImageDimension3D
            : w > lim->maxImageDimension2D || h > lim->maxImageDimension2D || layers > lim->maxImageArrayLayers ||
                  (cube && w > lim->maxImageDimensionCube)) {
        LOG_LIMIT(1, "vulkan: a %ux%ux%u texture is bigger than the GPU's images can be (not drawn)\n", w, h, d);
        return NULL;
    }
    u32 fkey = (tic[0] & 0x7FFFF) | ((u32)srgb << 31) | ((u32)raw << 30);
    /* A texture sampled through its sRGB or its linear twin is the same image: its views take the header's sRGB-ness
       (vk_tex_view, from vk_view_formats' list), as a render target's twins do above. A twin made apart from it held
       the same bytes twice, and each saw the other's writes only through guest memory: one game's compute program
       stores a 2560x1440 RGBA8 frame through a linear header and its draw samples it through an sRGB one, and that
       twin loaded the 15 MB frame from the storage image's owner again every frame (a copy, a deswizzle and an
       upload). A sampled lookup takes the twin that holds memory's bytes now (nothing written since it was checked,
       as a storage image right after its publish, vk_tex_note_writeback), else its own, which the check below loads
       again as before. A storage or raw lookup keeps its own format, which its views need */
    Surf *same = NULL, *twin = NULL;
    for (Surf* t = vk_surf_first(gva); t; t = t->next) {
        if (t->is_rt || t->gva != gva || t->w != w || t->h != h || t->layers != layers || t->depth3d != d3 ||
            t->levels != levels || t->type != vt || t->bl != (u32)bl || t->bh != bh || t->bd != bd ||
            t->tile_w != tile_w || t->pitch != pitch || t->layer_size != stride || t->msaa != msaa)
            continue;
        if (t->fkey == fkey) {
            if (!same) same = t;
        } else if (!twin && !storage && !raw && t->fkey == (fkey ^ (1u << 31)) && t->f.vk != f.vk &&
                   vk_srgb_as(t->f.vk, srgb) == f.vk && t->f.conv == f.conv && !t->f.depth)
            twin = t;
    }
    if (twin &&
        (!same || (gpu_mem_dirty_since(same->gw, gva, same->size) && !gpu_mem_dirty_since(twin->gw, gva, twin->size))))
        same = twin;
    if (same) return tex_check(same, gva);
    if (!format_samples(f.vk)) return NULL;
    /* this texture's levels in one layer; the layers before the last one take their whole stride. Its levels from the
       base level on, of the base level's size and blocks: of the size of level 0 (gw, gh), a view of one level of a
       128x128 cube's mip chain took the bytes of a 128x128 level from that level's start in each layer, which reached
       over the next levels, and loaded each again when another view wrote one of them */
    u64 sz = 0;
    u32 vw = w << ds_msaa_log2_x(msaa), vh = h << ds_msaa_log2_y(msaa);
    for (u32 l = 0; l < levels; l++) sz += bl ? bl_level_size(vw, vh, d3, &f, bh, bd, tile_w, l, NULL, NULL) : stride;
    sz += stride * (layers - 1);
    TexRuns runs = {0};
    int holes = 0;
    if (!gmem_host(gva, sz)) {
        gmmu_pieces(gva, sz, tex_run, &runs);
        holes = runs.mapped < sz;
    }
    /* an ASTC texture with holes in its mapping held as BC7 whatever Auto chose where the device samples BCn: a game's
       sparse one is 68 layers of 8192x8192, 341 MB a mapped layer as RGBA8, which filled a 12 GB GPU, 85 as BC7 */
    if (holes && f.conv == VKC_ASTC && g_vk.feat.textureCompressionBC &&
        (f.vk == VK_FORMAT_R8G8B8A8_UNORM || f.vk == VK_FORMAT_R8G8B8A8_SRGB))
        f.vk = f.vk == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    Surf* t = tex_create(gva, w, h, layers, d3, levels, vt, f, fkey, bl, bh, bd, tile_w, pitch, stride, msaa, holes);
    if (!t) return NULL;
    t->size = sz;
    /* loaded whole, a pitch linear or an ASTC one takes its changes in pieces (an ASTC one's holes decode as memory's
       zero bytes do, as in its whole upload). One with memory only where it is mapped reads zeros in its holes, an
       ASTC one too: it is loaded a piece at a time */
    t->piecewise = holes && t->bl && (f.conv == VKC_NONE || t->bricks) && tex_can_piece(t);
    t->ww_epoch = vk_ww_epoch(); /* checked in this epoch: read before the hash, or the piecewise one's load */
    t->wseen = vk_ww_now();
    tex_written_since(t, gva, NULL); /* starts tracking it */
    t->wseen = vk_ww_now();
    t->gw = gpu_mem_generation();
    if (!t->piecewise) t->hash = mem_hash(gva, t->size); /* a piecewise one's stays 0: none of its bytes are read */
    t->checked = g_vk_present;
    t->next_check = g_vk_present + 4;
    return (t->piecewise ? tex_load_pieces(t) : tex_upload(t)) ? t : NULL;
}

VkFormat vk_tex_view_format(const Surf* s, int srgb) { return s->f.depth ? s->f.vk : vk_srgb_as(s->f.vk, srgb); }

const char* vk_view_type_name(VkImageViewType type) {
    static const char* const name[] = {"1D", "2D", "3D", "cube", "1D array", "2D array", "cube array"};
    return (unsigned)type < sizeof name / sizeof name[0] ? name[type] : "unknown";
}

/* what a view of each image type can be: 2D images (render targets too) take 2D views, 3D ones a 3D view, cubes need 6
   square layers of an image made cube compatible, which a render target isn't */
const char* vk_tex_view_refusal(const Surf* s, VkImageViewType type) {
    int is3d = s->type == VK_IMAGE_VIEW_TYPE_3D;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_2D:
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return is3d ? "a 3D image has no 2D view" : NULL;
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            if (is3d) return "a 3D image has no cube view";
            if (s->is_rt) return "a render target isn't made cube compatible";
            if (s->layers < 6) return "a cube needs 6 layers";
            if (s->w != s->h) return "a cube's faces are square";
            if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray)
                return "the device has no cube arrays";
            return NULL;
        case VK_IMAGE_VIEW_TYPE_3D: return is3d ? NULL : "a 2D image has no 3D view";
        default: return "textures take no view of that type";
    }
}

static VkImageView tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil,
                            u32 levels, int storage) {
    if (vk_tex_view_refusal(s, type)) return VK_NULL_HANDLE;
    u32 layers = type == VK_IMAGE_VIEW_TYPE_2D_ARRAY     ? s->layers
                 : type == VK_IMAGE_VIEW_TYPE_CUBE       ? 6
                 : type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? s->layers / 6 * 6
                                                         : 1;
    VkFormat fmt = vk_tex_view_format(s, srgb);
    stencil = stencil && s->f.stencil;
    u64 key = (u64)type | (u64)map->r << 4 | (u64)map->g << 8 | (u64)map->b << 12 | (u64)map->a << 16 | (u64)fmt << 20 |
              (u64)stencil << 60 | (u64)(levels != s->levels) << 61 | (u64)(storage != 0) << 62;
    for (int i = 0; i < s->nsv; i++)
        if (s->sv[i].key == key) return s->sv[i].v;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = type,
                                .format = fmt,
                                .components = *map,
                                .subresourceRange = {stencil      ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                     : s->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                  : VK_IMAGE_ASPECT_COLOR_BIT,
                                                     0, levels, 0, layers}};
    /* Explicit usage keeps a sampled sRGB view from inheriting the image's linear storage usage. */
    VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                     .usage = storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT};
    vi.pNext = &vu;
    VkImageView v;
    if (!VK_OK(vk_image_view_create(g_vk.dev, &vi, NULL, &v))) return VK_NULL_HANDLE;
    if (s->nsv == (int)(sizeof s->sv / sizeof s->sv[0])) { /* full: drop the oldest */
        vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, s->sv[0].v, VK_NULL_HANDLE);
        memmove(s->sv, s->sv + 1, sizeof s->sv - sizeof s->sv[0]);
        s->nsv--;
    }
    s->sv[s->nsv++] = (SurfView){key, v};
    return v;
}

VkImageView vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map, int stencil) {
    return tex_view(s, type, srgb, map, stencil, s->levels, 0);
}

VkImageView vk_tex_storage_view(Surf* s, VkImageViewType type) {
    const VkComponentMapping identity = {0};
    /* A storage descriptor exposes the first level selected by its TIC, not
       the entire sampled mip chain. Both views share the existing image. */
    return tex_view(s, type, 0, &identity, 0, 1, 1);
}

/* empty textures for slots whose texture is missing or doesn't match the shader: zero, or depth 1.0 for shadow
   samplers, which compare against depth formats only */
VkImageView vk_null_view(VkImageViewType type, int num, int ms) {
    static VkImageView views[2][8][4];
    static Surf* imgs[3][4]; /* [3D, or 2 multisampled][num] */
    if ((unsigned)type >= 8 || num < 0 || num > 3) return VK_NULL_HANDLE;
    if (num == 3 && type == VK_IMAGE_VIEW_TYPE_3D) num = 0; /* no 3D depth images (nor 3D shadow samplers) */
    ms = ms != 0;
    /* multisampled: 4 samples, which every format sampled as a color image has (sampledImageColorSampleCounts) */
    if (ms && (num == 3 || (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY)))
        return VK_NULL_HANDLE;
    if (views[ms][type][num]) return views[ms][type][num];
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) return VK_NULL_HANDLE;
    int is3d = type == VK_IMAGE_VIEW_TYPE_3D;
    Surf* s = imgs[ms ? 2 : is3d][num];
    if (!s) {
        static const VkFormat fm[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
                                       VK_FORMAT_D16_UNORM};
        s = calloc(1, sizeof *s);
        if (!s) return VK_NULL_HANDLE;
        s->w = s->h = s->sw = s->sh = s->levels = s->depth3d = 1;
        s->layers = is3d ? 1 : 6;
        s->msaa = ms ? DS_MSAA_2X2 : DS_MSAA_1X1;
        s->type = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        s->f = num == 3 ? (VkFmt){.vk = fm[3], .bpp = 2, .bw = 1, .bh = 1, .depth = 1}
                        : (VkFmt){fm[num], 4, 1, 1, 0, 0, (u8)(num != 0), 0, (u8)(num == 2)};
        s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = is3d || ms ? 0 : VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                .format = fm[num],
                                .extent = {1, 1, 1},
                                .mipLevels = 1,
                                .arrayLayers = s->layers,
                                .samples = vk_surf_samples(s),
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &s->img, &s->mem, NULL))) {
            free(s);
            return VK_NULL_HANDLE;
        }
        vk_mem_track(s->mem);
        vk_render_end(); /* clears happen outside rendering passes */
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange all = {vk_aspect(&s->f), 0, 1, 0, s->layers};
        if (num == 3) {
            VkClearDepthStencilValue farthest = {1.0f, 0}; /* not far: windef.h defines it away */
            rec_CmdClearDepthStencilImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &farthest, 1, &all);
        } else {
            VkClearColorValue z = {{0, 0, 0, 0}};
            rec_CmdClearColorImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &all);
        }
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imgs[ms ? 2 : is3d][num] = s;
    }
    VkComponentMapping id = {0};
    views[ms][type][num] = vk_tex_view(s, type, 0, &id, 0);
    return views[ms][type][num];
}

Surf* vk_null_storage(VkImageViewType type, int kind, VkImageView* view) {
    /* a 2D image for the 2D and 2D array views, a 3D one for the 3D view; by kind, the format of the program's image
       (shader.h ShaderInfo.img): 0 float, 1 an atomic's R32ui, 2 unsigned, 3 signed */
    static Surf* imgs[2][4];
    static const VkFormat fm[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
    *view = VK_NULL_HANDLE;
    if (kind < 0 || kind > 3 ||
        (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY && type != VK_IMAGE_VIEW_TYPE_3D))
        return NULL;
    int is3d = type == VK_IMAGE_VIEW_TYPE_3D;
    Surf* s = imgs[is3d][kind];
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) return NULL;
        s->w = s->h = s->sw = s->sh = s->levels = s->depth3d = s->layers = 1;
        s->msaa = DS_MSAA_1X1;
        s->type = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        s->f = (VkFmt){.vk = fm[kind], .bpp = 4, .bw = 1, .bh = 1, .is_int = kind != 0, .sint = kind == 3};
        s->storage_capable = 1;
        s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                .format = fm[kind],
                                .extent = {1, 1, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &s->img, &s->mem, NULL))) {
            free(s);
            return NULL;
        }
        vk_mem_track(s->mem);
        vk_render_end(); /* clears happen outside rendering passes */
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue z = {{0, 0, 0, 0}};
        rec_CmdClearColorImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &all);
        imgs[is3d][kind] = s;
    }
    *view = vk_tex_storage_view(s, type);
    return *view ? s : NULL;
}

/* ------------------------------------------------------------ samplers */
static VkSamplerAddressMode wrap(u32 m) {
    switch (m) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2:
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3:
        case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 5:
        case 7:
            /* an optional feature: without it mirrored repeat, the same over the first mirror */
            return g_vk.has_mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                         : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

/* tsc[0] bit 9 compare, bits 10-12 the compare function; tsc[1] filters (gpu/vk/render.c forces nearest for integer
   textures and the compare mode the shader declares) */
int g_vk_anisotropy;
VkSampler vk_sampler(const u32* tsc, int raise, int integer) {
    /* samplers are kept for good, and a device allows at least 4000 (maxSamplerAllocationCount): past 3072, a quarter
       of the table left free for probing, new sampler states get the first sampler made */
    enum { TAB = 4096, MAX = 3072 };
    static struct {
        u32 tsc[8];
        int raise, integer;
        VkSampler s;
    } tab[TAB];
    static u32 n, ncustom;
    static VkSampler first;
    u32 hk = 2166136261u;
    for (int i = 0; i < 8; i++) hk = (hk ^ tsc[i]) * 16777619u;
    hk = (hk ^ (u32)(raise << 1 | integer)) * 16777619u;
    u32 j = hk & (TAB - 1);
    for (u32 probe = 0; probe < TAB && tab[j].s; probe++, j = (j + 1) & (TAB - 1))
        if (!memcmp(tab[j].tsc, tsc, 32) && tab[j].raise == raise && tab[j].integer == integer) return tab[j].s;
    if (n >= MAX || tab[j].s) {
        LOG_LIMIT(1, "vulkan: more than %d sampler states: the others use the first one\n", MAX);
        return first;
    }
    TscFields sf;
    tsc_decode(tsc, &sf);
    u32 mag = sf.mag, min = sf.min, mip = sf.mip;
    float minl = (float)sf.min_lod / 256.f, maxl = (float)sf.max_lod / 256.f;
    float lb = (float)sf.lod_bias / 256.f, lbmax = g_vk.props.limits.maxSamplerLodBias;
    if (lb > lbmax) lb = lbmax;
    if (lb < -lbmax) lb = -lbmax;
    static const float aniso_lut[8] = {1, 2, 4, 6, 8, 10, 12, 16}; /* Ryujinx's SamplerDescriptor */
    u32 aniso = sf.anisotropy;
    float af = aniso_lut[aniso];
    /* anisotropy=N raises a mipmapped 2D texture's linear filtering (raise), a 3D scene's surfaces, to N. Nearest
       filtering (2D art, the HUD), samplers without a mip filter and depth compares (shadow maps) keep the game's */
    if (raise && (float)g_vk_anisotropy > af && min == 2 && mag == 2 && mip >= 2 && !sf.compare)
        af = (float)g_vk_anisotropy;
    /* tsc[1] bits 10-11: the least (1) or the greatest (2) of the texels the filter reads, not their weighted average
       (0), which vk_tex_bind_sampler leaves only where the view's format and the device take it */
    u32 reduction = sf.reduction;
    if (reduction == 3) {
        LOG_LIMIT(1, "vulkan: sampler reduction 3 names no mode: the texels are averaged\n");
        reduction = 0;
    }
    VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
                                           .reductionMode = reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN
                                                                           : VK_SAMPLER_REDUCTION_MODE_MAX};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = mag == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .minFilter = min == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = wrap(sf.wrap_u),
                              .addressModeV = wrap(sf.wrap_v),
                              .addressModeW = wrap(sf.wrap_p),
                              .mipLodBias = lb,
                              .anisotropyEnable = af > 1 && g_vk.feat.samplerAnisotropy,
                              .maxAnisotropy = af,
                              .compareEnable = sf.compare,
                              .compareOp = (VkCompareOp)sf.compare_op,
                              .minLod = minl,
                              .maxLod = mip <= 1 ? minl + 0.25f : maxl, /* no mip filter: the base level only */
                              .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK}; /* set below */
    if (si.maxAnisotropy > g_vk.props.limits.maxSamplerAnisotropy)
        si.maxAnisotropy = g_vk.props.limits.maxSamplerAnisotropy;
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    /* the border color: the TSC's four words as the texture's texels read, floats for a float view and integers for an
       integer one (nouveau writes the integer ones as their bits), as Vulkan's FLOAT or INT colors. A custom color
       takes one of the device's few custom-color samplers while there are any left, else the nearest built-in one */
    float bc[4];
    i32 bi[4];
    memcpy(bc, sf.border, 16);
    memcpy(bi, sf.border, 16);
    int black = integer ? !bi[0] && !bi[1] && !bi[2] : bc[0] == 0 && bc[1] == 0 && bc[2] == 0;
    int white = integer ? bi[0] == 1 && bi[1] == 1 && bi[2] == 1 : bc[0] == 1 && bc[1] == 1 && bc[2] == 1;
    int clear = integer ? !bi[3] : bc[3] == 0, opaque = integer ? bi[3] == 1 : bc[3] == 1;
    int custom = !((black && (clear || opaque)) || (white && opaque));
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType =
                                                      VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (custom && (!g_vk.has_border_color || ncustom >= g_vk.border_color_max)) {
        float a = integer ? (float)bi[3] : bc[3];
        float lum = integer ? ((float)bi[0] + (float)bi[1] + (float)bi[2]) / 3 : (bc[0] + bc[1] + bc[2]) / 3;
        if (g_vk.has_border_color)
            LOG_LIMIT(1,
                      "vulkan: more than %u samplers with a border color of their own: the others take the nearest "
                      "of black or white\n",
                      g_vk.border_color_max);
        custom = 0;
        clear = !(a >= 0.5f); /* NaN: transparent, and black */
        black = !(lum >= 0.5f);
    }
    if (custom) {
        if (integer)
            memcpy(cb.customBorderColor.int32, bi, 16);
        else
            memcpy(cb.customBorderColor.float32, bc, 16);
        si.borderColor = integer ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        si.pNext = &cb;
    } else if (clear)
        si.borderColor = integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (black)
        si.borderColor = integer ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else
        si.borderColor = integer ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (reduction) {
        rm.pNext = si.pNext;
        si.pNext = &rm;
    }
    VkSampler s;
    if (!VK_OK(vk_sampler_create(g_vk.dev, &si, NULL, &s))) return VK_NULL_HANDLE;
    memcpy(tab[j].tsc, tsc, 32);
    tab[j].raise = raise;
    tab[j].integer = integer;
    tab[j].s = s;
    ncustom += custom;
    if (!n++) first = s;
    return s;
}

void vk_tex_frame(void) {
    /* surfaces whose pixels can be had again go once unused for twenty seconds and in 120 presents (a game at 6 frames
       a second kept 600 presents' worth, 100 seconds, of the targets it made every frame); textures come back from
       memory if they are needed again. Those whose pixels would be lost (vk_surf_reclaim) go before five minutes
       only under the budget's pressure (vk_mem_trim), and a target holding a draw a texture would take stays until
       then. The recency lists hand out the oldest first, so this costs what goes. The lossy pass looks every 600
       presents: it passes over the targets it keeps, and asking whether one holds such a draw can hash its memory.
       Render targets later ones drew all over hold nothing: every 30 presents those unused for a second and eight
       presents go (a game at a dynamic resolution makes its scene's targets again each frame) */
    u32 before = vk_present_before(20000000000ull, 120);
    vk_surf_reclaim(VK_LRU_TEX, before, ~0ull, VK_RECLAIM_LOSSLESS);
    vk_surf_reclaim(VK_LRU_RT, before, ~0ull, VK_RECLAIM_LOSSLESS);
    if (g_vk_present % 30 == 0) vk_rt_reclaim_superseded(vk_present_before(1000000000ull, 8), ~0ull);
    if (g_vk_present % 600 == 0) {
        u32 lossy_before = vk_present_before(300000000000ull, 600);
        vk_surf_reclaim(VK_LRU_TEX, lossy_before, ~0ull, VK_RECLAIM_IDLE);
        vk_surf_reclaim(VK_LRU_RT, lossy_before, ~0ull, VK_RECLAIM_IDLE);
    }
}

/* s copied into t, both unchanged since: kept for t and one other texture, so two textures over one target don't
   copy it on each other's binds (a texture's upload generation is new even at an address reused) */
static int copied_into(const Surf* s, const Surf* t) {
    for (int i = 0; i < 2; i++)
        if (s->copied[i].t == t && s->copied[i].wgen == s->wgen && s->copied[i].tgen == t->wgen) return 1;
    return 0;
}
static void mark_copied(Surf* s, Surf* t) {
    t->took_drawn = 1;
    if (s->copied[0].t != t) s->copied[1] = s->copied[0];
    s->copied[0].t = t;
    s->copied[0].wgen = s->wgen;
    s->copied[0].tgen = t->wgen;
}
/* a scaled target (resolution= other than 1) reaches a texture by a blit, which f's tiling has to take both ways */
static int can_blit(VkFormat f) {
    VkFormatProperties p = {0};
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}
/* the command buffer with s ready to be read from and t written to, by copies or blits */
static VkCommandBuffer copy_begin(Surf* s, Surf* t) {
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return cb;
}
/* the first count layers of s into t's level l at layer or Z slice dst, w x h: copied, or blitted from s's scaled
   size, nearest (sampling applies the texture's filter later). A blit into a 3D image takes one slice from layer 0, so
   a scaled s goes into slices one at a time (count 1: layered targets take vk_unscaled_image's way there) */
static void copy_layers(VkCommandBuffer cb, Surf* s, Surf* t, u32 l, u32 dst, u32 count, u32 w, u32 h, int z) {
    /* a region takes one aspect: a depth target's depth, and its stencil in a second region when it has one */
    VkImageSubresourceLayers from = {t->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                     z ? 1 : count};
    VkImageSubresourceLayers to = {from.aspectMask, l, z ? 0 : dst, z ? 1 : count};
    u32 regions = t->f.depth && t->f.stencil ? 2 : 1;
    i32 dz = z ? (i32)dst : 0;
    if (s->sw == s->w && s->sh == s->h) { /* layers into layers, or into as many slices */
        VkImageCopy c[2];
        from.layerCount = count;
        c[0] = (VkImageCopy){
            .srcSubresource = from, .dstSubresource = to, .dstOffset = {0, 0, dz}, .extent = {w, h, z ? count : 1}};
        c[1] = c[0];
        c[1].srcSubresource.aspectMask = c[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        rec_CmdCopyImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         regions, c);
        return;
    }
    VkImageBlit b[2];
    b[0] = (VkImageBlit){.srcSubresource = from,
                         .srcOffsets = {{0, 0, 0}, {(i32)vk_sc(w), (i32)vk_sc(h), 1}},
                         .dstSubresource = to,
                         .dstOffsets = {{0, 0, dz}, {(i32)w, (i32)h, dz + 1}}};
    b[1] = b[0];
    b[1].srcSubresource.aspectMask = b[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    rec_CmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     regions, b, VK_FILTER_NEAREST);
}

/* a texture whose layers, levels or 3D slices were drawn as render targets of their own is copied from those targets
   on the GPU: they are never written back to its memory. Only a target matching one subresource (or, layered, the
   layers from one), whose backing memory has not been overwritten since its draw. Generated textures can be first
   sampled several frames later. Targets are never block compressed, but a compressed texture takes a target whose
   texels are as many bytes as its blocks, a texel for a block: a game compressing on the GPU draws the blocks so (one
   game's text labels, BC5 drawn as RGBA32UI) */
static void tex_take_drawn(Surf* t) {
    /* a converted format's texels differ from its host image's, but a depth texture takes its targets' host images
       as they are (the same format, below): D24S8's guest packing is converted only when it is loaded from memory */
    if (!t || t->is_rt || (t->f.conv && !t->f.depth)) return;
    /* nothing over it changed since its last search left nothing it could take, and it wasn't loaded again */
    u64 gen = vk_rt_take_gen();
    if (t->take_gen == gen && t->take_wgen == t->wgen) return;
    int settled = 1; /* every target found was taken, or can't be until a draw or a write moves the count */
    Surf* small[32];
    Surf** d = small;
    u64 stride = t->layer_size;
    u64 hi = t->gva + t->size;
    int n = vk_rt_drawn_in(t->gva, hi, d, 32);
    if (n > 32) {
        d = malloc((size_t)n * sizeof *d);
        if (d)
            n = vk_rt_drawn_in(t->gva, hi, d, n);
        else {
            d = small;
            n = 32;
            settled = 0; /* the targets past the first 32 weren't looked at */
        }
    }
    for (int i = 0; i < n; i++) {
        Surf* s = d[i];
        /* depth targets feed depth textures of the same host format, with their stencil when it has one (a depth
           pyramid of a D24S8 target: Pokemon Legends Z-A's lighting samples it with all its levels) */
        /* a multisampled target read as a single-sampled texture of its samples' size, the guest's layout of them
           (gpu/vk/msaa.c: a program resolving the samples itself): each layer's samples unpacked into that layout and
           copied into the texture's first level. A multisampled texture takes nothing of targets: memory has them */
        if (s->msaa && !t->msaa && s->gva == t->gva && t->type != VK_IMAGE_VIEW_TYPE_3D && !t->f.depth && !s->f.depth &&
            t->f.bw == 1 && s->f.bpp == t->f.bpp && t->w == s->w << ds_msaa_log2_x(s->msaa) &&
            t->h == s->h << ds_msaa_log2_y(s->msaa) && (s->layers == 1 || s->layer_size == stride) && vk_ms_moves(s) &&
            !copied_into(s, t) && vk_rt_drawn_fresh(s)) {
            u32 count = s->layers < t->layers ? s->layers : t->layers;
            vk_render_end();
            VkCommandBuffer cb = vk_cmd();
            for (u32 k = 0; k < count; k++) {
                VkImage grid = vk_ms_unpack(cb, s, k, 0, t->h);
                if (!grid) {
                    settled = 0;
                    break;
                }
                vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, k, 1},
                                 .extent = {t->w, t->h, 1}};
                rec_CmdCopyImage(cb, grid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            }
            mark_copied(s, t);
            continue;
        }
        if (s->f.depth != t->f.depth || s->f.stencil != t->f.stencil || s->f.bpp != t->f.bpp ||
            (t->f.depth && s->f.vk != t->f.vk) || copied_into(s, t) || s->msaa || t->msaa)
            continue;
        /* a blit converts formats where a copy keeps the bits: a scaled target only into a texture of its own format */
        int scaled = s->sw != s->w || s->sh != s->h;
        if (scaled && (s->f.vk != t->f.vk || !can_blit(t->f.vk))) continue;
        u64 off = s->gva - t->gva;
        if (t->type == VK_IMAGE_VIEW_TYPE_3D) {
            if (t->f.depth) continue;
            /* A layered color target can be sampled as a 3D texture at the same address. Its slices only exist in
               the host image: guest memory is not updated by draws. Each array layer goes to the matching Z slice */
            /* Its layers are the texture's slices when they are GOB slices (a volume) or lie inside the texture. A
               target over the texture's first slice with layers a whole texture apart (one game's colour grading
               table, slice 0 drawn through it and the others through targets of their own) holds the first slice
               alone: it is taken as that slice below */
            if (s->layers > 1 && off == 0 && s->w == t->w && s->h == t->h && s->layers >= t->depth3d &&
                (s->volume || s->layer_size * (t->depth3d - 1) < t->size) && s->f.vk == t->f.vk &&
                vk_rt_drawn_fresh(s)) {
                /* a blit can't take array layers into a 3D image's slices, only a copy can: scaled, the layers
                   are blitted down to an array at the guest's size first, and copied into the slices from there */
                VmaAllocation mm = VK_NULL_HANDLE;
                VkImage mid = scaled ? vk_unscaled_image(s, t->depth3d, &mm) : VK_NULL_HANDLE;
                if (scaled && !mid) {
                    settled = 0;
                    continue;
                }
                VkCommandBuffer cb = copy_begin(s, t);
                if (scaled) {
                    VkImageSubresourceLayers all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, t->depth3d};
                    VkImageBlit b = {.srcSubresource = all,
                                     .srcOffsets = {{0, 0, 0}, {(i32)vk_sc(t->w), (i32)vk_sc(t->h), 1}},
                                     .dstSubresource = all,
                                     .dstOffsets = {{0, 0, 0}, {(i32)t->w, (i32)t->h, 1}}};
                    vk_barrier(cb, mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    rec_CmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mid,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
                    vk_barrier(cb, mid, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    /* layer z into slice z, all in one region */
                    VkImageCopy c = {.srcSubresource = all,
                                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .extent = {t->w, t->h, t->depth3d}};
                    rec_CmdCopyImage(cb, mid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                    vk_defer_image(mid, mm, VK_NULL_HANDLE, VK_NULL_HANDLE); /* freed once the frame is done */
                } else
                    copy_layers(cb, s, t, 0, 0, t->depth3d, t->w, t->h, 1);
                mark_copied(s, t);
                continue;
            }
            /* A 3D block-linear texture can be built a Z slice at a time through 2D targets. The target may be
               wider than the texture: its rows include the 64-byte GOB padding of a narrow slice. */
            if (!t->bl || !s->bl || s->f.vk != t->f.vk) continue;
            u64 lo = 0;
            for (u32 l = 0; l < t->levels; l++) {
                u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
                u32 ld = t->depth3d >> l ? t->depth3d >> l : 1, lbh, lbd;
                u64 size = bl_level_size(t->w, t->h, t->depth3d, &t->f, t->bh, t->bd, t->tile_w, l, &lbh, &lbd);
                u32 rowb = lw * t->f.bpp, gobs_x = bl_level_gobs_x(rowb, lh, ld, t->bh, t->bd, t->tile_w);
                u32 blocks_y = ((lh + 7) / 8 + (1u << lbh) - 1) >> lbh;
                /* A block is one GOB wide, 2^lbh high and 2^lbd deep, its GOBs Y first and then Z, and the blocks of
                   a level go X, then Y, then Z. So slice z's first GOB is at block z >> lbd's depth plane, z & (2^lbd
                   - 1) GOB columns of 2^lbh into the first block: slices 512 << lbh bytes apart inside a depth plane,
                   a plane (slab) apart across them. Without block depth that is a slice after another */
                u64 slab = (u64)gobs_x * blocks_y * (512ull << (lbh + lbd)), step = 512ull << lbh;
                u32 sbh;
                u64 source_size = bl_level_size(s->w, s->h, 1, &s->f, s->bh, 0, 0, 0, &sbh, NULL);
                if (off >= lo && off - lo < size && (off - lo) % slab % step == 0 &&
                    (off - lo) % slab / step < (1u << lbd) && s->w >= lw && s->h == lh && sbh == lbh &&
                    (lbd || source_size == slab) && vk_rt_drawn_fresh(s)) {
                    u64 z = ((off - lo) / slab << lbd) + (off - lo) % slab / step;
                    if (z < ld) {
                        copy_layers(copy_begin(s, t), s, t, l, (u32)z, 1, lw, lh, 1);
                        mark_copied(s, t);
                        break;
                    }
                }
                lo += size;
            }
            continue;
        }
        u32 layer = stride ? (u32)(off / stride) : 0;
        u64 rem = off - (u64)layer * stride, lo = 0;
        for (u32 l = 0; l < t->levels && layer < t->layers; l++) {
            u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
            /* a compressed level in blocks, the target's texels. The copy's extent counts the target's texels, and
               the blocks over a level's edge are the level's */
            lw = (lw + t->f.bw - 1) / t->f.bw;
            lh = (lh + t->f.bh - 1) / t->f.bh;
            /* a block-linear level's rows take whole GOBs (64 bytes), so a target drawn as wide as them covers the
               level (a 120-texel depth pyramid level drawn 128 wide, a cube mip narrower than a GOB one GOB wide, a
               230-texel BC5 label's 58 blocks drawn 60 wide): only its left lw texels are the level's */
            int padded_row =
                t->bl && s->bl && (s->f.vk == t->f.vk || t->f.bw > 1) && s->w > lw &&
                ((u64)s->w * s->f.bpp + 63) / 64 == bl_level_gobs_x(lw * t->f.bpp, lh, 1, t->bh, t->bd, t->tile_w);
            if (rem == lo && (s->w == lw || padded_row) && s->h == lh) {
                if (!vk_rt_drawn_fresh(s)) break; /* newer guest memory at this address takes precedence */
                /* a layered target spaced as the texture's layers fills as many of them as it has, at any level: one
                   game draws each level of its reflection cube through a target of six layers a whole face apart,
                   whose layer k is face k of that level (taken at level 0 alone, faces 1 to 5 of the blurred levels
                   kept guest memory's bytes, and its shiny surfaces reflected noise) */
                u32 count = 1;
                if (s->layers > 1 && !s->volume && s->layer_size == stride) /* a volume's are GOB slices */
                    count = s->layers < t->layers - layer ? s->layers : t->layers - layer;
                copy_layers(copy_begin(s, t), s, t, l, layer, count, lw, lh, 0);
                mark_copied(s, t);
                break;
            }
            if (!t->bl) break;
            lo += bl_level_size(t->w, t->h, 1, &t->f, t->bh, t->bd, t->tile_w, l, NULL, NULL);
        }
    }
    if (d != small) free(d);
    if (settled && vk_rt_take_gen() == gen) {
        t->take_gen = gen;
        t->take_wgen = t->wgen;
    }
}
Surf* vk_tex_from_tic(const u32* tic) {
    Surf* t = tex_from_tic(tic, 0, 0);
    tex_take_drawn(t);
    return t;
}
Surf* vk_tex_from_tic_storage(const u32* tic) {
    Surf* t = tex_from_tic(tic, 0, 1);
    tex_take_drawn(t);
    return t;
}
Surf* vk_tex_recheck(Surf* t) {
    if (g_cs_any) vk_compute_mem_need(t->gva, t->layer_size * t->layers);
    t = tex_check(t, t->gva);
    tex_take_drawn(t);
    return t;
}
Surf* vk_tex_from_tic_raw(const u32* tic) {
    Surf* t = tex_from_tic(tic, 1, 0);
    tex_take_drawn(t);
    return t;
}
