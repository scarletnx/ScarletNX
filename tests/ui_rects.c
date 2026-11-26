/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A layer's quads as rectangles (common/utilities/ui_rects.c): filled into a picture, each pixel is what ui.frag writes
   (tests/support/ui_shape_ref.c, where a pixel's middle is clear of a bound), for every shape, size, label and look the
   on-screen controls draw, letters and fills at fractions of a pixel, and quads off the picture's edges. Few
   rectangles: a row like the one above it adds none */
#include "common/utilities/font.h"
#include "common/utilities/ui_rects.h"
#include "common/utilities/ui_ring.h"
#include "gui/icons.h"
#include "support/ui_shape_ref.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "ui_rects:%d: %s\n", __LINE__, #x);                                                        \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

enum { W = 420, H = 300 };
static u32 g_picture[H][W]; /* the rectangles', filled in their order */
static SnxUiQuad g_quads[64];
static SnxUiRects g_rects;

/* l's rectangles filled into the picture: how many pixels more than one of a quad's covered, -1 for one off it */
static long fill(const SnxUiList* l) {
    long twice = 0;
    memset(g_picture, 0, sizeof g_picture);
    if (!snx_ui_rects(&g_rects, l, W, H)) return -1;
    for (size_t i = 0; i < g_rects.count; ++i) {
        const SnxUiRect* r = &g_rects.rect[i];
        if (r->x < 0 || r->y < 0 || r->x + r->w > W || r->y + r->h > H || !r->w || !r->h) return -1;
        for (int y = r->y; y < r->y + r->h; ++y)
            for (int x = r->x; x < r->x + r->w; ++x) {
                twice += g_picture[y][x] != 0;
                g_picture[y][x] = r->rgba;
            }
    }
    return twice;
}

/* the pixels of one shape that are not ui.frag's, the unsure ones left out; the sure ones drawn counted */
static long shape_wrong(const SnxUiQuad* quad, long* drawn) {
    const SnxUiQuadView q = ui_view(quad);
    long wrong = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float px = (float)x + 0.5f - q.x, py = (float)y + 0.5f - q.y;
            int want = 0;
            if (px >= 0 && py >= 0 && px < q.w && py < q.h)
                want = ui_shape_ref(q.u, q.v, q.uw & 0x7FFFu, q.vh, q.w, q.h, px, py);
            if (fabsf(px) < 1e-3f || fabsf(py) < 1e-3f || fabsf(px - q.w) < 1e-3f || fabsf(py - q.h) < 1e-3f) continue;
            if (want < 0) continue;
            *drawn += want;
            wrong += (g_picture[y][x] != 0) != want;
        }
    return wrong;
}

