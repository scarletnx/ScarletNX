/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The display's clock: when each present reached the display, and the refresh each game frame is held for.

   Every present carries an id, and a thread of this file's learns when each is displayed: with VK_KHR_present_wait
   (or its second version, which a swapchain made for VK_EXT_present_timing takes), or from VK_GOOGLE_display_timing's
   past presentation timings (Android). Both take the swapchain externally synchronized, as the GPU thread's acquire
   and the swapchain's create and destroy do (g_vk.sc_lock), so the thread never blocks in them, which would hold the
   next acquire for up to a refresh: it asks without a timeout, from a little before the present is due, every
   POLL_NS. A present wait's display time is known to that, a past timing's exactly.

   Where the swapchain can hold a present until a time (VK_EXT_present_timing's presentAtAbsoluteTime, or
   VK_GOOGLE_display_timing's desired present time), each game frame is held for its refresh on the grid the display
   times give (vk_display_target): FIFO alone shows a frame at the first refresh after it is ready, so a 30 fps game
   on a 60 Hz display had frames shown for one refresh or three wherever its time to draw one varied.

   The game's pace (renderer.h display_wait, runtime/present.c) waits here for the display to take its frames, and
   every STATS game frames the log says how evenly they were displayed and how long they took to get there */
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DQ         64           /* presents not known displayed yet: one more is let go unwatched */
#define POLL_NS    250000ull    /* the polling step */
#define EARLY_NS   1500000ull   /* polling for a present starts this long before it is due */
#define GIVE_UP_NS 500000000ull /* a present not displayed in this long (a covered window on Wayland) is let go */
#define STATS      600          /* game frames a line of the log covers */
#define PAST       16           /* VK_GOOGLE_display_timing's past timings kept for presents not yet looked for */

typedef struct {
    VkSwapchainKHR sc;
    u64 id;
    u64 presented_ns; /* when show() queued it */
    u64 released_ns;  /* when the game began the frame (present.c), 0 unknown */
    u64 due;          /* the refresh it is held until (vk_display_target, plat_ns), 0 none */
    u64 seq;          /* a game frame's number among them */
    int game;         /* a frame of the game's, not the menu's or a black screen's */
    int clock;        /* the swapchain's VkClock: how the display time is learned */
    int made;         /* under lock: 0 not yet presented (vk_display_made), 1 presented, -1 the present failed */
} DPresent;

static struct {
    PMutex lock;
    PCond work;  /* a present queued, for the thread */
    PCond shown; /* presents displayed, for display_wait */
    DPresent q[DQ];
    u32 head, tail;
    u64 next_id; /* ids grow across swapchains, as each swapchain only needs them to grow */
    u64 handed;  /* game frames presented with an id */
    u64 done;    /* of those, displayed or let go */
    int started; /* atomic: the thread runs */
    int fifo;    /* atomic: the swapchain presents in FIFO order, a frame each refresh */
    /* the last game frame displayed with its time known to a step, and its number (under lock): the grid of
       refreshes the targets keep to, and where the frames after it can be displayed at the earliest */
    u64 grid_ns, grid_seq;
    /* under lock: game frames presented, and the refresh the last was due at (0 none) */
    u64 seq, due_last;
    u64 due_next;      /* the GPU thread's: the refresh the game frame being presented is held for, 0 none */
    u64 dropped, late; /* atomic: game frames not shown, and held for a later refresh than their period's, since the
                          stats line before */
    int held;          /* under lock: the last game frame presented was held until its refresh */
    /* the thread's own: the last game frame's display time (0 none, or not known to a step), the stats */
    u64 last_game_ns, last_ns;
    /* the thread's own: VK_GOOGLE_display_timing's timings of past_sc's presents not yet looked for, in order */
    VkSwapchainKHR past_sc;
    u32 npast;
    VkPastPresentationTimingGOOGLE past[PAST];
    u32 n, nlat, npres, ndrawn;
    u64 interval[STATS], latency[STATS], present[STATS], drawn[STATS];
} g_dc = {.lock = PMUTEX_INIT};

static int u64_cmp(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return x < y ? -1 : x > y;
}
static double ms(u64 ns) { return (double)ns / 1e6; }
/* n times sorted: their mean, 1st and 99th percentiles into out[0..2] (zeros for none) */
static void pct(u64* v, u32 n, u64 out[4]) {
    u64 sum = 0;
    out[0] = out[1] = out[2] = 0;
    if (!n) return;
    qsort(v, n, sizeof *v, u64_cmp);
    for (u32 i = 0; i < n; i++) sum += v[i];
    out[0] = sum / n;
    out[1] = v[n / 100];
    out[2] = v[n - 1 - n / 100];
}

/* every STATS game frames: their display times' spacing, and the ones held for another number of refreshes than
   most (a frame shown twice, or one dropped); the time from the game's release of a frame to its display, and from
   its present to its display */
static void stats_line(void) {
    u64 refresh = backend_display_refresh_ns(), sum = 0, sum2 = 0, mean = 0;
    u32 uneven = 0, most = 0;
    char held[96] = "";
    if (g_dc.n) {
        u32 count[8] = {0};
        for (u32 i = 0; i < g_dc.n; i++) sum += g_dc.interval[i];
        mean = sum / g_dc.n;
        for (u32 i = 0; i < g_dc.n; i++) {
            i64 d = (i64)(g_dc.interval[i] - mean);
            if (d > 1000000000 || d < -1000000000) d = 1000000000; /* a pause: kept from overflowing the sum */
            sum2 += (u64)(d * d / 1000);
        }
        if (refresh) {
            for (u32 i = 0; i < g_dc.n; i++) {
                u64 k = (g_dc.interval[i] + refresh / 2) / refresh;
                count[k < 8 ? k : 7]++;
            }
            for (u32 k = 1; k < 8; k++)
                if (count[k] > count[most]) most = k;
            uneven = g_dc.n - count[most];
            snprintf(held, sizeof held, ", %u held for other than %u refresh%s", uneven, most, most == 1 ? "" : "es");
        }
        qsort(g_dc.interval, g_dc.n, sizeof *g_dc.interval, u64_cmp);
    }
    u64 lat[4], pres[4], drawn[4];
    pct(g_dc.latency, g_dc.nlat, lat);
    pct(g_dc.present, g_dc.npres, pres);
    pct(g_dc.drawn, g_dc.ndrawn, drawn);
    LOG("vulkan: display: %u frame intervals of %.2f ms (deviation %.2f ms, 1st to 99th percentile %.2f to %.2f "
        "ms)%s; the game's frames went from release to present in %.1f ms (%.1f to %.1f), on to the display in %.1f "
        "ms (%.1f to %.1f), %.1f ms in all (%.1f to %.1f)\n",
        g_dc.n, ms(mean), g_dc.n ? sqrt((double)sum2 / g_dc.n * 1000) / 1e6 : 0.0,
        g_dc.n ? ms(g_dc.interval[g_dc.n / 100]) : 0.0, g_dc.n ? ms(g_dc.interval[g_dc.n - 1 - g_dc.n / 100]) : 0.0,
        held, ms(drawn[0]), ms(drawn[1]), ms(drawn[2]), ms(pres[0]), ms(pres[1]), ms(pres[2]), ms(lat[0]), ms(lat[1]),
        ms(lat[2]));
    LOG("vulkan: display: %llu frames not shown (no image free), %llu held for a later refresh than their period's\n",
        (unsigned long long)__atomic_exchange_n(&g_dc.dropped, 0, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_exchange_n(&g_dc.late, 0, __ATOMIC_RELAXED));
    g_dc.n = g_dc.nlat = g_dc.npres = g_dc.ndrawn = 0;
}

/* p displayed at t (exact to a step when exact): the game's pace may go on, and the stats take it */
static void displayed(const DPresent* p, u64 t, int exact) {
    if (p->game) {
        if (exact && g_dc.last_game_ns && g_dc.n < STATS) g_dc.interval[g_dc.n++] = t - g_dc.last_game_ns;
        g_dc.last_game_ns = exact ? t : 0;
        if (exact && p->released_ns && t > p->released_ns && g_dc.nlat < STATS)
            g_dc.latency[g_dc.nlat++] = t - p->released_ns;
        if (exact && t > p->presented_ns && g_dc.npres < STATS) g_dc.present[g_dc.npres++] = t - p->presented_ns;
        if (p->released_ns && p->presented_ns > p->released_ns && g_dc.ndrawn < STATS)
            g_dc.drawn[g_dc.ndrawn++] = p->presented_ns - p->released_ns;
        if (g_dc.n + 1 >= STATS || g_dc.npres >= STATS) stats_line();
    }
    g_dc.last_ns = t;
    pm_lock(&g_dc.lock);
    if (p->game && exact) {
        g_dc.grid_ns = t;
        g_dc.grid_seq = p->seq;
    }
    g_dc.tail++;
    if (p->game) g_dc.done++;
    pc_broadcast(&g_dc.shown);
    pm_unlock(&g_dc.lock);
}

/* p's display time from VK_GOOGLE_display_timing (sc_lock held): VK_SUCCESS with *at (plat_ns), VK_TIMEOUT while not
   known, VK_ERROR_OUT_OF_DATE_KHR when the timings went on past it without it (not shown) or can't be had */
static VkResult past_timing(const DPresent* p, u64* at) {
    u32 id = (u32)p->id, n = PAST - g_dc.npast;
    if (g_dc.past_sc != p->sc) {
        g_dc.past_sc = p->sc;
        g_dc.npast = 0;
        n = PAST;
    }
    if (n) {
        VkResult r = vkGetPastPresentationTimingGOOGLE(g_vk.dev, p->sc, &n, g_dc.past + g_dc.npast);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) return VK_ERROR_OUT_OF_DATE_KHR;
        g_dc.npast += n;
    }
    for (u32 i = 0; i < g_dc.npast; i++) {
        i32 d = (i32)(g_dc.past[i].presentID - id);
        if (d < 0) continue; /* an earlier present's, unwatched */
        /* the timings before it are done with */
        VkPastPresentationTimingGOOGLE t = g_dc.past[i];
        g_dc.npast -= i + (d == 0);
        memmove(g_dc.past, g_dc.past + i + (d == 0), sizeof *g_dc.past * g_dc.npast);
        if (d) return VK_ERROR_OUT_OF_DATE_KHR;
        /* CLOCK_MONOTONIC's time, onto plat_ns's */
        u64 mono = plat_monotonic_ns(), now = plat_ns();
        *at = t.actualPresentTime <= mono ? now - (mono - t.actualPresentTime) : now;
        return VK_SUCCESS;
    }
    g_dc.npast = 0;
    return VK_TIMEOUT;
}

static void display_thread(void* arg) {
    (void)arg;
    for (;;) {
        DPresent p;
        pm_lock(&g_dc.lock);
        while (g_dc.head == g_dc.tail) pc_wait(&g_dc.work, &g_dc.lock, -1);
        p = g_dc.q[g_dc.tail % DQ];
        pm_unlock(&g_dc.lock);
        /* due a refresh after the present before it, or as it is queued, or at the refresh it is held until: polled
           from a little before */
        u64 refresh = backend_display_refresh_ns(), due = p.presented_ns, now = plat_ns(), failed = 0;
        if (refresh && g_dc.last_ns + refresh > due) due = g_dc.last_ns + refresh;
        if (p.due > due + refresh / 4) due = p.due - refresh / 4; /* the display times can come a little early */
        if (due > now + EARLY_NS) plat_sleep_ns((i64)(due - EARLY_NS - now));
        for (;;) {
            VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
            u64 at = 0;
            pm_lock(&g_dc.lock);
            int made = g_dc.q[g_dc.tail % DQ].made;
            pm_unlock(&g_dc.lock);
            pm_lock(&g_vk.sc_lock);
            if (!made)
                r = VK_TIMEOUT;
            else if (made < 0)
                r = VK_ERROR_OUT_OF_DATE_KHR;
            else if (p.sc == g_vk.swapchain && p.clock == VK_CLOCK_WAIT2) {
                VkPresentWait2InfoKHR w = {.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR, .presentId = p.id};
                r = vkWaitForPresent2KHR(g_vk.dev, p.sc, &w);
            } else if (p.sc == g_vk.swapchain && p.clock == VK_CLOCK_GOOGLE)
                r = past_timing(&p, &at);
            else if (p.sc == g_vk.swapchain)
                r = vkWaitForPresentKHR(g_vk.dev, p.sc, p.id, 0);
            pm_unlock(&g_vk.sc_lock);
            now = plat_ns();
            if (at) { /* the display's own time */
                displayed(&p, at, 1);
                break;
            }
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
                /* between the last poll that said not yet and this one */
                displayed(&p, failed ? failed + (now - failed) / 2 : now, failed && now - failed <= 2 * POLL_NS);
                break;
            }
            if (r != VK_TIMEOUT || now - p.presented_ns > GIVE_UP_NS) { /* gone, or not shown: let go */
                g_dc.last_game_ns = 0;
                displayed(&(DPresent){.game = p.game}, now, 0);
                break;
            }
            failed = now;
            plat_sleep_ns((i64)POLL_NS);
        }
    }
}

