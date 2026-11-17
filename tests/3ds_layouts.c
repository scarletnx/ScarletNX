/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The one window's screen layouts (present.h's snx_ctr_layout_rects), the pure geometry the presenter uses: the
   stacked and one-screen layouts at a 480x400 window, the large and side layouts at the windows of the table below,
   and a narrow and a wide window, where both screens must still get a rectangle of their console's shape. A screen
   with no frame yet (size 0) gets no rectangle. Stacked, the other screen fills the box alone, and in the large and
   side layouts it keeps its place. */
#include "3ds/gpu/present.h"
#include <stdio.h>

static unsigned failures;

static void say(const char* name, bool pass, SnxCtrRects rects) {
    printf("%-30s top %d,%d %dx%d bottom %d,%d %dx%d: %s\n", name, rects.top.x, rects.top.y, rects.top.width,
           rects.top.height, rects.bottom.x, rects.bottom.y, rects.bottom.width, rects.bottom.height,
           pass ? "ok" : "FAIL");
    if (!pass) ++failures;
}
static bool same(SnxCtrRect rect, int x, int y, int width, int height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}
static bool same_rects(SnxCtrRects rects, SnxCtrRects want) {
    return same(rects.top, want.top.x, want.top.y, want.top.width, want.top.height) &&
           same(rects.bottom, want.bottom.x, want.bottom.y, want.bottom.width, want.bottom.height);
}
/* both rectangles inside the window and over no part of each other: touching is not overlapping */
static bool inside_apart(SnxCtrRects rects, uint32_t window_width, uint32_t window_height) {
    return rects.top.x >= 0 && rects.top.y >= 0 && rects.bottom.x >= 0 && rects.bottom.y >= 0 &&
           rects.top.x + rects.top.width <= (int32_t)window_width &&
           rects.top.y + rects.top.height <= (int32_t)window_height &&
           rects.bottom.x + rects.bottom.width <= (int32_t)window_width &&
           rects.bottom.y + rects.bottom.height <= (int32_t)window_height &&
           (rects.top.x + rects.top.width <= rects.bottom.x || rects.bottom.x + rects.bottom.width <= rects.top.x ||
            rects.top.y + rects.top.height <= rects.bottom.y || rects.bottom.y + rects.bottom.height <= rects.top.y);
}
/* the screen's width to its height within about a pixel of the console's width by 240: |w * 240 - h * cw| <= 240,
   the inclusive bound the wide (1600x300) large case sits on: its bottom 177x132 gives
   |177 * 240 - 132 * 320| = 240 */
static bool shaped(SnxCtrRect rect, int console_width) {
    int off = rect.width * 240 - rect.height * console_width;
    return off <= 240 && off >= -240;
}
/* both screens positive, of their console's shape, inside the window and apart (inside_apart) */
static bool both(SnxCtrRects rects, uint32_t window_width, uint32_t window_height) {
    return inside_apart(rects, window_width, window_height) && rects.top.width > 0 && rects.top.height > 0 &&
           rects.bottom.width > 0 && rects.bottom.height > 0 && shaped(rects.top, 400) && shaped(rects.bottom, 320);
}
static void expect_both(const char* name, SnxCtrLayout layout, bool swap, uint32_t window_width,
                        uint32_t window_height) {
    SnxCtrScreens screens = {400, 240, 320, 240};
    SnxCtrRects rects = {0};
    bool ok = snx_ctr_layout_rects(layout, swap, window_width, window_height, screens, &rects);
    say(name, ok && both(rects, window_width, window_height), rects);
}
/* the rectangles the layout should give for the 480x400 window, the screens at the sizes given */
static void expect_with(const char* name, SnxCtrLayout layout, bool swap, SnxCtrScreens screens, int top_x, int top_y,
                        int top_width, int top_height, int bottom_x, int bottom_y, int bottom_width,
                        int bottom_height) {
    SnxCtrRects rects = {0};
    bool ok = snx_ctr_layout_rects(layout, swap, 480, 400, screens, &rects);
    bool pass = ok && same(rects.top, top_x, top_y, top_width, top_height) &&
                same(rects.bottom, bottom_x, bottom_y, bottom_width, bottom_height);
    say(name, pass, rects);
}
/* the rectangles the layout should give for the 480x400 window, both screens at their console's size */
static void expect(const char* name, SnxCtrLayout layout, bool swap, int top_x, int top_y, int top_width,
                   int top_height, int bottom_x, int bottom_y, int bottom_width, int bottom_height) {
    expect_with(name, layout, swap, (SnxCtrScreens){400, 240, 320, 240}, top_x, top_y, top_width, top_height, bottom_x,
                bottom_y, bottom_width, bottom_height);
}
/* a window's rectangles against the expected ones, both inside the window and apart */
static void expect_at(const char* name, SnxCtrLayout layout, bool swap, SnxCtrScreens screens, uint32_t window_width,
                      uint32_t window_height, SnxCtrRects want) {
    SnxCtrRects rects = {0};
    bool ok = snx_ctr_layout_rects(layout, swap, window_width, window_height, screens, &rects);
    say(name, ok && same_rects(rects, want) && inside_apart(rects, window_width, window_height), rects);
}
/* a zero window size is refused, and the layout gives no rectangles */
static void expect_refused(const char* name, SnxCtrLayout layout, uint32_t window_width, uint32_t window_height) {
    SnxCtrRects rects = {0};
    bool ok =
        snx_ctr_layout_rects(layout, false, window_width, window_height, (SnxCtrScreens){400, 240, 320, 240}, &rects);
    say(name, !ok, rects);
}

