/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/hle_nvn.c */
/* nvn_direct's queue, syncs and window. A submission goes to the GPU thread, which replays it (gpu/nvn/nvn_replay.c):
   its fences are syncpoint values the replay reaches. The display sets the pace, as on the console: a window texture
   comes free once the display has taken the frames before it, and no earlier than one period after its own present's
   slot (nxnative's pacer, on the display's refresh where the renderer knows it); the sync AcquireTexture hands out
   signals then, a queue that waits for that sync finishes its later work no earlier, and the fences after it signal
   then. A game waits for those as it waits for the GPU, never on the GPU thread, and in AcquireTexture only for a
   texture whose last present's slot hasn't come yet */
#include "common/utilities/bits.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/nvdrv_ctrl_gpu.h"
#include "nx/runtime/clock.h"
#include "nx/hle/services/vi.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

/* the file's state: the submissions made; the window's frame period on the pacer's clock (atomic), which bounds a
   wait for the display; whether the display is stuck (atomic: it didn't show a present in two periods, a window
   covered, so syncs don't wait for it until it shows one again); the native window a window builder named last
   (atomic: SDK 5's 0x2c bytes of builder have no room for it, and a game has one, vi's layer) */
typedef struct {
    u64 submits, period;
    int stuck;
    u64 native_window;
} NvnQueueState;
static NvnQueueState g_nvn_queue;

/* when the queue's work so far is done: its last wait, or now */
static u64 queue_ready(const NvnQueue* q) {
    u64 now = plat_ns();
    return q->ready > now ? q->ready : now;
}
/* a sync that signals once syncpoint syncpt reaches value (0: none), the presents' syncpoint reaches shown (0: none)
   and the pacer's clock reaches ready (a window texture's) */
static void fence_at(NvnSync* s, u32 syncpt, u32 value, u32 shown, u64 ready) {
    s->syncpt = syncpt;
    s->value = value;
    s->shown = shown;
    s->ready = ready;
    __atomic_store_n(&s->fenced, 1, __ATOMIC_RELEASE);
}

/* ============================================================ queue */
#define QB(b) NVN_OBJ(NvnQueueBuilder, b)
void nvn_QueueBuilderSetDefaults(CPU* c, u64 b) {
    NvnQueueBuilder* o = QB(b);
    u64 device = o->device; /* SetDefaults keeps the device, as the game's driver does */
    (void)c;
    memset(o, 0, sizeof *o);
    o->device = device;
    o->cmd_size = 65536; /* the device's defaults */
    o->compute_size = 262144;
    o->control_size = 16384;
    o->flush_threshold = 4096;
    o->priority = 100; /* the driver's default (NORMAL) */
}
/* kept for its getter: the replay runs every queue's submissions in order on one GPU thread, as they come */
void nvn_QueueBuilderSetQueuePriority(CPU* c, u64 b, i32 priority) {
    (void)c;
    QB(b)->priority = (u32)priority;
}
i32 nvn_QueueBuilderGetQueuePriority(CPU* c, u64 b) {
    (void)c;
    return (i32)QB(b)->priority;
}
void nvn_QueueBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    QB(b)->device = d;
}
u64 nvn_QueueBuilderGetDevice(CPU* c, u64 b) {
    (void)c;
    return QB(b)->device;
}
/* the sizes, 32 bits as the game's driver keeps them, each marked as set */
void nvn_QueueBuilderSetCommandMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    QB(b)->cmd_size = (u32)size;
    QB(b)->set |= NVN_QB_SET_COMMAND;
}
void nvn_QueueBuilderSetControlMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    QB(b)->control_size = (u32)size;
    QB(b)->set |= NVN_QB_SET_CONTROL;
}
void nvn_QueueBuilderSetCommandFlushThreshold(CPU* c, u64 b, u64 size) {
    (void)c;
    QB(b)->flush_threshold = (u32)size;
    QB(b)->set |= NVN_QB_SET_FLUSH;
}
void nvn_QueueBuilderSetFlags(CPU* c, u64 b, i32 flags) {
    (void)c;
    QB(b)->flags = (u32)flags;
}
void nvn_QueueBuilderSetComputeMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    QB(b)->compute_size = (u32)size;
    QB(b)->set |= NVN_QB_SET_COMPUTE;
}
/* The getters of the sizes, as the game's driver answers: whether the game set the size, and in *size the size it set
   (0 when it set none: the driver's SetDefaults leaves them 0 and uses its defaults for them) */
