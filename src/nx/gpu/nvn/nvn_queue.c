/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/hle_nvn.c */
/* nvn_direct's queue, syncs and window. Phase 1 draws nothing: a submission walks its recordings only for what the
   CPU can see (counter reports, fences, event signals), and its counters hold their values. The display still sets
   the pace, as on the console: a window texture comes free one period after its last present was shown (nxnative's
   pacer), the sync AcquireTexture hands out signals then, a queue that waits for that sync finishes its later work no
   earlier, and the fences after it signal then. A game waits for those as it waits for the GPU */
#include "nx/gpu/nvn/nvn_object.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/clock.h"
#include "nx/hle/services/vi.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

/* the file's state: the submissions made */
typedef struct {
    u64 submits;
} NvnQueueState;
static NvnQueueState g_nvn_queue;

/* when the queue's work so far is done: its last wait, or now */
static u64 queue_ready(const NvnQueue* q) {
    u64 now = plat_ns();
    return q->ready > now ? q->ready : now;
}
static void fence(NvnSync* s, u64 ready) {
    s->ready = ready;
    __atomic_store_n(&s->fenced, 1, __ATOMIC_RELEASE);
}

/* ============================================================ queue */
#define QB(b) NVN_OBJ(NvnQueueBuilder, b)
void nvn_QueueBuilderSetDefaults(CPU* c, u64 b) {
    NvnQueueBuilder* o = QB(b);
    (void)c;
    memset(o, 0, sizeof *o);
    o->cmd_size = 65536; /* the device's defaults */
    o->compute_size = 262144;
    o->control_size = 16384;
    o->flush_threshold = 4096;
}
/* the runtime has one device, which the builder doesn't keep (it wouldn't fit SDK 5's builder) */
void nvn_QueueBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    (void)b;
    (void)d;
}
void nvn_QueueBuilderSetCommandMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    QB(b)->cmd_size = size;
}
void nvn_QueueBuilderSetControlMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    QB(b)->control_size = size;
}
void nvn_QueueBuilderSetCommandFlushThreshold(CPU* c, u64 b, u64 size) {
    (void)c;
    QB(b)->flush_threshold = size;
}
void nvn_QueueBuilderSetFlags(CPU* c, u64 b, i32 flags) {
    (void)c;
    QB(b)->flags = (u32)flags;
}
void nvn_QueueBuilderSetComputeMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    QB(b)->compute_size = size;
}
/* the memory the queue's command, compute and control blocks take, rounded to 4 KiB: nothing uses it, but the game
   allocates it */
u64 nvn_QueueBuilderGetQueueMemorySize(CPU* c, u64 b) {
    const NvnQueueBuilder* o = QB(b);
    (void)c;
    return (o->cmd_size + o->compute_size + o->control_size + 4095) & ~4095ull;
}
void nvn_QueueBuilderSetQueueMemory(CPU* c, u64 b, u64 memory, u64 size) {
    NvnQueueBuilder* o = QB(b);
    (void)c;
    o->memory = memory;
    o->memory_size = size;
}
int nvn_QueueInitialize(CPU* c, u64 q, u64 b) {
    NvnQueue o = {0};
    (void)c;
    (void)b;
    o.magic = NVN_MAGIC('Q', 'U', 'E', 'U');
    memcpy(GP(q), &o, sizeof o);
    LOG("nvn: queue %llx initialized\n", (unsigned long long)q);
    return 1;
}

void nvn_QueueFinalize(CPU* c, u64 q) {
    (void)c;
    NVN_OBJ(NvnQueue, q)->magic = 0;
}
/* every submission completes as it is made: nothing to flush or wait for */
void nvn_QueueFlush(CPU* c, u64 q) {
    (void)c;
    (void)q;
}
void nvn_QueueFinish(CPU* c, u64 q) {
    (void)c;
    (void)q;
}
/* the queue's later work waits on the GPU for the sync, so it is done no earlier than the sync signals */
int nvn_QueueWaitSync(CPU* c, u64 q, u64 sync) {
    NvnQueue* o = NVN_OBJ(NvnQueue, q);
    const NvnSync* s = NVN_OBJ(const NvnSync, sync);
    (void)c;
    if (s->fenced && s->ready > o->ready) o->ready = s->ready;
    return 1;
}

/* the guest address the GPU's n bytes at gva show, 0 (logged) when they aren't mapped in one run */
static u64 gpu_guest(u64 gva, u64 n) {
    u64 cpu = 0;
    if (gmmu_run(gva, n, &cpu) < n) {
        LOG_LIMIT(8, "nvn: the GPU writes %llu bytes at %llx, which aren't mapped\n", (unsigned long long)n,
                  (unsigned long long)gva);
        return 0;
    }
    return cpu;
}

/* What a recording does that the CPU sees, run as the GPU would reach it: a counter report writes its 16 bytes
   ({counter, timestamp}: the counter is the timestamp for the timestamp types, 0 for the others, which counted
   nothing drawn), a fence signals when the queue's work is done, an event takes its value. A JUMP continues in another
   segment, a CALL runs another recording, an END returns */
