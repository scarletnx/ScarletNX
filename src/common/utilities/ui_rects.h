/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A layer's quads (ui_quads.h) as rectangles of one colour each, whole pixels of the picture they go over, for the
   windows the Switch renderer's pipeline does not draw: SDL's renderer fills them, and a Vulkan window draws one an
   instance (common/gpu/vk/rects.h). Each pixel is the one ui.frag writes: opaque, or left to the picture */
#ifndef SNX_UI_RECTS_H
#define SNX_UI_RECTS_H
#include "common/utilities/ui_quads.h"

typedef struct {
    i16 x, y;
    u16 w, h;
    u32 rgba; /* R G B A from the low byte, opaque */
} SnxUiRect;
typedef struct {
    SnxUiRect* rect;
    size_t count, room;
    u32 version; /* counts the times they were made: the same, they are the same rectangles */
    /* the picture apart from the atlas that a UI_IMAGE quad draws (a state's thumbnail, ui_quads.h), SNX_UI_IMAGE_W x
       SNX_UI_IMAGE_H texels R G B A, which the caller keeps; NULL draws none of it */
    const u8* image;
} SnxUiRects;
/* out's rectangles made anew from quads (its fills, its letters, its shapes and its picture: the kinds the on-screen
   controls and the game's menu draw), in their order, what lies off a picture w by h cut. A picture's quad is its
   nearest texel at each pixel, 5 bits a channel as a quad's colour is, a run of one colour a rectangle. false, none of
   them, when out of memory */
bool snx_ui_rects(SnxUiRects* out, const SnxUiList* quads, int w, int h);
void snx_ui_rects_free(SnxUiRects* out);
#endif
