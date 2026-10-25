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
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/renderer.h"
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
    NvnTexLayout l;
    unsigned level = v && (v->set & NVN_VIEW_LEVELS) ? v->base_level : 0, th, td;
    u32 w, h;
    int bpb;
    memset(p, 0, sizeof *p);
    if (t->magic != NVN_MAGIC('T', 'E', 'X', 0) || !t->gpu) return 0;
    bpb = nvn_fmt_bpb((int)(v && (v->set & NVN_VIEW_FORMAT) ? v->format : t->d.format), &p->bw, &p->bhp);
    if (bpb <= 0 || p->bw <= 0 || p->bhp <= 0) return 0;
    nvn_tex_layout(&t->d, &l);
    w = t->d.width >> level ? t->d.width >> level : 1;
    h = t->d.height >> level ? t->d.height >> level : 1;
    p->bpb = (u32)bpb;
    p->wb = (w + (u32)p->bw - 1) / (u32)p->bw * (u32)bpb;
    p->rows = (h + (u32)p->bhp - 1) / (u32)p->bhp;
    p->layer = l.layer_size;
    p->base = t->gpu + nvn_tex_level_offset(&t->d, level);
    if (v && (v->set & NVN_VIEW_LAYERS)) p->base += (u64)v->min_layer * l.layer_size;
    p->volume = t->d.target == NVN_TEXTURE_TARGET_3D;
    p->depth = p->volume ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    if (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR) {
        p->pitch = t->d.stride ? t->d.stride : p->wb;
        return 1;
    }
    nvn_tex_level_tiles(&t->d, level, &th, &td);
    p->bl = 1;
    p->bh = th;
    p->bd = td;
    return 1;
}
/* the plane's side of a copy of the region at texels (x, y) of slice or layer z */
static void tex_side(const TexPlane* p, i32 x, i32 y, i32 z, GpuDmaSide* s) {
    memset(s, 0, sizeof *s);
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
    TexPlane p;
    GpuDmaOp o;
    i32 k, depth = r->r.depth > 0 ? r->r.depth : 1;
    u64 row, image;
    if (r->r.width <= 0 || r->r.height <= 0) return;
    if (!tex_plane(t, r->has_view ? &r->view : NULL, &p)) {
        LOG_LIMIT(4, "nvn: a copy of a texture of format %#x, which the runtime can't lay out\n", t->d.format);
        return;
    }
    memset(&o, 0, sizeof o);
    region_lines(&p, r->r.width, r->r.height, &o);
    row = stride[0] ? (u64)stride[0] : o.width;
    image = stride[1] ? (u64)stride[1] : row * o.lines;
    for (k = 0; k < depth; k++) {
        GpuDmaSide* b = to_tex ? &o.src : &o.dst;
        tex_side(&p, r->r.xoffset, r->r.yoffset, r->r.zoffset + k, to_tex ? &o.dst : &o.src);
        memset(b, 0, sizeof *b);
        b->addr = r->buffer + image * (u64)k;
        b->pitch = (u32)row;
        o.snap = snap ? snap + image * (u64)k : NULL;
        launch(&o);
    }
}
/* between two textures' regions of the same size and block bytes */
static void copy_texture_texture(const NvnPCopyT2T* r, const NvnTexture* s, const NvnTexture* d) {
    TexPlane ps, pd;
    GpuDmaOp o;
    i32 k, depth = r->sr.depth > 0 ? r->sr.depth : 1;
    if (r->flags & NVN_COPY_RESOLVE) {
        LOG_LIMIT(1, "nvn: a multisample resolve (Downsample) isn't replayed yet\n");
        return;
    }
    if (!tex_plane(s, r->has_sv ? &r->sv : NULL, &ps) || !tex_plane(d, r->has_dv ? &r->dv : NULL, &pd)) return;
    if (r->sr.width != r->dr.width || r->sr.height != r->dr.height || ps.bpb != pd.bpb || ps.bw != pd.bw ||
        ps.bhp != pd.bhp) {
        LOG_LIMIT(1,
                  "nvn: a texture copy that scales or converts (%dx%d to %dx%d, %u to %u bytes a block) isn't "
                  "replayed yet\n",
                  r->sr.width, r->sr.height, r->dr.width, r->dr.height, ps.bpb, pd.bpb);
        return;
    }
    if (r->sr.width <= 0 || r->sr.height <= 0) return;
    memset(&o, 0, sizeof o);
    region_lines(&ps, r->sr.width, r->sr.height, &o);
    for (k = 0; k < depth; k++) {
        tex_side(&ps, r->sr.xoffset, r->sr.yoffset, r->sr.zoffset + k, &o.src);
        tex_side(&pd, r->dr.xoffset, r->dr.yoffset, r->dr.zoffset + k, &o.dst);
        launch(&o);
    }
}
/* n bytes between buffers, as lines of up to 64 KiB */
static void copy_buffer_buffer(u64 src, u64 dst, u64 n) {
    const u32 line = 1u << 16;
    GpuDmaOp o;
    while (n) {
        u64 lines = n / line;
        memset(&o, 0, sizeof o);
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
        case OP_COPY_B2T:
        case OP_COPY_T2B: {
            NvnPCopyB2T r;
            NvnTexture t;
            memcpy(&r, GP(payload), sizeof r);
            if (!r.texture) return 1;
            memcpy(&t, GP(r.texture), sizeof t);
            copy_buffer_texture(&r, &t, op == OP_COPY_B2T, stride, NULL);
            return 1;
        }
        case OP_COPY_T2T: {
            NvnPCopyT2T r;
            NvnTexture s, d;
            memcpy(&r, GP(payload), sizeof r);
            if (!r.src || !r.dst) return 1;
            memcpy(&s, GP(r.src), sizeof s);
            memcpy(&d, GP(r.dst), sizeof d);
            copy_texture_texture(&r, &s, &d);
            return 1;
        }
    }
    return 0;
}