int main(void) {
    static const float sizes[][2] = {{118, 118}, {75.25f, 75.25f}, {281, 113.25f}, {145, 155.5f}, {270, 270},
                                     {24, 24},   {9.5f, 9.5f},     {40, 18},       {195.25f, 117}};
    static const float places[][2] = {{10, 12}, {20.25f, 7.75f}, {-30, -20}, {300.5f, 200.25f}};
    static const char* const labels[] = {"", "A", "ZL", "\x07", "ST"};
    static const int flags[] = {0, UI_SHAPE_FILLED, UI_SHAPE_BARE};
    SnxUiList l = {g_quads, 0, 64};
    long cases = 0, all = 0;
    size_t most = 0;
    for (int kind = UI_SHAPE_CIRCLE; kind <= UI_SHAPE_BOX; ++kind)
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; ++s)
            for (size_t p = 0; p < sizeof places / sizeof *places; ++p)
                for (size_t t = 0; t < sizeof labels / sizeof *labels; ++t)
                    for (size_t f = 0; f < sizeof flags / sizeof *flags; ++f)
                        for (int edge = 1; edge <= 7; edge += 3) {
                            long drawn = 0, wrong;
                            l.n = 0;
                            ui_shape(&l, places[p][0], places[p][1], sizes[s][0], sizes[s][1], kind, labels[t],
                                     flags[f], edge, 0xFFFFFFFFu);
                            CHECK(l.n == 1 && fill(&l) == 0);
                            wrong = shape_wrong(&g_quads[0], &drawn);
                            if (wrong) {
                                fprintf(stderr,
                                        "ui_rects: shape %d of %.2f x %.2f at %.2f, %.2f, label \"%s\", flags %d, edge "
                                        "%d: %ld pixels are not ui.frag's\n",
                                        kind, (double)sizes[s][0], (double)sizes[s][1], (double)places[p][0],
                                        (double)places[p][1], labels[t], flags[f], edge, wrong);
                                return 1;
                            }
                            ++cases;
                            all += drawn;
                            if (g_rects.count > most) most = g_rects.count;
                        }
    CHECK(all > 1000000);  /* the reference drew, so the comparison saw shapes */
    CHECK(most < 4 * 270); /* a 270 pixel shape's rows: its two sides and its label's runs, the like ones joined */

    /* letters at a whole scale and between pixels: each of the font's bits its own pixels, a blank cell none */
    for (int scale = 1; scale <= 3; ++scale) {
        u32 version = g_rects.version;
        l.n = 0;
        ui_text(&l, 8, 16, scale, "Az%", 0xFF0000FFu);
        int first = ui_cells(&l, 8, 80, scale, 4, 0xFF00FF00u);
        CHECK(first >= 0 && ui_cells_set(&l.q[first], 4, "1 ") == 1);
        CHECK(fill(&l) == 0 && g_rects.version == version + 1);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int c = (x - 8) / (8 * scale), col = (x - 8) / scale % 8;
                u32 want = 0;
                if (x >= 8 && c < 3 && y >= 16 && y < 16 + 16 * scale)
                    want = gui_vga_font[(u8) "Az%"[c] * 16 + (y - 16) / scale] >> (7 - col) & 1 ? 0xFF0000FFu : 0;
                if (x >= 8 && c < 1 && y >= 80 && y < 80 + 16 * scale)
                    want = gui_vga_font['1' * 16 + (y - 80) / scale] >> (7 - col) & 1 ? 0xFF00FF00u : 0;
                if (g_picture[y][x] != want) {
                    fprintf(stderr, "ui_rects: at scale %d the letters' pixel %d, %d is %08X, not %08X\n", scale, x, y,
                            (unsigned)g_picture[y][x], (unsigned)want);
                    return 1;
                }
            }
    }
    /* a fill is its pixels' middles, one rectangle; a later quad goes over an earlier one; off the picture, none */
    l.n = 0;
    ui_fill(&l, 10.25f, 20.5f, 30, 8, 0xFF112233u);
    ui_fill(&l, 20, 24, 400, 400, 0xFFFFFFFFu);
    ui_fill(&l, -50, -50, 20, 20, 0xFFFFFFFFu);
    ui_fill(&l, W, H, 20, 20, 0xFFFFFFFFu);
    CHECK(fill(&l) > 0 && g_rects.count == 2);
    CHECK(g_rects.rect[0].x == 10 && g_rects.rect[0].y == 20 && g_rects.rect[0].w == 30 && g_rects.rect[0].h == 8);
    CHECK(g_rects.rect[0].rgba == ui_view(&l.q[0]).rgba && g_picture[25][25] == 0xFFFFFFFFu);
    CHECK(g_rects.rect[1].x + g_rects.rect[1].w == W && g_rects.rect[1].y + g_rects.rect[1].h == H);
    l.n = 0;
    CHECK(snx_ui_rects(&g_rects, &l, W, H) && !g_rects.count);
    /* the menus' icons, their font's bits at whole scales (gui/icons.h) */
    for (int scale = 1; scale <= 3; ++scale)
        for (unsigned icon = GUI_ICON_FIRST; icon < GUI_ICON_END; icon += 7) {
            SnxUiList li = {g_quads, 0, 64};
            CHECK(ui_icon(&li, 10, 20, scale, icon, 0xFF00FF00u) == 16.0f * (float)scale && fill(&li) == 0);
            for (int y = 0; y < 16 * scale; ++y)
                for (int x = 0; x < 16 * scale; ++x) {
                    unsigned i = icon - GUI_ICON_FIRST, half = (unsigned)(x / scale / 8), row = (unsigned)(y / scale);
                    int on = gui_icon_font[(i * 2 + half) * 16 + row] >> (7 - x / scale % 8) & 1;
                    CHECK((g_picture[20 + y][10 + x] != 0) == on);
                }
        }
    /* a backed letter on its ground (ui_ground), every pixel of its cell one or the other */
    {
        SnxUiList lb = {g_quads, 0, 64};
        ui_text_backed(&lb, 10, 20, 2, "A", 0xFFFFFFFFu);
        CHECK(fill(&lb) > 0);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 16; ++x) {
                int on = gui_vga_font['A' * 16 + y / 2] >> (7 - x / 2) & 1;
                CHECK(g_picture[20 + y][10 + x] == (on ? 0xFFFFFFFFu : ui_ground()));
            }
    }
    /* the ring's bars where their slots put them, the newest at the right, past slow_from in the slow colour, none for a
       height of 0 */
    {
        static const u8 heights[3] = {0, 255, 128};
        SnxUiList lr = {g_quads, 0, 64};
        ui_ring_set(heights, 3, 2, 200, 0xFF0000FFu, 1);
        ui_ring_bars(&lr, 0, 0, 30, 100, 3, 0xFF00FF00u);
        CHECK(fill(&lr) == 0);
        CHECK(g_picture[90][25] == 0xFF00FF00u && !g_picture[30][25]); /* the newest: half the graph's height */
        CHECK(g_picture[10][15] == 0xFF0000FFu && g_picture[99][15] == 0xFF0000FFu); /* a full one, slow */
        CHECK(!g_picture[90][5]);                                                    /* height 0: nothing */
    }
    /* the picture apart (a state's thumbnail): its left half one colour and its right another, drawn at half its size,
       each texel the nearest at a pixel's middle, a run of one colour a rectangle; none without the picture */
    {
        static u8 image[SNX_UI_IMAGE_H][SNX_UI_IMAGE_W][4];
        for (int y = 0; y < SNX_UI_IMAGE_H; ++y)
            for (int x = 0; x < SNX_UI_IMAGE_W; ++x) {
                const u8 left[4] = {0x20, 0x80, 0xF8, 0xFF}, right[4] = {0xF8, 0x40, 0x08, 0xFF};
                memcpy(image[y][x], x < SNX_UI_IMAGE_W / 2 ? left : right, 4);
            }
        SnxUiList li = {g_quads, 0, 64};
        ui_quad_add(&li, 4, 6, SNX_UI_IMAGE_W / 2, SNX_UI_IMAGE_H / 2, 0, 0, SNX_UI_IMAGE_W, SNX_UI_IMAGE_H,
                    0xFFFFFFFFu, 0, UI_IMAGE, UI_ATLAS, 0);
        CHECK(fill(&li) == 0 && g_rects.count == 0);
        g_rects.image = &image[0][0][0];
        CHECK(fill(&li) == 0 && g_rects.count == 2 * SNX_UI_IMAGE_H / 2);
        CHECK(g_picture[6][4] == ui_rgba(0xFFF88020u) &&
              g_picture[6 + SNX_UI_IMAGE_H / 2 - 1][4 + 79] == ui_rgba(0xFFF88020u));
        CHECK(g_picture[50][4 + 80] == ui_rgba(0xFF0840F8u) && g_picture[50][4 + 159] == ui_rgba(0xFF0840F8u));
        CHECK(!g_picture[50][4 + 160] && !g_picture[6 + SNX_UI_IMAGE_H / 2][50] && !g_picture[5][50]);
        g_rects.image = NULL;
    }
    snx_ui_rects_free(&g_rects);
    CHECK(!g_rects.rect && !g_rects.count);
    printf(
        "ui_rects: %ld shapes, %ld pixels as ui.frag writes them, %zu rectangles at most; letters, fills, icons, grounds, the ring and the picture passed\n",
        cases, all, most);
    return 0;
}
