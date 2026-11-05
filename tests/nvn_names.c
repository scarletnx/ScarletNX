/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NVN functions of API 53 and 55 that nvn took over last, family by family, against what the games'
   drivers do (their code, NVN 53.314 and 55.15): command buffers (gpu/nvn/nvn_cmd.c) record and the queue's replay
   (nvn_draw.c) turns the records into the renderer's draw state, which a renderer of the test's takes; the device,
   textures, samplers, programs, events, syncs and state objects (nvn_object.c); and the queue and window
   (nvn_queue.c) */
#include "nx/runtime/arm.h"
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
/* all of the guest memory is mapped */
u64 mem_mapped_run(u64 addr, u64 size) {
    (void)addr;
    return size;
}
/* a 64-bit game's: no 32-bit thread runs the game's callbacks */
/* no GPU thread: each submission is read at once */
u64 nvn_replay_submitted(void) { return 0; }
u64 nvn_replay_walked(void) { return 0; }
SnxArmCPU* arm_cpu(CPU* c) {
    (void)c;
    return NULL;
}
void arm_run(SnxArmCPU* c, SnxArmBlock first, u32 stop_at) {
    (void)c;
    (void)first;
    (void)stop_at;
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
u64 nvn_loader_run_driver(CPU* c, GuestFn fn, u64 at, u64 a0, u64 a1, u64 a2) {
    (void)c;
    (void)fn;
    (void)at;
    (void)a0;
    (void)a1;
    (void)a2;
    return 0;
}
int nvn_driver_info(const u8* mem, u64 lo, u64 hi, u64 func, u32 item, i32* out) {
    (void)mem;
    (void)lo;
    (void)hi;
    (void)func;
    (void)item;
    (void)out;
    return 0;
}
int nvn_driver_info32(const u8* mem, u64 lo, u64 hi, u64 func, u32 item, i32* out) {
    return nvn_driver_info(mem, lo, hi, func, item, out);
}
int arm_code_range(u64* lo, u64* hi) {
    *lo = *hi = 0;
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
static DrawCall g_call;
static DrawTexture g_tex;
static int g_test_draws, g_test_tex_draws;
static void r_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    g_call = *dc;
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
        u32 op;
        memcpy(&h, GP(p), sizeof h);
        op = NVN_REC_OP(h.op);
        if (op == OP_END) return found;
        if (op == OP_JUMP) {
            p = gr64(p + 8);
            continue;
        }
        if (op == want) found++;
        nvn_draw_record(d, op, p + 8, h.size - 8);
        if (h.op & NVN_REC_LAST) return found;
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
    check(nvn_CommandBufferGetCommandMemoryFree(NULL, CMDBUF) == 0x8000,
          "command memory free: all of it, as the game's driver answers (its end less its write pointer)");
    check(nvn_CommandBufferGetControlMemorySize(NULL, CMDBUF) == 0x400 &&
              nvn_CommandBufferGetControlMemoryFree(NULL, CMDBUF) == 0x400,
          "control memory: what the game added, none of it used");
    nvn_CommandBufferBeginRecording(NULL, CMDBUF);
    nvn_CommandBufferSetCopyRowStride(NULL, CMDBUF, 0x140);
    nvn_CommandBufferSetCopyImageStride(NULL, CMDBUF, 0x5000);
    check(nvn_CommandBufferGetCopyRowStride(NULL, CMDBUF) == 0x140 &&
              nvn_CommandBufferGetCopyImageStride(NULL, CMDBUF) == 0x5000,
          "the copy strides' getters");
    check(nvn_CommandBufferGetCommandMemoryFree(NULL, CMDBUF) < 0x8000, "records take command memory");
    /* the rasterizer's state: a line width, the first vertex provoking, conservative rasterization of 0.3 pixels
       (rounded up to 2 quarters), every sample shaded */
    nvn_CommandBufferSetLineWidth(NULL, CMDBUF, 3.0f);
    nvn_CommandBufferSetProvokingVertexMode(NULL, CMDBUF, 0);
    nvn_CommandBufferSetPatchSize(NULL, CMDBUF, 3);
    nvn_CommandBufferSetAlphaRef(NULL, CMDBUF, 0.375f);
    nvn_CommandBufferSetPointSize(NULL, CMDBUF, 3.0f);
    nvn_CommandBufferSetDepthBounds(NULL, CMDBUF, 1, 0.125f, 0.875f);
    memcpy(GP(SCRATCH + 0x380), (const f32[2]){2, 3}, 8);
    memcpy(GP(SCRATCH + 0x3a0), (const f32[4]){4, 5, 6, 7}, 16);
    nvn_CommandBufferSetInnerTessellationLevels(NULL, CMDBUF, SCRATCH + 0x380);
    nvn_CommandBufferSetOuterTessellationLevels(NULL, CMDBUF, SCRATCH + 0x3a0);
    memset(GP(SCRATCH + 0x380), 0, 8);
    memset(GP(SCRATCH + 0x3a0), 0, 16);
    for (u32 i = 0; i < 16; i++) wrf(SCRATCH + 0x400 + 4 * i, (f32)i / 16);
    nvn_CommandBufferBindCoverageModulationTable(NULL, CMDBUF, SCRATCH + 0x400);
    memset(GP(SCRATCH + 0x400), 0, 64);
    const i32 scissors[2][4] = {{1, 2, 10, 20}, {4, 5, 6, 7}};
    const f32 depths[2][2] = {{0.25f, 0.75f}, {0.9f, 0.1f}};
    memcpy(GP(SCRATCH + 0x300), scissors, sizeof scissors);
    memcpy(GP(SCRATCH + 0x340), depths, sizeof depths);
    u64 memory_before = nvn_CommandBufferGetCommandMemoryFree(NULL, CMDBUF);
    nvn_CommandBufferSetScissors(NULL, CMDBUF, 3, 2, SCRATCH + 0x300);
    nvn_CommandBufferSetDepthRanges(NULL, CMDBUF, 3, 2, SCRATCH + 0x340);
    check(memory_before - nvn_CommandBufferGetCommandMemoryFree(NULL, CMDBUF) == 80,
          "indexed scissor and depth range setters charge all selected viewports");
    nvn_CommandBufferSetScissors(NULL, CMDBUF, 16, 1, SCRATCH + 0x300);
    nvn_CommandBufferSetDepthRanges(NULL, CMDBUF, 0, 0, SCRATCH + 0x340);
    check(memory_before - nvn_CommandBufferGetCommandMemoryFree(NULL, CMDBUF) == 80,
          "invalid or empty indexed ranges record nothing");
    memset(GP(SCRATCH + 0x300), 0, sizeof scissors);
    memset(GP(SCRATCH + 0x340), 0, sizeof depths);
    nvn_CommandBufferSetConservativeRasterEnable(NULL, CMDBUF, 1);
    nvn_CommandBufferSetConservativeRasterDilate(NULL, CMDBUF, 0.3f);
    check(nvn_ProgramInitialize(NULL, PROGRAM, DEVICE), "a program");
    nvn_ProgramSetSampleShading(NULL, PROGRAM, 1);
    nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM, NVN_SHADER_STAGE_FRAGMENT_BIT);
    nvn_CommandBufferBindUniformBuffer(NULL, CMDBUF, NVN_SHADER_STAGE_VERTEX, 6, 0x555000, 0x100);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    check(nvn_ProgramSetShaders(NULL, PROGRAM, 0, 0), "SetShaders of no stages");
    nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM, NVN_SHADER_STAGE_FRAGMENT_BIT);
    /* deferred binds: the ranges and handles as they are when the GPU runs the command */
    gw64(ranges, 0x111000);
    gw64(ranges + 8, 0x100);
    gw64(ranges + 16, 0x222000);
    gw64(ranges + 24, 0x200);
    nvn_CommandBufferBindUniformBuffersDeferred(NULL, CMDBUF, NVN_SHADER_STAGE_VERTEX, 1, 2, ranges);
    /* the driver's sizes (its BindUniformBuffer, NVN 55.15): CB_SIZE rounded up to 256 bytes, and a size of 0
       unbinds the slot */
    nvn_CommandBufferBindUniformBuffer(NULL, CMDBUF, NVN_SHADER_STAGE_VERTEX, 0, 0x333000, 0xA0);
    nvn_CommandBufferBindUniformBuffer(NULL, CMDBUF, NVN_SHADER_STAGE_VERTEX, 6, 0x555000, 0);
    gw64(images, 0x33);
    nvn_CommandBufferBindImagesDeferred(NULL, CMDBUF, NVN_SHADER_STAGE_FRAGMENT, 2, 1, images);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    nvn_CommandBufferDrawTransformFeedback(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_TRIANGLES, 0x650000);
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
    check(g_test_draws == 3, "two direct draws and one transform-feedback draw reach the renderer");
    check(g_ds.dyn.line_width == 3.0f, "the line width reaches the draw state");
    check(g_ds.rkey.provoking_last == 0, "the first vertex provokes");
    check(g_ds.rkey.conservative == 3, "conservative rasterization with 2 quarter pixels of dilation");
    check(g_ds.rkey.sample_shading == 1, "the program's sample shading, as its SetShaders applied it");
    check(g_ds.rkey.patch_vertices == 3, "the recorded patch size reaches the renderer key");
    check(g_ds.dyn.alpha_ref == 0.375f &&
              g_call.state_flags == (DS_DRAW_DEPTH_BOUNDS | DS_DRAW_POINT_SIZE | DS_DRAW_TESS_LEVELS) &&
              g_call.point_size == 3.0f && g_call.depth_bounds[0] == 0.125f && g_call.depth_bounds[1] == 0.875f &&
              !memcmp(g_call.tess_levels, (const f32[6]){4, 5, 6, 7, 2, 3}, sizeof g_call.tess_levels),
          "fixed-function command records snapshot their values and reach the renderer");
    check(g_call.xfb_control == 0x650000 && g_call.topology == NVN_DRAW_PRIMITIVE_TRIANGLES,
          "transform-feedback draw records retain their GPU control address and topology");
    for (u32 i = 0; i < 16; i++)
        check(d->coverage_table_enable && d->coverage_table[i] == i,
              "coverage modulation table records snapshot the caller's immediate coefficients");
    check(g_ds.vp.scissor_x[3] == (1u | 11u << 16) && g_ds.vp.scissor_y[4] == (5u | 12u << 16) &&
              g_ds.vp.z_near[3] == 0.25f && g_ds.vp.z_far[4] == 0.1f && g_ds.vp.z_far[0] == 1.0f,
          "indexed command records snapshot their inputs and preserve other viewports");
    check(g_ds.bind.cb_addr[0][4] == 0x111000 && g_ds.bind.cb_addr[0][5] == 0x444000 &&
              g_ds.bind.cb_size[0][5] == 0x200,
          "deferred uniform buffers: the ranges as the GPU reads them");
    check(g_ds.bind.cb_addr[0][3] == 0x333000 && g_ds.bind.cb_size[0][3] == 0x100,
          "a uniform buffer of 0xA0 bytes binds 0x100, as the driver's CB_SIZE");
    check(g_ds.bind.cb_addr[0][3 + 6] == 0,
          "a uniform buffer of size 0 unbinds its slot");
    check(snx_ld_le64(g_ds.inl.bank2[4] + 0x120 + 16) == 0x55, "deferred images: the handles as the GPU reads them");
    check(g_test_tex_draws == 1, "one texture draw, the empty one skipped");
    check(g_tex.handle == (7u | 5u << 20) && g_tex.x0 == 10.0f && g_tex.y0 == 20.0f && g_tex.w == 100.0f &&
              g_tex.h == 50.0f && g_tex.du == 0.5 && g_tex.dv == 0.5 && g_tex.sx0 == 0.0f && g_tex.sy0 == 0.0f,
          "the texture draw's rectangle and steps");
}

