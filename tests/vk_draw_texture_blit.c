/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* DRAW_TEXTURE's blit rectangles (gpu/vk/draw_texture.c): inside both images they are the draw's own; a rectangle
   partly past the target or the texture, on any side, is cut on both at the same point of the draw and keeps its
   scale; one wholly outside draws nothing; flips, a lower-left origin and the resolution scale carry through. Then a
   sweep of draws checks the blit against the console's rasterized rectangle: the pixels written are those whose
   centre the rectangle covers on the target and reads inside the texture, each reads where the console reads, and
   no corner leaves either image (VUID-vkCmdBlitImage-srcOffset-00243, dstOffset-00248) */
#include "nx/gpu/vk/draw_texture.h"
#include <math.h>
#include <stdio.h>

/* the failures counted, and the sweep's generator */
static struct {
    int failures;
    unsigned long long seed;
} g_vk_draw_texture_blit = {0, 0x2545F4914F6CDD1Dull};

static DrawTexture draw(float x0, float y0, float w, float h, double du, double dv, float sx0, float sy0) {
    DrawTexture t = {0};
    t.x0 = x0, t.y0 = y0, t.w = w, t.h = h, t.du = du, t.dv = dv, t.sx0 = sx0, t.sy0 = sy0;
    return t;
}

/* a 32 x 32 texture and a 64 x 48 target, unscaled */
static const DrawTextureImage TEX = {1, 1, 32, 32}, TARGET = {1, 1, 64, 48};

/* the blit of t, against what it should be: drawn, cut, and the corners x0 y0 x1 y1 when drawn */
static void expect(const char* name, const DrawTexture* t, int lower_left, u32 clip_height, const DrawTextureImage* src,
                   const DrawTextureImage* dst, int drawn, int cut, const i32 s[4], const i32 d[4]) {
    DrawTextureBlit b;
    int r = vk_draw_texture_blit(t, lower_left, clip_height, src, dst, &b);
    int ok = r == drawn && b.cut == cut;
    for (int i = 0; ok && drawn && i < 4; i++) ok = b.src[i] == s[i] && b.dst[i] == d[i];
    if (ok) return;
    fprintf(stderr,
            "FAIL %s: drawn %d cut %d, src %d,%d %d,%d dst %d,%d %d,%d; wanted drawn %d cut %d, src %d,%d %d,%d dst "
            "%d,%d %d,%d\n",
            name, r, b.cut, b.src[0], b.src[1], b.src[2], b.src[3], b.dst[0], b.dst[1], b.dst[2], b.dst[3], drawn, cut,
            drawn ? s[0] : 0, drawn ? s[1] : 0, drawn ? s[2] : 0, drawn ? s[3] : 0, drawn ? d[0] : 0, drawn ? d[1] : 0,
            drawn ? d[2] : 0, drawn ? d[3] : 0);
    g_vk_draw_texture_blit.failures++;
}
#define DRAWN(name, t, s0, s1, s2, s3, d0, d1, d2, d3, cut)                                                            \
    do {                                                                                                               \
        const i32 s_[4] = {s0, s1, s2, s3}, d_[4] = {d0, d1, d2, d3};                                                  \
        DrawTexture t_ = t;                                                                                            \
        expect(name, &t_, 0, 0, &TEX, &TARGET, 1, cut, s_, d_);                                                        \
    } while (0)
#define NOT_DRAWN(name, t, cut)                                                                                        \
    do {                                                                                                               \
        DrawTexture t_ = t;                                                                                            \
        expect(name, &t_, 0, 0, &TEX, &TARGET, 0, cut, NULL, NULL);                                                    \
    } while (0)

