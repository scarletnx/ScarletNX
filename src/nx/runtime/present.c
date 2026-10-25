/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vi's buffers on the display, and the game's pace: how long vi holds back the game's next buffer */
#include "nx/runtime/main.h"
#include "nx/runtime/cheats.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/vi.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"

int g_frames;

/* when each of vi's slots was last handed to the game (present_pace), for the display times the renderer logs: the
   game's threads write it as the dequeue returns, the GPU thread reads it at the slot's present (atomic) */
static u64 g_released_ns[VI_SLOTS];

void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform, int slot) {
    u64 a = nvmap_addr_by_id(nvmap_id);
    if (!a) {
        WARN_LIMIT(5, "vi: buffer of nvmap id %u has no memory, not shown\n", nvmap_id);
        return;
    }
    /* the flips are the display's: the guest's buffer is never turned */
    g_renderer->present_buffer(a + offset, w, h, pitch, layout, bhl, fmt, crop, transform,
                               slot >= 0 && slot < VI_SLOTS ? __atomic_load_n(&g_released_ns[slot], __ATOMIC_RELAXED)
                                                            : 0);
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
    cheats_frame(); /* the cheats run once per frame shown, as dmnt's run on the console */
    SNX_FRAME_MARK();
}

/* Several threads may queue frames, so the schedule is under a lock; the wait itself is outside it. Called on the
   game's thread from vi's DequeueBuffer: the GPU thread never waits for the display. As Ryujinx's SurfaceFlinger
   paces a swap interval of 0, the game's pace with interval 0 does not wait at all */
static PMutex g_pace_lock = PMUTEX_INIT;
static u64 g_pace_last; /* when the frame before was due, 0 before the first */

static void released(int slot) {
    if (slot >= 0 && slot < VI_SLOTS) __atomic_store_n(&g_released_ns[slot], plat_ns(), __ATOMIC_RELAXED);
}

void present_pace(u32 interval, int slot) {
    int fps = __atomic_load_n(&g_opt_fps, __ATOMIC_RELAXED);
    u64 refresh, period, now, next;
    if (!fps || (fps == SNX_FPS_GAME && !interval)) {
        released(slot);
        return;
    }
    refresh = 1000000000u / (fps == SNX_FPS_GAME ? (u64)VI_REFRESH_HZ : (u64)fps);
    period = fps == SNX_FPS_GAME ? refresh * interval : refresh;
    pm_lock(&g_pace_lock);
    now = plat_ns();
    next = g_pace_last + period;
    /* a schedule more than a refresh behind (a load, a pause) starts again from now: no burst to catch up */
    if (!g_pace_last || now > next + refresh) next = now;
    g_pace_last = next;
    pm_unlock(&g_pace_lock);
    if (next > now) plat_sleep_ns((i64)(next - now));
    released(slot);
}
