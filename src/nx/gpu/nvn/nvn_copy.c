/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's copies. NVN's copies between buffers and textures are the DMA engine's work on the console, and here
   they are the DMA engine's launches (gpu.h GpuDmaOp) that the renderer does on the GPU (vk/dma.c), as
   maxwell/dma.c's are: a texture is a block-linear (or pitch-linear) surface plane at its GPU address, a buffer a
   pitch-linear one. A copy of several slices or layers is a launch for each.

   The CPU's own texel writes (nvnTextureWriteTexels) swizzle into the texture's storage, which is the GPU's work
   (docs/BLOCKLINEAR.md): the source's bytes are read as the game calls, and the GPU writes them in order with the
   queues' work */
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/draw_state.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

/* A texture's level as a surface plane: the snapshot t, its view v (NULL for none), the level's block dimensions
   (texels a block: bw x bh, bpb bytes), its blocks a row and rows, and which slice a copy's z names */
typedef struct {
    u64 base;  /* the level's first layer */
    u64 layer; /* bytes between layers */
    int bl, volume;
    u32 pitch, wb, rows, depth, bh, bd, bpb;
    int bw, bhp; /* a block's texels */
} TexPlane;
static int tex_plane(const NvnTexture* t, const NvnView* v, TexPlane* p) {
    unsigned level = v && (v->set & NVN_VIEW_LEVELS) ? v->base_level : 0;
    *p = (TexPlane){0};
    if (t->magic != NVN_MAGIC('T', 'E', 'X', 0) || !t->gpu) return 0;
    int bpb = nvn_fmt_bpb((int)(v && (v->set & NVN_VIEW_FORMAT) ? v->format : t->d.format), &p->bw, &p->bhp);
    if (bpb <= 0 || p->bw <= 0 || p->bhp <= 0) return 0;
    NvnTexLayout l;
    nvn_tex_layout(&t->d, &l);
    u32 w = t->d.width >> level ? t->d.width >> level : 1;
    u32 h = t->d.height >> level ? t->d.height >> level : 1;
    p->bpb = (u32)bpb;
    p->wb = (w + (u32)p->bw - 1) / (u32)p->bw * (u32)bpb;
    p->rows = (h + (u32)p->bhp - 1) / (u32)p->bhp;
    p->layer = l.layer_size;
    p->base = t->gpu + nvn_tex_level_offset(&t->d, level);
    if (v && (v->set & NVN_VIEW_LAYERS)) p->base += (u64)v->min_layer * l.layer_size;
    p->volume = t->d.target == NVN_TEXTURE_TARGET_3D;
    p->depth = p->volume ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    if (t->d.flags & NVN_TEX_LINEAR) {
        p->pitch = t->d.stride ? t->d.stride : p->wb;
        return 1;
    }
    unsigned th, td;
    nvn_tex_level_tiles(&t->d, level, &th, &td);
    p->bl = 1;
    p->bh = th;
    p->bd = td;
    return 1;
}
/* the plane's side of a copy of the region at texels (x, y) of slice or layer z */
static void tex_side(const TexPlane* p, i32 x, i32 y, i32 z, GpuDmaSide* s) {
    *s = (GpuDmaSide){0};
    s->addr = p->base + (p->volume ? 0 : (u64)z * p->layer);
    s->bl = p->bl;
    s->pitch = p->pitch;
    s->wb = p->wb;
    s->h = p->rows;
    s->z = p->volume ? (u32)z : 0;
    s->bh = p->bh;
    s->bd = p->bd;
    s->ox = (u32)x / (u32)p->bw * p->bpb;
    s->oy = (u32)y / (u32)p->bhp;
}
/* the copy, on the GPU; a render target over the destination takes the new bytes */
static void launch(const GpuDmaOp* o) {
    if (!g_renderer_up || !g_renderer->dma_copy) {
        LOG_LIMIT(4, "nvn: a copy before the renderer started, skipped\n");
        return;
    }
    if (!g_renderer->dma_copy(o)) LOG_LIMIT(4, "nvn: the GPU could not do a copy, skipped\n");
    if (g_renderer->rt_reload) g_renderer->rt_reload(o->dst.addr);
}
/* lines of a region w texels wide and h high on the plane */
static void region_lines(const TexPlane* p, i32 w, i32 h, GpuDmaOp* o) {
    o->multi = 1;
    o->width = ((u32)w + (u32)p->bw - 1) / (u32)p->bw * p->bpb;
    o->lines = ((u32)h + (u32)p->bhp - 1) / (u32)p->bhp;
    o->line_in = o->line_out = o->width;
}