/* the large and side layouts, worked by hand from the integer steps of Azahar's LargeFrameLayout, not from present.c:
   the rectangles are (x, y, width, height) for the top screen 400x240 and the bottom 320x240 at console size */
typedef struct {
    const char* name;
    SnxCtrLayout layout;
    bool swap;
    uint32_t width, height;
    SnxCtrRects rects;
} LayoutCase;
static const LayoutCase cases[] = {
    {"2400x1080 large", SNX_CTR_LAYOUT_LARGE, false, 2400, 1080, {{0, 9, 1770, 1062}, {1770, 305, 628, 469}}},
    {"2400x1080 large, swapped", SNX_CTR_LAYOUT_LARGE, true, 2400, 1080, {{1522, 301, 796, 477}, {82, 0, 1440, 1080}}},
    {"2400x1080 side", SNX_CTR_LAYOUT_SIDE, false, 2400, 1080, {{0, 140, 1333, 800}, {1333, 140, 1066, 800}}},
    {"2400x1080 side, swapped", SNX_CTR_LAYOUT_SIDE, true, 2400, 1080, {{1066, 140, 1333, 800}, {0, 140, 1066, 800}}},
    {"1080x2400 large", SNX_CTR_LAYOUT_LARGE, false, 1080, 2400, {{0, 961, 796, 478}, {796, 1094, 282, 211}}},
    {"1080x2400 large, swapped", SNX_CTR_LAYOUT_LARGE, true, 1080, 2400, {{694, 1084, 384, 230}, {0, 939, 694, 521}}},
    {"1080x2400 side", SNX_CTR_LAYOUT_SIDE, false, 1080, 2400, {{0, 1020, 600, 360}, {600, 1020, 480, 360}}},
    {"1920x1080 large", SNX_CTR_LAYOUT_LARGE, false, 1920, 1080, {{0, 115, 1416, 850}, {1416, 352, 502, 375}}},
    {"1920x1080 large, swapped", SNX_CTR_LAYOUT_LARGE, true, 1920, 1080, {{1236, 335, 683, 409}, {0, 76, 1236, 927}}},
    {"1920x1080 side", SNX_CTR_LAYOUT_SIDE, false, 1920, 1080, {{0, 220, 1066, 640}, {1066, 220, 853, 640}}},
    {"1000x300 large", SNX_CTR_LAYOUT_LARGE, false, 1000, 300, {{161, 0, 500, 300}, {661, 84, 177, 132}}},
    {"1000x300 side", SNX_CTR_LAYOUT_SIDE, false, 1000, 300, {{50, 0, 500, 300}, {550, 0, 400, 300}}},
    {"480x400 large", SNX_CTR_LAYOUT_LARGE, false, 480, 400, {{0, 94, 353, 212}, {353, 153, 125, 93}}},
    {"480x400 side", SNX_CTR_LAYOUT_SIDE, false, 480, 400, {{0, 120, 266, 160}, {266, 120, 213, 160}}},
    {"200x1000 large", SNX_CTR_LAYOUT_LARGE, false, 200, 1000, {{0, 456, 146, 88}, {146, 481, 52, 38}}},
    {"200x1000 side", SNX_CTR_LAYOUT_SIDE, false, 200, 1000, {{0, 467, 110, 66}, {110, 467, 88, 66}}},
    {"1x1 large", SNX_CTR_LAYOUT_LARGE, false, 1, 1, {{0, 0, 0, 0}, {0, 0, 0, 0}}},
};

