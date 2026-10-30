/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NVN functions of API 53 and 55 that nvn_direct took over last, family by family, against what the games'
   drivers do (their code, NVN 53.314 and 55.15): command buffers (gpu/nvn/nvn_cmd.c) record and the queue's replay
   (nvn_draw.c) turns the records into the renderer's draw state, which a renderer of the test's takes; the device,
   textures, samplers, programs, events, syncs and state objects (nvn_object.c); and the queue and window
   (nvn_queue.c) */
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/loader.h"
#include "gpu/nvn/nvn.hook.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 memory[4 << 20];
u8* g_mem = memory; /* guest address a is memory + a */
u64 g_code_lo, g_code_hi;
int g_frames, g_opt_fps;

/* the memory: a pool's storage at POOL_MEM, objects below it */
#define POOL       0x1000u
#define POOL_B     0x1100u
#define POOL_MEM   0x100000u
#define CMDBUF     0x2000u
#define TEX        0x3000u
#define TEX_B      0x3100u
#define VIEW_A     0x3200u
#define VIEW_B     0x3210u
#define SAMPLER    0x3400u
#define SAMPLER_B  0x3480u
#define SAMPLER_P  0x3500u
#define EVENT      0x3600u
#define EVENT_B    0x3640u
#define PROGRAM    0x3700u
#define DEVICE     0x4000u
#define DEVICE_B   0x7000u
#define QUEUE_B    0x7100u
#define WINDOW     0x7200u
#define WINDOW_B   0x7400u
#define STATE      0x7500u
#define SCRATCH    0x8000u
#define SCRATCH2   0x8100u

/* ---- what the files need of the runtime: guest memory as the GPU's too, and nothing that waits */
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    static u64 lent = 0x300000;
    u64 at = lent;
    (void)state;
    (void)perm;
    lent += size;
    return at;
}
void nvn_verify_memory_back(u64 mem, u64 size) {
    (void)mem;
    (void)size;
}
void nvn_verify_live_draw(const DsState* ds, const DrawCall* dc) {
    (void)ds;
    (void)dc;
}
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}
KThread* kthread_cur(void) { return NULL; }
void kthread_block(KThread* t) { (void)t; }
void kthread_unblock(KThread* t) { (void)t; }
void core_enter(KThread* t) { (void)t; }
void core_leave(KThread* t) { (void)t; }
u64 guest_ticks(void) { return 0; }
u64 plat_ns(void) { return 1; }
void plat_sleep_ns(i64 ns) { (void)ns; }
u64 present_display_period(u64 period, u64 refresh) {
    (void)refresh;
    return period;
}
void loader_segments(LoaderSegEach each, void* ctx) {
    (void)each;
    (void)ctx;
}
u32 nvn_api_pack(i32 major, i32 minor) { return (u32)major << 16 | (u32)minor; }
int nvn_driver_info(const u8* mem, u64 lo, u64 hi, u64 func, u32 item, i32* out) {
    (void)mem;
    (void)lo;
    (void)hi;
    (void)func;
    (void)item;
    (void)out;
    return 0;
}
/* the GPU's addresses are the guest's: a pool at its storage */
u64 gmmu_space_alloc(u64 size, u64 align) {
    (void)size;
    (void)align;
    return POOL_MEM;
}
void gmmu_space_map(u64 gva, u64 cpu, u64 size) {
    (void)gva;
    (void)cpu;
    (void)size;
}
void gmmu_space_unmap(u64 gva, u64 size) {
    (void)gva;
    (void)size;
}
void gmmu_space_free(u64 gva, u64 size) {
    (void)gva;
    (void)size;
}
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    *cpu = gva;
    return max;
}
int gmem_read(u64 gva, void* dst, u64 size) {
    memcpy(dst, GP(gva), size);
    return 1;
}
int gmem_write(u64 gva, const void* src, u64 size) {
    memcpy(GP(gva), src, size);
    return 1;
}
u64 ds_generation_next(void) {
    static u64 g;
    return ++g;
}
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns) {
    (void)id;
    (void)thresh;
    (void)timeout_ns;
    return 1;
}
/* nvmap: the handles made and dropped */
static u32 g_nvmap_next = 40, g_nvmap_live;
u32 nvmap_handle_new(u64 addr, u64 size) {
    (void)addr;
    (void)size;
    g_nvmap_live++;
    return g_nvmap_next++;
}
void nvmap_handle_drop(u32 h) {
    if (h) g_nvmap_live--;
}
/* the texel reads: the strides they were given */
static i64 g_read_row, g_read_image;
void nvn_read_texels(u64 t, u64 v, u64 region, u64 dst, i64 row, i64 image) {
    (void)t;
    (void)v;
    (void)region;
    (void)dst;
    g_read_row = row;
    g_read_image = image;
}
void nvn_write_texels(u64 t, u64 v, u64 region, u64 src, i64 row, i64 image) {
    (void)t;
    (void)v;
    (void)region;
    (void)src;
    (void)row;
    (void)image;
}
/* the queue's replay: none here */
u32 nvn_replay_queue(void) { return 0; }
u32 nvn_replay_syncpt(u32 queue) {
    (void)queue;
    return 0;
}
u32 nvn_replay_shown_syncpt(void) { return 0; }
void nvn_replay_finish(u32 queue) { (void)queue; }
void nvn_replay_queue_free(u32 queue) { (void)queue; }
void nvn_replay_fence(u32 queue, u64 sync, u64 ready, u32 shown) {
    (void)queue;
    (void)sync;
    (void)ready;
    (void)shown;
}
void nvn_replay_submit(u32 queue, u32 n, u64 handles, u64* ready, u32* shown) {
    (void)queue;
    (void)n;
    (void)handles;
    *ready = 0;
    *shown = 0;
}
u32 nvn_replay_present(u32 queue, const NvnReplayPresent* p, u32* shown) {
    (void)queue;
    (void)p;
    *shown = 0;
    return 0;
}
void nvn_sync_hold(u64* ready, u32* shown, const NvnSync* s) {
    (void)ready;
    (void)shown;
    (void)s;
}

