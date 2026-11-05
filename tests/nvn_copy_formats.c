/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's copies between textures of the same size and bytes a pixel, as the 2D engine the game's driver copies with
   does them: between two formats each pixel converted (the renderer's image blit, which converts as the engine does),
   between one format its bytes; through memory (the DMA engine's copy) only when the renderer's blit can't take it.
   The records go in through nvn_copy_record, against a renderer that counts what it is asked */
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "common/filesystem/host.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 memory[1 << 20];
u8* g_mem = memory; /* guest address a is memory + a */

/* ---- the renderer: what it was asked, and whether its blit takes it */
static struct {
    int blits, copies, blit_ok, memory_blits, linear;
    u32 src_format, dst_format, src_width, dst_width;
    u64 blit_src[8], blit_dst[8];
    GpuDmaOp dma[8];
    GpuBlit memory;
    u32 columns[8], rows[8];
    u32 source_z[8], destination_z[8];
    double sx, sy, sdx, sdy;
} g_seen;
static int count_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy,
                      double sdx, double sdy) {
    (void)dx, (void)dy, (void)dw, (void)dh;
    g_seen.sx = sx;
    g_seen.sy = sy;
    g_seen.sdx = sdx;
    g_seen.sdy = sdy;
    if (g_seen.blits < 8) {
        g_seen.blit_src[g_seen.blits] = s->addr;
        g_seen.blit_dst[g_seen.blits] = d->addr;
    }
    g_seen.blits++;
    g_seen.src_format = s->format;
    g_seen.dst_format = d->format;
    g_seen.src_width = s->width;
    g_seen.dst_width = d->width;
    return g_seen.blit_ok;
}
static int count_blit_filter(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy,
                             double sdx, double sdy, int linear) {
    g_seen.linear = linear;
    return count_blit(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
}
static int count_blit_mem(const GpuBlit* b) {
    if (g_seen.memory_blits < 8) {
        g_seen.source_z[g_seen.memory_blits] = b->src.z;
        g_seen.destination_z[g_seen.memory_blits] = b->dst.z;
    }
    g_seen.memory_blits++;
    g_seen.memory = *b;
    u32 columns = b->x1 - b->x0, rows = b->y1 - b->y0;
    memcpy(g_seen.columns, b->scol, (columns > 8 ? 8 : columns) * sizeof(u32));
    memcpy(g_seen.rows, b->srow, (rows > 8 ? 8 : rows) * sizeof(u32));
    return 1;
}
static int count_copy(const GpuDmaOp* o) {
    if (g_seen.copies < 8) g_seen.dma[g_seen.copies] = *o;
    g_seen.copies++;
    return 1;
}
static void no_reload(u64 gva) { (void)gva; }
static Renderer g_counting = {.blit = count_blit,
                              .blit_filter = count_blit_filter,
                              .blit_mem = count_blit_mem,
                              .dma_copy = count_copy,
                              .rt_reload = no_reload};
const Renderer* g_renderer = &g_counting;
int g_renderer_up = 1;

/* ---- what the file needs of the runtime: the texture as a render target, and stubs the cases don't reach */
/* the render target formats of the NVN formats below (NVN 55.15's format table) */
static u32 rt_format(u32 nvn) {
    switch (nvn) {
        case NVN_FORMAT_RGBA8: return 0xD5;
        case NVN_FORMAT_RGBA8_SRGB: return 0xD6;
        case NVN_FORMAT_R11G11B10F: return 0xE0;
        case NVN_FORMAT_RGBA16F: return 0xCA;
        case NVN_FORMAT_RGBA32F: return 0xC0;
    }
    return 0;
}
u32 nvn_rt_format(u32 format, int* depth) {
    *depth = 0;
    return rt_format(format);
}
int nvn_draw_surface(u64 snapshot, const NvnView* v, DsSurf* s, int* depth, u32* msaa) {
    const NvnTexture* t = NVN_OBJ(const NvnTexture, snapshot);
    memset(s, 0, sizeof *s);
    s->gva = t->gpu;
    s->width = t->d.width;
    s->height = t->d.height;
    if (t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) {
        s->height = 1;
        s->layer_stride = 512;
        if (v && (v->set & NVN_VIEW_LAYERS)) s->gva += (u64)v->min_layer * s->layer_stride;
    }
    if (!(t->d.flags & NVN_TEX_LINEAR)) s->width = (s->width + 15u) & ~15u;
    s->format = rt_format(t->d.format);
    *depth = 0;
    *msaa = 0;
    return s->format != 0;
}
u32 gpu_color_bpp(u32 format) { return format == 0xCA ? 8 : format == 0xC0 ? 16 : format ? 4 : 0; }
int nvn_fmt_bpb(int format, int* bw, int* bh) {
    *bw = *bh = 1;
    return format == NVN_FORMAT_RGBA4 || format == NVN_FORMAT_RGB565 || format == NVN_FORMAT_RGB5A1
               ? 2
               : gpu_color_bpp(rt_format((u32)format));
}
void nvn_tex_layout(const NvnTexDesc* d, NvnTexLayout* o) {
    memset(o, 0, sizeof *o);
    o->layer_size = d->target == NVN_TEXTURE_TARGET_1D_ARRAY ? 512 : 0;
}
u64 nvn_tex_level_offset(const NvnTexDesc* d, unsigned level) {
    (void)d, (void)level;
    return 0;
}
void nvn_tex_level_tiles(const NvnTexDesc* d, unsigned level, unsigned* tile_h, unsigned* tile_d) {
    (void)d, (void)level;
    *tile_h = *tile_d = 0;
}
u32 nvn_tex_level_gobs(const NvnTexDesc* d, unsigned level) {
    (void)d, (void)level;
    return 0;
}
u32 nvn_tex_msaa(int samples) {
    (void)samples;
    return 0;
}
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
u64 gmmu_space_alloc(u64 size, u64 align) {
    (void)size, (void)align;
    return 0;
}
void gmmu_space_map(u64 gva, u64 cpu, u64 size) { (void)gva, (void)cpu, (void)size; }
void gmmu_space_unmap(u64 gva, u64 size) { (void)gva, (void)size; }
void gmmu_space_free(u64 gva, u64 size) { (void)gva, (void)size; }
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) {
    (void)fn, (void)arg, (void)chans, (void)nchans;
}
u32 syncpt_alloc(void) { return 0; }
void syncpt_incr(u32 id) { (void)id; }
u32 syncpt_promise(u32 id, u32 n) {
    (void)id, (void)n;
    return 0;
}
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns) {
    (void)id, (void)thresh, (void)timeout_ns;
    return 1;
}
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

