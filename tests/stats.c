/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The stats box over the game (runtime/platform/stats.c): nothing while off; its lines as the frames and the measures
   say them, ? for what is not known; the GPU's share and time a frame from its busy time over a second, ? where the
   renderer can't time it or wasn't at the second before; the graph's bars, a slow frame's in the failure colour; its
   corner, and beside showcase's box in showcase's corner; a word that is no corner; and off forgetting what it had */
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/controls_atlas.h"
#include "nx/runtime/platform/plat.h"
#include "gui_theme_colors.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ui.c's lock and log, and what vpad.c and the Display table name, without the platform layer */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
void options_language_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
void options_tv_mode_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
/* the test's state: the last line logged, the quads drawn, the line line_at read, and what the renderer reads of the
   GPU at each frame (nothing, at first) */
static struct {
    char logged[256];
    SnxUiQuad quads[4096];
    SnxUiList l;
    char line[STATS_COLS + 1];
    SnxStatsGpu gpu;
} g_test;
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_test.logged, sizeof g_test.logged, fmt, ap);
    va_end(ap);
}

#define FAILURE                                                                                                        \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFF00u) |             \
     (SNX_GUI_THEME_YORHA_FAILURE >> 16 & 0xFFu))
#define MS 1000000ull /* a millisecond, in ns */

static const int safe[4] = {100, 50, 1280, 720};
static void draw(u64 now) {
    g_test.l.q = g_test.quads;
    g_test.l.n = 0;
    g_test.l.max = 4096;
    stats_draw(&g_test.l, safe, 2, now);
}
/* the line of letters drawn at x, y in scale 2's letters, its spaces back, trimmed */
static const char* line_at(float x, float y) {
    int k, i, end = 0;
    for (k = 0; k < STATS_COLS; ++k) {
        g_test.line[k] = ' ';
        for (i = 0; i < g_test.l.n; ++i)
            if (g_test.quads[i].uw == 8 && g_test.quads[i].vh == 16 && g_test.quads[i].x == x + (float)k * 16 &&
                g_test.quads[i].y == y)
                g_test.line[k] = (char)((g_test.quads[i].v - SNX_CONTROLS_FONT_Y) / 16 * 16 +
                                        (g_test.quads[i].u - SNX_CONTROLS_FONT_X) / 8);
        if (g_test.line[k] != ' ') end = k + 1;
    }
    g_test.line[end] = 0;
    return g_test.line;
}
/* the box's line k from the top */
static const char* text(int k) {
    float box[4];
    stats_box(safe, 2, box);
    return line_at(box[0] + 16, box[1] + 8 + (float)k * 32);
}
/* the graph's bars: the plain fills narrower than it */
static int bars(const SnxUiQuad** newest) {
    float box[4];
    int i, n = 0;
    stats_box(safe, 2, box);
    *newest = NULL;
    for (i = 1; i < g_test.l.n; ++i)
        if (g_test.quads[i].uw == 2 && g_test.quads[i].w < box[2] - 32) {
            if (!*newest || g_test.quads[i].x > (*newest)->x) *newest = &g_test.quads[i];
            ++n;
        }
    return n;
}
/* n frames of ms each from at, the GPU busy for busy_ms of each, as the renderer reads it; the last frame's time */
static u64 frames(u64 at, int n, u64 ms, u64 busy_ms) {
    int k;
    for (k = 0; k < n; ++k) {
        g_test.gpu.busy_ns += busy_ms * MS;
        stats_frame(at += ms * MS, &g_test.gpu);
    }
    return at;
}