int main(void) {
    /* stacked: one box 400 wide (the top's) by 480 high, scaled by the 400-high window's 400/480 and centred across its
       480 (73 each side); the bottom, 320 * 400/480 wide, centred under the top: from 106.67 to 373.33, its edges
       truncated to 106 and 373, so 267 wide */
    expect("stacked", SNX_CTR_LAYOUT_STACKED, false, 73, 0, 333, 200, 106, 200, 267, 200);
    expect("stacked, swapped", SNX_CTR_LAYOUT_STACKED, true, 73, 200, 333, 200, 106, 0, 267, 200);
    expect("separate, no second window", SNX_CTR_LAYOUT_SEPARATE, false, 73, 0, 333, 200, 106, 200, 267, 200);
    /* single screen: the top alone, scaled by the 480 width's 480/400 to 480x288 and centred in the 400-high window */
    expect("single screen", SNX_CTR_LAYOUT_SINGLE, false, 0, 56, 480, 288, 0, 0, 0, 0);
    /* the swap's single screen is the bottom: 320x240 scaled by the 480 width's 1.5 to 480x360, centred */
    expect("single screen, swapped", SNX_CTR_LAYOUT_SINGLE, true, 0, 0, 0, 0, 0, 20, 480, 360);
    /* 1000x300, the height limiting the fit: the top 400x240 at 300/240 = 1.25 is 500x300 at x = (1000 - 500) / 2, and
       with the swap the bottom 320x240 at 1.25 is 400x300 at x = (1000 - 400) / 2. The other screen gets none */
    expect_at("1000x300 single", SNX_CTR_LAYOUT_SINGLE, false, (SnxCtrScreens){400, 240, 320, 240}, 1000, 300,
              (SnxCtrRects){{250, 0, 500, 300}, {0, 0, 0, 0}});
    expect_at("1000x300 single, swapped", SNX_CTR_LAYOUT_SINGLE, true, (SnxCtrScreens){400, 240, 320, 240}, 1000, 300,
              (SnxCtrRects){{0, 0, 0, 0}, {300, 0, 400, 300}});
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        expect_at(cases[i].name, cases[i].layout, cases[i].swap, (SnxCtrScreens){400, 240, 320, 240}, cases[i].width,
                  cases[i].height, cases[i].rects);
    /* a screen with no frame yet (size 0) gets no rectangle. Stacked, the other screen fills the box alone: with the
       top missing, the bottom takes 320x240 at scale 1.5, that is 480x360, centred vertically, y = 20 */
    expect_with("top without a frame", SNX_CTR_LAYOUT_STACKED, false, (SnxCtrScreens){0, 0, 320, 240}, 0, 0, 0, 0, 0,
                20, 480, 360);
    /* with the bottom missing, the top takes 400x240 at scale 1.2, that is 480x288, centred vertically at y = 56 */
    expect_with("bottom without a frame", SNX_CTR_LAYOUT_STACKED, false, (SnxCtrScreens){400, 240, 0, 0}, 0, 56, 480,
                288, 0, 0, 0, 0);
    /* with no frame in either screen, nothing is placed, and the call still returns true */
    expect_with("stacked, no frame", SNX_CTR_LAYOUT_STACKED, false, (SnxCtrScreens){0, 0, 0, 0}, 0, 0, 0, 0, 0, 0, 0,
                0);
    /* the top without a frame in the large layout: the bottom keeps its place in the 480x400 large layout */
    expect_with("large, top without a frame", SNX_CTR_LAYOUT_LARGE, false, (SnxCtrScreens){0, 0, 320, 240}, 0, 0, 0, 0,
                353, 153, 125, 93);
    /* the large layout with the bottom screen empty: the bottom takes the console's size in the layout, so the top
       keeps the rectangle of the 2400x1080 large case, the first row, and the bottom gets none */
    expect_at("large, bottom without a frame", SNX_CTR_LAYOUT_LARGE, false, (SnxCtrScreens){400, 240, 0, 0}, 2400, 1080,
              (SnxCtrRects){cases[0].rects.top, {0, 0, 0, 0}});
    /* a value past the five, which the settings clamp, is the stacked layout */
    expect("unknown as stacked", (SnxCtrLayout)99, false, 73, 0, 333, 200, 106, 200, 267, 200);
    expect_refused("zero window width", SNX_CTR_LAYOUT_LARGE, 0, 400);
    expect_refused("zero window height", SNX_CTR_LAYOUT_SIDE, 480, 0);
    /* a narrow window (100x900) and a wide one (1600x300): in every layout of both screens, the pair scaled to fit and
       centred leaves each a rectangle of its console's shape, inside the window and over no part of the other */
    expect_both("narrow, stacked", SNX_CTR_LAYOUT_STACKED, false, 100, 900);
    expect_both("narrow, stacked, swapped", SNX_CTR_LAYOUT_STACKED, true, 100, 900);
    expect_both("narrow, large screen", SNX_CTR_LAYOUT_LARGE, false, 100, 900);
    expect_both("narrow, large screen, swapped", SNX_CTR_LAYOUT_LARGE, true, 100, 900);
    expect_both("narrow, side by side", SNX_CTR_LAYOUT_SIDE, false, 100, 900);
    expect_both("narrow, side by side, swapped", SNX_CTR_LAYOUT_SIDE, true, 100, 900);
    expect_both("wide, stacked", SNX_CTR_LAYOUT_STACKED, false, 1600, 300);
    expect_both("wide, stacked, swapped", SNX_CTR_LAYOUT_STACKED, true, 1600, 300);
    expect_both("wide, large screen", SNX_CTR_LAYOUT_LARGE, false, 1600, 300);
    expect_both("wide, large screen, swapped", SNX_CTR_LAYOUT_LARGE, true, 1600, 300);
    expect_both("wide, side by side", SNX_CTR_LAYOUT_SIDE, false, 1600, 300);
    expect_both("wide, side by side, swapped", SNX_CTR_LAYOUT_SIDE, true, 1600, 300);
    return failures ? 1 : 0;
}