/* between a buffer at buf and a texture's region, either way (to_tex: the buffer is the source) */
static void copy_buffer_texture(const NvnPCopyB2T* r, const NvnTexture* t, int to_tex, const i64 stride[2],
                                const u8* snap) {
    i32 depth = r->r.depth > 0 ? r->r.depth : 1;
    if (r->r.width <= 0 || r->r.height <= 0) return;
    TexPlane p;
    if (!tex_plane(t, r->has_view ? &r->view : NULL, &p)) {
        LOG_LIMIT(4, "nvn: a copy of a texture of format %#x, which the runtime can't lay out\n", t->d.format);
        return;
    }
    GpuDmaOp o = {0};
    region_lines(&p, r->r.width, r->r.height, &o);
    u64 row = stride[0] ? (u64)stride[0] : o.width;
    u64 image = stride[1] ? (u64)stride[1] : row * o.lines;
    for (i32 k = 0; k < depth; k++) {
        GpuDmaSide* b = to_tex ? &o.src : &o.dst;
        tex_side(&p, r->r.xoffset, r->r.yoffset, r->r.zoffset + k, to_tex ? &o.dst : &o.src);
        *b = (GpuDmaSide){0};
        b->addr = r->buffer + image * (u64)k;
        b->pitch = (u32)row;
        o.snap = snap ? snap + image * (u64)k : NULL;
        launch(&o);
    }
}
/* A texture's slice z as a 2D engine surface (gpu.h Surf2D), the render target the 3D engine would bind there
   (gpu/nvn/nvn_draw.c): its format a target's, its bytes a pixel (0 for a depth format), a multisampled texture's
   size in samples, as the 2D engine sees one. 0 when it takes no target */
static int surf2d(u64 snapshot, const NvnView* v, int has_v, i32 z, Surf2D* o, u32* bpp) {
    DsSurf t;
    int depth;
    u32 msaa;
    *o = (Surf2D){0};
    if (!nvn_draw_surface(snapshot, has_v ? v : NULL, &t, &depth, &msaa) || (t.mem >> 16 & 1) || z < 0) return 0;
    *bpp = depth ? 0 : gpu_color_bpp(t.format);
    o->format = t.format;
    o->linear = t.mem >> 12 & 1;
    o->bh = t.mem >> 4 & 15;
    o->depth = 1;
    o->height = t.height;
    o->addr = t.gva + (u64)z * t.layer_stride;
    o->width = t.width;
    if (o->linear) { /* a pitch-linear target's width is its row pitch in bytes */
        o->pitch = t.width;
        o->width = *bpp ? t.width / *bpp : 0;
    }
    return o->width && o->height;
}
/* a surface as a plane of the GPU's blit from memory: its slice's rows, as fermi2d.c's f2d_plane lays a 2D surface */
static GpuDmaSide blit_plane(const Surf2D* s, u32 bpp) {
    GpuDmaSide p = {0};
    p.addr = s->addr;
    p.bl = !s->linear;
    p.pitch = s->pitch;
    p.wb = s->width * bpp;
    p.h = s->height;
    p.bh = s->bh;
    return p;
}
/* A copy between textures' regions of different sizes or formats: NVN's 2D engine blit, done as the 3D engine's
   channels' blits are (maxwell/fermi2d.c): the renderer's image blit when the source is a render target, filtered when
   it scales, else its blit from memory, nearest sampled, between formats of the same bytes a pixel */