/* ---- a renderer that keeps what it was given */
static DsState g_ds;
static DrawTexture g_tex;
static int g_test_draws, g_test_tex_draws;
static void r_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    (void)dc;
    (void)o;
    g_ds = *ds;
    g_test_draws++;
}
static void r_draw_texture(const DsState* ds, const DrawTexture* t) {
    (void)ds;
    g_tex = *t;
    g_test_tex_draws++;
}
static Renderer g_test_renderer;
const Renderer* g_renderer = &g_test_renderer;

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}
static f32 rdf(u64 a) {
    f32 v;
    memcpy(&v, GP(a), 4);
    return v;
}
static void wrf(u64 a, f32 v) { memcpy(GP(a), &v, 4); }

/* the replay of a recording: every record, in order, as the queue's replay hands them over (gpu/nvn/nvn_replay.c) */
static u32 replay(NvnDraw* d, u64 p, u32 want) {
    u32 found = 0;
    for (;;) {
        NvnRecHdr h;
        memcpy(&h, GP(p), sizeof h);
        if (h.op == OP_END) return found;
        if (h.op == OP_JUMP) {
            p = gr64(p + 8);
            continue;
        }
        if (h.op == want) found++;
        nvn_draw_record(d, h.op, p + 8, h.size - 8);
        p += h.size;
    }
}

static void pool_init(void) {
    nvn_MemoryPoolBuilderSetDefaults(NULL, POOL_B);
    nvn_MemoryPoolBuilderSetStorage(NULL, POOL_B, POOL_MEM, 0x100000);
    check(nvn_MemoryPoolInitialize(NULL, POOL, POOL_B), "the pool initializes");
}