static int qb_size(u64 b, u8 which, u32 v, u64 size) {
    int set = (QB(b)->set & which) != 0;
    gw32(size, set ? v : 0);
    return set;
}
int nvn_QueueBuilderGetCommandMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    return qb_size(b, NVN_QB_SET_COMMAND, QB(b)->cmd_size, size);
}
int nvn_QueueBuilderGetComputeMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    return qb_size(b, NVN_QB_SET_COMPUTE, QB(b)->compute_size, size);
}
int nvn_QueueBuilderGetControlMemorySize(CPU* c, u64 b, u64 size) {
    (void)c;
    return qb_size(b, NVN_QB_SET_CONTROL, QB(b)->control_size, size);
}
int nvn_QueueBuilderGetCommandFlushThreshold(CPU* c, u64 b, u64 size) {
    (void)c;
    return qb_size(b, NVN_QB_SET_FLUSH, QB(b)->flush_threshold, size);
}
i32 nvn_QueueBuilderGetFlags(CPU* c, u64 b) {
    (void)c;
    return (i32)QB(b)->flags;
}
u64 nvn_QueueBuilderGetMemory(CPU* c, u64 b) {
    (void)c;
    return QB(b)->memory;
}
u64 nvn_QueueBuilderGetMemorySize(CPU* c, u64 b) {
    (void)c;
    return QB(b)->memory_size;
}
/* The queue memory the game allocates, by the game's driver's rule (NVN 55.15's GetQueueMemorySize, read from its
   code): the command and control blocks, and an area of the driver's own. The area holds the fragment interlock
   buffer (1 KiB for each GPC and 4 bytes for each warp of each SM), the driver's state (more with separate samplers
   and textures), the ZCULL context and the compute memory, in blocks of 0x800 bytes (0xD00 with separate samplers).
   The GPU's shape is nvdrv's answers, which the driver reads into its device: GPCs and SMs, warps (characteristics),
   and the ZCULL context size (ZCULL_GET_CTX_SIZE), at the driver's alignment of 4 KiB. The driver's queue fails to
   initialize with less memory */
u64 nvn_QueueBuilderGetQueueMemorySize(CPU* c, u64 b) {
    const NvnQueueBuilder* o = QB(b);
    int separate = (nvn_device_flags() & NVN_DEVICE_FLAG_ENABLE_SEPARATE_SAMPLER_TEXTURE_SUPPORT) != 0;
    u64 block = separate ? 0xD00 : 0x800, area = 0;
    (void)c;
    if (!(o->flags & NVN_QUEUE_BUILDER_FLAGS_NO_FRAGMENT_INTERLOCK))
        area = snx_align_up(((u64)NVGPU_NUM_GPCS << 10) + (u64)NVGPU_SM_WARPS * NVGPU_NUM_SMS * 4, 0x100);
    area += separate ? 0x9400 : 0x7B00;
    if (!(o->flags & NVN_QUEUE_BUILDER_FLAGS_NO_ZCULL) && NVGPU_ZCULL_CTX_SIZE)
        area = snx_align_up(area, 0x1000) + snx_align_up(NVGPU_ZCULL_CTX_SIZE, 0x1000);
    area = snx_align_up(area, 0x100) + o->compute_size / block * block;
    return (u64)o->cmd_size + o->control_size + snx_align_up(area, 0x1000);
}
void nvn_QueueBuilderSetQueueMemory(CPU* c, u64 b, u64 memory, u64 size) {
    NvnQueueBuilder* o = QB(b);
    (void)c;
    o->memory = memory;
    o->memory_size = (u32)size;
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
    nvn_wait_begin();
    nvn_replay_finish(o->replay);
    nvn_wait_end();
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
    nvn_wait_begin();
    nvn_replay_finish(NVN_OBJ(NvnQueue, q)->replay);
    nvn_wait_end();
}
/* the queue's later work waits on the GPU for the sync, so it is done no earlier than the sync signals */
static void wait_sync(NvnQueue* o, const NvnSync* s) { nvn_sync_hold(&o->ready, &o->shown, s); }
int nvn_QueueWaitSync(CPU* c, u64 q, u64 sync) {
    (void)c;
    wait_sync(NVN_OBJ(NvnQueue, q), NVN_OBJ(const NvnSync, sync));
    return 1;
}

