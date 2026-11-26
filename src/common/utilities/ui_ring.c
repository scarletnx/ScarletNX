/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/ui_ring.h"
#include "common/utilities/ui_colors.h"
#include "gui/icons.h"
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
