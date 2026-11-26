/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/ui_rects.h"
#include "common/utilities/ui_ring.h"
#include "gui/icons.h"
#include "common/utilities/font.h"
#include <math.h>
#include <stdlib.h>

/* a row's runs of pixels: two sides of a shape, and the runs of a label's sixteen columns (two letters') between */
#define ROW_RUNS 24
typedef struct {
    int x0, x1;  /* from x0 up to x1, not included */
    size_t rect; /* the rectangle that holds it, as an index into the list */
} Run;
typedef struct {
    SnxUiRects* out;
    int w, h; /* the picture's */
    u32 rgba;
    Run was[ROW_RUNS]; /* the row above's */
    int n, y;
    bool failed;
} Rows;

static bool room_for(SnxUiRects* out, size_t more) {
    if (out->room - out->count >= more) return true;
    size_t room = out->room ? out->room * 2 : 256;
    SnxUiRect* grown = room > SIZE_MAX / sizeof *grown ? NULL : realloc(out->rect, room * sizeof *grown);
    if (!grown) return false;
    out->rect = grown;
    out->room = room;
    return true;
}

/* row y's runs (count of them, left to right), cut to the picture: one as wide as the row above's, at the same place,
   makes its rectangle a row taller */
static void row_put(Rows* rows, int y, Run* runs, int count) {
    Run now[ROW_RUNS];
    int n = 0, k = 0;
    for (int i = 0; i < count && y >= 0 && y < rows->h && !rows->failed; ++i) {
        int x0 = runs[i].x0 < 0 ? 0 : runs[i].x0, x1 = runs[i].x1 > rows->w ? rows->w : runs[i].x1;
        if (x0 >= x1) continue;
        while (rows->y == y - 1 && k < rows->n && rows->was[k].x0 < x0) ++k;
        if (rows->y == y - 1 && k < rows->n && rows->was[k].x0 == x0 && rows->was[k].x1 == x1) {
            ++rows->out->rect[rows->was[k].rect].h;
            now[n++] = rows->was[k];
            continue;
        }
        if (!room_for(rows->out, 1)) {
            rows->failed = true;
            return;
        }
        now[n++] = (Run){x0, x1, rows->out->count};
        rows->out->rect[rows->out->count++] = (SnxUiRect){(i16)x0, (i16)y, (u16)(x1 - x0), 1, rows->rgba};
    }
    for (int i = 0; i < n; ++i) rows->was[i] = now[i];
    rows->n = n;
    rows->y = y;
}

/* the first pixel whose middle is at v or past it */
static int from(float v) { return (int)ceilf(v - 0.5f); }

/* half the width, at dy from its middle, of what lies within t of a box of half sizes bx and by (t below 0: t inside
   it); below 0 for none */
static float box_half(float bx, float by, float t, float dy) {
    if (t < 0) return dy <= by + t ? bx + t : -1;
    float past = dy - by;
    if (past <= 0) return bx + t;
    return past <= t ? bx + sqrtf(t * t - past * past) : -1;
}
/* the same of a shape (ui_quads.h's UI_SHAPE_) w by h, as ui.frag's distance has it: the parts of a shape share their
   middle, so a row of their union is as wide as the wider */
static float shape_half(unsigned kind, float w, float h, float t, float dy) {
    float m = w < h ? w : h;
    switch (kind) {
        case UI_SHAPE_CIRCLE: return box_half(0, 0, m * 0.5f + t, dy);
        case UI_SHAPE_ROUND: return box_half(w * 0.5f - m * 0.25f, h * 0.5f - m * 0.25f, m * 0.25f + t, dy);
        case UI_SHAPE_CROSS: {
            float across = box_half(w * 0.5f, m / 6, t, dy), down = box_half(m / 6, h * 0.5f, t, dy);
            return across > down ? across : down;
        }
        default: return box_half(w * 0.5f, h * 0.5f, t, dy);
    }
}

/* the label of a shape w by h: how many letters (0 where it has no room for them at 1), their scale and their place
   from the shape's top left, as ui.vert lays them */
