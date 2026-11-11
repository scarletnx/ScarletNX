/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Debug settings' line over the game (runtime/platform/hud.c): nothing for none, a box at the top's middle of the
   safe area, every letter of the list drawn, cut between its items onto lines that keep inside the area */
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/plat.h"
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

/* the letters of s but its spaces, a quad each */
static int letters(const char* s) {
    int n = 0;
    for (; *s; ++s) n += *s != ' ';
    return n;
}
static SnxUiList drawn(const int safe[4], int scale) {
    SnxUiList l = {quads, 0, QUADS};
    hud_draw(&l, safe, scale);
    return l;
}
/* the rows the letters after the box take */
static int rows(const SnxUiList* l) {
    int i, n = 0;
    float last = -1;
    for (i = 1; i < l->n; ++i)
        if (qv(&quads[i]).y != last) {
            last = qv(&quads[i]).y;
            ++n;
        }
    return n;
}

int main(void) {
    static const int wide[4] = {40, 20, 1280, 720}, narrow[4] = {0, 0, 200, 400}, tiny[4] = {10, 0, 40, 400};
    static const char list[] = "log=2, vk_validation=1, shader_corpus, gdbstub";
    static const char longer[] = "renderdoc_frames=12345678901234567890123456789012345678901234567890";
    SnxUiList l;
    int i;
    /* none: nothing, and the same list again is no change */
    assert(!hud_debug_set(""));
    l = drawn(wide, 1);
    assert(l.n == 0);
    assert(hud_debug_set(list) && !hud_debug_set(list));
    /* one line on a wide picture: the box first, in the middle across, half a line under the safe area's top, then
       every letter in it */
    l = drawn(wide, 2);
    assert(l.n == 1 + letters(HUD_DEBUG_LEAD) + letters(list));
    assert(rows(&l) == 1);
    assert(qv(&quads[0]).w == (float)(strlen(HUD_DEBUG_LEAD) + strlen(list) + 2) * 16);
    assert(qv(&quads[0]).x + qv(&quads[0]).w / 2 == (float)wide[0] + (float)wide[2] / 2);
    assert(qv(&quads[0]).y == (float)wide[1] + 16 && qv(&quads[0]).h == 32 + 16);
    for (i = 1; i < l.n; ++i) assert(qv(&quads[i]).x >= qv(&quads[0]).x &&
                                     qv(&quads[i]).x + qv(&quads[i]).w <= qv(&quads[0]).x + qv(&quads[0]).w);
    /* 23 letters across a narrow one: "Debug on: log=2," then "vk_validation=1," then "shader_corpus, gdbstub", none
       of them cut, all of them inside it */
    l = drawn(narrow, 1);
    assert(l.n == 1 + letters(HUD_DEBUG_LEAD) + letters(list));
    assert(rows(&l) == 3 && qv(&quads[0]).h == 3 * 16 + 8);
    assert(qv(&quads[0]).w == (float)(22 + 2) * 8);
    for (i = 0; i < l.n; ++i) assert(qv(&quads[i]).x >= 0 && qv(&quads[i]).x + qv(&quads[i]).w <= (float)narrow[2]);
    /* an item longer than a line goes on over the next ones, a letter each still */
    assert(hud_debug_set(longer));
    l = drawn(narrow, 1);
    assert(l.n == 1 + letters(HUD_DEBUG_LEAD) + letters(longer) && rows(&l) == 4);
    /* a picture too narrow for the lead and eight letters: the box goes past its sides, and shows it all */
    l = drawn(tiny, 1);
    assert(l.n == 1 + letters(HUD_DEBUG_LEAD) + letters(longer));
    assert(qv(&quads[0]).w == (float)(strlen(HUD_DEBUG_LEAD) + 8 + 2) * 8 && qv(&quads[0]).x < (float)tiny[0]);
    assert(hud_debug_set("") && (l = drawn(wide, 1), l.n == 0));
    puts("hud passed");
    return 0;
}
