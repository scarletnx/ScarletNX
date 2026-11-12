/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Debug settings' list over the game (runtime/platform/hud.c): nothing for none; a title and a row per setting,
   its key at the left and its value at the right, as wide as the stats panel's lines; under the stats' box, to its
   right, or at the top's middle; a row too long sliding in the quads it has; the rows past the picture's room
   counted in one */
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/stats.h"
#include "common/utilities/slide.h"
#include "gui_theme_colors.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* a quad as it holds it (ui.h's ui_view): its place in pixels, its colour widened back to 8 bits */
static SnxUiQuadView qv(const SnxUiQuad* q) { return ui_view(q); }
/* ui.c's lock and log, without the platform layer */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
#define QUADS 4096
#define COLS  STATS_GRAPH_W
static SnxUiQuad quads[QUADS];
static SnxUiList drawn_at(const int safe[4], int scale, const float* stats, u64 ms) {
    SnxUiList l = {quads, 0, QUADS};
    hud_draw(&l, safe, scale, stats, ms);
    return l;
}
static SnxUiList drawn(const int safe[4], int scale, const float* stats) { return drawn_at(safe, scale, stats, 0); }
/* a theme's 0xRRGGBB as a quad holds it */
#define THEME(rgb) ui_rgba(0xFF000000u | ((rgb) & 0xFFu) << 16 | ((rgb) & 0xFF00u) | ((rgb) >> 16 & 0xFFu))
/* What is drawn, checked: a strip of the ink a quarter line high above the rows and one under them, as wide as the
   box, COLS letters; then each row's COLS cells, backed, one after the other from the box's left, the title's in
   the failure colour and the others' in the page's. The rows, the box in box (x, y, w, h), and each row's letters
   into text[row] */
