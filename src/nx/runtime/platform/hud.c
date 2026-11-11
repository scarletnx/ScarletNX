/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/ui_colors.h"
#include <stdio.h>
#include <string.h>

/* the layer's colours (ui_colors.h): the ink behind, the page's for the letters, the failure colour for the lead */
/* The lines' quads: laid out when the list, the place or the scale changes, and kept for the frames after. A line is a
   blank cell, a cell for each of its letters and a fill to its band's end: its letters and two. A line and the one
   after it hold a line's room of letters less one at the least (the first item of the second did not fit the first),
   and a line has room for 18 letters or more, so the lines are at most 2 for each 17 letters and one more: the quads
   under 5 for each 4 letters, and 2, and the two strips of the margin */
#define HUD_QUADS ((HUD_DEBUG_MAX + sizeof HUD_DEBUG_LEAD) * 5 / 4 + 4)
static struct {
    char debug[HUD_DEBUG_MAX];
    u32 gen; /* the lists set */
    struct {
        int safe[4], scale;
        u32 gen;
    } laid; /* what the quads are laid out for; gen 0: nothing yet */
    SnxUiQuad q[HUD_QUADS];
    int n;
    u32 version;
} g_hud;

int hud_debug_set(const char* text) {
    int changed;
    ui_lock();
    changed = strcmp(g_hud.debug, text) != 0;
    if (changed) {
        snprintf(g_hud.debug, sizeof g_hud.debug, "%s", text);
        if (!++g_hud.gen) g_hud.gen = 1;
    }
    ui_unlock();
    return changed;
}

/* the letters of the line that starts at s with room for room of them: up to its last ", " that fits, the comma kept,
   else room letters of an item longer than the line. *next: where the line after it starts */
static size_t line_of(const char* s, size_t room, const char** next) {
    size_t n = strlen(s), end = 0, i;
    if (n <= room) {
        *next = s + n;
        return n;
    }
    for (i = 0; i < room && i + 1 < n; ++i)
        if (s[i] == ',' && s[i + 1] == ' ') end = i + 1;
    if (!end) {
        *next = s + room;
        return room;
    }
    *next = s + end + 1;
    return end;
}

static void hud_layout(SnxUiList* l, const int safe[4], int scale) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, x, y;
    size_t lead = strlen(HUD_DEBUG_LEAD), cols = (size_t)((float)safe[2] / cw), longest = 0, lines = 0, n;
    const char *s, *next;
    char line[HUD_DEBUG_MAX];
    if (!g_hud.debug[0]) return;
    cols = cols > 2 ? cols - 2 : 0;       /* a letter's gap at either side */
    if (cols < lead + 8) cols = lead + 8; /* a picture narrower than that still shows it all, past its sides */
    for (s = g_hud.debug; *s; s = next, ++lines) {
        n = line_of(s, lines ? cols : cols - lead, &next) + (lines ? 0 : lead);
        if (n > longest) longest = n;
    }
    /* The box as it looks, a letter's gap at either side of the longest line and a quarter line above and under the
       lines, with no fill behind the letters: each line is a band of the layer's ground made of a blank cell, its
       letters, backed (ui.h's UI_BACKED), and a fill of the ink from them to the box's right; the margins are a strip
       of the ink each. Nothing is drawn under a letter, and all of it is the plain pipeline's */
    x = (float)safe[0] + ((float)safe[2] - (float)(longest + 2) * cw) / 2;
    y = (float)safe[1] + ch / 2;
    ui_fill(l, x, y, (float)(longest + 2) * cw, ch / 4, UI_INK);
    ui_fill(l, x, y + ch / 4 + (float)lines * ch, (float)(longest + 2) * cw, ch / 4, UI_INK);
    y += ch / 4;
    for (s = g_hud.debug, lines = 0; *s; s = next, ++lines) {
        float row = y + (float)lines * ch, at = x + ui_text_backed(l, x, row, scale, " ", UI_PAGE);
        n = line_of(s, lines ? cols : cols - lead, &next);
        memcpy(line, s, n);
        line[n] = 0;
        if (!lines) at += ui_text_backed(l, at, row, scale, HUD_DEBUG_LEAD, UI_FAILURE);
        at += ui_text_backed(l, at, row, scale, line, UI_PAGE);
        ui_fill(l, at, row, x + (float)(longest + 2) * cw - at, ch, UI_INK);
    }
}
void hud_draw(SnxUiList* l, const int safe[4], int scale) {
    if (!g_hud.debug[0]) return;
    if (g_hud.laid.gen != g_hud.gen || g_hud.laid.scale != scale ||
        memcmp(g_hud.laid.safe, safe, sizeof g_hud.laid.safe)) {
        SnxUiList kept = {g_hud.q, 0, (int)HUD_QUADS};
        hud_layout(&kept, safe, scale);
        g_hud.n = kept.n;
        memcpy(g_hud.laid.safe, safe, sizeof g_hud.laid.safe);
        g_hud.laid.scale = scale;
        g_hud.laid.gen = g_hud.gen;
        ++g_hud.version;
    }
    ui_append_kept(l, &(SnxUiList){g_hud.q, g_hud.n, (int)HUD_QUADS}, g_hud.version, &g_hud, NULL);
}