typedef struct {
    int letters, scale;
    float x, y;
} Label;
static Label label_of(unsigned label, float w, float h) {
    int n = label >> 8 ? 2 : label & 0xFF ? 1 : 0;
    if (!n || 16.0f > h - 2 || 8.0f * (float)n > w * 0.8f) return (Label){0};
    float s = floorf(fminf(h * 0.5f / 16, w * 0.6f / (8 * (float)n)));
    if (s < 1) s = 1;
    return (Label){n, (int)s, floorf((w - 8 * (float)n * s) * 0.5f), floorf((h - 16 * s) * 0.5f)};
}
/* the letters' bits on the font's row, the leftmost column's first in bit 15 */
static unsigned letters_row(unsigned c0, unsigned c1, int row) {
    return (unsigned)gui_vga_font[c0 * 16 + (unsigned)row] << 8 | gui_vga_font[c1 * 16 + (unsigned)row];
}

/* the row's pixels of a shape from its interval out (x0 to x1) less its hole, in which the label's bits stand for
   themselves, or are cut out of it whole when filled: the pixels drawn, as runs */
static int shape_row(const SnxUiQuadView* q, const Label* l, unsigned label, int y, Run runs[ROW_RUNS]) {
    float py = (float)y + 0.5f - q->y, dy = fabsf(py - q->h * 0.5f), cx = q->x + q->w * 0.5f;
    bool filled = q->uw & UI_SHAPE_FILLED, bare = q->uw & UI_SHAPE_BARE;
    float half = shape_half(q->u, q->w, q->h, 0, dy), hole = -1;
    if (half < 0) return 0;
    int x0 = from(cx - half), x1 = (int)floorf(cx + half - 0.5f) + 1, n = 0;
    /* the quad's own pixels alone, as the GPU draws no others */
    if (x0 < from(q->x)) x0 = from(q->x);
    if (x1 > from(q->x + q->w)) x1 = from(q->x + q->w);
    int h0 = x1, h1 = x1; /* what the edge leaves: from h0 up to h1 */
    if (bare)
        h0 = x0;
    else if (!filled && (hole = shape_half(q->u, q->w, q->h, -(float)q->vh, dy)) >= 0)
        h0 = from(cx - hole), h1 = (int)floorf(cx + hole - 0.5f) + 1;
    /* the label's row, and the pixels each of its columns covers */
    unsigned bits = 0;
    int lx = 0;
    if (l->letters) {
        float lrow = floorf((py - l->y) / (float)l->scale);
        if (lrow >= 0 && lrow < 16) bits = letters_row(label & 0xFF, label >> 8, (int)lrow);
        lx = from(q->x + l->x);
    }
    int x = x0;
    while (x < x1 && n < ROW_RUNS) { /* the pixels in columns and intervals that draw alike, a run at a time */
        int next = x < h0 ? h0 : x < h1 ? h1 : x1, col = bits && x >= lx ? (x - lx) / l->scale : -1;
        bool mark = col >= 0 && col < 8 * l->letters && bits >> (15 - col) & 1;
        if (next > x1) next = x1;
        if (bits && x < lx && next > lx) next = lx;
        if (col >= 0 && col < 8 * l->letters && next > lx + (col + 1) * l->scale) next = lx + (col + 1) * l->scale;
        bool edge = !filled && !bare && (x < h0 || x >= h1);
        if (filled ? !mark : mark || edge) {
            if (n && runs[n - 1].x1 == x)
                runs[n - 1].x1 = next;
            else
                runs[n++] = (Run){x, next, 0};
        }
        x = next;
    }
    return n;
}

