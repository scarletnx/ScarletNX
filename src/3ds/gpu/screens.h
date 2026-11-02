/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Where the window shows the console's two screens (the ctr_screen_layout and ctr_integer_scale settings): the
   presenter blits them there and the touch screen reads the mouse the same way */
#ifndef SNX_CTR_SCREENS_H
#define SNX_CTR_SCREENS_H
#include "3ds/input/input.h"

enum {
    SNX_CTR_SCREENS_STACKED,       /* the top screen above the bottom one, as on the console */
    SNX_CTR_SCREENS_SIDE_BY_SIDE,  /* the top screen left of the bottom one */
    SNX_CTR_SCREENS_LARGE_TOP,     /* the top screen large, the bottom one small at its lower right */
    SNX_CTR_SCREENS_LARGE_BOTTOM,  /* the bottom screen large, the top one small at its lower right */
    SNX_CTR_SCREENS_TOP_ONLY,
    SNX_CTR_SCREENS_BOTTOM_ONLY,
    SNX_CTR_SCREENS_LAYOUTS
};
typedef struct {
    double x, y, w, h; /* in the window's image, in pixels. A width of 0: the screen is not shown */
} SnxCtrScreenRect;
/* The two screens (0 the top, 1 the bottom) of layout->mode in the image, the whole fit to it and centred. With
   layout->integer the scale is a whole number of pixels for each console pixel as long as the window is big enough
   for one, and the corners then fall on whole pixels. An unknown mode is the stacked one */
void snx_ctr_screens_place(const SnxCtrWindowLayout* layout, SnxCtrScreenRect rects[2]);
#endif