int main(void) {
    const IniOpt *corner = ini_find(g_display_ini, "stats_corner"),
                 *showcase = ini_find(g_display_ini, "showcase_corner");
    const SnxUiQuad* newest;
    float box[4], show[4], gh = 3 * 32;
    u64 t = 1000 * MS, at;
    int k;
    assert(corner && showcase);
    /* off: nothing drawn, and no frame kept */
    stats_frame(t, &g_test.gpu);
    draw(t);
    assert(g_test.l.n == 0);
    /* on, in its default corner, top left: half a line in from the safe area's; ? before a frame or a measure */
    g_settings.stats = 1;
    stats_box(safe, 2, box);
    assert(box[0] == 100 + 16 && box[1] == 50 + 16 && box[2] == STATS_COLS * 16 && box[3] == 7 * 32 + 24);
    draw(t);
    assert(!strcmp(text(0), "? fps  ? ms") && !strcmp(text(1), "cpu ?  ? MiB"));
    assert(!strcmp(text(2), "gpu ?  ? ms") && !strcmp(text(3), "vram ?/? MiB"));
    /* 30 frames in half a second: 60.0 frames a second, the last 16.7 ms; 29 bars under the 60 FPS guide */
    for (k = 1; k <= 30; ++k) stats_frame(at = t + (u64)k * 16666667ull, &g_test.gpu);
    draw(t + 500 * MS);
    assert(!strcmp(text(0), "60.0 fps  16.7 ms"));
    assert(bars(&newest) == 29 && newest->rgba != FAILURE && newest->h > gh * 16 / 50 && newest->h < gh * 17 / 50);
    /* the processor's share and the memory as measured, and ? for one not told */
    stats_measured(&(SnxStatsMeasure){.cpu = 25, .memory = 512ull << 20, .gpu = -1});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu 25%  512 MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = -1, .memory = 512ull << 20, .gpu = -1});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu ?  512 MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = 7, .gpu = -1});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu 7%  ? MiB"));
    /* the GPU's share and its time a frame, in tenths of a millisecond rounded, and the video memory's use and
       budget, as measured; ? for one not told */
    stats_measured(&(SnxStatsMeasure){
        .cpu = 7, .gpu = 63, .gpu_ns = 10450000, .vram = 3120ull << 20, .vram_budget = 7980ull << 20});
    draw(t + 600 * MS);
    assert(!strcmp(text(1), "cpu 7%  ? MiB"));
    assert(!strcmp(text(2), "gpu 63%  10.5 ms") && !strcmp(text(3), "vram 3120/7980 MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = 7, .gpu = -1, .vram = 3120ull << 20});
    draw(t + 600 * MS);
    assert(!strcmp(text(2), "gpu ?  ? ms") && !strcmp(text(3), "vram 3120/? MiB"));
    stats_measured(&(SnxStatsMeasure){.cpu = 7, .gpu = 0, .gpu_ns = 40000, .vram_budget = 7980ull << 20});
    draw(t + 600 * MS);
    assert(!strcmp(text(2), "gpu 0%  0.0 ms") && !strcmp(text(3), "vram ?/7980 MiB"));
    /* a 40 ms frame in the failure colour, then an 80 ms one as tall as the graph */
    stats_frame(at += 40 * MS, &g_test.gpu);
    draw(t + 700 * MS);
    assert(strstr(text(0), "40.0 ms") && bars(&newest) == 30 && newest->rgba == FAILURE);
    assert(newest->h > gh * 39 / 50 && newest->h < gh * 41 / 50);
    stats_frame(at += 80 * MS, &g_test.gpu);
    draw(t + 800 * MS);
    assert(strstr(text(0), "80.0 ms") && bars(&newest) == 31 && newest->h == gh);
    /* no frame for half a second: 0.0 frames a second */
    draw(t + 1100 * MS); /* the window that counted the two slow frames */
    draw(t + 1700 * MS);
    assert(!strncmp(text(0), "0.0 fps", 7));
    /* past STATS_FRAMES frames: the graph keeps the last of them */
    for (k = 0; k < 2 * STATS_FRAMES; ++k) stats_frame(at += 16 * MS, &g_test.gpu);
    draw(at);
    assert(bars(&newest) == STATS_FRAMES - 1);
    /* the GPU's lines from what the renderer reads at each frame, measured once a second from the first frame: 12 ms
       busy in each 20 ms frame is 60% and 12.0 ms; the video memory as it reads it */
    g_settings.stats = 0;
    stats_frame(at += 20 * MS, &g_test.gpu); /* forgets the measures so far */
    g_settings.stats = 1;
    g_test.gpu = (SnxStatsGpu){.busy_ns = 5000 * MS, .timed = 1, .vram = 2048ull << 20, .vram_budget = 8192ull << 20};
    stats_frame(at += 20 * MS, &g_test.gpu);
    at = frames(at, 49, 20, 12);
    draw(at);
    assert(!strcmp(text(2), "gpu ?  ? ms") && !strcmp(text(3), "vram ?/? MiB")); /* 980 ms: not a second yet */
    at = frames(at, 1, 20, 12);
    draw(at);
    assert(!strcmp(text(2), "gpu 60%  12.0 ms") && !strcmp(text(3), "vram 2048/8192 MiB"));
    /* a second the renderer couldn't time: ?, and the next too, which has no start; then 5 ms of each 20 again */
    g_test.gpu.timed = 0;
    at = frames(at, 50, 20, 12);
    draw(at);
    assert(!strcmp(text(2), "gpu ?  ? ms") && !strcmp(text(3), "vram 2048/8192 MiB"));
    g_test.gpu.timed = 1;
    at = frames(at, 50, 20, 5);
    draw(at);
    assert(!strcmp(text(2), "gpu ?  ? ms"));
    at = frames(at, 50, 20, 5);
    draw(at);
    assert(!strcmp(text(2), "gpu 25%  5.0 ms"));
    /* a busy GPU at 30 frames a second: 33 ms of each 34; the video memory no longer told */
    g_test.gpu.vram = g_test.gpu.vram_budget = 0;
    at = frames(at, 30, 34, 33);
    draw(at);
    assert(!strcmp(text(2), "gpu 97%  33.0 ms") && !strcmp(text(3), "vram ?/? MiB"));
    /* bottom right; beside showcase's box in showcase's own corner, above it there, below it in a top one; showcase in
       another corner leaves it in its own */
    stats_corner_set(corner, "br");
    stats_box(safe, 2, box);
    assert(box[0] + box[2] == 100 + 1280 - 16 && box[1] + box[3] == 50 + 720 - 16);
    g_settings.showcase = 1;
    vpad_showcase_corner_set(showcase, "br");
    assert(vpad_showcase_box(safe, show) == 3);
    stats_box(safe, 2, box);
    assert(box[0] + box[2] == show[0] + show[2] && box[1] + box[3] == show[1] - 16);
    stats_corner_set(corner, "tl");
    vpad_showcase_corner_set(showcase, "tl");
    stats_box(safe, 2, box);
    assert(vpad_showcase_box(safe, show) == 0 && box[0] == show[0] && box[1] == show[1] + show[3] + 16);
    vpad_showcase_corner_set(showcase, "br");
    stats_box(safe, 2, box);
    assert(box[0] == 100 + 16 && box[1] == 50 + 16);
    g_settings.showcase = 0;
    /* a word that is no corner: the corner it had, said with both */
    stats_corner_set(corner, "middle");
    assert(!strcmp(g_test.logged, "settings: stats_corner=middle is not tl, tr, bl or br: it keeps tl\n") &&
           !strcmp(g_settings.stats_corner, "tl"));
    /* off forgets what it had: on again, ? until it has frames and measures again */
    g_settings.stats = 0;
    stats_frame(at += 16 * MS, &g_test.gpu);
    draw(at);
    assert(g_test.l.n == 0);
    g_settings.stats = 1;
    draw(at);
    assert(!strcmp(text(0), "? fps  ? ms") && !strcmp(text(1), "cpu ?  ? MiB") && !bars(&newest));
    assert(!strcmp(text(2), "gpu ?  ? ms") && !strcmp(text(3), "vram ?/? MiB"));
    puts("stats passed");
    return 0;
}