u64 vk_display_id(void) {
    if (!g_vk.sc_clock) return 0;
    pm_lock(&g_dc.lock);
    u64 id = ++g_dc.next_id;
    pm_unlock(&g_dc.lock);
    return id;
}

void vk_display_presented(VkSwapchainKHR sc, u64 id, int game, u64 released_ns) {
    if (!id) return;
    pm_lock(&g_dc.lock);
    if (!g_dc.started) {
        pc_init(&g_dc.work);
        pc_init(&g_dc.shown);
        if (plat_thread(display_thread, NULL, (size_t)1 << 20)) {
            pm_unlock(&g_dc.lock);
            WARN_LIMIT(1, "vulkan: the thread that learns when frames are displayed could not start\n");
            return;
        }
        __atomic_store_n(&g_dc.started, 1, __ATOMIC_RELEASE);
    }
    if (g_dc.head - g_dc.tail < DQ) { /* else unwatched: the queue is a refresh's worth of presents many times over */
        g_dc.q[g_dc.head++ % DQ] =
            (DPresent){sc, id, plat_ns(), released_ns, game ? g_dc.due_next : 0, g_dc.seq, game, g_vk.sc_clock, 0};
        if (game) {
            g_dc.handed++;
            g_dc.held = g_dc.due_next != 0;
        }
        pc_signal(&g_dc.work);
    }
    pm_unlock(&g_dc.lock);
}

