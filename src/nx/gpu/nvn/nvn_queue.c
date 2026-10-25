/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/hle_nvn.c */
/* nvn_direct's queue, syncs and window. Phase 1 draws nothing: a submission walks its recordings only for what the
   CPU can see (counter reports, fences, event signals) and completes as it is made, so every fence a game waits on
   has signalled, and its counters hold their values. A present counts the frame and hands the game the next of the
   window's textures, paced at the window's interval by the runtime's clock (present_pace), as vi paces a game that
   draws through nvdrv */
#include "nx/gpu/nvn/nvn_object.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/gpu/renderer.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

/* the file's state: submissions made, each one's number its fence, all of them complete */
typedef struct {
    u64 submits;
} NvnQueueState;
static NvnQueueState g_nvn_queue;

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
/* the queue's later work waits on the GPU for the sync: the work before it completed already */
int nvn_QueueWaitSync(CPU* c, u64 q, u64 sync) {
    (void)c;
    (void)q;
    (void)sync;
    return 1;
}

/* What a recording does that the CPU sees, run as the GPU would reach it: a counter report writes its 16 bytes
   ({counter, timestamp}: the counter is the timestamp for the timestamp types, 0 for the others, which counted
   nothing drawn), a fence signals with this submission, an event takes its value. A JUMP continues in another
   segment, a CALL runs another recording, an END returns */
static void run_effects(u64 p, u64 fence, int depth) {
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
            case OP_CALL: run_effects(gr64(p + 8), fence, depth + 1); break;
            case OP_REPORT_COUNTER: {
                const NvnPReportCounter* r = NVN_OBJ(NvnPReportCounter, p + 8);
                u64 ticks = guest_ticks(), counter = r->type == NVN_COUNTER_TYPE_TIMESTAMP ||
                                                             r->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP
                                                         ? ticks
                                                         : 0;
                gw64(r->address, counter);
                gw64(r->address + 8, ticks);
                break;
            }
            case OP_FENCE_SYNC: {
                NvnSync* s = NVN_OBJ(NvnSync, gr64(p + 8));
                s->fence = fence;
                s->fenced = 1;
                break;
            }
            case OP_SIGNAL_EVENT: {
                const NvnPSignalEvent* e = NVN_OBJ(NvnPSignalEvent, p + 8);
                gw32(e->value, e->mode == NVN_EVENT_SIGNAL_MODE_ADD ? gr32(e->value) + e->v : e->v);
                break;
            }
            default: break; /* drawing state and work: phase 2's */
        }
        p += h.size;
    }
}
void nvn_QueueSubmitCommands(CPU* c, u64 q, i32 n, u64 handles) {
    u64 fence = __atomic_add_fetch(&g_nvn_queue.submits, 1, __ATOMIC_ACQ_REL);
    i32 i;
    (void)c;
    NVN_OBJ(NvnQueue, q)->submits++;
    for (i = 0; i < n; i++) run_effects(gr64(handles + 8 * (u64)i), fence, 0);
}
/* all the work submitted before it has completed: it signals with the last submission */
void nvn_QueueFenceSync(CPU* c, u64 q, u64 sync, i32 condition, i32 flags) {
    NvnSync* s = NVN_OBJ(NvnSync, sync);
    (void)c;
    (void)q;
    (void)condition;
    (void)flags;
    s->fence = __atomic_load_n(&g_nvn_queue.submits, __ATOMIC_ACQUIRE);
    s->fenced = 1;
}
/* a sync never fenced waits out its timeout, as on the console; a fenced one has signalled already */
i32 nvn_SyncWait(CPU* c, u64 sync, u64 timeout) {
    if (NVN_OBJ(NvnSync, sync)->fenced) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    LOG_LIMIT(8, "nvn: sync %llx waited on before anything fenced it (timeout %llx, called from %llx)\n",
              (unsigned long long)sync, (unsigned long long)timeout, (unsigned long long)c->x[30]);
    return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
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
/* The next texture in turn. The console blocks here, not in the present, while every texture waits for the display:
   so does the pace, until the frame before has been on the display for the window's interval. The sync signals when
   the texture is free, which it is once acquired here */
i32 nvn_WindowAcquireTexture(CPU* c, u64 w, u64 sync, u64 index) {
    NvnWindow* o = NVN_OBJ(NvnWindow, w);
    i32 i = o->count ? o->next % o->count : 0;
    (void)c;
    o->next = o->count ? (i + 1) % o->count : 0;
    present_pace((u32)(o->interval > 0 ? o->interval : 1));
    gw32(index, (u32)i);
    if (sync) {
        NvnSync* s = NVN_OBJ(NvnSync, sync);
        s->fence = __atomic_load_n(&g_nvn_queue.submits, __ATOMIC_ACQUIRE);
        s->fenced = 1;
    }
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
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
    (void)q;
    if (!o->presents++) LOG("nvn: first present, texture %d of window %llx\n", index, (unsigned long long)w);
    if (!(o->presents % 1800))
        LOG("nvn: %llu presents, %llu submissions\n", (unsigned long long)o->presents,
            (unsigned long long)__atomic_load_n(&g_nvn_queue.submits, __ATOMIC_RELAXED));
    if (g_renderer_up) g_renderer->present_buffer(0, 0, 0, 0, 0, 0, 0, whole, 0); /* no buffer: black */
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
}