/* ---- command buffers */
static void test_command_buffer(void) {
    static NvnDraw draw; /* a queue's, in its replay state in the runtime */
    NvnDraw* d = &draw;
    u64 h, ranges = SCRATCH, images = SCRATCH2;
    nvn_draw_init(d, 1);
    nvn_CommandBufferInitialize(NULL, CMDBUF, DEVICE);
    nvn_CommandBufferAddCommandMemory(NULL, CMDBUF, POOL, 0, 0x8000);
    nvn_CommandBufferAddControlMemory(NULL, CMDBUF, SCRATCH + 0x1000, 0x400);
    check(nvn_CommandBufferGetCommandMemorySize(NULL, CMDBUF) == 0x8000, "command memory size: what the game added");
    check(nvn_CommandBufferGetCommandMemoryFree(NULL, CMDBUF) == 0x8000 - 16,
          "command memory free: all but a JUMP's room");
    check(nvn_CommandBufferGetControlMemorySize(NULL, CMDBUF) == 0x400 &&
              nvn_CommandBufferGetControlMemoryFree(NULL, CMDBUF) == 0x400,
          "control memory: what the game added, none of it used");
    nvn_CommandBufferBeginRecording(NULL, CMDBUF);
    nvn_CommandBufferSetCopyRowStride(NULL, CMDBUF, 0x140);
    nvn_CommandBufferSetCopyImageStride(NULL, CMDBUF, 0x5000);
    check(nvn_CommandBufferGetCopyRowStride(NULL, CMDBUF) == 0x140 &&
              nvn_CommandBufferGetCopyImageStride(NULL, CMDBUF) == 0x5000,
          "the copy strides' getters");
    check(nvn_CommandBufferGetCommandMemoryFree(NULL, CMDBUF) < 0x8000 - 16, "records take command memory");
    /* the rasterizer's state: a line width, the first vertex provoking, conservative rasterization of 0.3 pixels
       (rounded up to 2 quarters), every sample shaded */
    nvn_CommandBufferSetLineWidth(NULL, CMDBUF, 3.0f);
    nvn_CommandBufferSetProvokingVertexMode(NULL, CMDBUF, 0);
    nvn_CommandBufferSetConservativeRasterEnable(NULL, CMDBUF, 1);
    nvn_CommandBufferSetConservativeRasterDilate(NULL, CMDBUF, 0.3f);
    check(nvn_ProgramInitialize(NULL, PROGRAM, DEVICE), "a program");
    nvn_ProgramSetSampleShading(NULL, PROGRAM, 1);
    nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM, NVN_SHADER_STAGE_FRAGMENT_BIT);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    check(nvn_ProgramSetShaders(NULL, PROGRAM, 0, 0), "SetShaders of no stages");
    nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM, NVN_SHADER_STAGE_FRAGMENT_BIT);
    /* deferred binds: the ranges and handles as they are when the GPU runs the command */
    gw64(ranges, 0x111000);
    gw64(ranges + 8, 0x100);
    gw64(ranges + 16, 0x222000);
    gw64(ranges + 24, 0x200);
    nvn_CommandBufferBindUniformBuffersDeferred(NULL, CMDBUF, NVN_SHADER_STAGE_VERTEX, 1, 2, ranges);
    gw64(images, 0x33);
    nvn_CommandBufferBindImagesDeferred(NULL, CMDBUF, NVN_SHADER_STAGE_FRAGMENT, 2, 1, images);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    /* a texture's rectangle: 100x50 pixels at (10, 20) from 50x25 texels */
    wrf(SCRATCH + 0x200, 10.0f), wrf(SCRATCH + 0x204, 20.0f), wrf(SCRATCH + 0x208, 110.0f), wrf(SCRATCH + 0x20c, 70.0f);
    wrf(SCRATCH + 0x210, 0.0f), wrf(SCRATCH + 0x214, 0.0f), wrf(SCRATCH + 0x218, 50.0f), wrf(SCRATCH + 0x21c, 25.0f);
    nvn_CommandBufferDrawTexture(NULL, CMDBUF, 7u | 5u << 20, SCRATCH + 0x200, SCRATCH + 0x210);
    /* an empty destination draws nothing */
    wrf(SCRATCH + 0x220, 5.0f), wrf(SCRATCH + 0x224, 5.0f), wrf(SCRATCH + 0x228, 5.0f), wrf(SCRATCH + 0x22c, 9.0f);
    nvn_CommandBufferDrawTexture(NULL, CMDBUF, 7u | 5u << 20, SCRATCH + 0x220, SCRATCH + 0x210);
    check(nvn_CommandBufferInsertCheckpointStatic(NULL, CMDBUF, 0) && nvn_CommandBufferInsertCheckpointDynamic(NULL, CMDBUF, 0),
          "checkpoints answer 1, as the release driver does");
    /* the hints with nothing to record */
    nvn_CommandBufferSetColorReductionEnable(NULL, CMDBUF, 1);
    nvn_CommandBufferSetColorReductionThresholds(NULL, CMDBUF, 0, 10, 20);
    nvn_CommandBufferResolveDepthBuffer(NULL, CMDBUF);
    nvn_CommandBufferSetZCullZF32CompressionEnable(NULL, CMDBUF, 1);
    nvn_CommandBufferOverrideSubtileSize(NULL, CMDBUF, 1, 16);
    nvn_CommandBufferSetSubpixelPrecisionBias(NULL, CMDBUF, 1, 1);
    h = nvn_CommandBufferEndRecording(NULL, CMDBUF);
    /* the deferred binds read what is there at the replay */
    gw64(ranges + 16, 0x444000);
    gw64(images, 0x55);
    g_test_renderer.draw = r_draw;
    g_test_renderer.draw_texture = r_draw_texture;
    check(replay(d, h, OP_DRAW) == 2, "the recording replays its two draws");
    check(g_test_draws == 2, "both draws reach the renderer");
    check(g_ds.dyn.line_width == 3.0f, "the line width reaches the draw state");
    check(g_ds.rkey.provoking_last == 0, "the first vertex provokes");
    check(g_ds.rkey.conservative == 3, "conservative rasterization with 2 quarter pixels of dilation");
    check(g_ds.rkey.sample_shading == 1, "the program's sample shading, as its SetShaders applied it");
    check(g_ds.bind.cb_addr[0][4] == 0x111000 && g_ds.bind.cb_addr[0][5] == 0x444000 &&
              g_ds.bind.cb_size[0][5] == 0x200,
          "deferred uniform buffers: the ranges as the GPU reads them");
    check(snx_ld_le64(g_ds.inl.bank2[4] + 0x120 + 16) == 0x55, "deferred images: the handles as the GPU reads them");
    check(g_test_tex_draws == 1, "one texture draw, the empty one skipped");
    check(g_tex.handle == (7u | 5u << 20) && g_tex.x0 == 10.0f && g_tex.y0 == 20.0f && g_tex.w == 100.0f &&
              g_tex.h == 50.0f && g_tex.du == 0.5 && g_tex.dv == 0.5 && g_tex.sx0 == 0.0f && g_tex.sy0 == 0.0f,
          "the texture draw's rectangle and steps");
}