/* the recordings go to the GPU thread, their fences promised now */
void nvn_QueueSubmitCommands(CPU* c, u64 q, i32 n, u64 handles) {
    NvnQueue* o = NVN_OBJ(NvnQueue, q);
    (void)c;
    __atomic_add_fetch(&g_nvn_queue.submits, 1, __ATOMIC_RELAXED);
    o->submits++;
    if (n > 0) {
        u64 ready = queue_ready(o);
        nvn_replay_submit(o->replay, (u32)n, handles, &ready, &o->shown);
        o->ready = ready;
    }
}
/* signals when the work submitted before it is replayed and done */
void nvn_QueueFenceSync(CPU* c, u64 q, u64 sync, i32 condition, i32 flags) {
    NvnQueue* o = NVN_OBJ(NvnQueue, q);
    (void)c;
    (void)condition;
    (void)flags;
    nvn_replay_fence(o->replay, sync, queue_ready(o), o->shown);
}
/* A sync's present on the display, its shown value on the presents' syncpoint: waited for until the game's deadline
   (start + left, left < 0 for none), or two frame periods past the sync's ready time, after which the display counts
   as stuck (a covered window) and isn't waited for until it is found to have shown a present again, as present.c's
   pace does for vi. 0 when the game's deadline came first, else 1 (*waited set when it waited) */
static int shown_wait(const NvnSync* s, u64 start, i64 left, int* waited) {
    u32 id = nvn_replay_shown_syncpt();
    u64 now, bound, until;
    i64 wait;
    if (syncpt_wait(id, s->shown, 0)) {
        __atomic_store_n(&g_nvn_queue.stuck, 0, __ATOMIC_RELAXED);
        return 1;
    }
    if (__atomic_load_n(&g_nvn_queue.stuck, __ATOMIC_RELAXED)) return 1;
    now = plat_ns();
    bound = (s->ready > now ? s->ready : now) + 2 * __atomic_load_n(&g_nvn_queue.period, __ATOMIC_RELAXED);
    until = left >= 0 && start + (u64)left < bound ? start + (u64)left : bound;
    wait = until > now ? (i64)(until - now) : 0;
    if (wait && syncpt_wait(id, s->shown, wait)) {
        *waited = 1;
        return 1;
    }
    if (until < bound) return 0;
    __atomic_store_n(&g_nvn_queue.stuck, 1, __ATOMIC_RELAXED);
    LOG_LIMIT(8, "nvn: the display didn't show a frame in two periods: frames don't wait for it until it does\n");
    return 1;
}
/* A fenced sync signals once its syncpoint reaches its value (the replay reached it), the present it waits for is on
   the display (shown_wait) and the pacer's clock reaches its ready time: the wait blocks until all three, or for the
   timeout (nanoseconds, ~0 for good). One never fenced waits out its timeout, as on the console */