static int failures;
static void check(int ok, const char* what, ...) {
    if (!ok) {
        va_list ap;
        va_start(ap, what);
        fprintf(stderr, "FAIL: ");
        vfprintf(stderr, what, ap);
        fprintf(stderr, "\n");
        va_end(ap);
        failures++;
    }
}

#define SRC_TEX 0x1000u
#define DST_TEX 0x2000u
#define RECORD  0x3000u

static void texture(u64 at, u32 format, u64 gpu) {
    NvnTexture t = {0};
    t.magic = NVN_MAGIC('T', 'E', 'X', 0);
    t.d.target = NVN_TEXTURE_TARGET_2D;
    t.d.format = format;
    t.d.width = 64;
    t.d.height = 32;
    t.d.depth = 1;
    t.d.flags = NVN_TEX_LINEAR;
    t.gpu = gpu;
    memcpy(GP(at), &t, sizeof t);
}
/* the whole of a 64x32 source copied onto the whole of a 64x32 destination */
static void copy(u32 src_format, u32 dst_format, int blit_ok) {
    NvnPCopyT2T r = {0};
    i64 stride[2] = {0, 0};
    texture(SRC_TEX, src_format, 0x10000);
    texture(DST_TEX, dst_format, 0x20000);
    r.src = SRC_TEX;
    r.dst = DST_TEX;
    r.sr.width = r.dr.width = 64;
    r.sr.height = r.dr.height = 32;
    r.sr.depth = r.dr.depth = 1;
    memcpy(GP(RECORD), &r, sizeof r);
    memset(&g_seen, 0, sizeof g_seen);
    g_seen.blit_ok = blit_ok;
    check(nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride) == 1, "the copy record wasn't taken");
}

static void array1d(u64 at, u64 gpu) {
    texture(at, NVN_FORMAT_RGBA8, gpu);
    NvnTexture* t = NVN_OBJ(NvnTexture, at);
    t->d.target = NVN_TEXTURE_TARGET_1D_ARRAY;
    t->d.height = 8; /* layer count, not the height of a layer */
    t->d.flags = 0;
}
/* NVN copy regions use Y/height for 1D-array layers. The DMA planes each have one row; row stride cannot take
   the place of image stride between those layer planes. */