/* ---- the device and its debug API */
static void test_device(void) {
    nvn_DeviceBuilderSetDefaults(NULL, DEVICE_B);
    nvn_DeviceBuilderSetFlags(NULL, DEVICE_B, 0x15);
    check(nvn_DeviceBuilderGetFlags(NULL, DEVICE_B) == 0x15, "the device builder's flags");
    check(nvn_DeviceInitialize(NULL, DEVICE, DEVICE_B), "the device");
    check(nvn_device() == DEVICE, "the runtime's device");
    check(nvn_DeviceGenerateDebugDomainId(NULL, DEVICE, 0) == 0 && nvn_DeviceGenerateDebugDomainId(NULL, DEVICE, 0) == 1,
          "debug domains count from 0");
    check(!nvn_DeviceIsExternalDebuggerAttached(NULL, DEVICE), "no external debugger");
    check(nvn_DeviceSetAutomaticCheckpointsEnable(NULL, DEVICE, 1) && nvn_DeviceSetCheckpointMaxCallStacksDepth(NULL, DEVICE, 4),
          "the checkpoint setters answer 1");
    check(nvn_DeviceRegisterFastClearColori(NULL, DEVICE, SCRATCH, NVN_FORMAT_RGBA8I) &&
              nvn_DeviceRegisterFastClearColorui(NULL, DEVICE, SCRATCH, NVN_FORMAT_RGBA8UI),
          "integer fast clear colors register");
}

