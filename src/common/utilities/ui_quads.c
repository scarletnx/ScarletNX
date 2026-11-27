/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/ui_quads.h"
#include "common/platform/thread.h"
#include <string.h>

static PMutex g_lock = PMUTEX_INIT;
void ui_lock(void) { pm_lock(&g_lock); }
void ui_unlock(void) { pm_unlock(&g_lock); }

static_assert(sizeof(SnxUiQuad) == 20, "a quad is the renderer's instance, with no padding");

/* v in quarters of a pixel, rounded, within lo and hi */
static int quarters(float v, int lo, int hi) {
    float q = v * UI_Q_STEPS + (v < 0 ? -0.5f : 0.5f);
    return q <= (float)lo ? lo : q >= (float)hi ? hi : (int)q;
}
/* an 8-bit channel to 5 bits, rounded, and back as the GPU widens it: the top bits again under the 5 */
static unsigned to5(unsigned c) { return (c * 31 + 127) / 255; }
static unsigned from5(unsigned c) { return c << 3 | c >> 2; }
u32 ui_rgba(u32 rgba) {
    return from5(to5(rgba & 0xFF)) | from5(to5(rgba >> 8 & 0xFF)) << 8 | from5(to5(rgba >> 16 & 0xFF)) << 16 |
           (rgba & 0xFF000000u);
}
SnxUiQuad ui_quad(float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn, int sample,
                  int ink, int glyph) {
    unsigned a = rgba >> 24, color = to5(rgba & 0xFF) | to5(rgba >> 8 & 0xFF) << 5 | to5(rgba >> 16 & 0xFF) << 10 |
                                     (a >= 128 ? 1u : 0u) << 15;
    int alpha_glyph = ink == UI_ATLAS || ink == UI_SOLID; /* no glyph: the alpha's 8 bits in its byte */
    return (SnxUiQuad){.x = (i16)quarters(x, -32768, 32767),
                       .y = (i16)quarters(y, -32768, 32767),
                       .w = (u16)quarters(w, 0, 65535),
                       .h = (u16)quarters(h, 0, 65535),
                       .u = (u16)u,
                       .v = (u16)v,
                       .uw = (u16)uw,
                       .vh = (u16)vh,
                       .bits = color << UI_Q_COLOR | (u32)(turn & 3) << UI_Q_TURN |
                               (u32)(turn & UI_BACKED ? 1 : 0) << UI_Q_BACKED | (u32)(sample & 3) << UI_Q_SAMPLE |
                               (u32)(ink & 7) << UI_Q_INK | (u32)((alpha_glyph ? (int)a : glyph) & 0xFF) << UI_Q_GLYPH};
}
SnxUiQuadView ui_view(const SnxUiQuad* q) {
    unsigned ink = ui_q_bits(q, UI_Q_INK, 3), byte = ui_q_bits(q, UI_Q_GLYPH, 8), alpha_glyph = ink <= UI_SOLID;
    unsigned a = alpha_glyph ? byte : ui_q_bits(q, 15, 1) * 255;
    return (SnxUiQuadView){.x = (float)q->x / UI_Q_STEPS,
                           .y = (float)q->y / UI_Q_STEPS,
                           .w = (float)q->w / UI_Q_STEPS,
                           .h = (float)q->h / UI_Q_STEPS,
                           .u = q->u,
                           .v = q->v,
                           .uw = q->uw,
                           .vh = q->vh,
                           .rgba = from5(ui_q_bits(q, 0, 5)) | from5(ui_q_bits(q, 5, 5)) << 8 |
                                   from5(ui_q_bits(q, 10, 5)) << 16 | a << 24,
                           .turn = (u8)ui_q_bits(q, UI_Q_TURN, 2),
                           .backed = (u8)ui_q_bits(q, UI_Q_BACKED, 1),
                           .sample = (u8)ui_q_bits(q, UI_Q_SAMPLE, 2),
                           .ink = (u8)ink,
                           .glyph = (u8)(alpha_glyph ? 0 : byte)};
}
/* the renderer's list (ui_dirty_begin), the span of it written since, and the parts kept in it (ui_append_kept) this
   frame and the one before; and its spans drawn translucent (ui_fade_set) */
