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
    int blits, copies, blit_ok;
    u32 src_format, dst_format;
} g_seen;
static int count_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy,
                      double sdx, double sdy) {
    (void)dx, (void)dy, (void)dw, (void)dh, (void)sx, (void)sy, (void)sdx, (void)sdy;
    g_seen.blits++;
    g_seen.src_format = s->format;
    g_seen.dst_format = d->format;
    return g_seen.blit_ok;
}
static int count_copy(const GpuDmaOp* o) {
    (void)o;
    g_seen.copies++;
    return 1;
}
static void no_reload(u64 gva) { (void)gva; }
static Renderer g_counting = {.blit = count_blit, .dma_copy = count_copy, .rt_reload = no_reload};
const Renderer* g_renderer = &g_counting;
int g_renderer_up = 1;

/* ---- what the file needs of the runtime: the texture as a render target, and stubs the cases don't reach */
/* the render target formats of the NVN formats below (NVN 55.15's format table) */
static u32 rt_format(u32 nvn) {
    switch (nvn) {
        case NVN_FORMAT_RGBA8: return 0xD5;
        case NVN_FORMAT_RGBA8_SRGB: return 0xD6;
        case NVN_FORMAT_R11G11B10F: return 0xE0;
    }
    return 0;
}
int nvn_draw_surface(u64 snapshot, const NvnView* v, DsSurf* s, int* depth, u32* msaa) {
    const NvnTexture* t = NVN_OBJ(const NvnTexture, snapshot);
    (void)v;
    memset(s, 0, sizeof *s);
    s->gva = t->gpu;
    s->width = t->d.width;
    s->height = t->d.height;
    s->format = rt_format(t->d.format);
    *depth = 0;
    *msaa = 0;
    return s->format != 0;
}
u32 gpu_color_bpp(u32 format) { return format ? 4 : 0; }
int nvn_fmt_bpb(int format, int* bw, int* bh) {
    *bw = *bh = 1;
    return rt_format((u32)format) ? 4 : 0;
}
void nvn_tex_layout(const NvnTexDesc* d, NvnTexLayout* o) {
    (void)d;
    memset(o, 0, sizeof *o);
}
u64 nvn_tex_level_offset(const NvnTexDesc* d, unsigned level) {
    (void)d, (void)level;
    return 0;
}
void nvn_tex_level_tiles(const NvnTexDesc* d, unsigned level, unsigned* tile_h, unsigned* tile_d) {
    (void)d, (void)level;
    *tile_h = *tile_d = 0;
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

int main(void) {
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
    check(g_seen.blits == 1 && g_seen.copies == 1,
          "R11G11B10F to RGBA8_SRGB, no target: expected 1 blit tried and 1 copy through memory, found %d and %d",
          g_seen.blits, g_seen.copies);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("nvn copy formats: ok\n");
    return 0;
}