/* ---- textures and their views */
static void tex_init(u64 t, i32 format, i32 w, i32 h, i32 flags) {
    nvn_TextureBuilderSetDefaults(NULL, TEX_B);
    nvn_TextureBuilderSetDevice(NULL, TEX_B, DEVICE);
    nvn_TextureBuilderSetFormat(NULL, TEX_B, format);
    nvn_TextureBuilderSetSize2D(NULL, TEX_B, w, h);
    nvn_TextureBuilderSetFlags(NULL, TEX_B, flags);
    if (flags & NVN_TEXTURE_FLAGS_LINEAR) nvn_TextureBuilderSetStride(NULL, TEX_B, 0x400);
    nvn_TextureBuilderSetStorage(NULL, TEX_B, POOL, 0x20000);
    check(nvn_TextureInitialize(NULL, t, TEX_B), "a texture");
}
static void test_texture(void) {
    u64 s = SCRATCH;
    unsigned th, td;
    NvnTexDesc desc;
    tex_init(TEX, NVN_FORMAT_RGBA8, 256, 128, 0);
    check(nvn_TextureBuilderGetMemoryOffset(NULL, TEX_B) == 0x20000, "the builder's storage offset");
    nvn_TextureBuilderSetGLTextureName(NULL, TEX_B, 77);
    check(nvn_TextureBuilderGetGLTextureName(NULL, TEX_B) == 77, "the builder's OpenGL name");
    memset(GP(s), 0, 8);
    gw32(s, 0x9a3); /* a packaged layout: its 12 bits are kept */
    nvn_TextureBuilderSetPackagedTextureLayout(NULL, TEX_B, s);
    gw32(s + 8, 0xffffffff);
    check(nvn_TextureBuilderGetPackagedTextureLayout(NULL, TEX_B, s + 8) && gr32(s + 8) == 0x9a3,
          "the packaged layout's getter");
    nvn_TextureBuilderSetPackagedTextureLayout(NULL, TEX_B, 0);
    check(!nvn_TextureBuilderGetPackagedTextureLayout(NULL, TEX_B, s + 8) && gr32(s + 8) == 0,
          "no packaged layout");
    /* the raw storage classes, by the driver's table */
    check(nvn_TextureGetRawStorageClass(NULL, TEX) == 2, "a color texture: color and ZF32");
    nvn_TextureBuilderSetFormat(NULL, TEX_B, NVN_FORMAT_DEPTH16);
    check(nvn_TextureBuilderGetRawStorageClass(NULL, TEX_B) == 3, "depth 16: S8 and Z16");
    nvn_TextureBuilderSetFormat(NULL, TEX_B, NVN_FORMAT_DEPTH24_STENCIL8);
    check(nvn_TextureBuilderGetRawStorageClass(NULL, TEX_B) == 4, "depth 24 with stencil: ZF24S8");
    nvn_TextureBuilderSetFormat(NULL, TEX_B, NVN_FORMAT_DEPTH32F_STENCIL8);
    check(nvn_TextureBuilderGetRawStorageClass(NULL, TEX_B) == 5, "depth 32F with stencil: ZF32_X24S8");
    nvn_TextureBuilderSetFormat(NULL, TEX_B, 0);
    check(nvn_TextureBuilderGetRawStorageClass(NULL, TEX_B) == 0, "no format: none");
    nvn_TextureBuilderSetFormat(NULL, TEX_B, NVN_FORMAT_RGBA8);
    nvn_TextureBuilderSetFlags(NULL, TEX_B, NVN_TEXTURE_FLAGS_LINEAR);
    check(nvn_TextureBuilderGetRawStorageClass(NULL, TEX_B) == 1, "a linear texture: LINEAR");
    /* sparse tiles: none for a texture that isn't sparse, a 64 KiB page of 4-byte blocks for one that is */
    gw64(s, ~0ull), gw64(s + 8, ~0ull);
    nvn_TextureGetSparseTileLayout(NULL, TEX, s);
    check(!gr64(s) && !gr64(s + 8), "a texture that isn't sparse has no tiles");
    nvn_TextureBuilderSetFlags(NULL, TEX_B, NVN_TEXTURE_FLAGS_SPARSE);
    nvn_TextureBuilderSetLevels(NULL, TEX_B, 3);
    nvn_TextureBuilderGetSparseTileLayout(NULL, TEX_B, s);
    check(gr32(s) == 1 && gr32(s + 4) == 128 && gr32(s + 8) == 128 && gr32(s + 12) == 1,
          "a sparse 256x128 RGBA8 texture: 128x128 tiles, one level before the tail");
    /* views */
    nvn_TextureViewSetDefaults(NULL, VIEW_A);
    nvn_TextureViewSetDefaults(NULL, VIEW_B);
    check(nvn_TextureViewCompare(NULL, VIEW_A, VIEW_B), "two default views are the same");
    nvn_TextureViewSetLevels(NULL, VIEW_A, 1, 2);
    check(!nvn_TextureViewCompare(NULL, VIEW_A, VIEW_B), "a view with levels set differs");
    nvn_TextureViewSetLevels(NULL, VIEW_B, 1, 2);
    check(nvn_TextureViewCompare(NULL, VIEW_A, VIEW_B), "the same levels set");
    nvn_TextureBuilderSetFlags(NULL, TEX_B, 0);
    nvn_TextureBuilderSetLevels(NULL, TEX_B, 3);
    check(nvn_TextureBuilderGetViewOffset(NULL, TEX_B, VIEW_A) ==
              (i64)nvn_tex_level_offset(&NVN_OBJ(NvnTexBuilder, TEX_B)->d, 1),
          "a builder's view offset: its first level's");
    /* the NvRm surface of a block-linear RGBA8 texture and of a linear one */
    nvn_TextureGetNvRmSurfaceNVX(NULL, TEX, 0, s);
    desc = NVN_OBJ(NvnTexture, TEX)->d;
    nvn_tex_level_tiles(&desc, 0, &th, &td);
    check(gr32(s) == 256 && gr32(s + 4) == 128 && gr64(s + 8) == 0x200532120ull, "the surface's size and format");
    check(gr32(s + 0x10) == 3 && gr32(s + 0x14) == 1024 && gr32(s + 0x20) == 0xfe && gr32(s + 0x24) == th,
          "block linear: its pitch, kind and block height");
    check(gr32(s + 0x18) == 40 && gr32(s + 0x1c) == 0x20000, "the pool's nvmap handle and the texture's offset");
    check(gr64(s + 0x38) == 1024ull * ((128 + (8u << th) - 1) & ~((8u << th) - 1)) && gr64(s + 0x30) >> 63,
          "the surface's size");
    nvn_TextureGetNvRmSurfaceNVX(NULL, TEX, 0, s + 0x100);
    check(gr32(s + 0x118) == 40 && g_nvmap_live == 1, "the pool's handle is made once");
    tex_init(TEX + 0x80, NVN_FORMAT_R8, 300, 20, NVN_TEXTURE_FLAGS_LINEAR);
    nvn_TextureGetNvRmSurfaceNVX(NULL, TEX + 0x80, 0, s);
    check(gr32(s + 0x10) == 1 && gr32(s + 0x14) == 0x400 && gr32(s + 0x20) == 0 && gr64(s + 8) == 0xf09200408ull &&
              gr64(s + 0x38) == 0x400ull * 20,
          "pitch linear: the texture's stride");
    nvn_MemoryPoolFinalize(NULL, POOL);
    check(g_nvmap_live == 0, "the pool's handle goes with it");
    nvn_TextureReadTexelsStrided(NULL, TEX, 0, SCRATCH, SCRATCH2, 0x300, 0x9000);
    check(g_read_row == 0x300 && g_read_image == 0x9000, "a strided read keeps its strides");
    check(nvn_TextureGetDebugID(NULL, TEX) == 0 && nvn_SamplerGetDebugID(NULL, SAMPLER) == 0 &&
              nvn_BufferGetDebugID(NULL, SAMPLER) == 0,
          "the release driver's debug IDs are 0");
    pool_init();
}

