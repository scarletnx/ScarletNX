/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The one window's screen layouts (present.h's snx_ctr_layout_rects), the pure geometry the presenter uses: every
   layout's rectangles for a 480x400 window at the console's 400x240 top and 320x240 bottom screens, and a narrow and
   a wide window's, where both screens must still get a rectangle of their console's shape. A screen with no frame yet
   (size 0) gets no rectangle, and the other keeps the place both leave it. */
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
/* the screen's width to its height within about a pixel of the console's width by 240: |w * 240 - h * cw| <= 240,
   the inclusive bound the narrow large case sits on */
static bool shaped(SnxCtrRect rect, int console_width) {
    int off = rect.width * 240 - rect.height * console_width;
    return off <= 240 && off >= -240;
}
/* both screens positive and inside the window, of their console's shape, and not one over the other: touching is not
   overlapping */
static bool both(SnxCtrRects rects, uint32_t window_width, uint32_t window_height) {
    return rects.top.x >= 0 && rects.top.y >= 0 && rects.bottom.x >= 0 && rects.bottom.y >= 0 &&
           rects.top.x + rects.top.width <= (int32_t)window_width &&
           rects.top.y + rects.top.height <= (int32_t)window_height &&
           rects.bottom.x + rects.bottom.width <= (int32_t)window_width &&
           rects.bottom.y + rects.bottom.height <= (int32_t)window_height && rects.top.width > 0 &&
           rects.top.height > 0 && rects.bottom.width > 0 && rects.bottom.height > 0 && shaped(rects.top, 400) &&
           shaped(rects.bottom, 320) &&
           (rects.top.x + rects.top.width <= rects.bottom.x || rects.bottom.x + rects.bottom.width <= rects.top.x ||
            rects.top.y + rects.top.height <= rects.bottom.y || rects.bottom.y + rects.bottom.height <= rects.top.y);
}
static void expect_both(const char* name, SnxCtrLayout layout, bool swap, uint32_t window_width,
                        uint32_t window_height) {
    SnxCtrScreens screens = {400, 240, 320, 240};
    SnxCtrRects rects = {0};
    bool ok = snx_ctr_layout_rects(layout, swap, window_width, window_height, screens, &rects);
    say(name, ok && both(rects, window_width, window_height), rects);
}

int main(void) {
    /* stacked: one box 400 wide (the top's) by 480 high, scaled by the 400-high window's 400/480 and centred across its
       480 (73 each side); the bottom, 320 * 400/480 wide, centred under the top */
    expect("stacked", SNX_CTR_LAYOUT_STACKED, false, 73, 0, 333, 200, 106, 200, 266, 200);
    expect("stacked, swapped", SNX_CTR_LAYOUT_STACKED, true, 73, 200, 333, 200, 106, 0, 266, 200);
    /* single screen: the top alone, scaled by the 480 width's 480/400 to 480x288 and centred in the 400-high window */
    expect("single screen", SNX_CTR_LAYOUT_SINGLE, false, 0, 56, 480, 288, 0, 0, 0, 0);
    /* the swap's single screen is the bottom: 320x240 scaled by the 480 width's 1.5 to 480x360, centred */
    expect("single screen, swapped", SNX_CTR_LAYOUT_SINGLE, true, 0, 0, 0, 0, 0, 20, 480, 360);
    /* large screen: the top 400x240 and the bottom 320x240 over 4 (80x60) are one 480x240 box, the window's width,
       so a scale of 1; the top 400x240 at the left, 80 down it, the bottom 80x60 touching it at the right,
       its height centred on the top's (90 above it) */
    expect("large screen", SNX_CTR_LAYOUT_LARGE, false, 0, 80, 400, 240, 400, 170, 80, 60);
    /* swapped: the bottom 320x240 is the large one and the top 400x240 over 4 (100x60) the small, a box 420x240
       scaled by the width's 480/420 (8/7) to 480x274.29, centred (62.86 above); the bottom 365x274 at the left,
       the top 114x68 touching it at the right, its height centred on the bottom's (165.71 above, at 165) */
    expect("large screen, swapped", SNX_CTR_LAYOUT_LARGE, true, 365, 165, 114, 68, 0, 62, 365, 274);
    /* side by side: the top 400x240 and the bottom 320x240 (proportion 1) are one box 720x240, scaled by the 480
       width's 480/720 to 480x160 and centred in the 400 height (120 above) */
    expect("side by side", SNX_CTR_LAYOUT_SIDE, false, 0, 120, 266, 160, 266, 120, 213, 160);
    expect("side by side, swapped", SNX_CTR_LAYOUT_SIDE, true, 213, 120, 266, 160, 0, 120, 213, 160);
    /* a screen with no frame yet (size 0) gets no rectangle, the other keeping the place both leave it: here the top
       is the one that has not presented, and the bottom keeps the stacked place it has with both */
    expect_with("top without a frame", SNX_CTR_LAYOUT_STACKED, false, (SnxCtrScreens){0, 0, 320, 240}, 0, 0, 0, 0, 106,
                200, 266, 200);
    expect_with("bottom without a frame", SNX_CTR_LAYOUT_STACKED, false, (SnxCtrScreens){400, 240, 0, 0}, 73, 0, 333,
                200, 0, 0, 0, 0);
    expect_with("large, top without a frame", SNX_CTR_LAYOUT_LARGE, false, (SnxCtrScreens){0, 0, 320, 240}, 0, 0, 0, 0,
                400, 170, 80, 60);
    /* a value past the four, which the settings clamp, is the stacked layout */
    expect("unknown as stacked", (SnxCtrLayout)99, false, 73, 0, 333, 200, 106, 200, 266, 200);
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