void vk_display_made(u64 id, VkResult r) {
    if (!id) return;
    pm_lock(&g_dc.lock);
    for (u32 i = g_dc.tail; i != g_dc.head; i++) /* unwatched (the queue was full): not there */
        if (g_dc.q[i % DQ].id == id) g_dc.q[i % DQ].made = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ? 1 : -1;
    pm_unlock(&g_dc.lock);
}

/* (lock held) The refresh game frame number seq, the one after due_last's, is due at for a frame period of period_ns (n
   refreshes): a period after the frame before's, and at least as many periods after the last one displayed as there
   are frames between them (a frame displayed late moves the ones after it: it is shown longer, then the period
   again), on the grid of refreshes the display times give. Presented at now, a frame later than half a refresh
   before that goes to the first refresh half a refresh away (*late). 0 when there's no grid yet, or
   the period is one refresh or one the display can't keep */
static u64 due_at(u64 seq, u64 period_ns, u64 refresh, u64 now, int* late) {
    u64 grid = g_dc.grid_ns, n, d, due;
    *late = 0;
    if (!period_ns || !refresh || !grid) return 0;
    n = (period_ns + refresh / 2) / refresh;
    d = n * refresh;
    /* FIFO shows a frame a refresh at least already: one held for a period of one would only come later. A game
       that keeps its own pace of fewer frames with a swap interval of 1 (one at 30 fps) only came later, by half a
       refresh, while its frames went to the display no more evenly */
    if (n < 2 || (d > period_ns ? d - period_ns : period_ns - d) > period_ns / 32) return 0;
    due = g_dc.due_last ? g_dc.due_last + d : 0;
    n = seq > g_dc.grid_seq ? seq - g_dc.grid_seq : 1;
    if (due < grid + n * d) due = grid + n * d;
    /* onto the grid's nearest refresh: the display's own refresh isn't quite SDL's */
    due = due >= grid ? grid + (due - grid + refresh / 2) / refresh * refresh
                      : grid - (grid - due + refresh / 2) / refresh * refresh;
    if (due < now + refresh / 2) {
        *late = 1;
        due = grid + (now + refresh / 2 > grid ? (now + refresh / 2 - grid + refresh - 1) / refresh * refresh : 0);
    }
    return due;
}

