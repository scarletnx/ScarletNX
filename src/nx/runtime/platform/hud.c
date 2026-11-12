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
        int safe[4], scale, boxed;
        float stats[4];
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

/* the lines the list takes at cols letters a line, and the longest of them in *longest */
static size_t lines_at(size_t cols, size_t* longest) {
    size_t lead = strlen(HUD_DEBUG_LEAD), lines = 0, n;
    const char *s, *next;
    *longest = 0;
    for (s = g_hud.debug; *s; s = next, ++lines) {
        n = line_of(s, lines ? cols : cols - lead, &next) + (lines ? 0 : lead);
        if (n > *longest) *longest = n;
    }
    return lines;
}
/* the letters a band of width w holds, a letter's gap at either side, and never under the lead and 8: a place
   narrower than that still shows it all, past its sides */
static size_t cols_of(float w, float cw) {
    size_t cols = w > 2 * cw ? (size_t)(w / cw) - 2 : 0;
    return cols < strlen(HUD_DEBUG_LEAD) + 8 ? strlen(HUD_DEBUG_LEAD) + 8 : cols;
}
static void hud_layout(SnxUiList* l, const int safe[4], int scale, const float* stats) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, x, y;
    size_t lead = strlen(HUD_DEBUG_LEAD), cols = cols_of((float)safe[2], cw), longest = 0, lines, n;
    const char *s, *next;
    char line[HUD_DEBUG_MAX];
    int beside = 0; /* 1 under the stats' box, 2 to its right */
    if (!g_hud.debug[0]) return;
    /* the stats' box shown: the list keeps clear of it, under it in its column, or to its right when the picture has
       no room under it, each wrapped to the room there */
    if (stats) {
        size_t c = cols_of(stats[2], cw);
        float under = stats[1] + stats[3] + ch / 2, right = (float)(safe[0] + safe[2]) - (stats[0] + stats[2] + cw);
        if (under + (float)lines_at(c, &longest) * ch + ch / 2 <= (float)(safe[1] + safe[3])) {
            beside = 1;
            cols = c;
        } else if (right >= (float)(lead + 10) * cw) {
            beside = 2;
            cols = cols_of(right, cw);
        }
    }
    lines = lines_at(cols, &longest);
    /* The box as it looks, a letter's gap at either side of the longest line and a quarter line above and under the
       lines, with no fill behind the letters: each line is a band of the layer's ground made of a blank cell, its
       letters, backed (ui.h's UI_BACKED), and a fill of the ink from them to the box's right; the margins are a strip
       of the ink each. Nothing is drawn under a letter, and all of it is the plain pipeline's */
    if (beside == 1) {
        x = stats[0];
        y = stats[1] + stats[3] + ch / 2;
    } else if (beside == 2) {
        x = stats[0] + stats[2] + cw;
        y = stats[1];
    } else {
        x = (float)safe[0] + ((float)safe[2] - (float)(longest + 2) * cw) / 2;
        y = (float)safe[1] + ch / 2;
    }
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
void hud_draw(SnxUiList* l, const int safe[4], int scale, const float* stats) {
    float none[4] = {0};
    if (!g_hud.debug[0]) return;
    if (g_hud.laid.gen != g_hud.gen || g_hud.laid.scale != scale ||
        memcmp(g_hud.laid.safe, safe, sizeof g_hud.laid.safe) || g_hud.laid.boxed != (stats != NULL) ||
        memcmp(g_hud.laid.stats, stats ? stats : none, sizeof g_hud.laid.stats)) {
        SnxUiList kept = {g_hud.q, 0, (int)HUD_QUADS};
        hud_layout(&kept, safe, scale, stats);
        g_hud.n = kept.n;
        memcpy(g_hud.laid.safe, safe, sizeof g_hud.laid.safe);
        memcpy(g_hud.laid.stats, stats ? stats : none, sizeof g_hud.laid.stats);
        g_hud.laid.boxed = stats != NULL;
        g_hud.laid.scale = scale;
        g_hud.laid.gen = g_hud.gen;
        ++g_hud.version;
    }
    ui_append_kept(l, &(SnxUiList){g_hud.q, g_hud.n, (int)HUD_QUADS}, g_hud.version, &g_hud, NULL);
}
