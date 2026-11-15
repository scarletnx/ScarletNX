/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ui_raster.h"
#include "ui_shape_ref.h"
#include <math.h>

/* one quad as it is drawn: a shape from its place alone, else the atlas's nearest texel times its colour, written where
   it is half opaque or more (ui.frag's rule) */
static void quad(const SnxUiQuadView* q, const u32* atlas, int atlas_h, u8* rgba, int w, int h) {
    float color[4] = {(float)(q->rgba & 0xFF) / 255.0f, (float)(q->rgba >> 8 & 0xFF) / 255.0f,
                      (float)(q->rgba >> 16 & 0xFF) / 255.0f, (float)(q->rgba >> 24) / 255.0f};
    int x0 = (int)floorf(q->x), y0 = (int)floorf(q->y), x1 = (int)ceilf(q->x + q->w), y1 = (int)ceilf(q->y + q->h);
    if (!q->uw || q->sample == UI_IMAGE) return;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < h; ++y)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; ++x) {
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f, src[4];
            u8* out = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (cx < q->x || cx > q->x + q->w || cy < q->y || cy > q->y + q->h) continue;
            if (q->ink == UI_SHAPE) {
                if (ui_shape_ref(q->u, q->v, q->uw & 0x7FFFu, q->vh, q->w, q->h, cx - q->x, cy - q->y) <= 0) continue;
                for (int k = 0; k < 4; ++k) out[k] = (u8)(color[k] * 255.0f + 0.5f);
                out[3] = 255;
                continue;
            }
            int tu = (int)floorf((float)q->u + (cx - q->x) / q->w * (float)q->uw);
            int tv = (int)floorf((float)q->v + (cy - q->y) / q->h * (float)q->vh);
            if (tu < 0 || tv < 0 || tu >= SNX_UI_ATLAS_W || tv >= atlas_h) continue;
            const u8* t = (const u8*)&atlas[(size_t)tv * SNX_UI_ATLAS_W + (size_t)tu];
            for (int k = 0; k < 3; ++k) src[k] = (float)t[k] / 255.0f * color[k] * color[3];
            src[3] = (float)t[3] / 255.0f * color[3];
            if (src[3] < 0.5f) continue;
            for (int k = 0; k < 3; ++k) out[k] = (u8)(src[k] / src[3] * 255.0f + 0.5f);
            out[3] = 255;
        }
}
void ui_raster(const SnxUiQuad* q, int n, const u32* atlas, int atlas_h, u8* rgba, int w, int h, const u8 back[4]) {
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i)
        for (int k = 0; k < 4; ++k) rgba[i * 4 + k] = back[k];
    for (int i = 0; i < n; ++i) {
        SnxUiQuadView ground, drawn = ui_drawn(&q[i]);
        if (ui_drawn_ground(&q[i], &ground)) quad(&ground, atlas, atlas_h, rgba, w, h);
        quad(&drawn, atlas, atlas_h, rgba, w, h);
    }
}