/* With frame_pacing 2, where the swapchain can, every game frame is held until the refresh it is due at (due_at):
   from half a refresh before, so the refresh after the time is it, wherever the time falls within a step of the
   grid */
u64 vk_display_target(u64 period_ns) {
    u64 refresh = backend_display_refresh_ns(), now = plat_ns(), due = 0;
    int pacing = __atomic_load_n(&g_settings.frame_pacing, __ATOMIC_RELAXED), late = 0;
    pm_lock(&g_dc.lock);
    g_dc.seq++;
    if (pacing >= 2 && g_vk.sc_hold && __atomic_load_n(&g_dc.fifo, __ATOMIC_RELAXED))
        due = due_at(g_dc.seq, period_ns, refresh, now, &late);
    g_dc.due_last = due;
    pm_unlock(&g_dc.lock);
    if (late) __atomic_add_fetch(&g_dc.late, 1, __ATOMIC_RELAXED);
    g_dc.due_next = due;
    if (!due) return 0;
    u64 mono = plat_monotonic_ns() + (due - refresh / 2 - now);
    if (!g_vk.sc_time_stage) return mono;
    /* a present stage's own clock: from a pair of it and CLOCK_MONOTONIC taken now (both in ns) */
    VkSwapchainCalibratedTimestampInfoEXT sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT,
                                                 .swapchain = g_vk.swapchain,
                                                 .presentStage = g_vk.sc_time_stage,
                                                 .timeDomainId = g_vk.sc_time_domain};
    VkCalibratedTimestampInfoKHR ci[2] = {
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
         .pNext = &sci,
         .timeDomain = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT},
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR}};
    u64 ts[2], dev;
    if (vkGetCalibratedTimestampsKHR(g_vk.dev, 2, ci, ts, &dev) != VK_SUCCESS) {
        g_dc.due_next = 0;
        return 0;
    }
    return ts[0] + (mono - ts[1]);
}

