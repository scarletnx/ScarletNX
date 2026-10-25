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

/* the game's pace (present_pace). Several threads may queue frames, so the schedule is under a lock */
static struct {
    PMutex lock;
    u64 last;          /* when the frame before was due, 0 before the first */
    int display_stuck; /* atomic: the display didn't take the frames in time: they aren't waited for */
    /* when each of vi's slots was last handed to the game, for the display times the renderer logs, and the game's
       pace for its frame, 0 none (renderer.h present_buffer): the game's threads write them as the dequeue returns,
       the GPU thread reads them at the slot's present (atomic) */
    u64 released_ns[VI_SLOTS], period_ns[VI_SLOTS];
} g_pace = {.lock = PMUTEX_INIT};

void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform, int slot) {
    u64 a = nvmap_addr_by_id(nvmap_id);
    if (!a) {
        WARN_LIMIT(5, "vi: buffer of nvmap id %u has no memory, not shown\n", nvmap_id);
        return;
    }
    /* the flips are the display's: the guest's buffer is never turned */
    int known = slot >= 0 && slot < VI_SLOTS;
    g_renderer->present_buffer(a + offset, w, h, pitch, layout, bhl, fmt, crop, transform,
                               known ? __atomic_load_n(&g_pace.released_ns[slot], __ATOMIC_RELAXED) : 0,
                               known ? __atomic_load_n(&g_pace.period_ns[slot], __ATOMIC_RELAXED) : 0);
    __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
    cheats_frame(); /* the cheats run once per frame shown, as dmnt's run on the console */
    SNX_FRAME_MARK();
}

/* The schedule is under g_pace's lock, the wait itself outside it. Called on the game's thread from vi's
   DequeueBuffer: the GPU thread never waits for the display. As Ryujinx's SurfaceFlinger paces a swap interval of 0,
   the game's pace with interval 0 does not wait at all.

   Where the renderer knows when frames are displayed (renderer.h display_wait, the frame_pacing setting), the game's
   pace also goes by the display: a frame is released once the game's frames presented before it have been displayed
   (all but one where the display doesn't hold them until their refresh), so none queues for the display behind
   another, and the timer's period is the display's refresh times the refreshes nearest the swap interval's, so it
   doesn't drift from the display (a 59.94 Hz one). A display that stops taking frames (a window covered) leaves the
   timer alone until it takes them again */

static void released(int slot, u64 period) {
    if (slot < 0 || slot >= VI_SLOTS) return;
    __atomic_store_n(&g_pace.released_ns[slot], plat_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_pace.period_ns[slot], period, __ATOMIC_RELAXED);
}

void present_pace(u32 interval, int slot) {
    int fps = __atomic_load_n(&g_opt_fps, __ATOMIC_RELAXED);
    u64 refresh, period, now, next;
    if (!fps || (fps == SNX_FPS_GAME && !interval)) {
        released(slot, 0);
        return;
    }
    refresh = 1000000000u / (fps == SNX_FPS_GAME ? (u64)VI_REFRESH_HZ : (u64)fps);
    period = fps == SNX_FPS_GAME ? refresh * interval : refresh;
    if (fps == SNX_FPS_GAME && g_renderer->display_wait) {
        int stuck = __atomic_load_n(&g_pace.display_stuck, __ATOMIC_RELAXED);
        u64 shown = 0, start = plat_ns();
        /* two periods at most, or none while the display is stuck: only whether it took the frames since */
        int r = g_renderer->display_wait(stuck ? start : start + 2 * period, &shown);
        if (r >= 0) __atomic_store_n(&g_pace.display_stuck, !r, __ATOMIC_RELAXED);
        if (r > 0 && shown) {
            u64 n = (period + shown / 2) / shown, d = n * shown;
            if (n && (d > period ? d - period : period - d) <= period / 32) {
                refresh = shown;
                period = d;
            }
        }
    }
    pm_lock(&g_pace.lock);
    now = plat_ns();
    next = g_pace.last + period;
    /* a schedule more than a refresh behind (a load, a pause) starts again from now: no burst to catch up */
    if (!g_pace.last || now > next + refresh) next = now;
    g_pace.last = next;
    pm_unlock(&g_pace.lock);
    if (next > now) plat_sleep_ns((i64)(next - now));
    released(slot, fps == SNX_FPS_GAME ? period : 0);
}