static void run_effects(u64 p, u64 ready, int depth) {
    if (depth > 8) fatal("nvn: recordings call each other more than 8 deep (at %llx)", (unsigned long long)p);
    for (;;) {
        NvnRecHdr h;
        memcpy(&h, GP(p), sizeof h);
        if (h.op >= OP_COUNT_ || h.size < 8 || (h.size & 7))
            fatal("nvn: a command record at %llx has op %u and size %u, which nvn_direct never writes",
                  (unsigned long long)p, h.op, h.size);
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: p = gr64(p + 8); continue;
            case OP_CALL: run_effects(gr64(p + 8), ready, depth + 1); break;
            case OP_REPORT_COUNTER: {
                const NvnPReportCounter* r = NVN_OBJ(NvnPReportCounter, p + 8);
                u64 ticks = guest_ticks(), counter = r->type == NVN_COUNTER_TYPE_TIMESTAMP ||
                                                             r->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP
                                                         ? ticks
                                                         : 0;
                u64 at = gpu_guest(r->address, 16);
                if (at) {
                    gw64(at, counter);
                    gw64(at + 8, ticks);
                }
                break;
            }
            case OP_FENCE_SYNC: fence(NVN_OBJ(NvnSync, gr64(p + 8)), ready); break;
            case OP_SIGNAL_EVENT: {
                const NvnPSignalEvent* e = NVN_OBJ(NvnPSignalEvent, p + 8);
                u64 at = gpu_guest(e->value, 4);
                if (at) gw32(at, e->mode == NVN_EVENT_SIGNAL_MODE_ADD ? gr32(at) + e->v : e->v);
                break;
            }
            default: break; /* drawing state and work: phase 2's */
        }
        p += h.size;
    }
}
void nvn_QueueSubmitCommands(CPU* c, u64 q, i32 n, u64 handles) {
    NvnQueue* o = NVN_OBJ(NvnQueue, q);
    u64 ready = queue_ready(o);
    i32 i;
    (void)c;
    __atomic_add_fetch(&g_nvn_queue.submits, 1, __ATOMIC_RELAXED);
    o->submits++;
    for (i = 0; i < n; i++) run_effects(gr64(handles + 8 * (u64)i), ready, 0);
}
/* signals when the work submitted before it is done */
void nvn_QueueFenceSync(CPU* c, u64 q, u64 sync, i32 condition, i32 flags) {
    (void)c;
    (void)condition;
    (void)flags;
    fence(NVN_OBJ(NvnSync, sync), queue_ready(NVN_OBJ(NvnQueue, q)));
}
/* A fenced sync signals at its ready time: the wait sleeps until then, or for the timeout (nanoseconds) when that is
   shorter. One never fenced waits out its timeout, as on the console */
i32 nvn_SyncWait(CPU* c, u64 sync, u64 timeout) {
    const NvnSync* s = NVN_OBJ(const NvnSync, sync);
    u64 now = plat_ns(), ready;
    if (!__atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE)) {
        LOG_LIMIT(8, "nvn: sync %llx waited on before anything fenced it (timeout %llx, called from %llx)\n",
                  (unsigned long long)sync, (unsigned long long)timeout, (unsigned long long)c->x[30]);
        return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    }
    ready = s->ready;
    if (ready <= now) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    if (ready - now > timeout) {
        if (timeout) plat_sleep_ns((i64)timeout);
        return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    }
    plat_sleep_ns((i64)(ready - now));
    return NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;
}

