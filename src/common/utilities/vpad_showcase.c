/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/vpad_showcase.h"
#include "common/utilities/stats.h"
#include "common/utilities/common.h"
#include "gui/icons.h"
#include "common/utilities/ui_colors.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* the showcase's quads, kept from frame to frame as the controls' are (common/utilities/vpad.c) */
static u32 g_show_version, g_show_place; /* count the changes to what the showcase draws, and to its place */
u32 vpad_showcase_place_version(void) { return g_show_place; }
static struct {
    int laid, safe[4], fps, scale;
    u32 drawn; /* g_show_version they show */
    SnxUiQuad q[64];
    int n;
    u32 version;
} g_shown;
static void showcase_draw(SnxUiList* l, const int safe[4], int fps, int scale);
/* the showcase: player 1's controller as the game gets it, in a corner */
static u64 g_show_buttons;
static float g_show_axis[2][2];  /* each stick, -1 to 1 right and down */
static _Atomic int g_corner = 3; /* showcase_corner's, an index of CORNERS */
static const char* const CORNERS[4] = {"tl", "tr", "bl", "br"};
/* each control's middle across and down the showcase's box (5:3) and its longer side, of the box's height; none for
   Capture and Home, which never reach the game, nor for L3 and R3, which press their stick's knob */
static const float SHOWCASE[SNX_VPAD_COUNT][3] = {
    [SNX_VPAD_ZL] = {0.13f, 0.11f, 0.22f},      [SNX_VPAD_ZR] = {0.87f, 0.11f, 0.22f},
    [SNX_VPAD_L] = {0.13f, 0.30f, 0.22f},       [SNX_VPAD_R] = {0.87f, 0.30f, 0.22f},
    [SNX_VPAD_MINUS] = {0.40f, 0.30f, 0.10f},   [SNX_VPAD_PLUS] = {0.60f, 0.30f, 0.10f},
    [SNX_VPAD_STICK_L] = {0.22f, 0.60f, 0.34f}, [SNX_VPAD_DPAD] = {0.40f, 0.84f, 0.28f},
    [SNX_VPAD_STICK_R] = {0.62f, 0.84f, 0.30f}, [SNX_VPAD_X] = {0.80f, 0.44f, 0.13f},
    [SNX_VPAD_Y] = {0.72f, 0.58f, 0.13f},       [SNX_VPAD_A] = {0.88f, 0.58f, 0.13f},
    [SNX_VPAD_B] = {0.80f, 0.72f, 0.13f}};
#define SHOWCASE_BOX 0.30f /* the box's height, of safe's shorter side; its gap to the corner a fifteenth of that */

