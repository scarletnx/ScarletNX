/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* redraw.h: async_pipelines skips a draw only into a target the game regenerates every frame, drawn
   SNX_VK_REDRAW_FRAMES frames in a row and cleared whole in this one. A menu's layer drawn once and kept, one drawn
   for a few frames of an animation and kept, and one drawn every frame but never cleared (marks that add up) don't
   count; a scene target cleared and drawn every frame does from its eighth frame on */
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

/* a frame of present count f that clears the target whole (or not) and draws into it */
static void frame(SnxVkRedraw* r, uint32_t f, int clear) {
    if (clear) snx_vk_redraw_cleared(r, f);
    snx_vk_redraw_seen(r, f);
}
static int skippable(const SnxVkRedraw* r, uint32_t f) {
    return snx_vk_redraw_run(r, f) >= SNX_VK_REDRAW_FRAMES && snx_vk_redraw_cleared_now(r, f);
}

int main(void) {
    SnxVkRedraw layer = {0}, anim = {0}, marks = {0}, scene = {0};
    /* a layer drawn once, cleared first, in frame 5: no, in its frame and after */
    CHECK(!skippable(&layer, 5));
    frame(&layer, 5, 1);
    CHECK(!skippable(&layer, 5));
    CHECK(!skippable(&layer, 6));
    /* an animation's layer, cleared and drawn frames 10 to 15 (six frames), then kept: no in each */
    for (uint32_t f = 10; f < 16; f++) {
        frame(&anim, f, 1);
        CHECK(!skippable(&anim, f));
    }
    CHECK(!skippable(&anim, 16));
    /* drawn every frame but never cleared: its draws add up, no */
    for (uint32_t f = 0; f < 20; f++) {
        frame(&marks, f, 0);
        CHECK(!skippable(&marks, f));
    }
    /* cleared and drawn every frame from frame 0: no for its first seven frames, yes from the eighth (present 7) */
    for (uint32_t f = 0; f < 12; f++) {
        frame(&scene, f, 1);
        CHECK(skippable(&scene, f) == (f + 1 >= SNX_VK_REDRAW_FRAMES));
    }
    /* the next frame asks before its clear: no until the clear, the run counting this frame */
    CHECK(snx_vk_redraw_run(&scene, 12) == 13 && !skippable(&scene, 12));
    snx_vk_redraw_cleared(&scene, 12);
    CHECK(skippable(&scene, 12));
    /* a frame it missed starts the run over */
    frame(&scene, 14, 1);
    CHECK(snx_vk_redraw_run(&scene, 14) == 1 && !skippable(&scene, 14));
    if (!g_failures) printf("vk_redraw: ok\n");
    return g_failures != 0;
}
