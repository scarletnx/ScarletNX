/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real replay queue allocator with an ordered fake GPU stream. Destruction must follow submitted
   work and its deferred fence, and repeated temporary queues must release their slots and syncpoints. A sync the game
   frees before the submission that fences it keeps the bytes its heap wrote over it. */
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_copy.h"
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
#include <time.h>

static u8 memory[4096];
u8* g_mem = memory;
Settings g_settings;
int g_renderer_up;
static struct {
    u32 used[32], current[32], promised[32], pending[32], npending;
    struct { void (*fn)(void*); void* arg; } jobs[64];
    u32 head, count, allocated, freed, draws, in_gpu, zero_strides;
    u64 unfinished;
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
int nvn_cmd_unfinished(u64 p) { return p == fixture.unfinished; }
void nvn_cmd_describe(u64 p, char* out, size_t n) {
    (void)p;
    if (n) out[0] = 0;
}
u64 guest_ticks(void) { return 0; }
u64 guest_gpu_ticks(void) { return 0; }
u64 gmmu_translate(u64 gva) { return gva; }
u32 gmem_r32(u64 gva) { return gr32(gva); }
void gpu_release_write(u64 gva, const void* data, u32 n) { memcpy(GP(gva), data, n); }

static void submit(u32 queue) {
    u64 ready = 0;
    u32 shown = 0;
    nvn_replay_submit(queue, 1, 0x100, &ready, &shown);
}
/* One game records nvnCommandBufferFenceSync into a command buffer, then finalizes and frees the sync, then submits
   the recording. The game's driver fences the sync at the submission all the same, writing from its byte 8 on, and a
   freed block's first 8 bytes are its heap's free list link, which the heap's next allocation of that size decodes */
static void freed_sync(void) {
    const u64 live = 0x400, freed = 0x440, link = 0x2cd4aad71d5bed03ull; /* a free list link as one heap encodes it */
    const NvnRecHdr fence = {OP_FENCE_SYNC, 16}, end = {OP_END, 8};
    NvnSync s = {0};
    u64 ready = 0;
    u32 shown = 0, queue = nvn_replay_queue(), id = nvn_replay_syncpt(queue);
    memcpy(GP(0x300), &fence, sizeof fence);
    snx_st_le64(GP(0x308), live);
    memcpy(GP(0x310), &fence, sizeof fence);
    snx_st_le64(GP(0x318), freed);
    memcpy(GP(0x320), &end, sizeof end);
    snx_st_le64(GP(0x108), 0x300);
    s.magic = NVN_MAGIC('S', 'Y', 'N', 'C'); /* nvn_SyncInitialize's */
    memcpy(GP(live), &s, sizeof s);
    memcpy(GP(freed), &s, sizeof s);
    snx_st_le64(GP(freed), link); /* finalized and freed after the recording */
    nvn_replay_submit(queue, 1, 0x108, &ready, &shown);
    assert(gr64(freed) == link);
    const NvnSync* f = NVN_OBJ(const NvnSync, live);
    assert(f->magic == NVN_MAGIC('S', 'Y', 'N', 'C') && f->fenced == 1 && f->syncpt == id);
    u32 first = f->value;
    nvn_replay_fence(queue, freed, 0, 0); /* nvnQueueFenceSync writes where the submission's fence does */
    assert(gr64(freed) == link);
    nvn_replay_finish(queue);
    assert((i32)(fixture.current[id] - first) >= 0); /* the live sync signals once the submission is replayed */
    nvn_replay_queue_free(queue);
}
/* The promise pass must see current sync records inside calls and jumps, while replay still sees ordinary draws.
   The guest can change a nested record between submissions; no prior scan may hide the new sync or opcode. */
static void nested_promises(void) {
    const NvnRecHdr call = {OP_CALL, 16}, jump = {OP_JUMP, 16}, draw = {OP_DRAW, 8}, fence = {OP_FENCE_SYNC, 16},
                    wait = {OP_WAIT_SYNC, 16}, last_fence = {OP_FENCE_SYNC | NVN_REC_LAST, 16}, end = {OP_END, 8};
    memcpy(GP(0x500), &call, sizeof call);
    snx_st_le64(GP(0x508), 0x600);
    memcpy(GP(0x510), &draw, sizeof draw);
    memcpy(GP(0x518), &jump, sizeof jump);
    snx_st_le64(GP(0x520), 0x700);
    memcpy(GP(0x600), &fence, sizeof fence);
    snx_st_le64(GP(0x608), 0x800);
    memcpy(GP(0x610), &wait, sizeof wait);
    snx_st_le64(GP(0x618), 0x880);
    memcpy(GP(0x620), &end, sizeof end);
    memcpy(GP(0x700), &last_fence, sizeof last_fence);
    snx_st_le64(GP(0x708), 0x840);
    NvnSync external = {.fenced = 1, .ready = 200, .shown = 9};
    memcpy(GP(0x880), &external, sizeof external);
    snx_st_le64(GP(0x110), 0x500);
    u32 queue = nvn_replay_queue(), id = nvn_replay_syncpt(queue);
    u64 ready = 13;
    u32 shown = 3, draws_before = fixture.draws;
    nvn_replay_submit(queue, 1, 0x110, &ready, &shown);
    const NvnSync* before = NVN_OBJ(const NvnSync, 0x800);
    const NvnSync* after = NVN_OBJ(const NvnSync, 0x840);
    assert(before->syncpt == id && before->fenced && before->ready == 13 && before->shown == 3);
    assert(after->syncpt == id && after->fenced && after->ready == 200 && after->shown == 9);
    assert(after->value == before->value + 1 && ready == 200 && shown == 9);
    nvn_replay_finish(queue);
    assert(fixture.draws == draws_before + 1);

    external.ready = 400;
    external.shown = 15;
    memcpy(GP(0x8c0), &external, sizeof external);
    snx_st_le64(GP(0x618), 0x8c0);
    ready = 0;
    shown = 0;
    nvn_replay_submit(queue, 1, 0x110, &ready, &shown);
    assert(after->ready == 400 && after->shown == 15 && ready == 400 && shown == 15);
    nvn_replay_finish(queue);

    NvnRecHdr changed = {OP_DRAW, 16};
    memcpy(GP(0x610), &changed, sizeof changed);
    ready = 0;
    shown = 0;
    nvn_replay_submit(queue, 1, 0x110, &ready, &shown);
    assert(after->ready == 0 && after->shown == 0 && !ready && !shown);
    nvn_replay_finish(queue);
    assert(fixture.draws == draws_before + 4); /* one draw, one draw, then the changed record and outer draw */

    u32 value = after->value;
    fixture.unfinished = 0x700;
    nvn_replay_submit(queue, 1, 0x110, &ready, &shown);
    nvn_replay_finish(queue);
    assert(after->value == value); /* the unfinished jump target has no fence yet */
    fixture.unfinished = 0;
    nvn_replay_queue_free(queue);
}
static void drain(void) {
    while (fixture.count) {
        u32 i = fixture.head++ % 64;
        fixture.count--;
        fixture.in_gpu = 1;
        fixture.jobs[i].fn(fixture.jobs[i].arg);
        fixture.in_gpu = 0;
    }
}
static void bench_replay(void) {
    static const u32 record_counts[] = {1, 32, 256};
    const u32 operations = 50000;
    for (u32 workload = 0; workload < 3; workload++) {
        u32 records = record_counts[workload], queue = nvn_replay_queue(), before = fixture.draws;
        for (u32 i = 0; i < records; i++) {
            NvnRecHdr draw = {OP_DRAW | (i + 1 == records ? NVN_REC_LAST : 0), 8};
            memcpy(GP(0x200 + i * 8), &draw, sizeof draw);
        }
        snx_st_le64(GP(0x100), 0x200);
        clock_t start = clock();
        for (u32 i = 0; i < operations; i++) {
            submit(queue);
            drain();
        }
        clock_t ticks = clock() - start;
        int exact = fixture.draws - before == operations * records && !fixture.count && !fixture.npending;
        assert(exact);
        printf("GPU_HOTPATH_BENCH workload=replay-%u operations=%u ns=%llu exact=%d records=%u\n", records, operations,
               (unsigned long long)((u64)ticks * 1000000000ull / CLOCKS_PER_SEC), exact, records);
        nvn_replay_queue_free(queue);
    }
}
int main(int argc, char** argv) {
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
    freed_sync();
    nested_promises();
    if (argc == 2 && !strcmp(argv[1], "--bench-replay")) bench_replay();
    puts("nvn_queue_lifetime: ok");
    return 0;
}