static void check_array1d_copies(void) {
    static const u32 ops[] = {OP_COPY_B2T, OP_COPY_T2B};
    array1d(SRC_TEX, 0x10000);
    for (u32 i = 0; i < 2; i++) {
        NvnPCopyB2T r = {0};
        r.texture = SRC_TEX;
        r.buffer = 0x80000;
        r.r = (NVNcopyRegion){.xoffset = 3, .yoffset = 2, .zoffset = 0, .width = 5, .height = 3, .depth = 1};
        r.has_view = 1;
        r.view.set = NVN_VIEW_LAYERS;
        r.view.min_layer = 1;
        memcpy(GP(RECORD), &r, sizeof r);
        i64 stride[2] = {32, 128};
        memset(&g_seen, 0, sizeof g_seen);
        check(nvn_copy_record(ops[i], RECORD, sizeof r, stride), "1D array buffer copy accepted");
        check(g_seen.copies == 3, "1D array buffer copy has three one-row layer planes, found %d", g_seen.copies);
        for (u32 k = 0; k < 3 && k < (u32)g_seen.copies; k++) {
            const GpuDmaOp* o = &g_seen.dma[k];
            const GpuDmaSide* texture_side = i ? &o->src : &o->dst;
            const GpuDmaSide* buffer_side = i ? &o->dst : &o->src;
            check(o->width == 20 && o->lines == 1 && texture_side->h == 1,
                  "1D array buffer copy layer %u has five RGBA8 texels in one row", k);
            check(texture_side->addr == 0x10000 + (u64)(3 + k) * 512 && texture_side->ox == 12 && !texture_side->oy,
                  "1D array buffer copy layer %u includes the view and region layer offsets", k);
            check(buffer_side->addr == 0x80000 + (u64)k * 128 && buffer_side->pitch == 32,
                  "1D array buffer copy layer %u advances the buffer by image stride", k);
        }
        r.flags = NVN_COPY_FLAGS_MIRROR_Z_BIT;
        memcpy(GP(RECORD), &r, sizeof r);
        memset(&g_seen, 0, sizeof g_seen);
        nvn_copy_record(ops[i], RECORD, sizeof r, stride);
        check(g_seen.copies == 3, "mirrored 1D array buffer copy has three layers");
        for (u32 k = 0; k < 3 && k < (u32)g_seen.copies; k++) {
            const GpuDmaOp* o = &g_seen.dma[k];
            check((i ? o->dst.addr : o->src.addr) == 0x80000 + (u64)(2 - k) * 128,
                  "mirrored 1D array buffer copy layer %u reverses the buffer image order", k);
            check((i ? o->src.addr : o->dst.addr) == 0x10000 + (u64)(3 + k) * 512,
                  "mirrored 1D array buffer copy layer %u keeps texture layer order", k);
        }
        /* Tightly packed image stride is one row, even though the region spans three layers. */
        r.flags = 0;
        memcpy(GP(RECORD), &r, sizeof r);
        stride[1] = 0;
        memset(&g_seen, 0, sizeof g_seen);
        nvn_copy_record(ops[i], RECORD, sizeof r, stride);
        check(g_seen.copies == 3, "1D array packed buffer copy has three layers");
        if (g_seen.copies == 3)
            check((i ? g_seen.dma[2].dst.addr : g_seen.dma[2].src.addr) == 0x80000 + 64,
                  "1D array packed buffer image stride is one row");
    }
    NvnPCopyT2T r = {0};
    array1d(DST_TEX, 0x20000);
    r.src = SRC_TEX;
    r.dst = DST_TEX;
    r.sr = (NVNcopyRegion){.xoffset = 3, .yoffset = 2, .width = 5, .height = 3, .depth = 1};
    r.dr = (NVNcopyRegion){.xoffset = 7, .yoffset = 4, .width = 5, .height = 3, .depth = 1};
    memcpy(GP(RECORD), &r, sizeof r);
    i64 stride[2] = {0};
    memset(&g_seen, 0, sizeof g_seen);
    nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
    check(g_seen.copies == 3, "1D array texture copy has three layer planes");
    for (u32 k = 0; k < 3 && k < (u32)g_seen.copies; k++) {
        const GpuDmaOp* o = &g_seen.dma[k];
        check(o->src.addr == 0x10000 + (u64)(2 + k) * 512 && o->dst.addr == 0x20000 + (u64)(4 + k) * 512,
              "1D array texture copy layer %u has independent source/destination layer coordinates", k);
        check(o->width == 20 && o->lines == 1 && !o->src.oy && !o->dst.oy,
              "1D array texture copy layer %u copies one row", k);
    }
    r.flags = NVN_COPY_FLAGS_MIRROR_Z_BIT;
    memcpy(GP(RECORD), &r, sizeof r);
    memset(&g_seen, 0, sizeof g_seen);
    nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
    check(g_seen.copies == 3, "mirrored 1D array texture copy has three layers");
    for (u32 k = 0; k < 3 && k < (u32)g_seen.copies; k++) {
        const GpuDmaOp* o = &g_seen.dma[k];
        check(o->src.addr == 0x10000 + (u64)(4 - k) * 512 && o->dst.addr == 0x20000 + (u64)(4 + k) * 512,
              "mirrored 1D array texture copy layer %u reverses source slices only", k);
    }
    memset(&g_seen, 0, sizeof g_seen);
    g_seen.blit_ok = 1;
    nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
    check(g_seen.blits == 3 && !g_seen.copies, "mirrored 1D array texture copy can use three GPU image blits");
    for (u32 k = 0; k < 3 && k < (u32)g_seen.blits; k++)
        check(g_seen.blit_src[k] == 0x10000 + (u64)(4 - k) * 512 && g_seen.blit_dst[k] == 0x20000 + (u64)(4 + k) * 512,
              "mirrored 1D array image blit layer %u reverses source slices only", k);
}