static void blit_texture(const NvnPCopyT2T* r) {
    i32 depth = r->sr.depth > 0 ? r->sr.depth : 1;
    if (r->sr.width <= 0 || r->sr.height <= 0 || r->dr.width <= 0 || r->dr.height <= 0) return;
    double sdx = (double)r->sr.width / r->dr.width;
    double sdy = (double)r->sr.height / r->dr.height;
    LOG_LIMIT(1, "nvn: the first texture blit, %dx%d to %dx%d\n", r->sr.width, r->sr.height, r->dr.width, r->dr.height);
    for (i32 k = 0; k < depth; k++) {
        Surf2D s, d;
        u32 bs = 0, bd = 0;
        if (!surf2d(r->src, &r->sv, r->has_sv, r->sr.zoffset + k, &s, &bs) ||
            !surf2d(r->dst, &r->dv, r->has_dv, r->dr.zoffset + k, &d, &bd)) {
            LOG_LIMIT(4, "nvn: a texture blit of a texture no render target takes isn't replayed\n");
            return;
        }
        if (g_renderer_up && g_renderer->blit &&
            g_renderer->blit(&s, &d, r->dr.xoffset, r->dr.yoffset, r->dr.width, r->dr.height, r->sr.xoffset,
                             r->sr.yoffset, sdx, sdy))
            continue; /* the destination target holds the pixels: nothing to load from memory (fermi2d.c's too) */
        if (!bs || bs != bd || !g_renderer_up || !g_renderer->blit_mem) {
            LOG_LIMIT(4, "nvn: a texture blit from format %#x to %#x isn't replayed\n", s.format, d.format);
            continue;
        }
        /* the destination rectangle within the destination, each column and row its source's, nearest */
        GpuBlit b = {0};
        b.x0 = r->dr.xoffset > 0 ? (u32)r->dr.xoffset : 0;
        b.y0 = r->dr.yoffset > 0 ? (u32)r->dr.yoffset : 0;
        b.x1 = (u32)((i64)r->dr.xoffset + r->dr.width < (i64)d.width ? (i64)r->dr.xoffset + r->dr.width : d.width);
        b.y1 = (u32)((i64)r->dr.yoffset + r->dr.height < (i64)d.height ? (i64)r->dr.yoffset + r->dr.height
                                                                         : d.height);
        if (b.x0 >= b.x1 || b.y0 >= b.y1) continue;
        u32 n = b.x1 - b.x0;
        if (n > NVN_BLIT_MAX || b.y1 - b.y0 > NVN_BLIT_MAX)
            fatal("nvn: a texture blit %u pixels wide and %u high, larger than the %u a side nvn_direct holds", n,
                  b.y1 - b.y0, NVN_BLIT_MAX);
        u32* tab = g_nvn.copy.blit; /* the GPU thread's: blit_mem copies the tables before it returns */
        for (u32 i = 0; i < n; i++) {
            double at = r->sr.xoffset + ((double)((i64)b.x0 + i - r->dr.xoffset) + 0.5) * sdx;
            tab[i] = at <= 0 ? 0 : at >= (double)s.width ? s.width - 1 : (u32)at;
        }
        for (u32 i = b.y0; i < b.y1; i++) {
            double at = r->sr.yoffset + ((double)((i64)i - r->dr.yoffset) + 0.5) * sdy;
            tab[n + i - b.y0] = at <= 0 ? 0 : at >= (double)s.height ? s.height - 1 : (u32)at;
        }
        b.src = blit_plane(&s, bs);
        b.dst = blit_plane(&d, bd);
        b.bpp = bs;
        b.scol = tab;
        b.srow = tab + n;
        if (!g_renderer->blit_mem(&b))
            LOG_LIMIT(4, "nvn: the GPU could not do a texture blit, skipped\n");
        else if (g_renderer->rt_reload)
            g_renderer->rt_reload(d.addr);
    }
}
/* between two textures' regions of the same size and block bytes */
static void copy_texture_texture(const NvnPCopyT2T* r, const NvnTexture* s, const NvnTexture* d) {
    i32 depth = r->sr.depth > 0 ? r->sr.depth : 1;
    /* a multisample resolve (Downsample): the 2D engine's blit of the source's samples onto the destination's
       pixels, which the renderer resolves on the GPU when the source is its multisampled target (vk_blit2d) */
    if (r->flags & NVN_COPY_RESOLVE) {
        NvnPCopyT2T q = *r;
        u32 mode = nvn_tex_msaa(s->d.samples);
        i32 bx = (i32)(1u << ds_msaa_log2_x(mode)), by = (i32)(1u << ds_msaa_log2_y(mode));
        q.sr.xoffset *= bx;
        q.sr.width *= bx;
        q.sr.yoffset *= by;
        q.sr.height *= by;
        blit_texture(&q);
        return;
    }
    TexPlane ps, pd;
    if (!tex_plane(s, r->has_sv ? &r->sv : NULL, &ps) || !tex_plane(d, r->has_dv ? &r->dv : NULL, &pd)) return;
    if (r->sr.width != r->dr.width || r->sr.height != r->dr.height || ps.bpb != pd.bpb || ps.bw != pd.bw ||
        ps.bhp != pd.bhp) {
        blit_texture(r);
        return;
    }
    if (r->sr.width <= 0 || r->sr.height <= 0) return;
    GpuDmaOp o = {0};
    region_lines(&ps, r->sr.width, r->sr.height, &o);
    for (i32 k = 0; k < depth; k++) {
        tex_side(&ps, r->sr.xoffset, r->sr.yoffset, r->sr.zoffset + k, &o.src);
        tex_side(&pd, r->dr.xoffset, r->dr.yoffset, r->dr.zoffset + k, &o.dst);
        launch(&o);
    }
}
/* n bytes between buffers, as lines of up to 64 KiB */
static void copy_buffer_buffer(u64 src, u64 dst, u64 n) {
    const u32 line = 1u << 16;
    while (n) {
        u64 lines = n / line;
        GpuDmaOp o = {0};
        o.multi = 1;
        if (lines) {
            o.width = line;
            o.lines = lines > 1024 ? 1024 : (u32)lines;
        } else {
            o.width = (u32)n;
            o.lines = 1;
        }
        o.line_in = o.line_out = o.width;
        o.src.addr = src;
        o.src.pitch = o.width;
        o.dst.addr = dst;
        o.dst.pitch = o.width;
        launch(&o);
        src += (u64)o.width * o.lines;
        dst += (u64)o.width * o.lines;
        n -= (u64)o.width * o.lines;
    }
}