static i32 sync_wait(CPU* c, u64 sync, u64 timeout) {
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
    if (s->shown) {
        now = plat_ns();
        if (left >= 0) left = (u64)left > now - start ? left - (i64)(now - start) : 0;
        start = now;
        if (!shown_wait(s, start, left, &waited)) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
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
/* off the guest core while it waits (nvn_wait_begin); a poll (timeout 0) never waits */
i32 nvn_SyncWait(CPU* c, u64 sync, u64 timeout) {
    i32 r;
    if (!timeout) return sync_wait(c, sync, timeout);
    nvn_wait_begin();
    r = sync_wait(c, sync, timeout);
    nvn_wait_end();
    return r;
}

/* ============================================================ window */
#define WB(b) NVN_OBJ(NvnWindowBuilder, b)
void nvn_WindowBuilderSetDefaults(CPU* c, u64 b) {
    (void)c;
    memset(GP(b), 0, sizeof(NvnWindowBuilder));
    WB(b)->interval = 1;
}
/* the device and the native window: one each, which SDK 5's 0x2c bytes of builder have no room for. The runtime has
   one device (nvn_device), and the native window is the last one a builder named (NvnQueueState) */
void nvn_WindowBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    (void)b;
    (void)d;
}
void nvn_WindowBuilderSetNativeWindow(CPU* c, u64 b, u64 w) {
    (void)c;
    (void)b;
    __atomic_store_n(&g_nvn_queue.native_window, w, __ATOMIC_RELAXED);
}
u64 nvn_WindowBuilderGetDevice(CPU* c, u64 b) {
    (void)c;
    (void)b;
    return nvn_device();
}
u64 nvn_WindowBuilderGetNativeWindow(CPU* c, u64 b) {
    (void)c;
    (void)b;
    return __atomic_load_n(&g_nvn_queue.native_window, __ATOMIC_RELAXED);
}
void nvn_WindowBuilderSetTextures(CPU* c, u64 b, i32 n, u64 textures) {
    NvnWindowBuilder* o = WB(b);
    i32 i;
    (void)c;
    if (n < 0 || n > 4) {
        LOG("nvn: a window of %d textures: nvn_direct keeps 4\n", n);
        n = n < 0 ? 0 : 4;
    }
    o->count = (i16)n;
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
/* the textures the window takes in turn: its first n (0 or more than it has: all of them) */
void nvn_WindowBuilderSetNumActiveTextures(CPU* c, u64 b, i32 n) {
    (void)c;
    WB(b)->active = (i16)(n < 0 ? 0 : n > 0x7fff ? 0x7fff : n);
}
i32 nvn_WindowBuilderGetNumActiveTextures(CPU* c, u64 b) {
    (void)c;
    return WB(b)->active;
}
/* the textures in turn from the next acquire on, as the game's driver tells its native window */
void nvn_WindowSetNumActiveTextures(CPU* c, u64 w, i32 n) {
    (void)c;
    NVN_OBJ(NvnWindow, w)->active = n;
}
/* the textures the window takes in turn: the active ones, all when SetNumActiveTextures named none or too many */
static i32 window_active(const NvnWindow* o) { return o->active > 0 && o->active < o->count ? o->active : o->count; }
i32 nvn_WindowGetNumActiveTextures(CPU* c, u64 w) {
    (void)c;
    return window_active(NVN_OBJ(const NvnWindow, w));
}
i32 nvn_WindowGetNumTextures(CPU* c, u64 w) {
    (void)c;
    return NVN_OBJ(const NvnWindow, w)->count;
}
u64 nvn_WindowGetNativeWindow(CPU* c, u64 w) {
    (void)c;
    return NVN_OBJ(const NvnWindow, w)->native;
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
    o.active = s->active;
    o.native = __atomic_load_n(&g_nvn_queue.native_window, __ATOMIC_RELAXED);
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
/* A frame's time on the display: the window's interval in refreshes (0: no wait), or what fps_limit says. With the
   game's own pace, where the renderer learns when frames are displayed (renderer.h display_wait: a display clock,
   FIFO, frame_pacing on), the period goes onto the display's refresh as vi's does (present_display_period), and
   *paced says the window's textures also wait for the display to show the present after them */
static u64 present_period(const NvnWindow* o, int* paced) {
    int fps = __atomic_load_n(&g_opt_fps, __ATOMIC_RELAXED);
    u64 period, refresh = 0;
    *paced = 0;
    if (!fps) return 0;
    if (fps != SNX_FPS_GAME) return 1000000000ull / (u64)fps;
    if (o->interval <= 0) return 0;
    period = (u64)o->interval * 1000000000ull / VI_REFRESH_HZ;
    /* asked without waiting: whether the renderer can tell, and the refresh */
    if (!g_renderer->display_wait || g_renderer->display_wait(0, &refresh) < 0) return period;
    *paced = 1;
    return present_display_period(period, refresh);
}
/* The next texture in turn, at once: the sync says when it is free, once the present that took it off the display is
   replayed, one period after its own present's slot, and once the display has taken the frames presented before, and
   the game waits for that on the CPU (SyncWait) or the GPU (QueueWaitSync), as on the console, where the display lets
   a texture go when the next one is shown, after the work before that present (one game waits for it before it
   records over its last frame's command memory). A game that renders a frame ahead keeps doing so, and the time it
   measures for a frame is its own work, not the display's pace.

   The renderer copies a texture to the display's own image as it presents it, so the display holds none of the
   game's: what the display decides is the pace. The frame is released once the frames presented before it are
   displayed but the last, so one is ready behind the display and none queues behind another, as vi's present_pace
   keeps it where frames are shown as they come. Where they are held until their refresh (frame_pacing 2), vi's pace
   waits for the last too, but a game whose frame takes it longer than a period from its release to its present (one
   that waits for its earlier frames' fences within the frame) then lost a refresh most frames: with the last frame
   left out, a frame has two periods to get to its refresh, and is held there.

   The one wait in AcquireTexture is for a texture whose last present's slot hasn't come yet: the game's driver
   takes the texture from the display's buffer queue there, which never hands back a buffer the display hasn't taken,
   so a game is never more than its window's textures ahead of the display. A game that waits for its syncs isn't
   held there (one that renders a frame ahead acquires about a period before the texture's sync signals, after its
   present's slot), but one whose GPU alone waits for them (nvnCommandBufferWaitSync), and which never waits for the
   GPU on the CPU, ran on without it: one game presented 141 frames a second with an interval of 1, its presents'
   slots a minute ahead of the clock. Only the pacer's slot is waited for: the display's showing of the frame is the
   sync's (on_display), which such a game's queue carries to its fences */
static void slot_wait(const NvnWindow* o, i32 i) {
    u64 now = plat_ns();
    if (o->shown_at[i] <= now) return;
    nvn_wait_begin();
    plat_sleep_ns((i64)(o->shown_at[i] - now));
    nvn_wait_end();
}
static void acquire(NvnWindow* o, NvnSync* s, u64 index) {
    i32 n = window_active(o), i = n ? o->next % n : 0;
    o->next = n ? (i + 1) % n : 0;
    gw32(index, (u32)i);
    if (i < 4) {
        slot_wait(o, i);
        u64 now = plat_ns();
        o->released[i] = o->available[i] > now ? o->available[i] : now;
        fence_at(s, o->syncpt[i], o->value[i], o->on_display[1], o->available[i]);
    } else
        fence_at(s, 0, 0, 0, 0);
}
i32 nvn_WindowAcquireTexture(CPU* c, u64 w, u64 sync, u64 index) {
    NvnSync none = {0};
    (void)c;
    acquire(NVN_OBJ(NvnWindow, w), sync ? NVN_OBJ(NvnSync, sync) : &none, index);
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
/* as the game's driver does it: the window's acquire fences a sync of the driver's own, which the queue then waits for
   (QueueWaitSync), its later work held back until the texture comes free */
i32 nvn_QueueAcquireTexture(CPU* c, u64 q, u64 w, u64 index) {
    NvnSync s = {0};
    (void)c;
    acquire(NVN_OBJ(NvnWindow, w), &s, index);
    wait_sync(NVN_OBJ(NvnQueue, q), &s);
    return NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_SUCCESS;
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
    NvnQueue* queue = NVN_OBJ(NvnQueue, q);
    u64 tex = index >= 0 && index < 4 && index < o->count ? o->texture[index] : 0;
    NvnReplayPresent p;
    NvnTexture t;
    u32 value, shown;
    int paced;
    u64 period = present_period(o, &paced), slot;
    (void)c;
    memset(&p, 0, sizeof p);
    if (o->crop[2] > 0 && o->crop[3] > 0) {
        p.crop[0] = o->crop[0];
        p.crop[1] = o->crop[1];
        p.crop[2] = o->crop[0] + o->crop[2];
        p.crop[3] = o->crop[1] + o->crop[3];
    }
    if (tex) memcpy(&t, GP(tex), sizeof t);
    if (tex && t.magic == NVN_MAGIC('T', 'E', 'X', 0)) {
        p.fmt = vi_format(t.d.format);
        if (!p.fmt) LOG_LIMIT(1, "nvn: a window texture of format %#x, which vi doesn't show\n", t.d.format);
    }
    if (p.fmt) { /* else nothing vi shows: black */
        unsigned th = 0, td = 0;
        nvn_tex_level_tiles(&t.d, 0, &th, &td);
        p.gva = t.gpu;
        p.w = t.d.width;
        p.h = t.d.height;
        p.linear = (t.d.flags & NVN_TEX_LINEAR) != 0;
        p.bhl = th;
        p.pitch = t.d.stride ? t.d.stride : t.d.width * 4;
    }
    /* the game's pace for the renderer's holds, with interval 0 or fps_limit none, as vi's present_pace gives it */
    p.period_ns = __atomic_load_n(&g_opt_fps, __ATOMIC_RELAXED) == SNX_FPS_GAME ? period : 0;
    p.released_ns = index >= 0 && index < 4 ? o->released[index] : 0;
    value = nvn_replay_present(queue->replay, &p, &shown);
    o->on_display[1] = o->on_display[0];
    o->on_display[0] = paced ? shown : 0;
    if (index >= 0 && index < 4) {
        u32 id = nvn_replay_syncpt(queue->replay);
        int i;
        /* this present takes the texture shown before off the display, and shows this one */
        for (i = 0; i < 4; i++)
            if (i == index || ((o->shown >> i) & 1)) {
                o->syncpt[i] = id;
                o->value[i] = value;
            }
        o->shown = 1u << index;
    }
    if (!o->presents++) LOG("nvn: first present, texture %d of window %llx\n", index, (unsigned long long)w);
    if (!(o->presents % 1800))
        LOG("nvn: %llu presents, %llu submissions\n", (unsigned long long)o->presents,
            (unsigned long long)__atomic_load_n(&g_nvn_queue.submits, __ATOMIC_RELAXED));
    __atomic_store_n(&g_nvn_queue.period, period, __ATOMIC_RELAXED);
    slot = pace(o, queue_ready(queue), period);
    if (index >= 0 && index < 4) {
        o->available[index] = period ? slot + period : 0;
        o->shown_at[index] = period ? slot : 0;
    }
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
}

/* ============================================================ window getters: what was set */
u64 nvn_WindowBuilderGetTexture(CPU* c, u64 b, i32 i) {
    (void)c;
    return i >= 0 && i < WB(b)->count && i < 4 ? WB(b)->texture[i] : 0;
}
i32 nvn_WindowBuilderGetNumTextures(CPU* c, u64 b) {
    (void)c;
    return WB(b)->count;
}
i32 nvn_WindowBuilderGetPresentInterval(CPU* c, u64 b) {
    (void)c;
    return WB(b)->interval;
}
i32 nvn_WindowGetPresentInterval(CPU* c, u64 w) {
    (void)c;
    return NVN_OBJ(const NvnWindow, w)->interval;
}
/* into an NVNrectangle: x, y, width, height */
void nvn_WindowGetCrop(CPU* c, u64 w, u64 rect) {
    const NvnWindow* o = NVN_OBJ(const NvnWindow, w);
    int i;
    (void)c;
    for (i = 0; i < 4; i++) gw32(rect + 4 * (u64)i, (u32)o->crop[i]);
}
/* The queue's own memory used since ResetMemoryUsageCounts (the game's driver's command, control and compute memory,
   which it writes its submissions' glue and compute state into): the replay writes none of the queue memory, so it
   used none of each */
u64 nvn_QueueGetTotalCommandMemoryUsed(CPU* c, u64 q) {
    (void)c;
    (void)q;
    return 0;
}
u64 nvn_QueueGetTotalControlMemoryUsed(CPU* c, u64 q) {
    (void)c;
    (void)q;
    return 0;
}
u64 nvn_QueueGetTotalComputeMemoryUsed(CPU* c, u64 q) {
    (void)c;
    (void)q;
    return 0;
}
void nvn_QueueResetMemoryUsageCounts(CPU* c, u64 q) {
    (void)c;
    (void)q;
}
/* no error: the replay's work has no channel of the GPU's that could fault (NVNqueueErrorInfo is only for errors) */
i32 nvn_QueueGetError(CPU* c, u64 q, u64 info) {
    (void)c;
    (void)q;
    (void)info;
    return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
