/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* redraw.h: async_pipelines skips a draw only into a target the frame before drew too. A menu's layer drawn once and
   kept answers no for its whole frame, and again after frames that didn't draw it; a scene target drawn every frame
   answers yes from its second frame on */
#include "nx/gpu/vk/redraw.h"
#include <stdio.h>

static int g_failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

int main(void) {
    SnxVkRedraw layer = {0}, scene = {0};
    /* never drawn: no */
    CHECK(!snx_vk_redrawn(&layer, 5));
    /* first drawn in frame 5: no, for every pass of frame 5, the ones after the first too */
    snx_vk_redraw_seen(&layer, 5);
    CHECK(!snx_vk_redrawn(&layer, 5));
    snx_vk_redraw_seen(&layer, 5);
    CHECK(!snx_vk_redrawn(&layer, 5));
    /* frame 6 asks before its first pass into it: frame 5 drew it */
    CHECK(snx_vk_redrawn(&layer, 6));
    /* frame 7 didn't draw it, so frame 8 drawing it again (the menu changed) answers no */
    CHECK(!snx_vk_redrawn(&layer, 8));
    snx_vk_redraw_seen(&layer, 8);
    CHECK(!snx_vk_redrawn(&layer, 8));
    /* a target drawn from the first frame (present count 0) on: no in frame 0, yes from frame 1 */
    snx_vk_redraw_seen(&scene, 0);
    CHECK(!snx_vk_redrawn(&scene, 0));
    for (uint32_t f = 1; f < 4; f++) {
        snx_vk_redraw_seen(&scene, f);
        CHECK(snx_vk_redrawn(&scene, f));
    }
    if (!g_failures) printf("vk_redraw: ok\n");
    return g_failures != 0;
}