/* ============================================================ window */
#define WB(b) NVN_OBJ(NvnWindowBuilder, b)
void nvn_WindowBuilderSetDefaults(CPU* c, u64 b) {
    (void)c;
    memset(GP(b), 0, sizeof(NvnWindowBuilder));
    WB(b)->interval = 1;
}
/* the device and the native window: one each, which the builder doesn't keep */
void nvn_WindowBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    (void)b;
    (void)d;
}
void nvn_WindowBuilderSetNativeWindow(CPU* c, u64 b, u64 w) {
    (void)c;
    (void)b;
    (void)w;
}
void nvn_WindowBuilderSetTextures(CPU* c, u64 b, i32 n, u64 textures) {
    NvnWindowBuilder* o = WB(b);
    i32 i;
    (void)c;
    if (n < 0 || n > 4) {
        LOG("nvn: a window of %d textures: nvn_direct keeps 4\n", n);
        n = n < 0 ? 0 : 4;
    }
    o->count = n;
    for (i = 0; i < n; i++) o->texture[i] = gr64(textures + 8 * (u64)i);
}
void nvn_WindowFinalize(CPU* c, u64 w) {
    (void)c;
    NVN_OBJ(NvnWindow, w)->magic = 0;
}
void nvn_WindowBuilderSetPresentInterval(CPU* c, u64 b, i32 interval) {
    (void)c;
    WB(b)->interval = interval;
}
/* the window takes its textures in turn, all of them */
void nvn_WindowBuilderSetNumActiveTextures(CPU* c, u64 b, i32 n) {
    (void)c;
    (void)b;
    LOG_LIMIT(1, "nvn: a window of %d active textures takes all it was given in turn\n", n);
}
void nvn_WindowSetPresentInterval(CPU* c, u64 w, i32 interval) {
    NvnWindow* o = NVN_OBJ(NvnWindow, w);
    (void)c;
    if (o->interval != interval) LOG_LIMIT(8, "nvn: present interval %d\n", interval);
    o->interval = interval;
}
int nvn_WindowInitialize(CPU* c, u64 w, u64 b) {
    const NvnWindowBuilder* s = WB(b);
    NvnWindow o = {0};
    (void)c;
    o.magic = NVN_MAGIC('W', 'I', 'N', 'D');
    o.count = s->count;
    o.interval = s->interval;
    memcpy(o.texture, s->texture, sizeof o.texture);
    memcpy(GP(w), &o, sizeof o);
    LOG("nvn: window %llx: %d textures, first %dx%d format %#x, present interval %d\n", (unsigned long long)w,
        o.count, o.count && o.texture[0] ? (int)NVN_OBJ(NvnTexture, o.texture[0])->d.width : 0,
        o.count && o.texture[0] ? (int)NVN_OBJ(NvnTexture, o.texture[0])->d.height : 0,
        o.count && o.texture[0] ? NVN_OBJ(NvnTexture, o.texture[0])->d.format : 0, o.interval);
    return 1;
}
void nvn_WindowSetCrop(CPU* c, u64 w, i32 x, i32 y, i32 width, i32 height) {
    NvnWindow* o = NVN_OBJ(NvnWindow, w);
    (void)c;
    if (o->crop[0] != x || o->crop[1] != y || o->crop[2] != width || o->crop[3] != height)
        LOG_LIMIT(8, "nvn: window crop %d,%d %dx%d\n", x, y, width, height);
    o->crop[0] = x;
    o->crop[1] = y;
    o->crop[2] = width;
    o->crop[3] = height;
}
/* A frame's time on the display: the window's interval in refreshes (0: no wait), or what fps_limit says */
static u64 present_period(const NvnWindow* o) {
    int fps = __atomic_load_n(&g_opt_fps, __ATOMIC_RELAXED);
    if (!fps) return 0;
    if (fps != SNX_FPS_GAME) return 1000000000ull / (u64)fps;
    return o->interval > 0 ? (u64)o->interval * 1000000000ull / VI_REFRESH_HZ : 0;
}
/* The next texture in turn, at once: the sync says when it is free, one period after its last present was shown, and
   the game waits for that on the CPU (SyncWait) or the GPU (QueueWaitSync), as on the console. A game that renders a
   frame ahead keeps doing so, and the time it measures for a frame is its own work, not the display's pace */
i32 nvn_WindowAcquireTexture(CPU* c, u64 w, u64 sync, u64 index) {
    NvnWindow* o = NVN_OBJ(NvnWindow, w);
    i32 i = o->count ? o->next % o->count : 0;
    (void)c;
    o->next = o->count ? (i + 1) % o->count : 0;
    gw32(index, (u32)i);
    if (sync) fence(NVN_OBJ(NvnSync, sync), i < 4 ? o->available[i] : 0);
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
/* nxnative's pacer, as vsync: a present is shown one period after the one before. Early, it waits for its slot; late
   by less than a period, it is shown at once and keeps the cadence; later than that, the cadence starts again from
   now, with no burst to catch up. The slot of this present */
static u64 pace(NvnWindow* o, u64 now, u64 period) {
    u64 next;
    if (!period || !o->pace) return o->pace = now;
    next = o->pace + period;
    if (now <= next) return o->pace = next;
    return o->pace = now - next < period ? next : now;
}
/* The frame is shown. Phase 1 draws nothing, so the window shows black, but the present still ends the renderer's
   frame: the engines that run on it without NVN (NVDEC's pictures, VIC's compositions of a movie) stage their uploads
   and take their descriptors in the current frame, which only a present hands back. Without it one frame never ended:
   its staging grew by a fresh ring each time one filled (a movie's pictures several megabytes each, at its frame
   rate) and none came back, until about a minute into a movie the pictures stopped and the game, waiting on them,
   stopped presenting */
void nvn_QueuePresentTexture(CPU* c, u64 q, u64 w, i32 index) {
    static const i32 whole[4] = {0, 0, 0, 0};
    NvnWindow* o = NVN_OBJ(NvnWindow, w);
    (void)c;
    if (!o->presents++) LOG("nvn: first present, texture %d of window %llx\n", index, (unsigned long long)w);
    if (!(o->presents % 1800))
        LOG("nvn: %llu presents, %llu submissions\n", (unsigned long long)o->presents,
            (unsigned long long)__atomic_load_n(&g_nvn_queue.submits, __ATOMIC_RELAXED));
    if (g_renderer_up) g_renderer->present_buffer(0, 0, 0, 0, 0, 0, 0, whole, 0); /* no buffer: black */
    {
        u64 period = present_period(o), slot = pace(o, queue_ready(NVN_OBJ(NvnQueue, q)), period);
        if (index >= 0 && index < 4) o->available[index] = period ? slot + period : 0;
    }
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
}