static void check_buffer_mirrors(void) {
    texture(SRC_TEX, NVN_FORMAT_RGBA8, 0x10000);
    NvnPCopyB2T r = {
        .buffer = 0x80000, .texture = SRC_TEX, .r = {3, 2, 0, 5, 3, 1}, .flags = NVN_COPY_FLAGS_MIRROR_Y_BIT};
    i64 stride[2] = {32, 128};
    for (u32 to_texture = 0; to_texture < 2; to_texture++) {
        memset(&g_seen, 0, sizeof g_seen);
        memcpy(GP(RECORD), &r, sizeof r);
        nvn_copy_record(to_texture ? OP_COPY_B2T : OP_COPY_T2B, RECORD, sizeof r, stride);
        check(g_seen.memory_blits == 1 && !g_seen.copies, "Y-mirrored buffer copy uses one GPU gather");
        check(g_seen.memory.bpp == 4 && g_seen.memory.x1 - g_seen.memory.x0 == 5 &&
                  g_seen.memory.y1 - g_seen.memory.y0 == 3,
              "buffer mirror preserves pixel size and rectangle");
        for (u32 i = 0; i < 5; i++)
            check(g_seen.columns[i] == (to_texture ? i : 3 + i), "buffer mirror source column %u", i);
        for (u32 i = 0; i < 3; i++)
            check(g_seen.rows[i] == (to_texture ? 2 - i : 4 - i), "buffer mirror source row %u", i);
        check(g_seen.memory.x0 == (to_texture ? 3u : 0u) && g_seen.memory.y0 == (to_texture ? 2u : 0u),
              "buffer mirror keeps the destination origin");
    }
    r.flags |= NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_ENGINE_2D_BIT;
    memcpy(GP(RECORD), &r, sizeof r);
    memset(&g_seen, 0, sizeof g_seen);
    nvn_copy_record(OP_COPY_B2T, RECORD, sizeof r, stride);
    for (u32 i = 0; i < 5; i++) check(g_seen.columns[i] == 4 - i, "ENGINE_2D buffer copy mirrors source column %u", i);
}

