/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether the game draws a render target again every frame, which async_pipelines asks before it skips a draw whose
   pipeline a worker still makes: a later frame draws the skipped draw again only into such a target (render.c
   targets_redrawn). Each target keeps the frame a pass last drew into it and whether the frame before that one drew
   into it too; the first pass of a frame into it sets both for the whole frame */
#ifndef SNX_VK_REDRAW_H
#define SNX_VK_REDRAW_H
#include <stdint.h>

typedef struct {
    uint32_t frame; /* the present count plus 1 of the last frame a pass drew into the target, 0 for none */
    int again;      /* that frame's first pass found it drawn the frame before */
} SnxVkRedraw;

/* a pass draws into the target in the frame whose present count (presents done) is present */
static inline void snx_vk_redraw_seen(SnxVkRedraw* r, uint32_t present) {
    if (r->frame == present + 1) return;
    r->again = r->frame != 0 && r->frame == present;
    r->frame = present + 1;
}

/* 1 when the frame before this one drew into the target, so the next frame draws into it again */
static inline int snx_vk_redrawn(const SnxVkRedraw* r, uint32_t present) {
    if (r->frame == present + 1) return r->again;
    return r->frame != 0 && r->frame == present;
}

#endif
