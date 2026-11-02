/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/gpu/screens.h"
#include <math.h>

/* the small screen of the large layouts, in parts of its own size */
#define SMALL 0.4

void snx_ctr_screens_place(const SnxCtrWindowLayout* l, SnxCtrScreenRect rects[2]) {
    rects[0] = rects[1] = (SnxCtrScreenRect){0};
    double tw = l->top_w, th = l->top_h, bw = l->bottom_w, bh = l->bottom_h;
    if (!l->image_w || !l->image_h || !tw || !th || !bw || !bh) return;
    double total_w, total_h;
    switch (l->mode) {
    case SNX_CTR_SCREENS_SIDE_BY_SIDE:
        total_w = tw + bw, total_h = th > bh ? th : bh;
        rects[0] = (SnxCtrScreenRect){0, (total_h - th) / 2, tw, th};
        rects[1] = (SnxCtrScreenRect){tw, (total_h - bh) / 2, bw, bh};
        break;
    case SNX_CTR_SCREENS_LARGE_TOP:
        total_w = tw + bw * SMALL, total_h = th > bh * SMALL ? th : bh * SMALL;
        rects[0] = (SnxCtrScreenRect){0, 0, tw, th};
        rects[1] = (SnxCtrScreenRect){tw, total_h - bh * SMALL, bw * SMALL, bh * SMALL};
        break;
    case SNX_CTR_SCREENS_LARGE_BOTTOM:
        total_w = bw + tw * SMALL, total_h = bh > th * SMALL ? bh : th * SMALL;
        rects[1] = (SnxCtrScreenRect){0, 0, bw, bh};
        rects[0] = (SnxCtrScreenRect){bw, total_h - th * SMALL, tw * SMALL, th * SMALL};
        break;
    case SNX_CTR_SCREENS_TOP_ONLY:
        total_w = tw, total_h = th;
        rects[0] = (SnxCtrScreenRect){0, 0, tw, th};
        break;
    case SNX_CTR_SCREENS_BOTTOM_ONLY:
        total_w = bw, total_h = bh;
        rects[1] = (SnxCtrScreenRect){0, 0, bw, bh};
        break;
    default: /* stacked: the narrower screen centred under or over the wider */
        total_w = tw > bw ? tw : bw, total_h = th + bh;
        rects[0] = (SnxCtrScreenRect){(total_w - tw) / 2, 0, tw, th};
        rects[1] = (SnxCtrScreenRect){(total_w - bw) / 2, th, bw, bh};
        break;
    }
    double scale = fmin(l->image_w / total_w, l->image_h / total_h);
    if (l->integer && scale >= 1) scale = floor(scale);
    double left = (l->image_w - total_w * scale) / 2, top = (l->image_h - total_h * scale) / 2;
    if (l->integer) left = floor(left), top = floor(top);
    for (unsigned i = 0; i < 2; ++i) {
        if (!rects[i].w) continue;
        rects[i] = (SnxCtrScreenRect){left + rects[i].x * scale, top + rects[i].y * scale, rects[i].w * scale,
                                      rects[i].h * scale};
        if (l->integer) { /* a small screen's 0.4 of a pixel is no whole one: the corners stay on whole pixels */
            rects[i].x = floor(rects[i].x);
            rects[i].y = floor(rects[i].y);
            rects[i].w = floor(rects[i].w);
            rects[i].h = floor(rects[i].h);
        }
    }
}