static void cases(void) {
    /* inside both: the rectangles as given */
    DRAWN("inside", draw(8, 4, 16, 16, 1, 1, 2, 3), 2, 3, 18, 19, 8, 4, 24, 20, 0);
    /* partly past the target on each side: the target's edge, and the texels that land there */
    DRAWN("past the left", draw(-6, 4, 16, 8, 1, 1, 0, 0), 6, 0, 16, 8, 0, 4, 10, 12, 0);
    DRAWN("past the right", draw(56, 4, 16, 8, 1, 1, 0, 0), 0, 0, 8, 8, 56, 4, 64, 12, 0);
    DRAWN("past the top", draw(8, -5, 8, 10, 1, 1, 0, 0), 0, 5, 8, 10, 8, 0, 16, 5, 0);
    DRAWN("past the bottom", draw(8, 40, 8, 16, 1, 1, 0, 4), 0, 4, 8, 12, 8, 40, 16, 48, 0);
    DRAWN("past two sides", draw(-4, -2, 72, 52, 0.25, 0.5, 0, 0), 1, 1, 17, 25, 0, 0, 64, 48, 0);
    /* reading past the texture on each side: only the pixels that read inside it, and the cut is said */
    DRAWN("texels past the left", draw(8, 4, 16, 8, 1, 1, -4, 0), 0, 0, 12, 8, 12, 4, 24, 12, 1);
    DRAWN("texels past the right", draw(8, 4, 16, 8, 1, 1, 24, 0), 24, 0, 32, 8, 8, 4, 16, 12, 1);
    DRAWN("texels past the top", draw(8, 4, 8, 16, 1, 1, 0, -6), 0, 0, 8, 10, 8, 10, 16, 20, 1);
    DRAWN("texels past the bottom", draw(8, 4, 8, 16, 1, 1, 0, 28), 0, 28, 8, 32, 8, 4, 16, 8, 1);
    /* past the target and the texture at once, the texture's edge the nearer one */
    DRAWN("both cut", draw(-4, 4, 16, 8, 1, 1, -8, 0), 0, 0, 8, 8, 4, 4, 12, 12, 1);
    /* wholly outside: past the target draws nothing, as on the console; past the texture the console reads by its
       wrap mode, which a blit can't, so the cut is said */
    NOT_DRAWN("left of the target", draw(-20, 4, 16, 8, 1, 1, 0, 0), 0);
    NOT_DRAWN("right of the target", draw(64, 4, 16, 8, 1, 1, 0, 0), 0);
    NOT_DRAWN("above the target", draw(8, -9, 8, 8, 1, 1, 0, 0), 0);
    NOT_DRAWN("below the target", draw(8, 48, 8, 8, 1, 1, 0, 0), 0);
    NOT_DRAWN("right of the texture", draw(8, 4, 8, 8, 1, 1, 40, 0), 1);
    NOT_DRAWN("below the texture", draw(8, 4, 8, 8, 1, 1, 0, 32), 1);
    NOT_DRAWN("outside both", draw(80, 4, 8, 8, 1, 1, 40, 0), 0);
    NOT_DRAWN("empty", draw(8, 4, 0, 8, 1, 1, 0, 0), 0);
    /* flipped: a negative size runs the destination backwards, and the source with it unless the step is negative
       too (the source's end is its start plus the step times the size, as Eden reads it) */
    DRAWN("flipped destination", draw(24, 4, -16, 8, -1, 1, 0, 0), 0, 0, 16, 8, 24, 4, 8, 12, 0);
    DRAWN("flipped destination past the left", draw(4, 4, -16, 8, -1, 1, 0, 0), 0, 0, 4, 8, 4, 4, 0, 12, 0);
    DRAWN("both flipped", draw(24, 4, -16, 8, 1, 1, 16, 0), 16, 0, 0, 8, 24, 4, 8, 12, 0);
    DRAWN("flipped source", draw(8, 4, 16, 8, -1, 1, 16, 0), 16, 0, 0, 8, 8, 4, 24, 12, 0);
    DRAWN("flipped source past the left", draw(-4, 4, 16, 8, -1, 1, 16, 0), 12, 0, 0, 8, 0, 4, 12, 12, 0);
    DRAWN("flipped source past the texture", draw(8, 4, 16, 8, -1, 1, 8, 0), 8, 0, 0, 8, 8, 4, 16, 12, 1);
    /* scaled: two pixels a texel, and two texels a pixel, cut keeping the scale */
    DRAWN("magnified past the left", draw(-10, 4, 40, 8, 0.5, 1, 0, 0), 5, 0, 20, 8, 0, 4, 30, 12, 0);
    DRAWN("minified past the bottom", draw(8, 40, 8, 16, 1, 2, 0, 0), 0, 0, 8, 16, 8, 40, 16, 48, 0);
    DRAWN("minified past the texture", draw(8, 4, 32, 8, 2, 1, 0, 0), 0, 0, 32, 8, 8, 4, 24, 12, 1);
    /* a step of 0: every pixel reads one texel column */
    DRAWN("one column", draw(8, 4, 16, 8, 0, 1, 5.5f, 0), 5, 0, 6, 8, 8, 4, 24, 12, 0);
    NOT_DRAWN("one column past the texture", draw(8, 4, 16, 8, 0, 1, 32, 0), 1);
    /* a step so large that only a sliver of no pixel's centre reads inside the texture (a 32.32 step near 2^31
       overflowed the float to i32 cast) */
    NOT_DRAWN("huge step", draw(8, 4, 16, 8, 2147483647.0, 1, 0, 0), 1);
    /* a fractional corner covers the pixels whose centres it covers, as rasterization does: 2.75 to 10.75 covers 3
       to 10, which read from 0.25 and 8.25 */
    DRAWN("fractional", draw(2.75f, 4, 8, 8, 1, 1, 0, 0), 0, 0, 8, 8, 3, 4, 11, 12, 0);

    /* a lower-left origin counts y up from the bottom of the surface clip (Eden's DrawTexture): 48 - 40 = 8 */
    {
        const i32 s[4] = {0, 0, 8, 16}, d[4] = {8, 8, 16, 24};
        DrawTexture t = draw(8, 40, 8, 16, 1, 1, 0, 0);
        expect("lower left", &t, 1, 48, &TEX, &TARGET, 1, 0, s, d);
    }
    /* there a y past the clip's height lands above the target: 48 - 60 = -12, cut at 0 */
    {
        const i32 s[4] = {0, 12, 8, 16}, d[4] = {8, 0, 16, 4};
        DrawTexture t = draw(8, 60, 8, 16, 1, 1, 0, 0);
        expect("lower left above the target", &t, 1, 48, &TEX, &TARGET, 1, 0, s, d);
    }
    /* a target at twice the resolution, read from a texture made from one: cut in the images' pixels */
    {
        const DrawTextureImage tex2 = {2, 2, 64, 64}, target2 = {2, 2, 128, 96};
        const i32 s[4] = {0, 0, 8, 16}, d[4] = {120, 80, 128, 96};
        DrawTexture t = draw(60, 40, 16, 16, 1, 1, 0, 0);
        expect("scaled images", &t, 0, 0, &tex2, &target2, 1, 0, s, d);
    }
}