/* n bytes at dst filled with the 32-bit value, as the DMA engine fills: its remap writes constant A into every
   component, reading nothing */
static void fill_buffer(u64 dst, u64 n, u32 value) {
    const u32 line = 1u << 14; /* elements of 4 bytes a line */
    n /= 4;
    while (n) {
        u64 lines = n / line;
        GpuDmaOp o = {0};
        o.multi = o.remap = o.fill = 1;
        o.cs = 4;
        o.ns = o.nd = 1;
        o.sel[0] = 4; /* constant A */
        o.const_a = value;
        o.width = lines ? line : (u32)n;
        o.lines = lines ? (lines > 1024 ? 1024 : (u32)lines) : 1;
        o.line_in = o.line_out = o.width * 4;
        o.dst.addr = dst;
        o.dst.pitch = o.line_out;
        launch(&o);
        dst += (u64)o.line_out * o.lines;
        n -= (u64)o.width * o.lines;
    }
}

int nvn_copy_record(u32 op, u64 payload, u32 size, i64 stride[2]) {
    (void)size;
    switch (op) {
        case OP_COPY_STRIDE: {
            const NvnPCopyStride* r = NVN_OBJ(const NvnPCopyStride, payload);
            stride[r->image ? 1 : 0] = r->stride;
            return 1;
        }
        case OP_COPY_B2B: {
            const NvnPCopyB2B* r = NVN_OBJ(const NvnPCopyB2B, payload);
            copy_buffer_buffer(r->src, r->dst, r->size);
            return 1;
        }
        case OP_CLEAR_BUF: {
            const NvnPClearBuf* r = NVN_OBJ(const NvnPClearBuf, payload);
            fill_buffer(r->address, r->size, r->value);
            return 1;
        }
        case OP_COPY_B2T:
        case OP_COPY_T2B: {
            NvnPCopyB2T r;
            memcpy(&r, GP(payload), sizeof r);
            if (!r.texture) return 1;
            NvnTexture t = {0}; /* past the snapshot's bytes */
            memcpy(&t, GP(r.texture), NVN_TEX_SNAP);
            copy_buffer_texture(&r, &t, op == OP_COPY_B2T, stride, NULL);
            return 1;
        }
        case OP_COPY_T2T: {
            NvnPCopyT2T r;
            memcpy(&r, GP(payload), sizeof r);
            if (!r.src || !r.dst) return 1;
            NvnTexture s = {0}, d = {0}; /* past the snapshots' bytes */
            memcpy(&s, GP(r.src), NVN_TEX_SNAP);
            memcpy(&d, GP(r.dst), NVN_TEX_SNAP);
            copy_texture_texture(&r, &s, &d);
            return 1;
        }
    }
    return 0;
}

/* ---- the CPU's texel writes and reads */
/* A copy between a texture's region and a buffer: the region (the buffer's address in r.buffer for a read), with the
   texture snapshot below for its texture, and the buffer's row and image strides */
typedef struct {
    NvnPCopyB2T r;
    NvnTexture t;
    i64 stride[2];
} TexelCopy;
/* the copy of the region at guest address region of texture t (its view v, 0 for none), with rows row bytes apart and
   slices image bytes apart (0 for tightly packed): 1 and in *span the bytes of the buffer it covers, 0 when the
   runtime can't lay it out */
