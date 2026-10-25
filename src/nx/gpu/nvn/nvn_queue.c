/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/hle_nvn.c */
/* nvn_direct's queue, syncs and window. A submission goes to the GPU thread, which replays it (gpu/nvn/nvn_replay.c):
   its fences are syncpoint values the replay reaches. The display sets the pace, as on the console: a window texture
   comes free one period after its last present was shown (nxnative's pacer), the sync AcquireTexture hands out signals
   then, a queue that waits for that sync finishes its later work no earlier, and the fences after it signal then. A
   game waits for those as it waits for the GPU */
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_tex.h"
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
/* a sync that signals once syncpoint syncpt reaches value (0: none) and the pacer's clock reaches ready (a window
   texture's) */
static void fence_at(NvnSync* s, u32 syncpt, u32 value, u64 ready) {
    s->syncpt = syncpt;
    s->value = value;
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
    o.replay = nvn_replay_queue();
    memcpy(GP(q), &o, sizeof o);
    LOG("nvn: queue %llx initialized\n", (unsigned long long)q);
    return 1;
}

/* the queue's work is done before its memory goes back to the game */
void nvn_QueueFinalize(CPU* c, u64 q) {
    NvnQueue* o = NVN_OBJ(NvnQueue, q);
    (void)c;
    nvn_replay_finish(o->replay);
    o->magic = 0;
}
/* submissions go to the GPU thread as they are made: nothing to flush */
void nvn_QueueFlush(CPU* c, u64 q) {
    (void)c;
    (void)q;
}
/* the game may reuse every recording's memory after it */
void nvn_QueueFinish(CPU* c, u64 q) {
    (void)c;
    nvn_replay_finish(NVN_OBJ(NvnQueue, q)->replay);
}
/* the queue's later work waits on the GPU for the sync, so it is done no earlier than the sync signals */
int nvn_QueueWaitSync(CPU* c, u64 q, u64 sync) {
    NvnQueue* o = NVN_OBJ(NvnQueue, q);
    const NvnSync* s = NVN_OBJ(const NvnSync, sync);
    (void)c;
    if (s->fenced && s->ready > o->ready) o->ready = s->ready;
    return 1;
}

/* the recordings go to the GPU thread, their fences promised now */
void nvn_QueueSubmitCommands(CPU* c, u64 q, i32 n, u64 handles) {
    NvnQueue* o = NVN_OBJ(NvnQueue, q);
    (void)c;
    __atomic_add_fetch(&g_nvn_queue.submits, 1, __ATOMIC_RELAXED);
    o->submits++;
    if (n > 0) nvn_replay_submit(o->replay, (u32)n, handles, queue_ready(o));
}
/* signals when the work submitted before it is replayed and done */
void nvn_QueueFenceSync(CPU* c, u64 q, u64 sync, i32 condition, i32 flags) {
    NvnQueue* o = NVN_OBJ(NvnQueue, q);
    (void)c;
    (void)condition;
    (void)flags;
    nvn_replay_fence(o->replay, sync, queue_ready(o));
}
/* A fenced sync signals once its syncpoint reaches its value (the replay reached it) and the pacer's clock reaches its
   ready time: the wait blocks until both, or for the timeout (nanoseconds, ~0 for good). One never fenced waits out
   its timeout, as on the console */