/* a small deterministic generator, so the sweep is the same every run */
static double pick(double lo, double hi, double step) {
    g_vk_draw_texture_blit.seed = g_vk_draw_texture_blit.seed * 6364136223846793005ull + 1442695040888963407ull;
    double n = floor((hi - lo) / step) + 1;
    return lo + floor((double)(g_vk_draw_texture_blit.seed >> 11) / 9007199254740992.0 * n) * step;
}

static double sixteenths(double v) { return floor(v * 16) / 16; }
static double step(double v) { return floor(v * 65536) / 65536; }

/* what the console does on one axis of the draw, d0 to d1 on a target n long reading s0 to s1 from a texture m long:
   pixel i is written when its centre is inside the rectangle and the target, and read from inside the texture.
   Returns the pixels written, -1 when a centre sits on an edge (either answer is right there) */
typedef struct {
    double d0, d1, s0, s1;
    u32 n, m;
} Axis;
static int console_axis(const Axis* a, int on_target_only) {
    int written = 0;
    double lo = a->d0 < a->d1 ? a->d0 : a->d1, hi = a->d0 < a->d1 ? a->d1 : a->d0,
           k = (a->s1 - a->s0) / (a->d1 - a->d0);
    for (u32 i = 0; i < a->n; i++) {
        double c = i + 0.5, u = a->s0 + (c - a->d0) * k;
        if (fabs(c - lo) < 1e-6 || fabs(c - hi) < 1e-6) return -1;
        if (c < lo || c >= hi) continue;
        if (!on_target_only && a->s0 != a->s1 && (fabs(u) < 1e-6 || fabs(u - a->m) < 1e-6)) return -1;
        if (!on_target_only && (u < 0 || u > a->m || (a->s0 == a->s1 && u >= a->m))) continue;
        written++;
    }
    return written;
}
/* the blit's axis against the console's: the same pixels written, each reading where the console reads it, within
   the rounding of the blit's whole-texel corners (half a texel, plus half a pixel's worth where the destination's
   edge was rounded out past the texture's) */
static int blit_axis(const Axis* a, i32 d0, i32 d1, i32 s0, i32 s1) {
    double k = (a->s1 - a->s0) / (a->d1 - a->d0), tol = 0.5 * fabs(k) + 1.0 + 1e-9;
    double lo = a->d0 < a->d1 ? a->d0 : a->d1, hi = a->d0 < a->d1 ? a->d1 : a->d0;
    if (d0 < 0 || d1 < 0 || s0 < 0 || s1 < 0 || d0 > (i32)a->n || d1 > (i32)a->n || s0 > (i32)a->m || s1 > (i32)a->m ||
        d0 == d1 || s0 == s1)
        return 0;
    i32 first = d0 < d1 ? d0 : d1, last = d0 < d1 ? d1 : d0;
    for (u32 i = 0; i < a->n; i++) {
        double c = i + 0.5, u = a->s0 + (c - a->d0) * k;
        int console = c >= lo && c < hi && u >= 0 && u <= a->m && !(a->s0 == a->s1 && u >= a->m);
        int blit = (i32)i >= first && (i32)i < last;
        if (console != blit) return 0;
        /* vkCmdBlitImage's sample point for the pixel */
        double v = s0 + (c - d0) * (double)(s1 - s0) / (double)(d1 - d0);
        if (blit && fabs(v - u) > tol) return 0;
    }
    return 1;
}