/* The real SetShaders -> immutable serial metadata -> BindProgram recording -> replay path. Control memory is
   reused before replay, so the compiler declarations must have been read during SetShaders. */
static void tess_control(u64 control, u32 stage, u32 format, const u32 values[4], u32 declared) {
    memset(GP(control), 0, 0x900);
    gw32(control, 0x98761234);
    gw32(control + 4, 1);
    gw32(control + 8, format);
    gw32(control + 0x700, 0x200);
    gw32(control + 0x704, 0x400);
    gw32(control + 0x708, 0x30);
    gw32(control + 0x714, stage);
    u32 base = stage == NVN_SHADER_STAGE_TESS_EVALUATION ? 0x738 : 0x848;
    u32 flags = stage == NVN_SHADER_STAGE_TESS_EVALUATION ? 0x868 : 0x858;
    for (u32 i = 0; i < 4; i++) {
        gw32(control + base + 4 * i, values[i]);
        gw32(control + flags + 4 * i, (declared >> i) & 1);
    }
}
static void test_tessellation_metadata(void) {
    static NvnDraw draw;
    nvn_draw_init(&draw, 0x80000001u);
    const u64 control = 0xa000, evaluation = 0xb000, pairs = 0xc000;
    const struct {
        u32 format, control[4], evaluation[4], control_declared, evaluation_declared, count, expected;
    } cases[] = {{8, {0}, {7, 0x8e7b, 0, 0x901}, 0, 0, 2, 0x212},
                 {8, {0}, {4, 0x8e7c, 1, 0x900}, 0, 0, 2, 0x021},
                 {16, {7, 0x8e7c, 1, 0x900}, {4, 0x8e7b, 0, 0x901}, 15, 15, 2, 0x211},
                 {16, {7, 0x202, 1, 0x900}, {4, 0x8e7b, 0, 0x901}, 15, 10, 2, 0x012},
                 {16, {0x8e7a, 0x202, 0, 0x900}, {4, 0x8e7b, 0, 0x901}, 15, 0, 2, 0x100},
                 {16, {4, 0x202, 0, 0x900}, {4, 0x202, 0, 0x900}, 0, 0, 2, 0x201},
                 {16, {0}, {4, 0x202, 0, 0x900}, 0, 0, 1, 0x201}};
    nvn_CommandBufferInitialize(NULL, CMDBUF, DEVICE);
    nvn_CommandBufferAddCommandMemory(NULL, CMDBUF, POOL, 0x9000, 0x8000);
    nvn_CommandBufferAddControlMemory(NULL, CMDBUF, SCRATCH + 0x1000, 0x400);
    check(nvn_ProgramInitialize(NULL, PROGRAM, DEVICE), "a tessellation program");
    for (u32 i = 0; i < sizeof cases / sizeof *cases; i++) {
        tess_control(control, NVN_SHADER_STAGE_TESS_CONTROL, cases[i].format, cases[i].control,
                     cases[i].control_declared);
        tess_control(evaluation, NVN_SHADER_STAGE_TESS_EVALUATION, cases[i].format, cases[i].evaluation,
                     cases[i].evaluation_declared);
        gw64(pairs, 0x900000);
        gw64(pairs + 8, control);
        gw64(pairs + 16, 0xa00000);
        gw64(pairs + 24, evaluation);
        check(nvn_ProgramSetShaders(NULL, PROGRAM, (i32)cases[i].count, pairs + (cases[i].count == 1 ? 16 : 0)),
              "tessellation compiler control sections decode");
        nvn_CommandBufferBeginRecording(NULL, CMDBUF);
        nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM,
                                     NVN_SHADER_STAGE_TESS_CONTROL_BIT | NVN_SHADER_STAGE_TESS_EVALUATION_BIT);
        nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_PATCHES, 0, 3);
        u64 handle = nvn_CommandBufferEndRecording(NULL, CMDBUF);
        memset(GP(control), 0, 0x900);
        memset(GP(evaluation), 0, 0x900);
        check(replay(&draw, handle, OP_DRAW) == 1 && g_ds.prog.tess_params == cases[i].expected,
              "decoded domain, spacing, point and winding declarations survive recording and control reuse");
    }
    /* A control-only rebind retains declarations of the previously bound evaluation program. */
    const u32 eval_values[4] = {7, 0x8e7b, 0, 0x901}, control_values[4] = {4, 0x8e7c, 1, 0x900};
    tess_control(evaluation, NVN_SHADER_STAGE_TESS_EVALUATION, 16, eval_values, 15);
    gw64(pairs, 0xa00000);
    gw64(pairs + 8, evaluation);
    check(nvn_ProgramSetShaders(NULL, PROGRAM, 1, pairs), "an evaluation-only program");
    nvn_CommandBufferBeginRecording(NULL, CMDBUF);
    nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM, NVN_SHADER_STAGE_TESS_EVALUATION_BIT);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_PATCHES, 0, 3);
    check(replay(&draw, nvn_CommandBufferEndRecording(NULL, CMDBUF), OP_DRAW) == 1 && g_ds.prog.tess_params == 0x212,
          "evaluation declarations take effect without a control stage");
    tess_control(control, NVN_SHADER_STAGE_TESS_CONTROL, 16, control_values, 15);
    gw64(pairs, 0x900000);
    gw64(pairs + 8, control);
    check(nvn_ProgramSetShaders(NULL, PROGRAM, 1, pairs), "a control-only separable program");
    nvn_CommandBufferBeginRecording(NULL, CMDBUF);
    nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM, NVN_SHADER_STAGE_TESS_CONTROL_BIT);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_PATCHES, 0, 3);
    check(replay(&draw, nvn_CommandBufferEndRecording(NULL, CMDBUF), OP_DRAW) == 1 && g_ds.prog.tess_params == 0x212,
          "a separable control rebind preserves the evaluation stage's independently declared fields");
    nvn_CommandBufferBeginRecording(NULL, CMDBUF);
    nvn_CommandBufferBindProgram(NULL, CMDBUF, 0, NVN_SHADER_STAGE_TESS_EVALUATION_BIT);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_PATCHES, 0, 3);
    u64 handle = nvn_CommandBufferEndRecording(NULL, CMDBUF);
    check(replay(&draw, handle, OP_DRAW) == 1 && g_ds.prog.tess_params == 0x021,
          "unbinding evaluation restores independently declared control-stage state");

    /* Keep one recorded serial across more updates than the old fixed metadata table could hold. SetShaders reuses
       the same program and compiler-control storage, but the earlier recording still owns its declarations. */
    tess_control(evaluation, NVN_SHADER_STAGE_TESS_EVALUATION, 16, eval_values, 15);
    GP(evaluation)[0x6ec] = 1;
    gw16(evaluation + 0x6dc, 16);
    GP(evaluation)[0x6e8] = 1;
    GP(evaluation)[0xdc] = 0x20;
    GP(evaluation)[0x4dc] = 4;
    gw64(pairs, 0xa00000);
    gw64(pairs + 8, evaluation);
    check(nvn_ProgramSetShaders(NULL, PROGRAM, 1, pairs), "an evaluation program before metadata growth");
    nvn_CommandBufferBeginRecording(NULL, CMDBUF);
    nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM,
                                 NVN_SHADER_STAGE_TESS_CONTROL_BIT | NVN_SHADER_STAGE_TESS_EVALUATION_BIT);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_PATCHES, 0, 3);
    u64 saved_handle = nvn_CommandBufferEndRecording(NULL, CMDBUF);
    tess_control(evaluation, NVN_SHADER_STAGE_TESS_EVALUATION, 16, control_values, 15);
    GP(evaluation)[0x6ec] = 1;
    gw16(evaluation + 0x6dc, 16);
    GP(evaluation)[0x6e8] = 1;
    GP(evaluation)[0xdc] = 0x21;
    GP(evaluation)[0x4dc] = 4;
    for (u32 i = 0; i < 1153; i++)
        check(nvn_ProgramSetShaders(NULL, PROGRAM, 1, pairs), "a program update past the former metadata limit");
    check(g_nvn.object.ntess > 1024 && g_nvn.object.ntess <= g_nvn.object.tess_capacity,
          "tessellation metadata grows beyond1024 program updates");
    check(g_nvn.object.nxfb > 1024 && g_nvn.object.nxfb <= g_nvn.object.xfb_capacity,
          "transform feedback metadata grows beyond1024 program updates");
    for (u32 i = 0; i < g_nvn.object.ntess; i++) {
        NvnTessStage found[2] = {{0}};
        const NvnTessEntry* entry = &g_nvn.object.tess[i];
        check((!i || g_nvn.object.tess[i - 1].serial < entry->serial) && nvn_program_tess(entry->serial, found) &&
                  !memcmp(found, entry->stage, sizeof found),
              "every sorted tessellation serial resolves through the binary lookup");
    }
    NvnTessStage missing[2] = {{0}};
    check(!nvn_program_tess(0, missing) && !nvn_program_tess(~0ull, missing),
          "binary lookup rejects serials outside the tessellation table");
    for (u32 i = 0; i < g_nvn.object.nxfb; i++) {
        ShXfb found = {0};
        const NvnXfbEntry* entry = &g_nvn.object.xfb[i];
        check((!i || g_nvn.object.xfb[i - 1].serial < entry->serial) && nvn_program_xfb(entry->serial, &found) &&
                  !memcmp(&found, &entry->x, sizeof found),
              "every sorted transform feedback serial resolves through the binary lookup");
    }
    ShXfb missing_xfb = {0};
    check(!nvn_program_xfb(0, &missing_xfb) && !nvn_program_xfb(~0ull, &missing_xfb),
          "binary lookup rejects serials outside the transform feedback table");
    memset(GP(evaluation), 0, 0x900);
    check(replay(&draw, saved_handle, OP_DRAW) == 1 && g_ds.prog.tess_params == 0x212,
          "a recorded tessellation serial retains its declarations across metadata growth and control reuse");
    check(g_ds.xfb.count[0] == 4 && g_ds.xfb.stride[0] == 16 &&
              !memcmp(g_ds.xfb.layout[0], (const u8[4]){32, 33, 34, 35}, 4),
          "a recorded transform feedback layout survives metadata growth and compiler-control reuse");
    nvn_CommandBufferBeginRecording(NULL, CMDBUF);
    nvn_CommandBufferBindProgram(NULL, CMDBUF, PROGRAM,
                                 NVN_SHADER_STAGE_TESS_CONTROL_BIT | NVN_SHADER_STAGE_TESS_EVALUATION_BIT);
    nvn_CommandBufferDrawArrays(NULL, CMDBUF, NVN_DRAW_PRIMITIVE_PATCHES, 0, 3);
    check(replay(&draw, nvn_CommandBufferEndRecording(NULL, CMDBUF), OP_DRAW) == 1 && g_ds.prog.tess_params == 0x021,
          "the newest tessellation declarations remain available after metadata growth");
    check(g_ds.xfb.count[0] == 4 && g_ds.xfb.stride[0] == 16 &&
              !memcmp(g_ds.xfb.layout[0], (const u8[4]){36, 37, 38, 39}, 4),
          "the newest transform feedback layout remains available after metadata growth");
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
    /* the color formats of the 53.312 driver, whose NvRm counts color spaces from 1, of the 55.12 one, which counts
       from 2 as 53.314's NvRm does, and of the 55.15 one, which doubles bits 16 to 31 */
    static const struct {
        u32 format, api;
        u64 color;
        const char* what;
    } colors[] = {
        {NVN_FORMAT_RGBA8_SRGB, 53u << 16 | 312, 0x100532120ull, "53.312 RGBA8_SRGB"},
        {NVN_FORMAT_R8, 53u << 16 | 312, 0xe09200408ull, "53.312 R8"},
        {NVN_FORMAT_BGRA8, 53u << 16 | 105, 0x100d12120ull, "53.105 BGRA8, an older driver"},
        {NVN_FORMAT_RGBA8_SRGB, 53u << 16 | 314, 0x200532120ull, "53.314 RGBA8_SRGB, counted from 2"},
        {NVN_FORMAT_RGBA8_SRGB, 55u << 16 | 12, 0x200532120ull, "55.12 RGBA8_SRGB"},
        {NVN_FORMAT_RGBA8_SRGB, 55u << 16 | 15, 0x200a62120ull, "55.15 RGBA8_SRGB"},
        {NVN_FORMAT_RGBX8_SRGB, 55u << 16 | 12, 0x20a532120ull, "55.12 RGBX8_SRGB, no alpha"},
        {NVN_FORMAT_R8, 55u << 16 | 15, 0xf12400408ull, "55.15 R8"},
        {NVN_FORMAT_BGRA8, 55u << 16 | 15, 0x201a22120ull, "55.15 BGRA8"},
        {NVN_FORMAT_DEPTH32F_STENCIL8, 55u << 16 | 12, 0, "a depth format, which no driver describes"},
    };
    for (size_t i = 0; i < sizeof colors / sizeof *colors; i++) {
        g_nvn.loader.api = colors[i].api;
        tex_init(TEX + 0x80, colors[i].format, 64, 64, 0);
        nvn_TextureGetNvRmSurfaceNVX(NULL, TEX + 0x80, 0, s);
        if (gr64(s + 8) != colors[i].color)
            fprintf(stderr, "%s: color format %llx, expected %llx\n", colors[i].what, (unsigned long long)gr64(s + 8),
                    (unsigned long long)colors[i].color);
        check(gr64(s + 8) == colors[i].color, colors[i].what);
    }
    /* a STENCIL8 texture's ZCull save area: none without its zcull flag, but on the drivers before 53.209 */
    nvn_TextureBuilderSetDefaults(NULL, TEX_B);
    nvn_TextureBuilderSetFormat(NULL, TEX_B, NVN_FORMAT_STENCIL8);
    nvn_TextureBuilderSetSize2D(NULL, TEX_B, 100, 60);
    g_nvn.loader.api = 53u << 16 | 105;
    check(nvn_TextureBuilderGetZCullStorageSize(NULL, TEX_B) == 0x1e0, "53.105 STENCIL8: a ZCull area as for depth");
    g_nvn.loader.api = 53u << 16 | 209;
    check(nvn_TextureBuilderGetZCullStorageSize(NULL, TEX_B) == 0, "53.209 STENCIL8: no ZCull area");
    nvn_TextureBuilderSetFlags(NULL, TEX_B, NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL);
    check(nvn_TextureBuilderGetZCullStorageSize(NULL, TEX_B) == 0x1e0, "53.209 STENCIL8 with its zcull flag");
    g_nvn.loader.api = 0;
    nvn_MemoryPoolFinalize(NULL, POOL);
    check(g_nvmap_live == 0, "the pool's handle goes with it");
    nvn_TextureReadTexelsStrided(NULL, TEX, 0, SCRATCH, SCRATCH2, 0x300, 0x9000);
    check(g_read_row == 0x300 && g_read_image == 0x9000, "a strided read keeps its strides");
    check(nvn_TextureGetDebugID(NULL, TEX) == 0 && nvn_SamplerGetDebugID(NULL, SAMPLER) == 0 &&
              nvn_BufferGetDebugID(NULL, SAMPLER) == 0,
          "the release driver's debug IDs are 0");
    pool_init();
}