static int texel_copy(u64 t, u64 v, u64 region, i64 row, i64 image, TexelCopy* x, u64* span) {
    *x = (TexelCopy){0};
    memcpy(&x->t, GP(t), sizeof x->t);
    memcpy(&x->r.r, GP(region), sizeof x->r.r);
    if (v) {
        memcpy(&x->r.view, GP(v), sizeof x->r.view);
        x->r.has_view = 1;
    }
    TexPlane p;
    if (!tex_plane(&x->t, x->r.has_view ? &x->r.view : NULL, &p) || x->r.r.width <= 0 || x->r.r.height <= 0) return 0;
    u64 width = ((u64)x->r.r.width + (u64)p.bw - 1) / (u64)p.bw * p.bpb;
    u64 lines = ((u64)x->r.r.height + (u64)p.bhp - 1) / (u64)p.bhp;
    u64 depth = x->r.r.depth > 0 ? (u64)x->r.r.depth : 1;
    x->stride[0] = row ? row : (i64)width;
    x->stride[1] = image ? image : x->stride[0] * (i64)lines;
    *span = (u64)x->stride[1] * (depth - 1) + (u64)x->stride[0] * (lines - 1) + width;
    return 1;
}
/* A texel write in flight to the GPU thread: one block, the copy and the source's bytes as they were when the game
   wrote. Its size is the game's (a whole texture's upload), and how many are in flight at once is how far the GPU
   thread is behind, so it is the heap's, not a pool's */
typedef struct {
    TexelCopy c;
    u8 bytes[];
} TexelWrite;
static void texel_write(void* arg) {
    TexelWrite* w = (TexelWrite*)arg;
    copy_buffer_texture(&w->c.r, &w->c.t, 1, w->c.stride, w->bytes);
    free(w);
}
void nvn_write_texels(u64 t, u64 v, u64 region, u64 src, i64 row, i64 image) {
    TexelCopy x;
    u64 span;
    if (!texel_copy(t, v, region, row, image, &x, &span)) {
        LOG_LIMIT(4, "nvn: a texel write into texture %llx (format %#x, %dx%d) the runtime can't lay out\n",
                  (unsigned long long)t, x.t.d.format, x.r.r.width, x.r.r.height);
        return;
    }
    TexelWrite* w = span <= SIZE_MAX - sizeof *w ? (TexelWrite*)malloc(sizeof *w + (size_t)span) : NULL;
    if (!w) fatal("nvn: out of memory for a texel write's %llu bytes", (unsigned long long)span);
    w->c = x;
    memcpy(w->bytes, GP(src), span); /* the source's whole span, read as the game writes it */
    gpu_call(texel_write, w, NULL, 0);
}

/* A texel read: the copy, on the stack of the game's thread, which waits for the GPU thread's increment of syncpt
   after it */
typedef struct {
    TexelCopy c; /* the destination at c.r.buffer */
    u32 syncpt;
} TexelRead;
static void texel_read(void* arg) {
    const TexelRead* r = (const TexelRead*)arg;
    copy_buffer_texture(&r->c.r, &r->c.t, 0, r->c.stride, NULL);
    if (g_renderer->stream_incr) /* once the copy is done */
        g_renderer->stream_incr(r->syncpt);
    else
        syncpt_incr(r->syncpt);
}
void nvn_read_texels(u64 t, u64 v, u64 region, u64 dst, i64 row, i64 image) {
    TexelRead r;
    u64 span;
    if (!texel_copy(t, v, region, row, image, &r.c, &span)) {
        LOG_LIMIT(4, "nvn: a texel read from texture %llx (format %#x) the runtime can't lay out\n",
                  (unsigned long long)t, r.c.t.d.format);
        return;
    }
    /* the destination, mapped in the GPU's page table while the copy runs: the GPU writes it, and the CPU's read of
       it lands what the GPU wrote (a readback of host-cached memory the GPU copied into, gpu/vk/owned.c) */
    u64 first = dst & ~0xfffull;
    u64 size = (dst + span - first + 0xfff) & ~0xfffull;
    u64 va = gmmu_space_alloc(size, 0x1000);
    if (!va) fatal("nvn: no GPU address space left for a texel read of %llu bytes", (unsigned long long)span);
    gmmu_space_map(va, first, size);
    r.c.r.buffer = va + (dst - first);
    pm_lock(&g_nvn.copy.read_lock);
    if (!g_nvn.copy.read_syncpt) g_nvn.copy.read_syncpt = syncpt_alloc();
    r.syncpt = g_nvn.copy.read_syncpt;
    u32 value = syncpt_promise(r.syncpt, 1);
    gpu_call(texel_read, &r, NULL, 0);
    pm_unlock(&g_nvn.copy.read_lock);
    syncpt_wait(g_nvn.copy.read_syncpt, value, -1);
    gmmu_space_unmap(va, size);
    gmmu_space_free(va, size);
}
