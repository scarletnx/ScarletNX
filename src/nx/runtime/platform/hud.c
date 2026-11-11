/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/hud.h"
#include "gui_theme_colors.h"
#include <stdio.h>
#include <string.h>

/* the menu's colours (gui_theme_colors.h, the YoRHa theme) as R G B A bytes, opaque: its ink behind, its page's
   letters, and its failure colour for the lead */
#define HUD_RGBA(rgb, a) ((u32)(a) << 24 | ((rgb) & 0xFFu) << 16 | ((rgb) & 0xFF00u) | ((rgb) >> 16 & 0xFFu))
#define HUD_BACK         HUD_RGBA(SNX_GUI_THEME_YORHA_INK, 0xFFu)
#define HUD_TEXT         HUD_RGBA(SNX_GUI_THEME_YORHA_PAGE, 0xFFu)
#define HUD_WARN         HUD_RGBA(SNX_GUI_THEME_YORHA_FAILURE, 0xFFu)

/* the box, the lead and a letter for each of the list's: laid out when the list, the place or the scale changes, and
   kept for the frames after */
#define HUD_QUADS (1 + HUD_DEBUG_MAX + sizeof HUD_DEBUG_LEAD)
static struct {
    char debug[HUD_DEBUG_MAX];
    u32 gen; /* the lists set */
    struct {
        int safe[4], scale;
        u32 gen;
    } laid; /* what the quads are laid out for; gen 0: nothing yet */
    SnxUiQuad q[HUD_QUADS];
    int n;
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
    x = (float)safe[0] + ((float)safe[2] - (float)(longest + 2) * cw) / 2;
    y = (float)safe[1] + ch / 2;
    ui_fill(l, x, y, (float)(longest + 2) * cw, (float)lines * ch + ch / 2, HUD_BACK);
    x += cw;
    y += ch / 4;
    for (s = g_hud.debug, lines = 0; *s; s = next, ++lines) {
        float at = x;
        n = line_of(s, lines ? cols : cols - lead, &next);
        memcpy(line, s, n);
        line[n] = 0;
        if (!lines) at += ui_text(l, at, y, scale, HUD_DEBUG_LEAD, HUD_WARN);
        ui_text(l, at, y + (float)lines * ch, scale, line, HUD_TEXT);
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
    }
    ui_append(l, &(SnxUiList){g_hud.q, g_hud.n, (int)HUD_QUADS});
}