void vk_display_dropped(void) { __atomic_add_fetch(&g_dc.dropped, 1, __ATOMIC_RELAXED); }

void vk_display_fifo(int fifo) { __atomic_store_n(&g_dc.fifo, fifo, __ATOMIC_RELAXED); }

int vk_display_wait(u64 deadline_ns, u64* refresh_ns) {
    int r = 1;
    if (!__atomic_load_n(&g_dc.started, __ATOMIC_ACQUIRE) || !__atomic_load_n(&g_dc.fifo, __ATOMIC_RELAXED)) return -1;
    if (!__atomic_load_n(&g_settings.frame_pacing, __ATOMIC_RELAXED)) return -1;
    *refresh_ns = backend_display_refresh_ns();
    pm_lock(&g_dc.lock);
    /* the game's frames presented, all displayed where they are held until their refresh: a frame released then has
       its whole period to reach its own. Where FIFO shows them as they come, one may still wait for its refresh,
       but none behind another */
    u64 queued = !g_dc.held;
    /* asked without waiting (the display was late before): taking the frames again is a frame more behind at most */
    if (deadline_ns <= plat_ns()) queued++;
    while (g_dc.handed - g_dc.done > queued) {
        u64 now = plat_ns();
        if (now >= deadline_ns) {
            r = 0;
            break;
        }
        pc_wait(&g_dc.shown, &g_dc.lock, (i64)(deadline_ns - now));
    }
    pm_unlock(&g_dc.lock);
    return r;
}
