/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <math.h>
#include <string.h>
#include "nx/gpu/vk/draw_texture.h"

enum { AXIS_NONE, AXIS_OFF_TEXTURE, AXIS_DRAWN };

/* the part [*lo, *hi] of an axis of the draw, 0 at its first corner and 1 at its second, narrowed to where its edge
   e0 + t (e1 - e0) is inside an image n pixels long. e0 != e1 */
static void narrow(double e0, double e1, u32 n, double* lo, double* hi) {
    double a = -e0 / (e1 - e0), b = ((double)n - e0) / (e1 - e0);
    if (a > b) {
        double s = a;
        a = b;
        b = s;
    }
    if (a > *lo) *lo = a;
    if (b < *hi) *hi = b;
}
/* a whole edge e kept inside 0..n */
static i32 inside(double e, u32 n) {
    if (e < 0) return 0;
    if (e > (double)n) return (i32)n;
    return (i32)e;
}
/* the edge before the first pixel whose centre is at or past v, as rasterization covers pixels, inside 0..n */
static i32 pixel_edge(double v, u32 n) { return inside(ceil(v - 0.5), n); }
/* the texel edge nearest to v, inside 0..n */
static i32 texel_edge(double v, u32 n) { return inside(floor(v + 0.5), n); }

/* one axis: the destination from d0 to d1 in a target n pixels long and the source from s0 to s1 in a texture m long,
   the same points of the draw, in the images' pixels (either may run backwards: a flip). Both are cut where either
   leaves its image, then put on whole pixels: a destination edge where rasterization starts or stops covering
   pixels, and a source edge on the texel edge nearest to where that destination edge reads. Its corners go in
   dst[0], dst[1], src[0] and src[1], and *cut is set when the texture's edges leave out a pixel the target has */
static int axis(double d0, double d1, double s0, double s1, u32 n, u32 m, i32 dst[2], i32 src[2], int* cut) {
    double lo = 0, hi = 1;
    if (!isfinite(d0) || !isfinite(d1) || !isfinite(s0) || !isfinite(s1) || d0 == d1 || !n || !m) return AXIS_NONE;
    narrow(d0, d1, n, &lo, &hi);
    i32 ta = pixel_edge(d0 + lo * (d1 - d0), n), tb = pixel_edge(d0 + hi * (d1 - d0), n);
    if (!(lo < hi) || ta == tb) return AXIS_NONE;
    if (s0 != s1)
        narrow(s0, s1, m, &lo, &hi);
    else if (!(s0 >= 0 && s0 < (double)m)) /* every pixel reads the one texel column s0, which is not there */
        hi = lo;
    i32 a = pixel_edge(d0 + lo * (d1 - d0), n), b = pixel_edge(d0 + hi * (d1 - d0), n);
    if (a != ta || b != tb) *cut = 1;
    if (!(lo < hi) || a == b) {
        *cut = 1;
        return AXIS_OFF_TEXTURE;
    }
    double k = (s1 - s0) / (d1 - d0), u0 = s0 + (a - d0) * k, u1 = s0 + (b - d0) * k;
    i32 p = texel_edge(u0, m), q = texel_edge(u1, m);
    if (p == q) { /* less than a texel read: the one under the middle, the way the draw runs */
        i32 e = inside(floor((u0 + u1) / 2), m - 1);
        p = u0 <= u1 ? e : e + 1;
        q = u0 <= u1 ? e + 1 : e;
    }
    dst[0] = a, dst[1] = b, src[0] = p, src[1] = q;
    return AXIS_DRAWN;
}

int vk_draw_texture_blit(const DrawTexture* t, int lower_left, u32 clip_height, const DrawTextureImage* src,
                         const DrawTextureImage* dst, DrawTextureBlit* out) {
    double y0 = lower_left ? (double)clip_height - t->y0 : t->y0;
    double sx1 = t->sx0 + t->du * t->w, sy1 = t->sy0 + t->dv * t->h;
    i32 dx[2] = {0}, dy[2] = {0}, sx[2] = {0}, sy[2] = {0};
    int cut = 0;
    int ax = axis(t->x0 * dst->scale_x, ((double)t->x0 + t->w) * dst->scale_x, t->sx0 * src->scale_x,
                  sx1 * src->scale_x, dst->w, src->w, dx, sx, &cut);
    int ay = axis(y0 * dst->scale_y, (y0 + t->h) * dst->scale_y, t->sy0 * src->scale_y, sy1 * src->scale_y, dst->h,
                  src->h, dy, sy, &cut);
    memset(out, 0, sizeof *out);
    if (ax == AXIS_NONE || ay == AXIS_NONE) return 0; /* nothing on the target, as on the console */
    out->cut = cut;
    if (ax != AXIS_DRAWN || ay != AXIS_DRAWN) return 0;
    out->src[0] = sx[0], out->src[1] = sy[0], out->src[2] = sx[1], out->src[3] = sy[1];
    out->dst[0] = dx[0], out->dst[1] = dy[0], out->dst[2] = dx[1], out->dst[3] = dy[1];
    return 1;
}
