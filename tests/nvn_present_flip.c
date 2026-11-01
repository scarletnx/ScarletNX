/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_QueuePresentTexture's transform against the game's driver (nvn_verify, NVN 55.12): a lower left window is
   queued to vi flipped (FlipV, 2), an upper left one as it is (0). The present is caught at nvn_replay_present */
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_queue.h"
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 memory[1 << 16];
u8* g_mem = memory; /* guest address a is memory + a */

/* ---- what the file needs of the runtime: stubs */
const Renderer* g_renderer; /* null: the present is not paced by a display */
static NvnReplayPresent got;
static int presents;
u32 nvn_replay_present(u32 queue, const NvnReplayPresent* p, u32* shown) {
    (void)queue;
    got = *p;
    presents++;
    *shown = 0;
    return 1;
}
int g_opt_fps; /* 0: no pace, so a present neither waits nor holds */
int g_frames;
void pm_init(PMutex* m) { (void)m; }
u64 plat_ns(void) { return 0; }
void plat_sleep_ns(i64 ns) { (void)ns; }
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns) {
    (void)id;
    (void)thresh;
    (void)timeout_ns;
    return 0;
}
u64 present_display_period(u64 period, u64 refresh) {
    (void)refresh;
    return period;
}
u64 nvn_device(void) { return 0; }
u32 nvn_device_flags(void) { return 0; }
void nvn_wait_begin(void) {}
void nvn_wait_end(void) {}
u32 nvn_replay_queue(void) { return 0; }
void nvn_replay_queue_free(u32 queue) { (void)queue; }
u32 nvn_replay_shown_syncpt(void) { return 0; }
void nvn_replay_submit(u32 queue, u32 n, u64 handles, u64* ready, u32* shown) {
    (void)queue;
    (void)n;
    (void)handles;
    (void)ready;
    (void)shown;
}
void nvn_sync_hold(u64* ready, u32* shown, const NvnSync* s) {
    (void)ready;
    (void)shown;
    (void)s;
}
void nvn_replay_fence(u32 queue, u64 sync, u64 ready, u32 shown) {
    (void)queue;
    (void)sync;
    (void)ready;
    (void)shown;
}
void nvn_replay_finish(u32 queue) { (void)queue; }
u32 nvn_replay_syncpt(u32 queue) {
    (void)queue;
    return 1;
}
void nvn_tex_level_tiles(const NvnTexDesc* d, unsigned level, unsigned* tile_h, unsigned* tile_d) {
    (void)d;
    (void)level;
    *tile_h = *tile_d = 0;
}
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

/* guest addresses of the objects */
#define QUEUE  0x100u
#define WINDOW 0x200u
#define TEX    0x400u

/* a window of one RGBA8 texture, a queue, and the device's window origin set to origin */
static void setup(u32 origin) {
    NvnQueue* q = NVN_OBJ(NvnQueue, QUEUE);
    NvnWindow* w = NVN_OBJ(NvnWindow, WINDOW);
    NvnTexture* t = NVN_OBJ(NvnTexture, TEX);
    memset(memory, 0, sizeof memory);
    memset(&g_nvn, 0, sizeof g_nvn);
    g_nvn.draw.origin = origin;
    w->magic = NVN_MAGIC('W', 'I', 'N', 0);
    w->count = 1;
    w->texture[0] = TEX;
    t->magic = NVN_MAGIC('T', 'E', 'X', 0);
    t->d.format = NVN_FORMAT_RGBA8;
    t->d.width = 64;
    t->d.height = 32;
    t->gpu = 0x10000;
    (void)q;
    presents = 0;
    memset(&got, 0xff, sizeof got);
}

static void present(i32 index) { nvn_QueuePresentTexture(NULL, QUEUE, WINDOW, index); }

static void test_lower_left(void) {
    setup(NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
    present(0);
    check(presents == 1, "lower left: expected 1 present, found %d", presents);
    check(got.transform == 2, "lower left: expected transform 2 (FlipV), found %u", got.transform);
    check(got.fmt == 1 && got.w == 64 && got.h == 32,
          "lower left: expected the RGBA8 texture 64x32 (fmt 1) to be shown, found fmt %u %ux%u", got.fmt, got.w,
          got.h);
}

static void test_upper_left(void) {
    setup(NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    present(0);
    check(presents == 1, "upper left: expected 1 present, found %d", presents);
    check(got.transform == 0, "upper left: expected transform 0, found %u", got.transform);
    check(got.fmt == 1, "upper left: expected the RGBA8 texture (fmt 1) to be shown, found fmt %u", got.fmt);
}

/* a window with no texture shown (index out of range) still takes the origin's transform: black, but the right way up
   once a texture comes */
static void test_no_texture(void) {
    setup(NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
    present(7);
    check(got.transform == 2, "lower left, no texture: expected transform 2, found %u", got.transform);
    check(got.fmt == 0, "lower left, no texture: expected fmt 0 (black), found %u", got.fmt);
    setup(NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    present(7);
    check(got.transform == 0, "upper left, no texture: expected transform 0, found %u", got.transform);
}

int main(void) {
    test_lower_left();
    test_upper_left();
    test_no_texture();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
