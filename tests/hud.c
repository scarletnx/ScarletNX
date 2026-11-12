/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Debug settings' line over the game (runtime/platform/hud.c): nothing for none, bands of the layer's ground at the
   top's middle of the safe area, or clear of the stats' box when it shows, every letter of the list drawn and backed by
   it, cut between its items onto lines that keep inside the area */
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/plat.h"
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
static SnxUiQuad quads[QUADS];

/* the letters of s but its spaces */
static int letters(const char* s) {
    int n = 0;
    for (; *s; ++s) n += *s != ' ';
    return n;
}
static SnxUiList drawn_by(const int safe[4], int scale, const float* stats) {
    SnxUiList l = {quads, 0, QUADS};
    hud_draw(&l, safe, scale, stats);
    return l;
}
static SnxUiList drawn(const int safe[4], int scale) { return drawn_by(safe, scale, NULL); }
/* a theme's 0xRRGGBB as a quad holds it */
#define THEME(rgb) ui_rgba(0xFF000000u | ((rgb) & 0xFFu) << 16 | ((rgb) & 0xFF00u) | ((rgb) >> 16 & 0xFFu))
/* What is drawn, checked: a box of the layer's ground, the ink, with nothing under a letter. First its two margins,
   a strip of the ink a quarter line high above the lines and one under them, as wide as the box; then rows of scale
   times 16 pixels one under the other, each from the box's left to its right: a blank cell, then a cell for each
   letter, backed by the ground (the first row's first lead of them in the failure colour, the others in the page's),
   then one fill of the ink to the box's right; each quad right after the one before it, so none is under another.
   The rows, the box in box (x, y, w, h) and the letters drawn but the blanks in *inked */
static int rows(const SnxUiList* l, int scale, float box[4], int* inked) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, at = 0;
    int i, n = 0, cell = 0;
    SnxUiQuadView top = qv(&quads[0]), under = qv(&quads[1]);
    *inked = 0;
    assert(l->n > 2 && top.ink == UI_SOLID && under.ink == UI_SOLID && top.rgba == ui_ground() &&
           under.rgba == ui_ground() && ui_ground() == THEME(SNX_GUI_THEME_YORHA_INK));
    assert(top.h == ch / 4 && under.h == ch / 4 && top.x == under.x && top.w == under.w);
    box[0] = top.x;
    box[1] = top.y;
    box[2] = top.w;
    box[3] = under.y + under.h - top.y;
    for (i = 2; i < l->n; ++i) {
        SnxUiQuadView q = qv(&quads[i]);
        int first = i == 2 || qv(&quads[i - 1]).ink == UI_SOLID; /* of its row: the one before ended with its fill */
        if (first) {
            ++n;
            at = box[0];
            cell = 0;
        }
        assert(q.x == at && q.y == box[1] + ch / 4 + (float)(n - 1) * ch && q.h == ch && q.w > 0);
        at += q.w;
        if (q.ink == UI_SOLID) { /* the row's end, which is the box's right */
            assert(cell > 1 && q.rgba == ui_ground() && at == box[0] + box[2]);
            continue;
        }
        assert(q.ink == UI_GLYPH && q.backed && q.w == cw && q.uw == 8 && q.vh == 16);
        assert(cell ? 1 : q.glyph == ' ');
        assert(q.rgba == THEME(n == 1 && cell && cell <= (int)strlen(HUD_DEBUG_LEAD) ? SNX_GUI_THEME_YORHA_FAILURE
                                                                                     : SNX_GUI_THEME_YORHA_PAGE));
        *inked += q.glyph != ' ';
        ++cell;
    }
    assert(qv(&quads[l->n - 1]).ink == UI_SOLID && box[3] == (float)n * ch + ch / 2); /* the lines and the margins */
    return n;
}

