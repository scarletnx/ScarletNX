/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether the game regenerates a render target every frame, which async_pipelines asks before it skips a draw whose
   pipeline a worker still makes: a skipped draw comes back only where a later frame draws the target over again
   (render.c targets_skippable). A target counts when passes drew into it SNX_VK_REDRAW_FRAMES frames in a row and this
   frame cleared it whole: one drawn once and kept (a menu's layer) or only for a few frames (its animation) waits, and
   so does one the game never clears, whose draws add up across frames (marks, trails). Each target keeps the frame a
   pass last drew into it, how many frames in a row up to that one did, and the frame that last cleared it whole */
#ifndef SNX_VK_REDRAW_H
#define SNX_VK_REDRAW_H
#include <stdint.h>

#define SNX_VK_REDRAW_FRAMES 8

typedef struct {
    uint32_t frame;   /* the present count plus 1 of the last frame a pass drew into the target, 0 for none */
    uint32_t run;     /* frames in a row up to that one that drew into it */
    uint32_t cleared; /* the present count plus 1 of the last frame that cleared it whole, 0 for none */
} SnxVkRedraw;

/* a pass draws into the target in the frame whose present count (presents done) is present */
static inline void snx_vk_redraw_seen(SnxVkRedraw* r, uint32_t present) {
    if (r->frame == present + 1) return;
    r->run = r->frame != 0 && r->frame == present ? r->run + 1 : 1;
    r->frame = present + 1;
}

/* the frame clears the whole target (every pixel of its one layer, unpredicated) */
static inline void snx_vk_redraw_cleared(SnxVkRedraw* r, uint32_t present) { r->cleared = present + 1; }

/* the frames in a row that draw into the target, this one counted as one that does */
static inline uint32_t snx_vk_redraw_run(const SnxVkRedraw* r, uint32_t present) {
    if (r->frame == present + 1) return r->run;
    return r->frame != 0 && r->frame == present ? r->run + 1 : 1;
}

/* this frame cleared the whole target */
static inline int snx_vk_redraw_cleared_now(const SnxVkRedraw* r, uint32_t present) {
    return r->cleared == present + 1;
}

#endif