static void check_texture_filters(void) {
    texture(SRC_TEX, NVN_FORMAT_RGBA8, 0x10000);
    texture(DST_TEX, NVN_FORMAT_RGBA8, 0x20000);
    NvnPCopyT2T r = {.src = SRC_TEX,
                     .dst = DST_TEX,
                     .sr = {3, 2, 0, 5, 3, 1},
                     .dr = {0, 0, 0, 5, 3, 1},
                     .flags = NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT};
    i64 stride[2] = {0};
    memcpy(GP(RECORD), &r, sizeof r);
    memset(&g_seen, 0, sizeof g_seen);
    nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
    check(g_seen.memory_blits == 1 && !g_seen.copies, "mirrored texture fallback gathers on the GPU");
    check(g_seen.sx == 8 && g_seen.sy == 5 && g_seen.sdx == -1 && g_seen.sdy == -1,
          "mirrored texture image blit reverses source endpoints");
    for (u32 i = 0; i < 5; i++) check(g_seen.columns[i] == 7 - i, "texture mirror source column %u", i);
    for (u32 i = 0; i < 3; i++) check(g_seen.rows[i] == 4 - i, "texture mirror source row %u", i);
    r.dr.width = 10;
    r.dr.height = 6;
    for (u32 linear = 0; linear < 2; linear++) {
        r.flags = linear ? NVN_COPY_FLAGS_LINEAR_FILTER_BIT : 0;
        memcpy(GP(RECORD), &r, sizeof r);
        memset(&g_seen, 0, sizeof g_seen);
        g_seen.blit_ok = 1;
        nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
        check(g_seen.blits == 1 && g_seen.linear == (int)linear && g_seen.sdx == 0.5 && g_seen.sdy == 0.5,
              "scaled texture copies honor their explicit nearest/linear flag");
    }
    NVN_OBJ(NvnTexture, SRC_TEX)->d.width = 2;
    NVN_OBJ(NvnTexture, SRC_TEX)->d.flags = 0;
    NVN_OBJ(NvnTexture, DST_TEX)->d.width = 4;
    NVN_OBJ(NvnTexture, DST_TEX)->d.flags = 0;
    r.sr.xoffset = r.sr.yoffset = 0;
    r.sr.width = r.sr.height = 2;
    r.dr.width = r.dr.height = 4;
    memcpy(GP(RECORD), &r, sizeof r);
    memset(&g_seen, 0, sizeof g_seen);
    g_seen.blit_ok = 1;
    nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
    check(g_seen.src_width == 2 && g_seen.dst_width == 4,
          "copy surfaces keep logical texel widths separate from padded render-target rows");
}

static void check_volume_mirrors(void) {
    texture(SRC_TEX, NVN_FORMAT_RGBA8, 0x10000);
    texture(DST_TEX, NVN_FORMAT_RGBA8, 0x20000);
    for (u32 i = 0; i < 2; i++) {
        NvnTexture* texture = NVN_OBJ(NvnTexture, i ? DST_TEX : SRC_TEX);
        texture->d.target = NVN_TEXTURE_TARGET_3D;
        texture->d.flags = 0;
        texture->d.depth = 4;
    }
    NvnPCopyT2T r = {.src = SRC_TEX,
                     .dst = DST_TEX,
                     .sr = {3, 2, 1, 5, 3, 2},
                     .dr = {7, 4, 0, 5, 3, 2},
                     .flags = NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT};
    i64 stride[2] = {0};
    memcpy(GP(RECORD), &r, sizeof r);
    memset(&g_seen, 0, sizeof g_seen);
    nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
    check(g_seen.memory_blits == 2 && !g_seen.blits && !g_seen.copies, "mirrored volume uses one GPU gather per slice");
    check(g_seen.source_z[0] == 2 && g_seen.source_z[1] == 1 && g_seen.destination_z[0] == 0 &&
              g_seen.destination_z[1] == 1,
          "volume mirrors reverse source slices without changing destination slice order");
    for (u32 i = 0; i < 5; i++) check(g_seen.columns[i] == 7 - i, "volume mirror column %u", i);
    for (u32 i = 0; i < 3; i++) check(g_seen.rows[i] == 4 - i, "volume mirror row %u", i);
    check(g_seen.memory.x0 == 7 && g_seen.memory.y0 == 4 && g_seen.memory.x1 == 12 && g_seen.memory.y1 == 7,
          "volume mirror retains its destination window");
    r.flags = 0;
    r.dr.width = 10;
    r.dr.height = 6;
    memcpy(GP(RECORD), &r, sizeof r);
    memset(&g_seen, 0, sizeof g_seen);
    nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
    check(g_seen.memory_blits == 2, "nearest scaled volume uses one gather per slice");
    for (u32 i = 0; i < 8; i++) check(g_seen.columns[i] == 3 + i / 2, "nearest scaled volume column %u", i);
    for (u32 i = 0; i < 6; i++) check(g_seen.rows[i] == 2 + i / 2, "nearest scaled volume row %u", i);
}

