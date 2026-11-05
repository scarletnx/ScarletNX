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

static u8 memory[65536];
u8* g_mem = memory;
Settings g_settings;
int g_renderer_up;
static struct {
    u32 used[32], current[32], promised[32], pending[32], npending;
    struct {
        void (*fn)(void*);
        int (*poll)(void*);
        void* arg;
        u32 lane;
    } jobs[64];
    u32 head, count, allocated, freed, draws, in_gpu, zero_strides, presents, targets;
    u64 presented[2];
    u64 unfinished;
} fixture;

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
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
void syncpt_incr(u32 id) {
    assert(fixture.used[id]);
    fixture.current[id]++;
}
u32 syncpt_read(u32 id) { return fixture.current[id]; }
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 n) {
    (void)chans;
    (void)n;
    assert(fixture.count < 64);
    u32 i = (fixture.head + fixture.count++) % 64;
    fixture.jobs[i].fn = fn;
    fixture.jobs[i].poll = NULL;
    fixture.jobs[i].arg = arg;
}
void gpu_call_poll(int (*fn)(void*), void* arg, u32 lane) {
    assert(fixture.count < 64 && lane < 64);
    u32 i = (fixture.head + fixture.count++) % 64;
    fixture.jobs[i].poll = fn;
    fixture.jobs[i].fn = NULL;
    fixture.jobs[i].arg = arg;
    fixture.jobs[i].lane = lane;
}
static int pump(void) {
    u64 blocked = 0;
    assert(!fixture.in_gpu);
    for (u32 k = 0; k < fixture.count; k++) {
        u32 i = (fixture.head + k) % 64;
        if (fixture.jobs[i].poll && (blocked & (1ull << fixture.jobs[i].lane))) continue;
        fixture.in_gpu = 1;
        int done = 1;
        if (fixture.jobs[i].poll)
            done = fixture.jobs[i].poll(fixture.jobs[i].arg);
        else
            fixture.jobs[i].fn(fixture.jobs[i].arg);
        fixture.in_gpu = 0;
        if (!done) {
            blocked |= 1ull << fixture.jobs[i].lane;
            continue;
        }
        for (u32 j = k; j + 1 < fixture.count; j++)
            fixture.jobs[(fixture.head + j) % 64] = fixture.jobs[(fixture.head + j + 1) % 64];
        fixture.count--;
        return 1;
    }
    return 0;
}
int syncpt_wait(u32 id, u32 value, i64 timeout) {
    (void)timeout;
    while ((i32)(fixture.current[id] - value) < 0) assert(fixture.count && pump());
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
static void present_buffer(u64 cpu, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                           u32 transform, u64 released, u64 period, u32 shown) {
    (void)w;
    (void)h;
    (void)pitch;
    (void)layout;
    (void)bhl;
    (void)fmt;
    (void)crop;
    (void)transform;
    (void)released;
    (void)period;
    assert(fixture.presents < 2);
    fixture.presented[fixture.presents++] = cpu;
    syncpt_incr(shown);
}
static const Renderer renderer = {
    .stream_incr = stream_incr, .stream_end = stream_end, .present_buffer = present_buffer};
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
    (void)payload;
    (void)size;
    assert(fixture.in_gpu && d);
    if (op == OP_SET_RT) {
        const NvnPSetRT* r = NVN_OBJ(const NvnPSetRT, payload);
        const NvnTexture* t = NVN_OBJ(const NvnTexture, r->color[0]);
        assert(r->count == 1 && t->magic == NVN_MAGIC('T', 'E', 'X', 0));
        assert(t->gpu == 0x123456789ull && t->d.width == 37);
        fixture.targets++;
        return 1;
    }
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
int nvn_draw_samples(NvnDraw* d, u64 at, u64 ticks) {
    (void)d;
    (void)at;
    (void)ticks;
    return 0;
}
int nvn_cmd_unfinished(u64 p) { return p == fixture.unfinished; }
void nvn_cmd_describe(u64 p, char* out, size_t n) {
    (void)p;
    if (n) out[0] = 0;
}
u64 guest_ticks(void) { return 0; }
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
    while (fixture.count) assert(pump());
}
static void event_waits(void) {
    u32 a = nvn_replay_queue(), b = nvn_replay_queue();
    u64 ready = 0;
    u32 shown = 0, draws = fixture.draws;
    u64 before = nvn_replay_walked();
    /* Queue A enters a nested recording and stops between two draws. Queue B's later signal must run, while
       another submission and fence on A stay behind the wait. */
    *(NvnRecHdr*)(void*)GP(0x400) = (NvnRecHdr){OP_DRAW, 8};
    *(NvnRecHdr*)(void*)GP(0x408) = (NvnRecHdr){OP_WAIT_EVENT, 24};
    *(NvnPWaitEvent*)(void*)GP(0x410) = (NvnPWaitEvent){0x800, NVN_EVENT_WAIT_MODE_EQUAL, 7};
    *(NvnRecHdr*)(void*)GP(0x420) = (NvnRecHdr){OP_DRAW | NVN_REC_LAST, 8};
    *(NvnRecHdr*)(void*)GP(0x500) = (NvnRecHdr){OP_CALL, 16};
    *(u64*)(void*)GP(0x508) = 0x400;
    *(NvnRecHdr*)(void*)GP(0x510) = (NvnRecHdr){OP_DRAW | NVN_REC_LAST, 8};
    *(u64*)(void*)GP(0x108) = 0x500;
    nvn_replay_submit(a, 1, 0x108, &ready, &shown);
    assert(!pump() && fixture.draws == draws + 1 && nvn_replay_walked() == before);
    *(NvnRecHdr*)(void*)GP(0x600) = (NvnRecHdr){OP_DRAW | NVN_REC_LAST, 8};
    *(u64*)(void*)GP(0x108) = 0x600;
    nvn_replay_submit(a, 1, 0x108, &ready, &shown);
    nvn_replay_fence(a, 0x900, 0, 0);
    u32 fence_value = NVN_OBJ(NvnSync, 0x900)->value;
    assert(!pump() && fixture.draws == draws + 1 && fixture.current[nvn_replay_syncpt(a)] < fence_value);
    *(NvnRecHdr*)(void*)GP(0x680) = (NvnRecHdr){OP_SIGNAL_EVENT | NVN_REC_LAST, 24};
    *(NvnPSignalEvent*)(void*)GP(0x688) = (NvnPSignalEvent){0x800, NVN_EVENT_SIGNAL_MODE_WRITE, 7};
    *(u64*)(void*)GP(0x108) = 0x680;
    nvn_replay_submit(b, 1, 0x108, &ready, &shown);
    assert(pump() && gr32(0x800) == 7 && nvn_replay_walked() == before);
    drain();
    assert(fixture.draws == draws + 4 && fixture.current[nvn_replay_syncpt(a)] == fence_value);
    assert(nvn_replay_walked() == nvn_replay_submitted());
    /* Signed wrap comparison, including exact equality, and a signal/add before a wait in the same recording. */
    const u32 current[] = {0xfffffffeu, 0, 5, 0x80000000u};
    const u32 wanted[] = {0xfffffffdu, 0xffffffffu, 5, 0};
    for (u32 i = 0; i < 4; i++) {
        *(NvnRecHdr*)(void*)GP(0x700) = (NvnRecHdr){OP_SIGNAL_EVENT, 24};
        *(NvnPSignalEvent*)(void*)GP(0x708) = (NvnPSignalEvent){0x800, NVN_EVENT_SIGNAL_MODE_WRITE, current[i]};
        *(NvnRecHdr*)(void*)GP(0x718) = (NvnRecHdr){OP_WAIT_EVENT, 24};
        *(NvnPWaitEvent*)(void*)GP(0x720) = (NvnPWaitEvent){0x800, NVN_EVENT_WAIT_MODE_GEQUAL_WRAP, wanted[i]};
        *(NvnRecHdr*)(void*)GP(0x730) = (NvnRecHdr){OP_DRAW | NVN_REC_LAST, 8};
        *(u64*)(void*)GP(0x108) = 0x700;
        nvn_replay_submit(a, 1, 0x108, &ready, &shown);
        if (i == 3) {
            assert(!pump());
            *(u32*)(void*)GP(0x800) = wanted[i];
        }
        drain();
    }
    *(NvnRecHdr*)(void*)GP(0x700) = (NvnRecHdr){OP_SIGNAL_EVENT, 24};
    *(NvnPSignalEvent*)(void*)GP(0x708) = (NvnPSignalEvent){0x800, NVN_EVENT_SIGNAL_MODE_ADD, 3};
    *(NvnPWaitEvent*)(void*)GP(0x720) = (NvnPWaitEvent){0x800, NVN_EVENT_WAIT_MODE_EQUAL, 3};
    nvn_replay_submit(a, 1, 0x108, &ready, &shown);
    drain();
    nvn_replay_queue_free(a);
    nvn_replay_queue_free(b);
}
/* A sync cannot pass a queue stopped at an event. A fence before an event must still signal, and then the guest
   may reuse that recording's memory while replay retains the rest of the submission. */