int vpad_corner(const char* word) {
    int i;
    for (i = 0; i < 4; ++i)
        if (!strcmp(word, CORNERS[i])) return i;
    return -1;
}
const char* vpad_corner_word(int corner) { return CORNERS[corner & 3]; }
void vpad_showcase_corner_set(const IniOpt* o, const char* v) {
    ++g_show_version;
    ++g_show_place;
    int i = vpad_corner(v);
    if (i >= 0) {
        atomic_store_explicit(&g_corner, i, memory_order_relaxed); /* the renderer's thread reads it */
        return;
    }
    i = atomic_load_explicit(&g_corner, memory_order_relaxed);
    snx_error("settings: showcase_corner=%s is not tl, tr, bl or br: it keeps %s", v, CORNERS[i]);
    snprintf((char*)o->var, o->size, "%s", CORNERS[i]);
}
void vpad_showcase_input(u64 buttons, const i32 sticks[4]) {
    ++g_show_version;
    int s;
    g_show_buttons = buttons;
    for (s = 0; s < 2; ++s) {
        float ax = (float)sticks[s * 2] / 32767, ay = -(float)sticks[s * 2 + 1] / 32767, len = sqrtf(ax * ax + ay * ay);
        if (len > 1) { /* a square of keys reaches the corners: the knob stays in its ring */
            ax /= len;
            ay /= len;
        }
        g_show_axis[s][0] = ax;
        g_show_axis[s][1] = ay;
    }
}
int vpad_showcase_fps(u32 presents, u64 now_ns) {
    static u64 since_ns;
    static u32 since;
    static int fps = -1;
    if (!since_ns || now_ns - since_ns >= 500000000ull) {
        if (since_ns)
            fps = (int)(((u64)(presents - since) * 1000000000ull + (now_ns - since_ns) / 2) / (now_ns - since_ns));
        since_ns = now_ns;
        since = presents;
    }
    return fps;
}
int vpad_showcase_box(const int safe[4], float box[4]) {
    float shorter = (float)(safe[2] < safe[3] ? safe[2] : safe[3]), h = SHOWCASE_BOX * shorter, w = h * 5 / 3;
    int corner = atomic_load_explicit(&g_corner, memory_order_relaxed);
    if (!atomic_load_explicit((_Atomic int*)&g_settings.showcase, memory_order_relaxed)) return -1;
    box[0] = corner & 1 ? (float)(safe[0] + safe[2]) - h / 15 - w : (float)safe[0] + h / 15;
    box[1] = corner & 2 ? (float)(safe[1] + safe[3]) - h / 15 - h : (float)safe[1] + h / 15;
    box[2] = w;
    box[3] = h;
    return corner;
}
int vpad_showcase_draw(SnxUiList* l, const int safe[4], int fps, int scale) {
    float box[4];
    int how = 0;
    if (vpad_showcase_box(safe, box) < 0) return 2; /* hidden: the setting is off */
    if (atomic_load_explicit((_Atomic int*)&g_settings.stats, memory_order_relaxed))
        fps = -1; /* the stats panel says it already */
    if (!g_shown.laid || g_shown.drawn != g_show_version || g_shown.fps != fps || g_shown.scale != scale ||
        memcmp(g_shown.safe, safe, sizeof g_shown.safe)) {
        how = 1; /* laid out again */
        SnxUiList kept = {g_shown.q, 0, (int)(sizeof g_shown.q / sizeof g_shown.q[0])};
        showcase_draw(&kept, safe, fps, scale);
        g_shown.n = kept.n;
        memcpy(g_shown.safe, safe, sizeof g_shown.safe);
        g_shown.fps = fps;
        g_shown.scale = scale;
        g_shown.drawn = g_show_version;
        g_shown.laid = 1;
        ++g_shown.version;
    }
    ui_append_kept(l, &(SnxUiList){g_shown.q, g_shown.n, g_shown.n}, g_shown.version, &g_shown, NULL);
    return how;
}
static void showcase_draw(SnxUiList* l, const int safe[4], int fps, int scale) {
    float box[4], x0, y0, w, h, r[4];
    int c;
    char text[24];
    if (vpad_showcase_box(safe, box) < 0) return;
    x0 = box[0];
    y0 = box[1];
    w = box[2];
    h = box[3];
    ui_fill(l, x0, y0, w, h, UI_INK); /* the layer's ground, as the stats' */
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        float side = SHOWCASE[c][2] * h, aspect = vpad_aspect(c);
        if (side <= 0 || !vpad_has(c)) continue;
        r[2] = aspect >= 1 ? side : side * aspect;
        r[3] = aspect >= 1 ? side / aspect : side;
        r[0] = x0 + SHOWCASE[c][0] * w - r[2] / 2;
        r[1] = y0 + SHOWCASE[c][1] * h - r[3] / 2;
        vpad_control_draw(l, c, r, g_show_buttons, g_show_axis[c == SNX_VPAD_STICK_R]);
    }
    if (fps < 0) return;
    /* as the stats panel's first line (stats.c), in its letters and colour: a blank cell, the gauge's icon and the
       frames a second; a step smaller while they would not keep above the row under them (a quarter of the box's
       height), which keeps the eleven cells between the shoulder buttons too: the box is 5 by 3 */
    scale = stats_scale(scale);
    while (scale > 1 && (float)(16 * scale) + h / 25 > h * 0.24f) --scale;
    /* "999 fps" at most, the number to the right of its three cells: a field whose place and quads stay, whatever it
       says (ui_cells), each cell backed by the ground the box is filled with: no pixel of a letter left out */
    {
        char* at = text;
        int digits = fps > 99 ? 3 : fps > 9 ? 2 : 1, first;
        float x = x0 + (w - (float)(11 * 8 * scale)) / 2, y = y0 + h / 25;
        while (digits++ < 3) *at++ = ' ';
        *ui_put(ui_put_u(at, (u64)(fps < 999 ? fps : 999)), " fps") = 0;
        x += ui_text_backed(l, x, y, scale, " ", UI_PAGE);
        x += ui_icon_backed(l, x, y, scale, GUI_ICON_GAUGE, UI_PAGE);
        x += ui_text_backed(l, x, y, scale, " ", UI_PAGE);
        first = ui_cells_backed(l, x, y, scale, 7, UI_PAGE);
        if (first >= 0) ui_cells_set(&l->q[first], 7, text);
    }
}