static void check_memory_conversions(void) {
    const u32 source_formats[] = {NVN_FORMAT_RGBA32F, NVN_FORMAT_RGBA4, NVN_FORMAT_RGB5A1};
    const u32 destination_formats[] = {NVN_FORMAT_RGBA16F, NVN_FORMAT_RGBA4, NVN_FORMAT_RGB565};
    for (u32 kind = 0; kind < 3; kind++) {
        texture(SRC_TEX, source_formats[kind], 0x10000);
        texture(DST_TEX, destination_formats[kind], 0x20000);
        if (!kind)
            for (u32 i = 0; i < 2; i++) {
                NvnTexture* t = NVN_OBJ(NvnTexture, i ? DST_TEX : SRC_TEX);
                t->d.target = NVN_TEXTURE_TARGET_3D;
                t->d.flags = 0;
                t->d.depth = 3;
            }
        NvnPCopyT2T r = {.src = SRC_TEX,
                         .dst = DST_TEX,
                         .sr = {1, 2, 0, 4, 3, 1},
                         .dr = {3, 4, 0, 8, 6, 1},
                         .flags = NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT};
        i64 stride[2] = {0};
        memcpy(GP(RECORD), &r, sizeof r);
        memset(&g_seen, 0, sizeof g_seen);
        nvn_copy_record(OP_COPY_T2T, RECORD, sizeof r, stride);
        check(g_seen.memory_blits == 1 && !g_seen.blits && !g_seen.copies,
              "conversion/packed case %u uses one GPU gather without an attachment", kind);
        check(g_seen.memory.x0 == 3 && g_seen.memory.y0 == 4 && g_seen.memory.x1 == 11 && g_seen.memory.y1 == 10,
              "conversion/packed case %u retains the destination window", kind);
        check(g_seen.columns[0] == 4 && g_seen.columns[7] == 1 && g_seen.rows[0] == 4 && g_seen.rows[5] == 2,
              "conversion/packed case %u selects mirrored nearest samples", kind);
        check(kind == 1 ? g_seen.memory.fmt == 0 : (g_seen.memory.fmt & GPU_BLIT_CONVERT_BIT) != 0,
              "same-format packed bytes bypass conversion, distinct formats convert components (%u)", kind);
        check(GPU_BLIT_DST_BPP(&g_seen.memory) == (kind ? 2u : 8u),
              "conversion/packed case %u has the destination's byte width", kind);
    }
}

int main(void) {
    check_array1d_copies();
    check_buffer_mirrors();
    check_texture_filters();
    check_volume_mirrors();
    check_memory_conversions();
    /* a scene's R11G11B10F into an RGBA8 sRGB window texture: the engine converts each pixel, so the blit does it */
    copy(NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGBA8_SRGB, 1);
    check(g_seen.blits == 1 && g_seen.copies == 0,
          "R11G11B10F to RGBA8_SRGB: expected 1 blit and no copy through memory, found %d blits and %d copies",
          g_seen.blits, g_seen.copies);
    check(g_seen.src_format == 0xE0 && g_seen.dst_format == 0xD6,
          "R11G11B10F to RGBA8_SRGB: the blit's formats expected 0xe0 to 0xd6, found %#x to %#x", g_seen.src_format,
          g_seen.dst_format);
    /* one format: the blit copies its bytes, as before */
    copy(NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA8, 1);
    check(g_seen.blits == 1 && g_seen.copies == 0,
          "RGBA8 to RGBA8: expected 1 blit and no copy through memory, found %d blits and %d copies", g_seen.blits,
          g_seen.copies);
    /* a source the renderer's blit can't take (no render target over it): the bytes go through memory */
    copy(NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGBA8_SRGB, 0);
    check(g_seen.blits == 1 && g_seen.copies == 0 && g_seen.memory_blits == 1 &&
              (g_seen.memory.fmt & GPU_BLIT_CONVERT_BIT),
          "R11G11B10F to RGBA8_SRGB, rejected image conversion uses component conversion, found %d and %d",
          g_seen.blits, g_seen.copies);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("nvn copy formats: ok\n");
    return 0;
}