i32 nvn_SyncWait(CPU* c, u64 sync, u64 timeout) {
    const NvnSync* s = NVN_OBJ(const NvnSync, sync);
    u64 start = plat_ns(), now, ready;
    i64 left = timeout > (u64)INT64_MAX ? -1 : (i64)timeout;
    int waited = 0;
    if (!__atomic_load_n(&s->fenced, __ATOMIC_ACQUIRE)) {
        LOG_LIMIT(8, "nvn: sync %llx waited on before anything fenced it (timeout %llx, called from %llx)\n",
                  (unsigned long long)sync, (unsigned long long)timeout, (unsigned long long)c->x[30]);
        if (left > 0) plat_sleep_ns(left);
        return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    }
    if (s->syncpt && !syncpt_wait(s->syncpt, s->value, 0)) {
        if (!left || !syncpt_wait(s->syncpt, s->value, left)) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
        waited = 1;
    }
    now = plat_ns();
    ready = s->ready;
    if (ready > now) {
        u64 used = now - start;
        if (left >= 0 && (u64)left - (used < (u64)left ? used : (u64)left) < ready - now) {
            if ((u64)left > used) plat_sleep_ns(left - (i64)used);
            return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
        }
        plat_sleep_ns((i64)(ready - now));
        waited = 1;
    }
    return waited ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
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
/* The next texture in turn, at once: the sync says when it is free, one period after its last present was shown and
   once the present that took it off the display is replayed, and the game waits for that on the CPU (SyncWait) or the
   GPU (QueueWaitSync), as on the console, where the display lets a texture go when the next one is shown, after the
   work before that present (one game waits for it before it records over its last frame's command memory). A game
   that renders a frame ahead keeps doing so, and the time it measures for a frame is its own work, not the display's
   pace */
i32 nvn_WindowAcquireTexture(CPU* c, u64 w, u64 sync, u64 index) {
    NvnWindow* o = NVN_OBJ(NvnWindow, w);
    i32 i = o->count ? o->next % o->count : 0;
    (void)c;
    o->next = o->count ? (i + 1) % o->count : 0;
    gw32(index, (u32)i);
    if (sync && i < 4)
        fence_at(NVN_OBJ(NvnSync, sync), o->syncpt[i], o->value[i], o->available[i]);
    else if (sync)
        fence_at(NVN_OBJ(NvnSync, sync), 0, 0, 0);
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
/* the vi pixel format (Android's) of a window texture's NVN format, 0 for one vi doesn't show */
static u32 vi_format(u32 format) {
    switch (format) {
        case NVN_FORMAT_RGBA8:
        case NVN_FORMAT_RGBA8_SRGB: return 1; /* RGBA_8888 */
        case NVN_FORMAT_RGBX8:
        case NVN_FORMAT_RGBX8_SRGB: return 2; /* RGBX_8888 */
        case NVN_FORMAT_RGB565: return 4;
        case NVN_FORMAT_BGRA8:
        case NVN_FORMAT_BGRA8_SRGB: return 5; /* BGRA_8888 */
    }
    return 0;
}
/* The frame is shown: the window's texture, once the replay has reached the present, the way vi shows a buffer (the
   render target over its memory, else its bytes). The present also ends the renderer's frame, which hands back what
   the engines that run without NVN (NVDEC's pictures, VIC's compositions of a movie) took in it: without one, a
   frame's staging grew by a fresh ring each time one filled, until about a minute into a movie the pictures stopped
   and the game, waiting on them, stopped presenting */
void nvn_QueuePresentTexture(CPU* c, u64 q, u64 w, i32 index) {
    NvnWindow* o = NVN_OBJ(NvnWindow, w);
    u64 tex = index >= 0 && index < 4 && index < o->count ? o->texture[index] : 0;
    i32 crop[4] = {0, 0, 0, 0};
    u32 value;
    (void)c;
    if (o->crop[2] > 0 && o->crop[3] > 0) {
        crop[0] = o->crop[0];
        crop[1] = o->crop[1];
        crop[2] = o->crop[0] + o->crop[2];
        crop[3] = o->crop[1] + o->crop[3];
    }
    {
        NvnTexture t;
        u32 fmt = 0;
        if (tex) memcpy(&t, GP(tex), sizeof t);
        if (tex && t.magic == NVN_MAGIC('T', 'E', 'X', 0)) {
            fmt = vi_format(t.d.format);
            if (!fmt) LOG_LIMIT(1, "nvn: a window texture of format %#x, which vi doesn't show\n", t.d.format);
        }
        if (fmt) {
            unsigned th = 0, td = 0;
            nvn_tex_level_tiles(&t.d, 0, &th, &td);
            value = nvn_replay_present(NVN_OBJ(NvnQueue, q)->replay, t.gpu, t.d.width, t.d.height, fmt,
                                       (t.d.flags & NVN_TEX_LINEAR) != 0, th,
                                       t.d.stride ? t.d.stride : t.d.width * 4, crop);
        } else /* nothing vi shows: black */
            value = nvn_replay_present(NVN_OBJ(NvnQueue, q)->replay, 0, 0, 0, 0, 0, 0, 0, crop);
        if (index >= 0 && index < 4) {
            u32 id = nvn_replay_syncpt(NVN_OBJ(NvnQueue, q)->replay);
            int i;
            /* this present takes the texture shown before off the display, and shows this one */
            for (i = 0; i < 4; i++)
                if (i == index || ((o->shown >> i) & 1)) {
                    o->syncpt[i] = id;
                    o->value[i] = value;
                }
            o->shown = 1u << index;
        }
    }
    if (!o->presents++) LOG("nvn: first present, texture %d of window %llx\n", index, (unsigned long long)w);
    if (!(o->presents % 1800))
        LOG("nvn: %llu presents, %llu submissions\n", (unsigned long long)o->presents,
            (unsigned long long)__atomic_load_n(&g_nvn_queue.submits, __ATOMIC_RELAXED));
    {
        u64 period = present_period(o), slot = pace(o, queue_ready(NVN_OBJ(NvnQueue, q)), period);
        if (index >= 0 && index < 4) o->available[index] = period ? slot + period : 0;
    }
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
}