static void cross_queue_fences(void) {
    u32 a = nvn_replay_queue(), b = nvn_replay_queue(), id = nvn_replay_syncpt(a);
    u64 ready = 0;
    u32 shown = 0, draws = fixture.draws;
    gw32(0x800, 0);
    *(NvnRecHdr*)(void*)GP(0x400) = (NvnRecHdr){OP_WAIT_EVENT, 24};
    *(NvnPWaitEvent*)(void*)GP(0x408) = (NvnPWaitEvent){0x800, NVN_EVENT_WAIT_MODE_EQUAL, 1};
    *(NvnRecHdr*)(void*)GP(0x418) = (NvnRecHdr){OP_FENCE_SYNC | NVN_REC_LAST, 16};
    gw64(0x420, 0x900);
    gw64(0x108, 0x400);
    nvn_replay_submit(a, 1, 0x108, &ready, &shown);
    NvnSync sync = *NVN_OBJ(NvnSync, 0x900);
    nvn_replay_wait(b, &sync);
    *(NvnRecHdr*)(void*)GP(0x500) = (NvnRecHdr){OP_DRAW | NVN_REC_LAST, 8};
    gw64(0x108, 0x500);
    nvn_replay_submit(b, 1, 0x108, &ready, &shown);
    assert(!pump() && fixture.draws == draws && fixture.current[id] != sync.value);
    gw32(0x800, 1);
    drain();
    assert(fixture.draws == draws + 1 && fixture.current[id] == sync.value);

    gw32(0x800, 0);
    *(NvnRecHdr*)(void*)GP(0x400) = (NvnRecHdr){OP_FENCE_SYNC, 16};
    gw64(0x408, 0x900);
    *(NvnRecHdr*)(void*)GP(0x410) = (NvnRecHdr){OP_WAIT_EVENT, 24};
    *(NvnPWaitEvent*)(void*)GP(0x418) = (NvnPWaitEvent){0x800, NVN_EVENT_WAIT_MODE_EQUAL, 2};
    *(NvnRecHdr*)(void*)GP(0x428) = (NvnRecHdr){OP_DRAW | NVN_REC_LAST, 8};
    gw64(0x108, 0x400);
    nvn_replay_submit(a, 1, 0x108, &ready, &shown);
    sync = *NVN_OBJ(NvnSync, 0x900);
    *(NvnRecHdr*)(void*)GP(0x500) = (NvnRecHdr){OP_WAIT_SYNC, 16};
    gw64(0x508, 0x900);
    *(NvnRecHdr*)(void*)GP(0x510) = (NvnRecHdr){OP_SIGNAL_EVENT | NVN_REC_LAST, 24};
    *(NvnPSignalEvent*)(void*)GP(0x518) = (NvnPSignalEvent){0x800, NVN_EVENT_SIGNAL_MODE_WRITE, 2};
    gw64(0x108, 0x500);
    nvn_replay_submit(b, 1, 0x108, &ready, &shown);
    memset(GP(0x900), 0xa5, sizeof(NvnSync)); /* the captured wait does not read a reused sync object */
    assert(pump() && fixture.current[id] == sync.value && gr32(0x800) == 2);
    memset(GP(0x400), 0, 0x30); /* the signaled fence permits this command memory reuse */
    drain();
    assert(fixture.draws == draws + 2);
    nvn_replay_queue_free(a);
    nvn_replay_queue_free(b);
}
static void present_order(void) {
    u32 a = nvn_replay_queue(), b = nvn_replay_queue(), shown = 0;
    u64 ready = 0;
    gw32(0x800, 0);
    *(NvnRecHdr*)(void*)GP(0x400) = (NvnRecHdr){OP_WAIT_EVENT | NVN_REC_LAST, 24};
    *(NvnPWaitEvent*)(void*)GP(0x408) = (NvnPWaitEvent){0x800, NVN_EVENT_WAIT_MODE_EQUAL, 1};
    gw64(0x108, 0x400);
    nvn_replay_submit(a, 1, 0x108, &ready, &shown);
    NvnReplayPresent present = {.gva = 1, .w = 1, .h = 1};
    g_renderer_up = 1;
    nvn_replay_present(a, &present, &shown);
    present.gva = 2;
    nvn_replay_present(b, &present, &shown);
    assert(!pump() && !fixture.presents);
    gw32(0x800, 1);
    drain();
    assert(fixture.presents == 2 && fixture.presented[0] == 1 && fixture.presented[1] == 2);
    assert(syncpt_read(nvn_replay_shown_syncpt()) == shown);
    g_renderer_up = 0;
    nvn_replay_queue_free(a);
    nvn_replay_queue_free(b);
}
static void retained_texture_snapshot(void) {
    u32 a = nvn_replay_queue(), b = nvn_replay_queue(), shown = 0;
    u64 ready = 0;
    gw32(0x800, 0);
    *(NvnRecHdr*)(void*)GP(0x1000) = (NvnRecHdr){OP_FENCE_SYNC, 16};
    gw64(0x1008, 0x900);
    *(NvnRecHdr*)(void*)GP(0x1010) = (NvnRecHdr){OP_WAIT_EVENT, 24};
    *(NvnPWaitEvent*)(void*)GP(0x1018) = (NvnPWaitEvent){0x800, NVN_EVENT_WAIT_MODE_EQUAL, 3};
    u64 at = 0x1028;
    u32 bytes = (u32)((8 + sizeof(NvnPSetRT) + NVN_TEX_SNAP + 7) & ~(size_t)7);
    *(NvnRecHdr*)(void*)GP(at) = (NvnRecHdr){OP_SET_RT, bytes};
    NvnPSetRT r = {.count = 1};
    r.color[0] = at + 8 + sizeof r;
    memcpy(GP(at + 8), &r, sizeof r);
    NvnTexture texture = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = 0x123456789ull};
    texture.d.width = 37;
    memcpy(GP(r.color[0]), &texture, NVN_TEX_SNAP);
    *(NvnRecHdr*)(void*)GP(at + bytes) = (NvnRecHdr){OP_DRAW | NVN_REC_LAST, 8192};
    gw64(0x108, 0x1000);
    nvn_replay_submit(a, 1, 0x108, &ready, &shown);
    assert(!pump());
    memset(GP(0x1000), 0xa5, (size_t)(at + bytes + 8192 - 0x1000));
    *(NvnRecHdr*)(void*)GP(0x500) = (NvnRecHdr){OP_WAIT_SYNC, 16};
    gw64(0x508, 0x900);
    *(NvnRecHdr*)(void*)GP(0x510) = (NvnRecHdr){OP_SIGNAL_EVENT | NVN_REC_LAST, 24};
    *(NvnPSignalEvent*)(void*)GP(0x518) = (NvnPSignalEvent){0x800, NVN_EVENT_SIGNAL_MODE_WRITE, 3};
    gw64(0x108, 0x500);
    nvn_replay_submit(b, 1, 0x108, &ready, &shown);
    drain();
    assert(fixture.targets == 1);
    nvn_replay_queue_free(a);
    nvn_replay_queue_free(b);
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
    event_waits();
    cross_queue_fences();
    present_order();
    retained_texture_snapshot();
    if (argc == 2 && !strcmp(argv[1], "--bench-replay")) bench_replay();
    puts("nvn_queue_lifetime: ok");
    return 0;
}