/* 1D array render surfaces keep layer count separate from physical row count, including through a mip/layer view. */
static void test_array_surface(void) {
    NvnTexture t = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = 0x900000};
    t.d.target = NVN_TEXTURE_TARGET_1D_ARRAY;
    t.d.format = NVN_FORMAT_RGBA8;
    t.d.width = 64;
    t.d.height = 7;
    t.d.depth = 1;
    t.d.levels = 3;
    memcpy(GP(SCRATCH2), &t, NVN_TEX_SNAP);
    NvnTexLayout layout;
    nvn_tex_layout(&t.d, &layout);
    DsSurf surface;
    int depth;
    u32 samples;
    check(nvn_draw_surface(SCRATCH2, NULL, &surface, &depth, &samples) && surface.height == 1 && surface.layers == 7 &&
              surface.layer_stride == layout.layer_size && surface.gva == t.gpu,
          "a 1D array render target has one row and all seven layers");
    NvnView view = {.set = NVN_VIEW_LEVELS | NVN_VIEW_LAYERS, .base_level = 1, .min_layer = 2, .layers = 3};
    check(nvn_draw_surface(SCRATCH2, &view, &surface, &depth, &samples) && surface.height == 1 && surface.layers == 3 &&
              surface.gva == t.gpu + nvn_tex_level_offset(&t.d, 1) + 2 * layout.layer_size,
          "a 1D array render view applies its mip and layer offsets without turning layers into rows");
}