/* ---- the CPU's texel writes */
typedef struct {
    NvnPCopyB2T r; /* the region, with the texture snapshot below for its texture */
    NvnTexture t;
    i64 stride[2];
    u8* bytes; /* the source's span, as it was when the game wrote */
} TexelWrite;
static void texel_write(void* arg) {
    TexelWrite* w = (TexelWrite*)arg;
    copy_buffer_texture(&w->r, &w->t, 1, w->stride, w->bytes);
    free(w->bytes);
    free(w);
}
void nvn_write_texels(u64 t, u64 v, u64 region, u64 src, i64 row, i64 image) {
    TexelWrite* w = (TexelWrite*)calloc(1, sizeof *w);
    TexPlane p;
    u64 lines, width, span, depth;
    if (!w) fatal("nvn: out of memory for a texel write");
    memcpy(&w->t, GP(t), sizeof w->t);
    memcpy(&w->r.r, GP(region), sizeof w->r.r);
    if (v) {
        memcpy(&w->r.view, GP(v), sizeof w->r.view);
        w->r.has_view = 1;
    }
    if (!tex_plane(&w->t, w->r.has_view ? &w->r.view : NULL, &p) || w->r.r.width <= 0 || w->r.r.height <= 0) {
        LOG_LIMIT(4, "nvn: a texel write into texture %llx (format %#x, %dx%d) the runtime can't lay out\n",
                  (unsigned long long)t, w->t.d.format, w->r.r.width, w->r.r.height);
        free(w);
        return;
    }
    width = ((u64)w->r.r.width + (u64)p.bw - 1) / (u64)p.bw * p.bpb;
    lines = ((u64)w->r.r.height + (u64)p.bhp - 1) / (u64)p.bhp;
    depth = w->r.r.depth > 0 ? (u64)w->r.r.depth : 1;
    w->stride[0] = row ? row : (i64)width;
    w->stride[1] = image ? image : w->stride[0] * (i64)lines;
    span = (u64)w->stride[1] * (depth - 1) + (u64)w->stride[0] * (lines - 1) + width;
    w->bytes = (u8*)malloc(span);
    if (!w->bytes) fatal("nvn: out of memory for a texel write's %llu bytes", (unsigned long long)span);
    memcpy(w->bytes, GP(src), span); /* the source's whole span, read as the game writes it */
    gpu_call(texel_write, w, NULL, 0);
}
