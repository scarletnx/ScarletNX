/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real replay queue allocator with an ordered fake GPU stream. Destruction must follow submitted
   work and its deferred fence, and repeated temporary queues must release their slots and syncpoints. */
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_verify.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/options.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 memory[4096];
u8* g_mem = memory;
Settings g_settings;
int g_renderer_up;
static struct {
    u32 used[32], current[32], promised[32], pending[32], npending;
    struct { void (*fn)(void*); void* arg; } jobs[64];
    u32 head, count, allocated, freed, draws, in_gpu, zero_strides;
} fixture;

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int level, const char* fmt, ...) { (void)level; (void)fmt; }
void fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    exit(1);
}
u32 syncpt_alloc(void) {
    for (u32 i = 1; i < 32; i++)
        if (!fixture.used[i]) {
            fixture.used[i] = 1;
            fixture.promised[i] = fixture.current[i];
            return i;
        }
    abort();
}
void syncpt_free(u32 id) {
    assert(fixture.used[id] && fixture.current[id] == fixture.promised[id]);
    fixture.used[id] = 0;
}
u32 syncpt_promise(u32 id, u32 n) {
    assert(fixture.used[id]);
    return fixture.promised[id] += n;
}
void syncpt_incr(u32 id) { assert(fixture.used[id]); fixture.current[id]++; }
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 n) {
    (void)chans; (void)n;
    assert(fixture.count < 64);
    u32 i = (fixture.head + fixture.count++) % 64;
    fixture.jobs[i].fn = fn;
    fixture.jobs[i].arg = arg;
}
int syncpt_wait(u32 id, u32 value, i64 timeout) {
    (void)timeout;
    while ((i32)(fixture.current[id] - value) < 0) {
        assert(fixture.count && !fixture.in_gpu);
        u32 i = fixture.head++ % 64;
        fixture.count--;
        fixture.in_gpu = 1;
        fixture.jobs[i].fn(fixture.jobs[i].arg);
        fixture.in_gpu = 0;
    }
    return 0;
}
static void stream_incr(u32 id) {
    assert(fixture.in_gpu && fixture.npending < 32);
    fixture.pending[fixture.npending++] = id;
}
static void stream_end(void) {
    assert(fixture.in_gpu);
    for (u32 i = 0; i < fixture.npending; i++) syncpt_incr(fixture.pending[i]);
    fixture.npending = 0;
}
static const Renderer renderer = {.stream_incr = stream_incr, .stream_end = stream_end};
const Renderer* g_renderer = &renderer;
void nvn_draw_init(NvnDraw* d, u32 owner) {
    d->ds.owner = owner;
    fixture.allocated++;
}
void nvn_draw_free(NvnDraw* d) {
    assert(fixture.in_gpu && d);
    fixture.freed++;
}
int nvn_draw_record(NvnDraw* d, u32 op, u64 payload, u32 size) {
    (void)payload; (void)size;
    assert(fixture.in_gpu && d);
    if (op != OP_DRAW) return 0;
    fixture.draws++;
    return 1;
}
int nvn_copy_record(u32 op, u64 payload, u32 size, i64 stride[2]) {
    (void)size;
    assert(op == OP_COPY_STRIDE);
    if (!stride[0] && !stride[1]) fixture.zero_strides++;
    stride[0] = (i64)gr64(payload);
    stride[1] = (i64)gr64(payload + 8);
    return 1;
}
int nvn_draw_samples(NvnDraw* d, u64 at, u64 ticks) { (void)d; (void)at; (void)ticks; return 0; }
void nvn_verify_replay_submit(u32 queue) { (void)queue; }
u64 guest_ticks(void) { return 0; }
u64 gmmu_translate(u64 gva) { return gva; }
u32 gmem_r32(u64 gva) { return gr32(gva); }
void gpu_release_write(u64 gva, const void* data, u32 n) { memcpy(GP(gva), data, n); }

static void submit(u32 queue) {
    u64 ready = 0;
    u32 shown = 0;
    nvn_replay_submit(queue, 1, 0x100, &ready, &shown);
}
int main(void) {
    NvnRecHdr draw = {OP_DRAW, 8}, stride = {OP_COPY_STRIDE, 24}, end = {OP_END, 8};
    memcpy(GP(0x200), &draw, sizeof draw);
    memcpy(GP(0x208), &stride, sizeof stride);
    snx_st_le64(GP(0x210), 64);
    snx_st_le64(GP(0x218), 256);
    memcpy(GP(0x220), &end, sizeof end);
    snx_st_le64(GP(0x100), 0x200);

    u32 q[8], ids[8];
    for (u32 i = 0; i < 8; i++) {
        q[i] = nvn_replay_queue();
        ids[i] = nvn_replay_syncpt(q[i]);
        assert(q[i] == i + 1);
        for (u32 j = 0; j < i; j++) assert(ids[i] != ids[j]);
    }
    submit(q[3]);
    assert(fixture.draws == 0 && fixture.freed == 0);
    nvn_replay_queue_free(q[3]);
    assert(fixture.draws == 1 && fixture.freed == 1 && !fixture.used[ids[3]]);
    u32 reused = nvn_replay_queue();
    assert(reused == q[3] && nvn_replay_syncpt(reused) == ids[3]);
    submit(reused);
    nvn_replay_finish(reused);
    assert(fixture.zero_strides == 2); /* recycled state must not retain the earlier copy strides */
    for (u32 i = 0; i < 8; i++) nvn_replay_queue_free(q[i]);
    assert(fixture.allocated == fixture.freed);

    u32 permanent = nvn_replay_queue();
    for (u32 i = 0; i < 256; i++) {
        u32 temporary = nvn_replay_queue();
        submit(temporary);
        nvn_replay_queue_free(temporary);
        assert(fixture.allocated - fixture.freed == 1);
    }
    submit(permanent);
    nvn_replay_queue_free(permanent);
    assert(fixture.draws == 259 && fixture.allocated == fixture.freed && fixture.count == 0);
    u32 live_syncpoints = 0;
    for (u32 i = 1; i < 32; i++) live_syncpoints += fixture.used[i];
    assert(live_syncpoints == 1); /* one display syncpoint shared by the lifetime of the process */
    puts("nvn_queue_lifetime: ok");
    return 0;
}