/* ---- samplers */
static void test_sampler(void) {
    u64 s = SCRATCH;
    u32 i, tsc_b[8], tsc_s[8];
    nvn_SamplerBuilderSetDefaults(NULL, SAMPLER_B);
    nvn_SamplerBuilderGetMinMagFilter(NULL, SAMPLER_B, s + 0x20, s + 0x24);
    nvn_SamplerBuilderGetLodClamp(NULL, SAMPLER_B, s + 0x34, s + 0x38);
    check(gr32(s + 0x20) == NVN_MIN_FILTER_LINEAR && gr32(s + 0x24) == NVN_MAG_FILTER_LINEAR && rdf(s + 0x34) == 0.0f &&
              rdf(s + 0x38) == 1000.0f,
          "NVN sampler defaults use linear filtering without mipmaps and a nonnegative minimum LOD");
    u32 defaults[8];
    nvn_tsc_encode(&NVN_OBJ(NvnSamplerBuilder, SAMPLER_B)->p, defaults);
    check((defaults[1] & 0xff) == (2u | 2u << 4 | 1u << 6) && (defaults[2] & 0xfff) == 0 &&
              (defaults[2] >> 12 & 0xfff) == 15u * 256,
          "default sampler filters and LOD clamps reach the hardware descriptor");
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
    memset(GP(POOL_MEM + 0x40000), 0, 2 * 32);
    check(nvn_SamplerPoolInitialize(NULL, SAMPLER_P, POOL, 0x40000, 16), "a sampler pool");
    /* the driver's samplers 0 and 1, which the pool starts with: wrap 2 (clamp to edge) on u, v and p, linear
       magnification and minification with no mip filter, LODs 0 to 15 (1000 clamped), anisotropy 1 then 2 */
    for (i = 0; i < 2; i++) {
        u32 w0 = gr32(POOL_MEM + 0x40000 + 32 * i), w1 = gr32(POOL_MEM + 0x40000 + 32 * i + 4),
            w2 = gr32(POOL_MEM + 0x40000 + 32 * i + 8);
        check((w0 & 0x1ff) == (2u | 2u << 3 | 2u << 6) && !(w0 >> 9 & 1) && (w0 >> 20 & 7) == i,
              "a reserved sampler's wrap, compare and anisotropy");
        check((w1 & 0xff) == (2u | 2u << 4 | 1u << 6) && !(w1 >> 10 & 3), "a reserved sampler's filters");
        check((w2 & 0xfff) == 0 && (w2 >> 12 & 0xfff) == 15u * 256, "a reserved sampler's LOD range");
    }
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
    test_tessellation_metadata();
    test_texture();
    test_array_surface();
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