static void sweep(void) {
    int drawn = 0, cut = 0, empty = 0, bad = 0;
    for (int i = 0; i < 400000; i++) {
        /* one pick a statement: the order a call's arguments or an initializer's values are worked out in is
           unspecified, and the sweep has to be the same everywhere */
        DrawTextureImage src, dst;
        src.scale_x = pick(1, 2, 0.5), src.scale_y = pick(1, 2, 0.5);
        src.w = (u32)pick(1, 80, 1), src.h = (u32)pick(1, 80, 1);
        dst.scale_x = pick(1, 2, 0.5), dst.scale_y = pick(1, 2, 0.5);
        dst.w = (u32)pick(1, 80, 1), dst.h = (u32)pick(1, 80, 1);
        /* corners and sizes in sixteenths, from half the image's size before it to half past it, so that most
           draws reach both images and many leave one; steps that read up to one and a half textures over the
           target's size, or 0 an eighth of the time */
        double tw = src.w / src.scale_x, th = src.h / src.scale_y, w = dst.w / dst.scale_x, h = dst.h / dst.scale_y;
        DrawTexture t = {0};
        t.x0 = (float)sixteenths(pick(-0.5, 1.5, 1.0 / 64) * w);
        t.y0 = (float)sixteenths(pick(-0.5, 1.5, 1.0 / 64) * h);
        t.w = (float)sixteenths(pick(-1.5, 1.5, 1.0 / 64) * w);
        t.h = (float)sixteenths(pick(-1.5, 1.5, 1.0 / 64) * h);
        t.du = pick(0, 7, 1) == 0 ? 0 : step(pick(-1.5, 1.5, 1.0 / 64) * tw / w);
        t.dv = pick(0, 7, 1) == 0 ? 0 : step(pick(-1.5, 1.5, 1.0 / 64) * th / h);
        t.sx0 = (float)sixteenths(pick(-0.5, 1.5, 1.0 / 64) * tw);
        t.sy0 = (float)sixteenths(pick(-0.5, 1.5, 1.0 / 64) * th);
        Axis x = {t.x0 * dst.scale_x,
                  ((double)t.x0 + t.w) * dst.scale_x,
                  t.sx0 * src.scale_x,
                  (t.sx0 + t.du * t.w) * src.scale_x,
                  dst.w,
                  src.w};
        Axis y = {t.y0 * dst.scale_y,
                  ((double)t.y0 + t.h) * dst.scale_y,
                  t.sy0 * src.scale_y,
                  (t.sy0 + t.dv * t.h) * src.scale_y,
                  dst.h,
                  src.h};
        if (x.d0 == x.d1 || y.d0 == y.d1) continue;
        int xt = console_axis(&x, 1), yt = console_axis(&y, 1), xw = console_axis(&x, 0), yw = console_axis(&y, 0);
        if (xt < 0 || yt < 0 || xw < 0 || yw < 0) continue;
        DrawTextureBlit b;
        int r = vk_draw_texture_blit(&t, 0, 0, &src, &dst, &b);
        int ok;
        if (!xt || !yt) /* nothing on the target: nothing drawn, nothing said */
            ok = !r && !b.cut, empty++;
        else if (!xw || !yw) /* on the target, but none of it read from inside the texture */
            ok = !r && b.cut, cut++;
        else
            ok = r && blit_axis(&x, b.dst[0], b.dst[2], b.src[0], b.src[2]) &&
                 blit_axis(&y, b.dst[1], b.dst[3], b.src[1], b.src[3]) && b.cut == (xw != xt || yw != yt),
            drawn++;
        if (!ok && bad++ < 5)
            fprintf(stderr,
                    "FAIL sweep: x0 %g y0 %g w %g h %g du %g dv %g sx0 %g sy0 %g, texture %ux%u at %gx%g, target %ux%u "
                    "at %gx%g: drawn %d cut %d src %d,%d %d,%d dst %d,%d %d,%d\n",
                    t.x0, t.y0, t.w, t.h, t.du, t.dv, t.sx0, t.sy0, src.w, src.h, src.scale_x, src.scale_y, dst.w,
                    dst.h, dst.scale_x, dst.scale_y, r, b.cut, b.src[0], b.src[1], b.src[2], b.src[3], b.dst[0],
                    b.dst[1], b.dst[2], b.dst[3]);
    }
    if (bad) fprintf(stderr, "FAIL sweep: %d of %d draws\n", bad, drawn + cut + empty);
    g_vk_draw_texture_blit.failures += bad;
    printf("sweep: %d drawn, %d off the texture, %d off the target\n", drawn, cut, empty);
}

int main(void) {
    cases();
    sweep();
    if (g_vk_draw_texture_blit.failures) return 1;
    puts("DRAW_TEXTURE's blit stays inside both images, cut together at the console's pixels");
    return 0;
}
