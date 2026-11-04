/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/interp_auto.h"
#include <stdlib.h>
#include <string.h>

#define SPAN_NS 500000000ull  /* the figures are taken every half second */
#define ON_NS   1000000000ull /* the fit must hold a second before the blend comes on */
#define OFF_NS  2000000000ull /* and fail two before it goes off */
#define LEAST   8u            /* fewer intervals than this say nothing yet */

static int by_time(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

/* Whether the game leaves room for two presents a refresh: the display refreshes at least 1.9 times the game's fastest
   1 % of frames (two presents of a frame at that pace still fit), and its slowest 1 % stays within twice that fastest
   one, so a game that hitches every few frames is not blended */
static int fits(const SnxInterpAuto* a, int display_hz) {
    uint64_t v[SNX_INTERP_AUTO_FRAMES];
    unsigned n = a->count;
    if (display_hz <= 0 || n < LEAST) return 0;
    memcpy(v, a->at, n * sizeof *v);
    qsort(v, n, sizeof *v, by_time);
    if ((uint64_t)display_hz * v[n / 100] * 10 < 19000000000ull) return 0;
    return v[n - 1 - n / 100] <= v[n / 100] * 2;
}

void snx_interp_auto_init(SnxInterpAuto* a) {
    if (a) memset(a, 0, sizeof *a);
}

int snx_interp_auto_frame(SnxInterpAuto* a, uint64_t now_ns, int display_hz) {
    uint64_t interval;
    int fit;
    if (!a) return 0;
    if (a->last_ns && now_ns > a->last_ns) {
        interval = now_ns - a->last_ns;
        if (interval < SPAN_NS) { /* half a second or more: a pause, not a frame */
            a->at[a->next] = interval;
            a->next = (a->next + 1) % SNX_INTERP_AUTO_FRAMES;
            if (a->count < SNX_INTERP_AUTO_FRAMES) ++a->count;
        }
    }
    a->last_ns = now_ns;
    if (!a->since_ns) { /* the first half second starts at the first frame */
        a->since_ns = now_ns;
        return a->on;
    }
    if (now_ns - a->since_ns < SPAN_NS) return a->on;
    a->since_ns = now_ns;
    fit = fits(a, display_hz);
    if (fit == a->on) { /* the answer holds: a switch needs the opposite held */
        a->pending_ns = 0;
        return a->on;
    }
    if (!a->pending_ns) {
        a->pending_ns = now_ns;
        return a->on;
    }
    if (fit && now_ns - a->pending_ns >= ON_NS) {
        a->on = 1;
        a->pending_ns = 0;
    } else if (!fit && now_ns - a->pending_ns >= OFF_NS) {
        a->on = 0;
        a->pending_ns = 0;
    }
    return a->on;
}