/* ---- samplers */
static void test_sampler(void) {
    u64 s = SCRATCH;
    u32 i, tsc_b[8], tsc_s[8];
    nvn_SamplerBuilderSetDefaults(NULL, SAMPLER_B);
    nvn_SamplerBuilderSetMinMagFilter(NULL, SAMPLER_B, NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_NEAREST);
    nvn_SamplerBuilderSetWrapMode(NULL, SAMPLER_B, NVN_WRAP_MODE_CLAMP, NVN_WRAP_MODE_REPEAT,
                                  NVN_WRAP_MODE_MIRRORED_REPEAT);
    nvn_SamplerBuilderSetLodClamp(NULL, SAMPLER_B, 1.0f, 4.0f);
    nvn_SamplerBuilderSetLodBias(NULL, SAMPLER_B, 0.5f);
    nvn_SamplerBuilderSetMaxAnisotropy(NULL, SAMPLER_B, 8.0f);
    nvn_SamplerBuilderSetCompare(NULL, SAMPLER_B, NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE, NVN_COMPARE_FUNC_GEQUAL);
    nvn_SamplerBuilderSetReductionFilter(NULL, SAMPLER_B, NVN_SAMPLER_REDUCTION_MAX);
    for (i = 0; i < 4; i++) gw32(s + 4 * i, 0xfffffff0u + i);
    nvn_SamplerBuilderSetBorderColori(NULL, SAMPLER_B, s);
    nvn_SamplerBuilderGetMinMagFilter(NULL, SAMPLER_B, s + 0x20, s + 0x24);
    nvn_SamplerBuilderGetWrapMode(NULL, SAMPLER_B, s + 0x28, s + 0x2c, s + 0x30);
    check(gr32(s + 0x20) == NVN_MIN_FILTER_LINEAR && gr32(s + 0x24) == NVN_MAG_FILTER_NEAREST &&
              gr32(s + 0x28) == NVN_WRAP_MODE_CLAMP && gr32(s + 0x2c) == NVN_WRAP_MODE_REPEAT &&
              gr32(s + 0x30) == NVN_WRAP_MODE_MIRRORED_REPEAT,
          "the builder's filters and wrap modes");
    nvn_SamplerBuilderGetLodClamp(NULL, SAMPLER_B, s + 0x34, s + 0x38);
    check(rdf(s + 0x34) == 1.0f && rdf(s + 0x38) == 4.0f && nvn_SamplerBuilderGetLodBias(NULL, SAMPLER_B) == 0.5f &&
              nvn_SamplerBuilderGetMaxAnisotropy(NULL, SAMPLER_B) == 8.0f,
          "the builder's LOD and anisotropy");
    nvn_SamplerBuilderGetCompare(NULL, SAMPLER_B, s + 0x3c, s + 0x40);
    check(gr32(s + 0x3c) == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE && gr32(s + 0x40) == NVN_COMPARE_FUNC_GEQUAL &&
              nvn_SamplerBuilderGetReductionFilter(NULL, SAMPLER_B) == NVN_SAMPLER_REDUCTION_MAX,
          "the builder's compare and reduction");
    nvn_SamplerBuilderGetBorderColorui(NULL, SAMPLER_B, s + 0x50);
    check(!memcmp(GP(s), GP(s + 0x50), 16), "an integer border color's words, as given");
    check(nvn_SamplerBuilderGetDevice(NULL, SAMPLER_B) == DEVICE, "the builder's device: the runtime's");
    check(nvn_SamplerInitialize(NULL, SAMPLER, SAMPLER_B), "a sampler");
    nvn_SamplerGetBorderColori(NULL, SAMPLER, s + 0x60);
    check(!memcmp(GP(s), GP(s + 0x60), 16), "the sampler's integer border color");
    /* a pool's descriptor from the builder: the sampler's */
    check(nvn_SamplerPoolInitialize(NULL, SAMPLER_P, POOL, 0x40000, 16), "a sampler pool");
    nvn_SamplerPoolRegisterSamplerBuilder(NULL, SAMPLER_P, 3, SAMPLER_B);
    nvn_SamplerPoolRegisterSampler(NULL, SAMPLER_P, 4, SAMPLER);
    memcpy(tsc_b, GP(POOL_MEM + 0x40000 + 32 * 3), 32);
    memcpy(tsc_s, GP(POOL_MEM + 0x40000 + 32 * 4), 32);
    check(!memcmp(tsc_b, tsc_s, 32), "RegisterSamplerBuilder writes the sampler's descriptor");
}

