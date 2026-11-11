/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/ui_shape_ref.h"
#include "common/utilities/font.h"
#include <math.h>

#define UI_SHAPE_SURE 1e-3f /* the distance from a bound past which the GPU's rounding can't move a pixel's middle */

static float box_distance(float px, float py, float bx, float by) {
    float dx = fabsf(px) - bx, dy = fabsf(py) - by, ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    float in = dx > dy ? dx : dy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0);
}
static float shape_distance(unsigned kind, float px, float py, float w, float h) {
    float cx = px - w * 0.5f, cy = py - h * 0.5f, m = w < h ? w : h, a, b;
    if (kind == 0) return sqrtf(cx * cx + cy * cy) - m * 0.5f;
    if (kind == 1) return box_distance(cx, cy, w * 0.5f - m * 0.25f, h * 0.5f - m * 0.25f) - m * 0.25f;
    if (kind == 3) return box_distance(cx, cy, w * 0.5f, h * 0.5f);
    a = box_distance(cx, cy, w * 0.5f, m / 6.0f);
    b = box_distance(cx, cy, m / 6.0f, h * 0.5f);
    return a < b ? a : b;
}
/* the label's bit at px, py: 1, 0, or -1 on a cell's bound */
static int label_bit(unsigned label, float px, float py, float w, float h) {
    unsigned c0 = label & 0xFF, c1 = label >> 8, n = c1 ? 2 : c0 ? 1 : 0, x;
    float s, ax, ay, qx, qy, fx, fy;
    if (!n || 16.0f > h * 0.8f || 8.0f * (float)n > w * 0.8f) return 0;
    s = floorf(fminf(h * 0.5f / 16.0f, w * 0.6f / (8.0f * (float)n)));
    s = s < 1 ? 1 : s;
    ax = floorf((w - 8.0f * (float)n * s) * 0.5f);
    ay = floorf((h - 16.0f * s) * 0.5f);
    fx = (px - ax) / s;
    fy = (py - ay) / s;
    if (fabsf(fx - roundf(fx)) < UI_SHAPE_SURE || fabsf(fy - roundf(fy)) < UI_SHAPE_SURE) return -1;
    qx = floorf(fx);
    qy = floorf(fy);
    if (qx < 0 || qy < 0 || qx >= 8.0f * (float)n || qy >= 16) return 0;
    x = (unsigned)qx;
    return gui_vga_font[(x < 8 ? c0 : c1) * 16 + (unsigned)qy] >> (7 - (x & 7)) & 1;
}
int ui_shape_ref(unsigned kind, unsigned label, unsigned flags, unsigned edge, float w, float h, float px, float py) {
    float d = shape_distance(kind, px, py, w, h);
    int mark = label_bit(label, px, py, w, h);
    if (fabsf(d) < UI_SHAPE_SURE || fabsf(d + (float)edge) < UI_SHAPE_SURE || mark < 0) return -1;
    if (d > 0) return 0;
    if (flags & 1) return !mark;
    return mark || (!(flags & 2) && d > -(float)edge);
}