static int rows(const SnxUiList* l, int scale, float box[4], char text[][COLS + 1]) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale;
    SnxUiQuadView top = qv(&quads[0]), under = qv(&quads[1]);
    int i, n = (l->n - 2) / COLS;
    assert(l->n == 2 + n * COLS && n >= 2);
    assert(top.ink == UI_SOLID && under.ink == UI_SOLID && top.rgba == ui_ground() && under.rgba == ui_ground() &&
           ui_ground() == THEME(SNX_GUI_THEME_YORHA_INK));
    assert(top.h == ch / 4 && under.h == ch / 4 && top.x == under.x && top.w == COLS * cw && under.w == top.w);
    box[0] = top.x;
    box[1] = top.y;
    box[2] = top.w;
    box[3] = under.y + under.h - top.y;
    assert(under.y == top.y + ch / 4 + (float)n * ch);
    for (i = 0; i < n * COLS; ++i) {
        SnxUiQuadView q = qv(&quads[2 + i]);
        int r = i / COLS, c = i % COLS;
        assert(q.ink == UI_GLYPH && q.backed && q.w == cw && q.h == ch && q.x == box[0] + (float)c * cw &&
               q.y == box[1] + ch / 4 + (float)r * ch);
        assert(q.rgba == THEME(r ? SNX_GUI_THEME_YORHA_PAGE : SNX_GUI_THEME_YORHA_FAILURE));
        text[r][c] = (char)q.glyph;
        text[r][COLS] = 0;
    }
    return n;
}
/* the row text as it shows: a blank, left, spaces, right, a blank, in COLS letters */
static const char* row_of(const char* left, const char* right) {
    static char s[COLS + 1];
    memset(s, ' ', COLS);
    s[COLS] = 0;
    memcpy(s + 1, left, strlen(left));
    memcpy(s + COLS - 1 - strlen(right), right, strlen(right));
    return s;
}
int main(void) {
    static const int wide[4] = {40, 20, 1280, 720}, low[4] = {0, 0, 1280, 120};
    static const char list[] = "log=2, vk_validation=1, shader_corpus, gdbstub";
    static const char longer[] = "log=2, shader_corpus=/home/someone/a/very/long/path/to/the/corpus/file.spv";
    static char text[HUD_ROWS_MAX][COLS + 1], many[HUD_DEBUG_MAX];
    SnxUiList l;
    float box[4];
    int n, i;
    /* none: nothing, and the same list again is no change */
    assert(!hud_debug_set(""));
    l = drawn(wide, 1, NULL);
    assert(l.n == 0);
    assert(hud_debug_set(list) && !hud_debug_set(list));
    /* no stats' box: at the top's middle, half a line under the safe area's top; the title, then a row each, its key
       at the left and its value at the right, a setting with no value its key alone */
    l = drawn(wide, 1, NULL);
    assert(rows(&l, 1, box, text) == 5);
    assert(box[0] + box[2] / 2 == (float)wide[0] + (float)wide[2] / 2 && box[1] == (float)wide[1] + 8);
    assert(box[3] == 5 * 16 + 8);
    assert(!strcmp(text[0], row_of(HUD_DEBUG_TITLE, "")));
    assert(!strcmp(text[1], row_of("log", "2")) && !strcmp(text[2], row_of("vk_validation", "1")));
    assert(!strcmp(text[3], row_of("shader_corpus", "")) && !strcmp(text[4], row_of("gdbstub", "")));
    /* at scale 2 the same rows in letters twice as large */
    l = drawn(wide, 2, NULL);
    assert(rows(&l, 2, box, text) == 5 && box[2] == COLS * 16);
    /* the stats' box shown: under it in its column, half a line apart; to its right, from its top, when the picture
       has no room under it; and at the top's middle once it is gone */
    {
        static const float stats[4] = {48, 28, 35 * 8, 7 * 16 + 12}, tall[4] = {48, 28, 35 * 8, 690};
        l = drawn(wide, 1, stats);
        assert(rows(&l, 1, box, text) == 5);
        assert(box[0] == stats[0] && box[1] == stats[1] + stats[3] + 8);
        l = drawn(wide, 1, tall);
        assert(rows(&l, 1, box, text) == 5 && box[0] == tall[0] + tall[2] + 8 && box[1] == tall[1]);
        l = drawn(wide, 1, NULL);
        assert(rows(&l, 1, box, text) == 5 && box[0] + box[2] / 2 == (float)wide[0] + (float)wide[2] / 2);
    }
    /* a row too long for its room: the key, a space and the value from the start, cut at the room; resting
       SNX_SLIDE_WAIT steps, then a column further each HUD_SLIDE_MS in the same quads; at its end it rests, and
       comes back */
    assert(hud_debug_set(longer));
    l = drawn_at(wide, 1, NULL, 0);
    n = rows(&l, 1, box, text);
    assert(n == 3 && !strcmp(text[1], row_of("log", "2")));
    assert(!strcmp(text[2], " shader_corpus /home/someone/a/ve "));
    {
        int over = (int)strlen("shader_corpus /home/someone/a/very/long/path/to/the/corpus/file.spv") - (COLS - 2);
        SnxUiList again;
        assert(over > 0);
        again = drawn_at(wide, 1, NULL, (u64)(SNX_SLIDE_WAIT - 1) * HUD_SLIDE_MS); /* still resting */
        assert(again.n == l.n && rows(&again, 1, box, text) == 3 && !strncmp(text[2], " shader_corpus /", 16));
        again = drawn_at(wide, 1, NULL, (u64)(SNX_SLIDE_WAIT + 1) * HUD_SLIDE_MS); /* a column on */
        assert(again.n == l.n && rows(&again, 1, box, text) == 3 && !strncmp(text[2], " hader_corpus /", 15));
        again = drawn_at(wide, 1, NULL, (u64)(SNX_SLIDE_WAIT + over) * HUD_SLIDE_MS); /* at its end */
        assert(rows(&again, 1, box, text) == 3 && !strncmp(text[2] + COLS - 1 - 8, "file.spv ", 9));
        again = drawn_at(wide, 1, NULL, (u64)(2 * (SNX_SLIDE_WAIT + over)) * HUD_SLIDE_MS); /* back at the start */
        assert(rows(&again, 1, box, text) == 3 && !strncmp(text[2], " shader_corpus /", 16));
    }
    /* more settings than the picture has room for: the rows that fit, the last one counting the rest */
    for (n = 0, i = 0; i < 40; ++i) n += snprintf(many + n, sizeof many - (size_t)n, "%sk%d=%d", i ? ", " : "", i, i);
    assert(hud_debug_set(many));
    l = drawn(low, 1, NULL); /* 120 high: under half a line and its margins, 6 rows */
    n = rows(&l, 1, box, text);
    assert(n == (120 - 8 - 8) / 16 && !strcmp(text[0], row_of(HUD_DEBUG_TITLE, "")));
    assert(!strcmp(text[1], row_of("k0", "0")));
    {
        char more[32];
        snprintf(more, sizeof more, "+%d more", 40 - (n - 2));
        assert(!strcmp(text[n - 1], row_of(more, "")));
    }
    l = drawn(wide, 1, NULL); /* a tall picture: HUD_ROWS_MAX rows at most */
    assert(rows(&l, 1, box, text) == HUD_ROWS_MAX);
    assert(hud_debug_set("") && (l = drawn(wide, 1, NULL), l.n == 0));
    puts("hud passed");
    return 0;
}