/* ---- events, programs, syncs */
static void test_objects(void) {
    u64 s = SCRATCH;
    nvn_EventBuilderSetDefaults(NULL, EVENT_B);
    nvn_EventBuilderSetStorage(NULL, EVENT_B, POOL, 0x60);
    check(nvn_EventBuilderGetMemoryPool(NULL, EVENT_B) == POOL && nvn_EventBuilderGetMemoryOffset(NULL, EVENT_B) == 0x60,
          "the event builder's storage");
    check(nvn_EventBuilderGetStorage(NULL, EVENT_B, s) == POOL && gr64(s) == 0x60, "GetStorage: the pool and offset");
    check(nvn_EventInitialize(NULL, EVENT, EVENT_B), "an event");
    check(nvn_EventGetMemoryPool(NULL, EVENT) == POOL && nvn_EventGetMemoryOffset(NULL, EVENT) == 0x60,
          "the event's storage");
    check(nvn_ProgramInitialize(NULL, PROGRAM, DEVICE), "a program");
    check(nvn_ProgramGetSubtileSize(NULL, PROGRAM) == 0, "no subtile size");
    check(!nvn_ProgramSetSubroutineLinkage(NULL, PROGRAM, 1, s), "no subroutine linkage");
    gw32(s, 7);
    check(!nvn_SyncInitializeFromFencedGLSync(NULL, SCRATCH2, DEVICE, 5, s) && gr32(s) == 1,
          "no OpenGL sync: status 1");
    check(nvn_SyncCreateGLSync(NULL, SCRATCH2, s) == 4, "no OpenGL sync to make");
}

/* ---- state objects */
static void test_state(void) {
    u64 s = STATE, xy = SCRATCH;
    nvn_BlendStateSetDefaults(NULL, s);
    check(nvn_BlendStateGetAdvancedMode(NULL, s) == 0 && nvn_BlendStateGetAdvancedOverlap(NULL, s) == 0 &&
              nvn_BlendStateGetAdvancedPremultipliedSrc(NULL, s) && nvn_BlendStateGetAdvancedNormalizedDst(NULL, s),
          "the advanced blend defaults: premultiplied, normalized, uncorrelated");
    nvn_BlendStateSetAdvancedMode(NULL, s, 0x2f);
    nvn_BlendStateSetAdvancedOverlap(NULL, s, NVN_BLEND_ADVANCED_OVERLAP_CONJOINT);
    nvn_BlendStateSetAdvancedPremultipliedSrc(NULL, s, 0);
    check(nvn_BlendStateGetAdvancedMode(NULL, s) == 0x2f &&
              nvn_BlendStateGetAdvancedOverlap(NULL, s) == NVN_BLEND_ADVANCED_OVERLAP_CONJOINT &&
              !nvn_BlendStateGetAdvancedPremultipliedSrc(NULL, s) && nvn_BlendStateGetAdvancedNormalizedDst(NULL, s),
          "the advanced blend setters");
    nvn_MultisampleStateSetDefaults(NULL, s);
    nvn_MultisampleStateGetSampleLocationsGrid(NULL, s, xy, xy + 4);
    check(gr32(xy) == 4 && gr32(xy + 4) == 4, "one sample: a 4x4 grid");
    nvn_MultisampleStateSetSamples(NULL, s, 4);
    nvn_MultisampleStateGetSampleLocationsGrid(NULL, s, xy, xy + 4);
    check(gr32(xy) == 2 && gr32(xy + 4) == 2, "four samples: a 2x2 grid");
    nvn_MultisampleStateSetRasterSamples(NULL, s, 8);
    nvn_MultisampleStateGetSampleLocationsGrid(NULL, s, xy, xy + 4);
    check(gr32(xy) == 1 && gr32(xy + 4) == 2, "eight raster samples: a 1x2 grid");
    wrf(xy, 0.5f), wrf(xy + 4, 1.0f), wrf(xy + 8, -1.0f), wrf(xy + 12, 0.96f);
    nvn_MultisampleStateSetSampleLocations(NULL, s, 1, 2, xy);
    check(NVN_OBJ(NvnMultisample, s)->location[1] == (8 | 15 << 4) && NVN_OBJ(NvnMultisample, s)->location[2] == (0 | 15 << 4),
          "sample locations in sixteenths, rounded and clamped");
    nvn_MultisampleStateSetSampleLocationsEnable(NULL, s, 1);
    nvn_MultisampleStateSetCoverageToColorEnable(NULL, s, 1);
    check(nvn_MultisampleStateGetSampleLocationsEnable(NULL, s) && nvn_MultisampleStateGetCoverageToColorEnable(NULL, s) &&
              !nvn_MultisampleStateGetAlphaToCoverageDither(NULL, s),
          "the multisample state's flags");
}

