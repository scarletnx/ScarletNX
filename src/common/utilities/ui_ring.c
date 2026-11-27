/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/ui_ring.h"
#include "common/utilities/ui_colors.h"
#include "gui/icons.h"
#include <math.h>
#include <string.h>

u32 ui_ground(void) { return ui_rgba(UI_INK); }
int ui_drawn_ground(const SnxUiQuad* q, SnxUiQuadView* fill) {
    SnxUiQuad ground;
    unsigned ink = ui_q_bits(q, UI_Q_INK, 3);
    if ((ink != UI_GLYPH && ink != UI_ICON) || !ui_q_bits(q, UI_Q_BACKED, 1) || !q->uw) return 0;
    ground = *q; /* its place as it is, in quarters */
    ground.u = SNX_UI_WHITE_X + 1;
    ground.v = SNX_UI_WHITE_Y + 1;
    ground.uw = ground.vh = 2;
    ground.bits = ui_quad(0, 0, 0, 0, 0, 0, 0, 0, UI_INK, 0, UI_SHARP, UI_SOLID, 0).bits;
    *fill = ui_view(&ground);
    return 1;
}
float ui_icon_backed(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba) {
    int n = l->n;
    float w = ui_icon(l, x, y, scale, icon, rgba);
    if (l->n > n) l->q[n].bits |= 1u << UI_Q_BACKED;
    return w;
}
static SnxUiRing g_ring;
void ui_ring_set(const u8* height, int n, int head, int slow_from, u32 slow, u32 version) {
    n = n < 0 ? 0 : n > SNX_UI_RING_MAX ? SNX_UI_RING_MAX : n;
    if (version != g_ring.version || n != g_ring.n) memcpy(g_ring.height, height, (size_t)n);
    g_ring.version = version;
    g_ring.n = (u16)n;
    g_ring.head = (u16)(n ? head % n : 0);
    g_ring.slow_from = (u8)slow_from;
    g_ring.slow = slow;
}
const SnxUiRing* ui_ring(void) { return &g_ring; }
void ui_ring_bars(SnxUiList* l, float x, float y, float w, float h, int n, u32 rgba) {
    int k, first = l->n;
    for (k = 0; k < n && l->n < l->max; ++k)
        l->q[l->n++] = ui_quad(x, y, w, h, SNX_UI_WHITE_X + 1, SNX_UI_WHITE_Y + 1, 2, 2, rgba, 0, UI_SHARP, UI_RING, k);
    ui_written(l, first, l->n);
}
SnxUiQuadView ui_drawn(const SnxUiQuad* q) {
    SnxUiQuadView d = ui_view(q);
    unsigned age, height, slot = ui_q_bits(q, UI_Q_GLYPH, 8);
    float bw;
    if (d.ink != UI_RING) return d;
    height = g_ring.n && slot < g_ring.n ? g_ring.height[slot] : 0;
    if (!height) {
        d.uw = d.vh = 0;
        return d;
    }
    /* as ui.vert places it: a bar's width over from the graph's left for each slot older than the newest */
    age = ((unsigned)g_ring.head + g_ring.n - slot) % g_ring.n;
    bw = d.w / (float)g_ring.n;
    d.x += bw * (float)(g_ring.n - 1 - age);
    d.w = bw;
    d.h = d.h * (float)height / 255.0f;
    d.y = (float)q->y / UI_Q_STEPS + (float)q->h / UI_Q_STEPS - d.h;
    if (height > g_ring.slow_from) d.rgba = g_ring.slow;
    return d;
}
float ui_icon(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba) {
    if (icon < GUI_ICON_FIRST || icon >= GUI_ICON_END) return 0;
    ui_quad_add(l, x, y, 16.0f * (float)scale, 16.0f * (float)scale, SNX_UI_ICONS_X + 16 * (int)(icon - GUI_ICON_FIRST),
                SNX_UI_PICTURE_Y, 16, 16, rgba, 0, UI_SHARP, UI_ICON, (int)(icon - GUI_ICON_FIRST));
    return 16.0f * (float)scale;
}
void ui_loading_ring(SnxUiList* l, float cx, float cy, int scale, u64 now_ms, int still, u32 dim, u32 soft, u32 ink) {
    /* the launcher's strokes are a line a pixel in from each whole pixel of their width: 1.5 at 1x takes two */
    const float s = (float)scale, r = 7.0f * s, inner = r * 0.62f, thick = ceilf(s * 1.5f);
    const float mid = inner - thick / 2, from = still ? 0.0f : (float)(now_ms / 4 % 360);
    const int edge = (int)ceilf(s * 0.5f);
    ui_shape(l, cx - r, cy - r, 2 * r, 2 * r, UI_SHAPE_CIRCLE, NULL, 0, edge, dim);
    ui_shape(l, cx - inner, cy - inner, 2 * inner, 2 * inner, UI_SHAPE_CIRCLE, NULL, 0, (int)thick, soft);
    for (int i = 0; i < UI_RING_BAR_DOTS; ++i) { /* dots a stroke wide, overlapping along the bar */
        const float a = (from + 70.0f * (float)i / (UI_RING_BAR_DOTS - 1)) * 3.14159265f / 180.0f;
        ui_shape(l, cx + mid * cosf(a) - thick / 2, cy + mid * sinf(a) - thick / 2, thick, thick, UI_SHAPE_CIRCLE, NULL,
                 UI_SHAPE_FILLED, 1, ink);
    }
}
