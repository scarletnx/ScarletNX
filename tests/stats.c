/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The stats box over the game (runtime/platform/stats.c): nothing while off; its lines as the frames and the measures
   say them, ? for what is not known; the graph's bars, a slow frame's in the failure colour; its corner, and beside
   showcase's box in showcase's corner; a word that is no corner; and off forgetting what it had */
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
/* the test's state: the last line logged, the quads drawn, and the line line_at read */
static struct {
    char logged[256];
    SnxUiQuad quads[4096];
    SnxUiList l;
    char line[STATS_COLS + 1];
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
static const char* first(void) {
    float box[4];
    stats_box(safe, 2, box);
    return line_at(box[0] + 16, box[1] + 8);
}
static const char* second(void) {
    float box[4];
    stats_box(safe, 2, box);
    return line_at(box[0] + 16, box[1] + 8 + 32);
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

int main(void) {
    const IniOpt *corner = ini_find(g_display_ini, "stats_corner"),
                 *showcase = ini_find(g_display_ini, "showcase_corner");
    const SnxUiQuad* newest;
    float box[4], show[4], gh = 3 * 32;
    u64 t = 1000 * MS, at;
    int k;
    assert(corner && showcase);
    /* off: nothing drawn, and no frame kept */
    stats_frame(t);
    draw(t);
    assert(g_test.l.n == 0);
    /* on, in its default corner, top left: half a line in from the safe area's; ? before a frame or a measure */
    g_settings.stats = 1;
    stats_box(safe, 2, box);
    assert(box[0] == 100 + 16 && box[1] == 50 + 16 && box[2] == STATS_COLS * 16 && box[3] == 5 * 32 + 24);
    draw(t);
    assert(!strcmp(first(), "? fps  ? ms") && !strcmp(second(), "cpu ?  ? MiB"));
    /* 30 frames in half a second: 60.0 frames a second, the last 16.7 ms; 29 bars under the 60 FPS guide */
    for (k = 1; k <= 30; ++k) stats_frame(at = t + (u64)k * 16666667ull);
    draw(t + 500 * MS);
    assert(!strcmp(first(), "60.0 fps  16.7 ms"));
    assert(bars(&newest) == 29 && newest->rgba != FAILURE && newest->h > gh * 16 / 50 && newest->h < gh * 17 / 50);
    /* the processor's share and the memory as measured, and ? for one not told */
    stats_measured(25, 512ull << 20);
    draw(t + 600 * MS);
    assert(!strcmp(second(), "cpu 25%  512 MiB"));
    stats_measured(-1, 512ull << 20);
    draw(t + 600 * MS);
    assert(!strcmp(second(), "cpu ?  512 MiB"));
    stats_measured(7, 0);
    draw(t + 600 * MS);
    assert(!strcmp(second(), "cpu 7%  ? MiB"));
    /* a 40 ms frame in the failure colour, then an 80 ms one as tall as the graph */
    stats_frame(at += 40 * MS);
    draw(t + 700 * MS);
    assert(strstr(first(), "40.0 ms") && bars(&newest) == 30 && newest->rgba == FAILURE);
    assert(newest->h > gh * 39 / 50 && newest->h < gh * 41 / 50);
    stats_frame(at += 80 * MS);
    draw(t + 800 * MS);
    assert(strstr(first(), "80.0 ms") && bars(&newest) == 31 && newest->h == gh);
    /* no frame for half a second: 0.0 frames a second */
    draw(t + 1100 * MS); /* the window that counted the two slow frames */
    draw(t + 1700 * MS);
    assert(!strncmp(first(), "0.0 fps", 7));
    /* past STATS_FRAMES frames: the graph keeps the last of them */
    for (k = 0; k < 2 * STATS_FRAMES; ++k) stats_frame(at += 16 * MS);
    draw(at);
    assert(bars(&newest) == STATS_FRAMES - 1);
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
    stats_frame(at += 16 * MS);
    draw(at);
    assert(g_test.l.n == 0);
    g_settings.stats = 1;
    draw(at);
    assert(!strcmp(first(), "? fps  ? ms") && !strcmp(second(), "cpu ?  ? MiB") && !bars(&newest));
    puts("stats passed");
    return 0;
}
