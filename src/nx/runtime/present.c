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
#include <stdatomic.h>

_Atomic int g_frames;
_Atomic int g_vi_refresh_hz = VI_REFRESH_HZ;

/* the game's pace (present_pace). Several threads may queue frames, so the schedule is under a lock */
static struct {
    PMutex lock;
    u64 last;          /* when the frame before was due, 0 before the first */
    _Atomic int display_stuck; /* atomic: the display didn't take the frames in time: they aren't waited for */
    /* when each of vi's slots was last handed to the game, for the display times the renderer logs, and the game's
       pace for its frame, 0 none (renderer.h present_buffer): the game's threads write them as the dequeue returns,
       the GPU thread reads them at the slot's present (atomic) */
    _Atomic u64 released_ns[VI_SLOTS], period_ns[VI_SLOTS];
} g_pace = {.lock = PMUTEX_INIT};

void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform, int slot) {
    u64 a = nvmap_addr_by_id(nvmap_id);
    if (!a) {
        SNX_ZONE_TV(z_drop, "runtime: present dropped", nvmap_id, "nvmap %u has no memory, slot %d", nvmap_id, slot);
        WARN_LIMIT(5, "vi: buffer of nvmap id %u has no memory, not shown\n", nvmap_id);
        return;
    }
    /* the flips are the display's: the guest's buffer is never turned */
    int known = slot >= 0 && slot < VI_SLOTS;
    g_renderer->present_buffer(a + offset, w, h, pitch, layout, bhl, fmt, crop, transform,
                               known ? atomic_load_explicit(&g_pace.released_ns[slot], memory_order_relaxed) : 0,
                               known ? atomic_load_explicit(&g_pace.period_ns[slot], memory_order_relaxed) : 0, 0);
    atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
    cheats_frame(); /* the cheats run once per frame shown, as dmnt's run on the console */
    SNX_FRAME_MARK();
    SNX_FRAME_MARK_NAMED("present"); /* the presented frames as their own continuous set */
}

/* The schedule is under g_pace's lock, the wait itself outside it. Called on the game's thread from vi's
   DequeueBuffer: the GPU thread never waits for the display. As Ryujinx's SurfaceFlinger paces a swap interval of 0,
   the game's pace with interval 0 does not wait at all.

   Where the renderer knows when frames are displayed (renderer.h display_wait, the frame_pacing setting), the game's
   pace also goes by the display: a frame is released once the game's frames presented before it have been displayed
   (all but one where the display doesn't hold them until their refresh), so none queues for the display behind
   another, and the timer's period follows a near-identical display clock (a 59.94 Hz one). Incompatible refresh
   ratios keep the game's period instead of rounding its frame rate down. A display that stops taking frames leaves
   the timer alone until it takes them again */

u64 present_display_period(u64 period, u64 refresh) {
    u64 n, d;
    if (!refresh) return period;
    n = (period + refresh / 2) / refresh;
    d = n * refresh;
    /* Allow clock drift, including 60/1.001 Hz, rather than a different frame rate. A 350 Hz display rounds a
       30 Hz period to twelve refreshes (29.17 Hz); the old 1/32 tolerance silently applied that slower pace. */
    return n && (d > period ? d - period : period - d) <= period / 512 ? d : period;
}

static void released(int slot, u64 period) {
    if (slot < 0 || slot >= VI_SLOTS) return;
    atomic_store_explicit(&g_pace.released_ns[slot], plat_ns(), memory_order_relaxed);
    atomic_store_explicit(&g_pace.period_ns[slot], period, memory_order_relaxed);
}

void present_pace(u32 interval, int slot) {
    int fps = atomic_load_explicit(&g_opt_fps, memory_order_relaxed);
    u64 refresh, period, now, next;
    int reset = 0; /* why the schedule started again: 0 none, 1 first, 2 behind, 3 late */
    if (!fps || (fps == SNX_FPS_GAME && !interval)) {
        /* value: 0 fps off, 1 game fps interval 0 */
        SNX_ZONE_TV(z_free, "runtime: present free", !fps ? 0 : 1, "%s", !fps ? "fps off" : "game fps interval 0");
        released(slot, 0);
        return;
    }
    refresh = 1000000000u / (fps == SNX_FPS_GAME ? (u64)vi_refresh_hz() : (u64)fps);
    period = fps == SNX_FPS_GAME ? refresh * interval : refresh;
    [[maybe_unused]] u64 started = snx_tracy_attached() ? plat_ns() : 0; /* a plot's alone */
    if (fps == SNX_FPS_GAME && g_renderer->display_wait) {
        int stuck = atomic_load_explicit(&g_pace.display_stuck, memory_order_relaxed);
        u64 shown = 0, start = plat_ns();
        /* two periods at most, or none while the display is stuck: only whether it took the frames since */
        int r = g_renderer->display_wait(stuck ? start : start + 2 * period, &shown);
        if (r >= 0) atomic_store_explicit(&g_pace.display_stuck, !r, memory_order_relaxed);
        if (r > 0 && shown) {
            u64 d = present_display_period(period, shown);
            if (d != period) {
                SNX_ZONE_TV(z_refresh, "runtime: present refresh", (u64)shown, "%llu Hz display, period %llu ns",
                            (unsigned long long)shown, (unsigned long long)d);
                refresh = shown;
                period = d;
            }
        } else {
            /* value: 1 no frame, 2 the display wait failed */
            SNX_ZONE_TV(z_stuck, "runtime: present delayed", r < 0 ? 2 : 1, "%s",
                        r < 0 ? "display wait failed" : "display took no frame");
        }
    }
    pm_lock(&g_pace.lock);
    now = plat_ns();
    next = g_pace.last + period;
    /* a schedule more than a refresh behind (a load, a pause) starts again from now: no burst to catch up. A numeric
       fps_limit is a ceiling, so a frame already late starts it again at once too: the frames after it were let
       through until the schedule caught up, and presents went above the limit */
    if (!g_pace.last)
        reset = 1;
    else if (now > next + refresh)
        reset = 2; /* a load or a pause left the schedule more than a refresh behind */
    else if (fps != SNX_FPS_GAME && now >= next)
        reset = 3; /* a numeric fps is a ceiling: a frame already late starts it again */
    if (reset) next = now;
    g_pace.last = next;
    pm_unlock(&g_pace.lock);
    if (next > now) {
        SNX_PLOT("rt present sleep ms", (double)(next - now) / 1e6);
        plat_sleep_ns((i64)(next - now)); /* the wait stays outside the zone below */
    }
    SNX_ZONE_TV(z_sched, "runtime: present schedule", (u64)reset, "%s",
                reset == 1 ? "first" : reset == 2 ? "behind" : reset == 3 ? "late" : "steady");
    SNX_PLOT("present pace ms", (double)(plat_ns() - started) / 1e6);
    released(slot, fps == SNX_FPS_GAME ? period : 0);
}
