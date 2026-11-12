/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/ui_colors.h"
#include "common/utilities/slide.h"
#include <stdio.h>
#include <string.h>

/* the layer's colours (ui_colors.h): the ink behind, the page's for the rows, the failure colour for the title */
#define HUD_COLS  STATS_GRAPH_W /* the list's width in letters: a blank, the row's room and a blank */
#define HUD_ROOM  (HUD_COLS - 2)
#define HUD_QUADS (HUD_ROWS_MAX * HUD_COLS + 2) /* its rows' cells and the two strips of its margin */
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
    /* each row laid out: its cells' first quad, its setting's place in debug (at, len; len 0 for the title and the
       row of the ones left out), how many columns it is over its room and how many it skips now */
    int rows, first[HUD_ROWS_MAX], at[HUD_ROWS_MAX], len[HUD_ROWS_MAX], over[HUD_ROWS_MAX], skip[HUD_ROWS_MAX];
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

/* the settings of the list, each up to its ", ": how many, and the place of each of the first max in at and len */
static int items(int* at, int* len, int max) {
    const char *s = g_hud.debug, *end;
    int n = 0;
    while (*s) {
        end = strstr(s, ", ");
        if (!end) end = s + strlen(s);
        if (n < max) {
            at[n] = (int)(s - g_hud.debug);
            len[n] = (int)(end - s);
        }
        ++n;
        s = *end ? end + 2 : end;
    }
    return n;
}
/* the setting of len letters at s as its row says it, a blank at either side in HUD_COLS letters: its key at the left
   and its value at the right where both fit the room, else the key, a space and the value from skip on, cut at the
   room. How many letters key, space and value are over the room (0 when they fit) */
static int row_text(const char* s, int len, int skip, char out[HUD_COLS + 1]) {
    const char* eq = memchr(s, '=', (size_t)len);
    int key = eq ? (int)(eq - s) : len, value = eq ? len - key - 1 : 0, whole = key + (value ? 1 + value : 0), i;
    memset(out, ' ', HUD_COLS);
    out[HUD_COLS] = 0;
    if (whole <= HUD_ROOM) {
        memcpy(out + 1, s, (size_t)key);
        if (value) memcpy(out + 1 + HUD_ROOM - value, eq + 1, (size_t)value);
        return 0;
    }
    for (i = 0; i < HUD_ROOM; ++i) {
        int k = skip + i; /* the letter k of key, a space and value */
        out[1 + i] = k < key ? s[k] : k == key ? ' ' : eq[1 + (k - key - 1)];
    }
    return whole - HUD_ROOM;
}
/* row r's letters at its skip into its cells */
static void row_set(int r) {
    char text[HUD_COLS + 1];
    row_text(g_hud.debug + g_hud.at[r], g_hud.len[r], g_hud.skip[r], text);
    ui_cells_set(&g_hud.q[g_hud.first[r]], HUD_COLS, text);
}

static void hud_layout(SnxUiList* l, const int safe[4], int scale, const float* stats) {
    float cw = 8.0f * (float)scale, ch = 16.0f * (float)scale, w = HUD_COLS * cw, x, y, h;
    int at[HUD_ROWS_MAX], len[HUD_ROWS_MAX], n = items(at, len, HUD_ROWS_MAX), rows, fit, r;
    char text[HUD_COLS + 1];
    g_hud.rows = 0;
    if (!n) return;
    rows = 1 + n; /* the title and a row each */
    h = (float)rows * ch + ch / 2;
    /* the stats' box shown: under it in its column, or to its right when the picture has no room under it */
    if (stats && stats[1] + stats[3] + ch / 2 + h <= (float)(safe[1] + safe[3])) {
        x = stats[0];
        y = stats[1] + stats[3] + ch / 2;
    } else if (stats && stats[0] + stats[2] + cw + w <= (float)(safe[0] + safe[2])) {
        x = stats[0] + stats[2] + cw;
        y = stats[1];
    } else {
        x = (float)safe[0] + ((float)safe[2] - w) / 2;
        y = (float)safe[1] + ch / 2;
    }
    /* as many rows as the picture holds under y, the title and one at the least, and HUD_ROWS_MAX at most */
    fit = (int)(((float)(safe[1] + safe[3]) - y - ch / 2) / ch);
    if (fit > HUD_ROWS_MAX) fit = HUD_ROWS_MAX;
    if (fit < 2) fit = 2;
    if (rows > fit) rows = fit;
    ui_fill(l, x, y, w, ch / 4, UI_INK);
    ui_fill(l, x, y + ch / 4 + (float)rows * ch, w, ch / 4, UI_INK);
    for (r = 0; r < rows; ++r) {
        float ry = y + ch / 4 + (float)r * ch;
        g_hud.first[r] = ui_cells_backed(l, x, ry, scale, HUD_COLS, r ? UI_PAGE : UI_FAILURE);
        g_hud.skip[r] = g_hud.over[r] = g_hud.len[r] = 0;
        g_hud.at[r] = 0;
        if (!r) { /* the title */
            row_text(HUD_DEBUG_TITLE, (int)strlen(HUD_DEBUG_TITLE), 0, text);
            ui_cells_set(&l->q[g_hud.first[r]], HUD_COLS, text);
        } else if (r == rows - 1 && 1 + n > rows) { /* the ones that do not fit, counted */
            snprintf(text, sizeof text, " +%d more", n - (rows - 2));
            ui_cells_set(&l->q[g_hud.first[r]], HUD_COLS, text);
        } else {
            g_hud.at[r] = at[r - 1];
            g_hud.len[r] = len[r - 1];
            g_hud.over[r] = row_text(g_hud.debug + at[r - 1], len[r - 1], 0, text);
            ui_cells_set(&l->q[g_hud.first[r]], HUD_COLS, text);
        }
    }
    g_hud.rows = rows;
}
void hud_draw(SnxUiList* l, const int safe[4], int scale, const float* stats, u64 now_ms) {
    float none[4] = {0};
    int r, moved = 0;
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
        moved = 1;
    }
    /* a row too long slides: its letters change in the quads it has, nothing is laid out again */
    for (r = 0; r < g_hud.rows; ++r) {
        int skip = snx_slide(g_hud.over[r], now_ms, HUD_SLIDE_MS);
        if (!g_hud.over[r] || skip == g_hud.skip[r]) continue;
        g_hud.skip[r] = skip;
        row_set(r);
        moved = 1;
    }
    if (moved) ++g_hud.version;
    ui_append_kept(l, &(SnxUiList){g_hud.q, g_hud.n, (int)HUD_QUADS}, g_hud.version, &g_hud, NULL);
}
