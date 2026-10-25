/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The display's clock (VK_KHR_present_id and VK_KHR_present_wait): when each present reached the display.

   Every present carries an id, and a thread of this file's learns when each is displayed. vkWaitForPresentKHR takes
   the swapchain externally synchronized, as the GPU thread's acquire and the swapchain's create and destroy do
   (g_vk.sc_lock), so the thread never blocks in it, which would hold the next acquire for up to a refresh: it asks
   without a timeout, from a little before the present is due, every POLL_NS. A display time is known to that.

   The game's pace (renderer.h display_wait, runtime/present.c) waits here for the display to take its frames, and
   every STATS game frames the log says how evenly they were displayed and how long they took to get there */
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include <math.h>
#include <stdlib.h>

#define DQ         64           /* presents not known displayed yet: one more is let go unwatched */
#define POLL_NS    250000ull    /* the polling step */
#define EARLY_NS   1500000ull   /* polling for a present starts this long before it is due */
#define GIVE_UP_NS 500000000ull /* a present not displayed in this long (a covered window on Wayland) is let go */
#define STATS      600          /* game frames a line of the log covers */

typedef struct {
    VkSwapchainKHR sc;
    u64 id;
    u64 presented_ns; /* when show() queued it */
    u64 released_ns;  /* when the game began the frame (present.c), 0 unknown */
    int game;         /* a frame of the game's, not the menu's or a black screen's */
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
    /* the thread's own: the last game frame's display time (0 none, or not known to a step), the stats */
    u64 last_game_ns, last_ns;
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
    g_dc.tail++;
    if (p->game) g_dc.done++;
    pc_broadcast(&g_dc.shown);
    pm_unlock(&g_dc.lock);
}

static void display_thread(void* arg) {
    (void)arg;
    for (;;) {
        DPresent p;
        pm_lock(&g_dc.lock);
        while (g_dc.head == g_dc.tail) pc_wait(&g_dc.work, &g_dc.lock, -1);
        p = g_dc.q[g_dc.tail % DQ];
        pm_unlock(&g_dc.lock);
        /* due a refresh after the present before it, or as it is queued: polled from a little before */
        u64 refresh = backend_display_refresh_ns(), due = p.presented_ns, now = plat_ns(), failed = 0;
        if (refresh && g_dc.last_ns + refresh > due) due = g_dc.last_ns + refresh;
        if (due > now + EARLY_NS) plat_sleep_ns((i64)(due - EARLY_NS - now));
        for (;;) {
            VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
            pm_lock(&g_vk.sc_lock);
            if (p.sc == g_vk.swapchain) r = vkWaitForPresentKHR(g_vk.dev, p.sc, p.id, 0);
            pm_unlock(&g_vk.sc_lock);
            now = plat_ns();
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
    if (!g_vk.has_present_wait) return 0;
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
        g_dc.q[g_dc.head++ % DQ] = (DPresent){sc, id, plat_ns(), released_ns, game};
        if (game) g_dc.handed++;
        pc_signal(&g_dc.work);
    }
    pm_unlock(&g_dc.lock);
}

void vk_display_fifo(int fifo) { __atomic_store_n(&g_dc.fifo, fifo, __ATOMIC_RELAXED); }

int vk_display_wait(u32 queued, u64 deadline_ns, u64* refresh_ns) {
    int r = 1;
    if (!__atomic_load_n(&g_dc.started, __ATOMIC_ACQUIRE) || !__atomic_load_n(&g_dc.fifo, __ATOMIC_RELAXED)) return -1;
    pm_lock(&g_dc.lock);
    while (g_dc.handed - g_dc.done > queued) {
        u64 now = plat_ns();
        if (now >= deadline_ns) {
            r = 0;
            break;
        }
        pc_wait(&g_dc.shown, &g_dc.lock, (i64)(deadline_ns - now));
    }
    pm_unlock(&g_dc.lock);
    *refresh_ns = backend_display_refresh_ns();
    return r;
}