#define UI_PARTS 16
typedef struct {
    const void* who;
    u32 version;
    int at, n;
} UiPart;
static struct {
    const SnxUiQuad* list;
    const SnxUiQuad* was; /* the list of the frame before */
    int first, end;
    UiPart part[2][UI_PARTS];
    int parts[2], now;
    SnxUiFade fade[SNX_UI_FADES];
    int fades;
} g_dirty;
void ui_fade_set(int first, int end, unsigned alpha) {
    if (first >= end || alpha >= 255 || g_dirty.fades == SNX_UI_FADES) return;
    g_dirty.fade[g_dirty.fades++] = (SnxUiFade){first, end, alpha};
}
int ui_fades(SnxUiFade fades[SNX_UI_FADES]) {
    memset(fades, 0, sizeof *fades * SNX_UI_FADES);
    memcpy(fades, g_dirty.fade, sizeof *fades * (size_t)g_dirty.fades);
    return g_dirty.fades;
}
void ui_dirty_begin(const SnxUiQuad* list) {
    g_dirty.fades = 0;
    g_dirty.now ^= 1;
    g_dirty.parts[g_dirty.now] = 0;
    if (list != g_dirty.was) g_dirty.parts[g_dirty.now ^ 1] = 0; /* another list: nothing in it is known */
    g_dirty.list = g_dirty.was = list;
    g_dirty.first = g_dirty.end = 0;
}
void ui_dirty_end(int* first, int* end) {
    *first = g_dirty.first;
    *end = g_dirty.end;
    g_dirty.list = NULL;
}
/* l's quads first to end written */
void ui_written(const SnxUiList* l, int first, int end) {
    if (l->q != g_dirty.list || first >= end) return;
    if (g_dirty.first >= g_dirty.end || first < g_dirty.first) g_dirty.first = first;
    if (end > g_dirty.end) g_dirty.end = end;
}
void ui_quad_add(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn,
                 int sample, int ink, int glyph) {
    if (l->n >= l->max || w <= 0 || h <= 0) return;
    l->q[l->n] = ui_quad(x, y, w, h, u, v, uw, vh, rgba, turn, sample, ink, glyph);
    ui_written(l, l->n, l->n + 1);
    ++l->n;
}
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba) {
    /* the block's inner texels: a blend at its edge never reaches the transparent ones around it */
    ui_quad_add(l, x, y, w, h, SNX_UI_WHITE_X + 1, SNX_UI_WHITE_Y + 1, 2, 2, rgba, 0, UI_SHARP, UI_SOLID, 0);
}
void ui_shape(SnxUiList* l, float x, float y, float w, float h, int kind, const char* label, int flags, int edge,
              u32 rgba) {
    unsigned c0 = label && label[0] ? (u8)label[0] : 0, c1 = c0 && label[1] ? (u8)label[1] : 0;
    ui_quad_add(l, x, y, w, h, kind, (int)(c0 | c1 << 8), 0x8000 | flags, edge < 1 ? 1 : edge, rgba, 0, UI_SHARP,
                UI_SHAPE, 0);
}
float ui_text(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba) {
    float x0 = x;
    for (; *s; ++s, x += 8.0f * (float)scale) {
        u8 c = (u8)*s;
        if (c != ' ')
            ui_quad_add(l, x, y, 8.0f * (float)scale, 16.0f * (float)scale, SNX_UI_FONT_X + c % 16 * 8,
                        SNX_UI_FONT_Y + c / 16 * 16, 8, 16, rgba, 0, UI_SHARP, UI_GLYPH, c);
    }
    return x - x0;
}
float ui_text_backed(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba) {
    float x0 = x;
    for (; *s; ++s, x += 8.0f * (float)scale) {
        u8 c = (u8)*s;
        int n = l->n;
        ui_quad_add(l, x, y, 8.0f * (float)scale, 16.0f * (float)scale, SNX_UI_FONT_X + c % 16 * 8,
                    SNX_UI_FONT_Y + c / 16 * 16, 8, 16, rgba, 0, UI_SHARP, UI_GLYPH, c);
        if (l->n > n) l->q[n].bits |= 1u << UI_Q_BACKED;
    }
    return x - x0;
}
int ui_cells_backed(SnxUiList* l, float x, float y, int scale, int n, u32 rgba) {
    int first = ui_cells(l, x, y, scale, n, rgba), k;
    if (first < 0) return -1;
    for (k = 0; k < n; ++k) l->q[first + k].bits |= 1u << UI_Q_BACKED;
    ui_cells_set(&l->q[first], n, ""); /* blank: the ground */
    return first;
}
int ui_cells(SnxUiList* l, float x, float y, int scale, int n, u32 rgba) {
    int first = l->n, k;
    if (n < 0 || n > l->max - l->n) return -1;
    for (k = 0; k < n; ++k)
        l->q[l->n++] = ui_quad(x + 8.0f * (float)(scale * k), y, 8.0f * (float)scale, 16.0f * (float)scale, 0, 0, 0, 0,
                               rgba, 0, UI_SHARP, UI_GLYPH, ' ');
    ui_written(l, first, l->n);
    return first;
}
int ui_cells_set(SnxUiQuad* q, int n, const char* s) {
    int k, changed = 0;
    for (k = 0; k < n; ++k) {
        u8 c = *s ? (u8)*s++ : ' ';
        int blank = c == ' ' && !ui_q_bits(&q[k], UI_Q_BACKED, 1);
        /* a blank's texels are none: the renderer draws nothing for it; a backed one is the space, the ground */
        u16 u = blank ? 0 : (u16)(SNX_UI_FONT_X + c % 16 * 8);
        u16 v = blank ? 0 : (u16)(SNX_UI_FONT_Y + c / 16 * 16);
        if (q[k].u == u && q[k].v == v && q[k].uw == (blank ? 0 : 8)) continue;
        q[k].bits = (q[k].bits & ~(0xFFu << UI_Q_GLYPH)) | (u32)c << UI_Q_GLYPH;
        q[k].u = u;
        q[k].v = v;
        q[k].uw = blank ? 0 : 8;
        q[k].vh = blank ? 0 : 16;
        ++changed;
    }
    return changed;
}
char* ui_put(char* at, const char* s) {
    while (*s) *at++ = *s++;
    return at;
}
char* ui_put_u(char* at, u64 v) {
    char d[20];
    int n = 0;
    do d[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n) *at++ = d[--n];
    return at;
}
char* ui_put_tenths(char* at, u64 tenths) {
    at = ui_put_u(at, tenths / 10);
    *at++ = '.';
    *at++ = (char)('0' + tenths % 10);
    return at;
}
void ui_append(SnxUiList* l, const SnxUiList* from) {
    int n = from->n < l->max - l->n ? from->n : l->max - l->n;
    if (n <= 0) return;
    memcpy(l->q + l->n, from->q, (size_t)n * sizeof *l->q);
    ui_written(l, l->n, l->n + n);
    l->n += n;
}
void ui_span_add(SnxUiSpan* span, int first, int end) {
    if (first >= end) return;
    if (span->first >= span->end || first < span->first) span->first = first;
    if (end > span->end) span->end = end;
}
void ui_append_kept(SnxUiList* l, const SnxUiList* from, u32 version, const void* who, SnxUiSpan* changed) {
    int n = from->n < l->max - l->n ? from->n : l->max - l->n, k, was_here = 0, same = 0, first = 0, end = n;
    if (n <= 0) return;
    if (l->q == g_dirty.list) { /* where the frame before had them: the same part, place and length, and version */
        const UiPart* was = g_dirty.part[g_dirty.now ^ 1];
        for (k = 0; k < g_dirty.parts[g_dirty.now ^ 1] && !was_here; ++k)
            if (was[k].who == who && was[k].at == l->n && was[k].n == n) {
                was_here = 1;
                same = was[k].version == version;
            }
        if (g_dirty.parts[g_dirty.now] < UI_PARTS)
            g_dirty.part[g_dirty.now][g_dirty.parts[g_dirty.now]++] = (UiPart){who, version, l->n, n};
        if (was_here && changed) { /* only what changed since */
            first = changed->first < 0 ? 0 : changed->first;
            end = changed->end > n ? n : changed->end;
        }
        if (changed) *changed = (SnxUiSpan){0, 0};
    }
    if (!same && first < end) {
        memcpy(l->q + l->n + first, from->q + first, (size_t)(end - first) * sizeof *l->q);
        ui_written(l, l->n + first, l->n + end);
    }
    l->n += n;
}