/* ---- the queue and the window */
static void test_queue_window(void) {
    u64 s = SCRATCH, sync = SCRATCH2;
    u32 i, got[7];
    nvn_QueueBuilderSetDevice(NULL, QUEUE_B, DEVICE);
    nvn_QueueBuilderSetDefaults(NULL, QUEUE_B);
    check(nvn_QueueBuilderGetDevice(NULL, QUEUE_B) == DEVICE, "SetDefaults keeps the queue builder's device");
    check(!nvn_QueueBuilderGetCommandMemorySize(NULL, QUEUE_B, s) && gr32(s) == 0, "a size not set: false and 0");
    nvn_QueueBuilderSetCommandMemorySize(NULL, QUEUE_B, 0x20000);
    nvn_QueueBuilderSetComputeMemorySize(NULL, QUEUE_B, 0x8000);
    nvn_QueueBuilderSetFlags(NULL, QUEUE_B, NVN_QUEUE_BUILDER_FLAGS_NO_ZCULL);
    nvn_QueueBuilderSetQueueMemory(NULL, QUEUE_B, 0x200000, 0x109000);
    check(nvn_QueueBuilderGetCommandMemorySize(NULL, QUEUE_B, s) && gr32(s) == 0x20000 &&
              nvn_QueueBuilderGetComputeMemorySize(NULL, QUEUE_B, s) && gr32(s) == 0x8000 &&
              !nvn_QueueBuilderGetControlMemorySize(NULL, QUEUE_B, s) &&
              !nvn_QueueBuilderGetCommandFlushThreshold(NULL, QUEUE_B, s),
          "the sizes set, and only those");
    check(nvn_QueueBuilderGetFlags(NULL, QUEUE_B) == NVN_QUEUE_BUILDER_FLAGS_NO_ZCULL &&
              nvn_QueueBuilderGetMemory(NULL, QUEUE_B) == 0x200000 &&
              nvn_QueueBuilderGetMemorySize(NULL, QUEUE_B) == 0x109000,
          "the queue builder's flags and memory");
    check(!nvn_QueueGetTotalCommandMemoryUsed(NULL, 0) && !nvn_QueueGetTotalControlMemoryUsed(NULL, 0) &&
              !nvn_QueueGetTotalComputeMemoryUsed(NULL, 0),
          "the replay uses none of the queue memory");
    /* a window of three textures, two of them active */
    for (i = 0; i < 3; i++) gw64(s + 8 * i, TEX + 0x80 * i);
    nvn_WindowBuilderSetDefaults(NULL, WINDOW_B);
    nvn_WindowBuilderSetDevice(NULL, WINDOW_B, DEVICE);
    nvn_WindowBuilderSetNativeWindow(NULL, WINDOW_B, 0x1234);
    nvn_WindowBuilderSetTextures(NULL, WINDOW_B, 3, s);
    nvn_WindowBuilderSetNumActiveTextures(NULL, WINDOW_B, 2);
    check(nvn_WindowBuilderGetNumActiveTextures(NULL, WINDOW_B) == 2 &&
              nvn_WindowBuilderGetNativeWindow(NULL, WINDOW_B) == 0x1234 &&
              nvn_WindowBuilderGetDevice(NULL, WINDOW_B) == DEVICE,
          "the window builder's getters");
    check(nvn_WindowInitialize(NULL, WINDOW, WINDOW_B), "a window");
    check(nvn_WindowGetNumTextures(NULL, WINDOW) == 3 && nvn_WindowGetNumActiveTextures(NULL, WINDOW) == 2 &&
              nvn_WindowGetNativeWindow(NULL, WINDOW) == 0x1234,
          "the window's getters");
    for (i = 0; i < 4; i++) {
        nvn_WindowAcquireTexture(NULL, WINDOW, sync, s + 0x40);
        got[i] = gr32(s + 0x40);
    }
    check(got[0] == 0 && got[1] == 1 && got[2] == 0 && got[3] == 1, "two active textures in turn");
    nvn_WindowSetNumActiveTextures(NULL, WINDOW, 3);
    for (i = 4; i < 7; i++) {
        nvn_WindowAcquireTexture(NULL, WINDOW, sync, s + 0x40);
        got[i] = gr32(s + 0x40);
    }
    check(nvn_WindowGetNumActiveTextures(NULL, WINDOW) == 3 && got[4] == 0 && got[5] == 1 && got[6] == 2,
          "all three in turn from then on");
}

int main(void) {
    nvn_init();
    test_device();
    pool_init();
    test_command_buffer();
    test_texture();
    test_sampler();
    test_objects();
    test_state();
    test_queue_window();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("nvn_names: all checks pass\n");
    return 0;
}