/* out's picture under q (its texels u, v, uw by vh) on a picture w by h, as snx_ui_rects says: false when out of memory */
static bool image_put(SnxUiRects* out, const SnxUiQuadView* q, int w, int h) {
    int y0 = from(q->y), y1 = from(q->y + q->h), x0 = from(q->x), x1 = from(q->x + q->w);
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;
    for (int y = y0; y < y1; ++y) {
        int tv = q->v + (int)(((float)y + 0.5f - q->y) * (float)q->vh / q->h);
        tv = tv < 0 ? 0 : tv >= SNX_UI_IMAGE_H ? SNX_UI_IMAGE_H - 1 : tv;
        const u8* row = out->image + (size_t)tv * SNX_UI_IMAGE_W * 4;
        u32 run = 0;
        for (int x = x0; x < x1; ++x) {
            int tu = q->u + (int)(((float)x + 0.5f - q->x) * (float)q->uw / q->w);
            tu = tu < 0 ? 0 : tu >= SNX_UI_IMAGE_W ? SNX_UI_IMAGE_W - 1 : tu;
            const u8* p = row + (size_t)tu * 4;
            u32 c = ui_rgba((u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | 0xFF000000u);
            if (x > x0 && c == run) {
                ++out->rect[out->count - 1].w;
                continue;
            }
            if (!room_for(out, 1)) return false;
            out->rect[out->count++] = (SnxUiRect){(i16)x, (i16)y, 1, 1, c};
            run = c;
        }
    }
    return true;
}

bool snx_ui_rects(SnxUiRects* out, const SnxUiList* quads, int w, int h) {
    out->count = 0;
    ++out->version;
    for (int i = 0; i < quads->n; ++i) {
        const SnxUiQuadView q = ui_drawn(&quads->q[i]); /* a ring's bar where its slot puts it */
        if (q.sample == UI_IMAGE && q.ink == UI_ATLAS) { /* the picture apart: the state's thumbnail */
            if (out->image && q.uw && !image_put(out, &q, w, h)) {
                out->count = 0;
                return false;
            }
            continue;
        }
        SnxUiQuadView ground;
        if (ui_drawn_ground(&quads->q[i],
                            &ground)) { /* a backed letter's or icon's ground first, as the SDL renderer */
            Rows under = {.out = out, .w = w, .h = h, .rgba = ground.rgba, .y = -2};
            int top = from(ground.y), foot = from(ground.y + ground.h);
            for (int y = top < 0 ? 0 : top; y < (foot > h ? h : foot) && !under.failed; ++y) {
                Run run = {from(ground.x), from(ground.x + ground.w), 0};
                row_put(&under, y, &run, 1);
            }
            if (under.failed) {
                out->count = 0;
                return false;
            }
        }
        Rows rows = {.out = out, .w = w, .h = h, .rgba = q.rgba, .y = -2};
        int y0 = from(q.y), y1 = from(q.y + q.h);
        if (!q.uw) continue; /* a blank cell */
        if (y0 < 0) y0 = 0;
        if (y1 > h) y1 = h;
        Label l = {0};
        if (q.ink == UI_SHAPE) l = label_of(q.v, q.w, q.h);
        for (int y = y0; y < y1 && !rows.failed; ++y) {
            Run runs[ROW_RUNS];
            int n = 0;
            if (q.ink == UI_SOLID || q.ink == UI_RING) {
                runs[n++] = (Run){from(q.x), from(q.x + q.w), 0};
            } else if (q.ink == UI_GLYPH) {
                int row = (int)(((float)y + 0.5f - q.y) * 16 / q.h);
                unsigned bits = letters_row(q.glyph, ' ', row < 15 ? row : 15);
                for (int c = 0; c < 8; ++c) {
                    if (!(bits >> (15 - c) & 1)) continue;
                    int x0 = from(q.x + q.w * (float)c / 8), x1 = from(q.x + q.w * (float)(c + 1) / 8);
                    if (n && runs[n - 1].x1 == x0)
                        runs[n - 1].x1 = x1;
                    else
                        runs[n++] = (Run){x0, x1, 0};
                }
            } else if (q.ink == UI_ICON) { /* its two halves' rows, as the VGA font's (gui/icons.h) */
                int row = (int)(((float)y + 0.5f - q.y) * 16 / q.h);
                row = row < 0 ? 0 : row > 15 ? 15 : row;
                unsigned bits = (unsigned)gui_icon_font[(q.glyph * 2u) * 16u + (unsigned)row] << 8 |
                                gui_icon_font[(q.glyph * 2u + 1u) * 16u + (unsigned)row];
                for (int c = 0; c < 16; ++c) {
                    if (!(bits >> (15 - c) & 1)) continue;
                    int x0 = from(q.x + q.w * (float)c / 16), x1 = from(q.x + q.w * (float)(c + 1) / 16);
                    if (n && runs[n - 1].x1 == x0)
                        runs[n - 1].x1 = x1;
                    else
                        runs[n++] = (Run){x0, x1, 0};
                }
            } else if (q.ink == UI_SHAPE)
                n = shape_row(&q, &l, q.v, y, runs);
            row_put(&rows, y, runs, n);
        }
        if (rows.failed) {
            out->count = 0;
            return false;
        }
    }
    return true;
}

void snx_ui_rects_free(SnxUiRects* out) {
    free(out->rect);
    *out = (SnxUiRects){0};
}