int main(void) {
    static const int wide[4] = {40, 20, 1280, 720}, narrow[4] = {0, 0, 200, 400}, tiny[4] = {10, 0, 40, 400};
    static const char list[] = "log=2, vk_validation=1, shader_corpus, gdbstub";
    static const char longer[] = "renderdoc_frames=12345678901234567890123456789012345678901234567890";
    static char most[HUD_DEBUG_MAX];
    SnxUiList l;
    float band[4];
    int inked, n;
    /* none: nothing, and the same list again is no change */
    assert(!hud_debug_set(""));
    l = drawn(wide, 1);
    assert(l.n == 0);
    assert(hud_debug_set(list) && !hud_debug_set(list));
    /* one line on a wide picture: the box in the middle across, half a line under the safe area's top, a line and its
       two quarters of margin high, a letter's gap wider than the line at either side, with the two margins' strips,
       a quad for each of the line's cells and the fill at its end */
    l = drawn(wide, 2);
    assert(rows(&l, 2, band, &inked) == 1 && inked == letters(HUD_DEBUG_LEAD) + letters(list));
    assert(l.n == (int)(strlen(HUD_DEBUG_LEAD) + strlen(list)) + 4);
    assert(band[2] == (float)(strlen(HUD_DEBUG_LEAD) + strlen(list) + 2) * 16);
    assert(band[0] + band[2] / 2 == (float)wide[0] + (float)wide[2] / 2 && band[1] == (float)wide[1] + 16);
    assert(band[3] == 32 + 16);
    /* 23 letters across a narrow one: "Debug on: log=2," then "vk_validation=1," then "shader_corpus, gdbstub", none
       of them cut, all of them inside it, each line's band as wide as the longest's and the box 3 lines and a half */
    l = drawn(narrow, 1);
    assert(rows(&l, 1, band, &inked) == 3 && inked == letters(HUD_DEBUG_LEAD) + letters(list));
    assert(band[2] == (float)(22 + 2) * 8 && band[0] >= 0 && band[0] + band[2] <= (float)narrow[2]);
    assert(band[3] == 3 * 16 + 8);
    /* an item longer than a line goes on over the next ones, a letter each still */
    assert(hud_debug_set(longer));
    l = drawn(narrow, 1);
    assert(rows(&l, 1, band, &inked) == 4 && inked == letters(HUD_DEBUG_LEAD) + letters(longer));
    /* a picture too narrow for the lead and eight letters: the band goes past its sides, and shows it all */
    l = drawn(tiny, 1);
    assert(rows(&l, 1, band, &inked) == 5 && inked == letters(HUD_DEBUG_LEAD) + letters(longer));
    assert(band[2] == (float)(strlen(HUD_DEBUG_LEAD) + 8 + 2) * 8 && band[0] < (float)tiny[0]);
    /* the longest list in the most lines it can take, a letter's item and one that just does not fit beside it in turn
       on the narrowest band (18 letters: "a," and 15 more with their comma are 19 with the space between): two lines
       for each pair but the last, which fits one, and the quads hud.c keeps hold every one of its letters */
    for (n = 0; n + 20 < (int)sizeof most; n += 20) memcpy(most + n, "a, bbbbbbbbbbbbbbb, ", 20);
    most[n - 2] = 0; /* no ", " after the last */
    assert(hud_debug_set(most));
    l = drawn(tiny, 1);
    assert(rows(&l, 1, band, &inked) == 2 * (n / 20) - 1 && inked == letters(HUD_DEBUG_LEAD) + letters(most));
    /* and 3000 lists of items of every length on boxes of every width, from a fixed seed: none loses a letter, and
       the lines are within what hud.c's count of its quads takes them to be, 2 for each line's room less one of
       the letters, and one */
    {
        unsigned seed = 0x5EED1234u;
        int k;
        for (k = 0; k < 3000; ++k) {
            int total, longest, safe[4] = {0, 0, 0, 400}, cols, lines, letters_all;
            seed = seed * 1664525u + 1013904223u;
            total = 1 + (int)(seed >> 8) % (HUD_DEBUG_MAX - 1);
            seed = seed * 1664525u + 1013904223u;
            longest = 1 + (int)(seed >> 8) % 40;
            seed = seed * 1664525u + 1013904223u;
            safe[2] = 8 * (4 + (int)(seed >> 8) % 100);
            for (n = 0; n < total;) {
                int item;
                seed = seed * 1664525u + 1013904223u;
                item = 1 + (int)(seed >> 8) % longest;
                if (n + item > total) item = total - n;
                memset(most + n, 'a' + k % 26, (size_t)item);
                n += item;
                if (n + 2 < total) most[n++] = ',', most[n++] = ' ';
            }
            most[n] = 0;
            hud_debug_set(most);
            l = drawn(safe, 1);
            cols = safe[2] / 8 > 2 + 18 ? safe[2] / 8 - 2 : 18;
            letters_all = (int)(strlen(HUD_DEBUG_LEAD) + strlen(most));
            lines = rows(&l, 1, band, &inked);
            assert(inked == letters(HUD_DEBUG_LEAD) + letters(most));
            assert(lines <= 2 * (letters_all / (cols - 1)) + 1);
        }
    }
    /* with the stats' box shown the list keeps clear of it: under it in its column, half a line apart, wrapped to its
       width (28 letters: "Debug on: log=2," then "vk_validation=1," then "shader_corpus, gdbstub"); to its right,
       from its top, when the picture has no room under it */
    assert(hud_debug_set(list));
    {
        static const float stats[4] = {48, 28, 30 * 8, 6 * 16}, tall[4] = {48, 28, 30 * 8, 700};
        l = drawn_by(wide, 1, stats);
        assert(rows(&l, 1, band, &inked) == 3 && inked == letters(HUD_DEBUG_LEAD) + letters(list));
        assert(band[0] == stats[0] && band[1] == stats[1] + stats[3] + 8 && band[2] == (float)(22 + 2) * 8 &&
               band[0] + band[2] <= stats[0] + stats[2]);
        l = drawn_by(wide, 1, tall);
        assert(rows(&l, 1, band, &inked) == 1 && inked == letters(HUD_DEBUG_LEAD) + letters(list));
        assert(band[0] == tall[0] + tall[2] + 8 && band[1] == tall[1]);
        l = drawn(wide, 1); /* and back at the top's middle once the box is gone */
        assert(rows(&l, 1, band, &inked) == 1 && band[0] + band[2] / 2 == (float)wide[0] + (float)wide[2] / 2);
    }
    assert(hud_debug_set("") && (l = drawn(wide, 1), l.n == 0));
    puts("hud passed");
    return 0;
}
