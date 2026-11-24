/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The boot splash's drawings: every one built in draws at any size, filling its square as Wimsy does, and Wimsy
   drawn here matches dist/icons' PNG, which rsvg-convert drew from the same SVG. A user's picture (snx_splash_picture)
   is a JPEG's middle square, square or round, and Wimsy over white with none */
#include "gui/splash.h"
#include <SDL3/SDL.h>
#include <plutovg.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the opaque share of a drawing, in thousandths, and the box its visible pixels take */
typedef struct {
    int opaque, left, top, right, bottom;
} Coverage;

static Coverage coverage(const Uint32* pixels, int side) {
    Coverage c = {0, side, side, -1, -1};
    int x, y, opaque = 0;
    for (y = 0; y < side; ++y)
        for (x = 0; x < side; ++x) {
            Uint32 a = pixels[y * side + x] >> 24;
            opaque += a == 255;
            if (!a) continue;
            if (x < c.left) c.left = x;
            if (y < c.top) c.top = y;
            if (x > c.right) c.right = x;
            if (y > c.bottom) c.bottom = y;
        }
    c.opaque = (int)((long long)opaque * 1000 / ((long long)side * side));
    return c;
}

/* a JPEG of w x h, its first half (the left one if wide, the top one if tall) red and the rest blue */
typedef struct {
    unsigned char data[16384];
    int size;
} Jpeg;
static void jpeg_write(void* closure, void* data, int size) {
    Jpeg* j = closure;
    assert(j->size + size <= (int)sizeof j->data);
    memcpy(j->data + j->size, data, (size_t)size);
    j->size += size;
}
static void halves(Jpeg* j, int w, int h) {
    plutovg_surface_t* s = plutovg_surface_create(w, h);
    plutovg_canvas_t* c = plutovg_canvas_create(s);
    plutovg_canvas_set_rgba(c, 0, 0, 1, 1);
    plutovg_canvas_fill_rect(c, 0, 0, (float)w, (float)h);
    plutovg_canvas_set_rgba(c, 1, 0, 0, 1);
    plutovg_canvas_fill_rect(c, 0, 0, w > h ? w / 2.0f : (float)w, w > h ? (float)h : h / 2.0f);
    plutovg_canvas_destroy(c);
    j->size = 0;
    assert(plutovg_surface_write_to_jpg_stream(s, jpeg_write, j, 95));
    plutovg_surface_destroy(s);
}
/* pixel x, y of a side-wide picture is r, g, b (within a JPEG's error) at alpha a */
static int is(const Uint32* p, int side, int x, int y, int r, int g, int b, int a) {
    Uint32 v = p[y * side + x];
    return abs((int)(v >> 16 & 255) - r) < 48 && abs((int)(v >> 8 & 255) - g) < 48 && abs((int)(v & 255) - b) < 48 &&
           abs((int)(v >> 24) - a) < 8;
}
static void pictures(Uint32* p) {
    enum { SIDE = 40 };
    Jpeg j;
    int x, white = 0;
    /* a wide one keeps its middle square: red on the left, blue on the right, every pixel opaque */
    halves(&j, 64, 32);
    assert(!snx_splash_picture(j.data, (size_t)j.size, SIDE, 0, p, SIDE * 4));
    assert(is(p, SIDE, 8, 20, 255, 0, 0, 255) && is(p, SIDE, 32, 20, 0, 0, 255, 255));
    assert(is(p, SIDE, 0, 0, 255, 0, 0, 255) && is(p, SIDE, SIDE - 1, SIDE - 1, 0, 0, 255, 255));
    /* round: the corners are transparent, the middle is not, and nothing outside the circle shows */
    assert(!snx_splash_picture(j.data, (size_t)j.size, SIDE, 1, p, SIDE * 4));
    assert(!(p[0] >> 24) && !(p[SIDE - 1] >> 24) && !(p[SIDE * SIDE - 1] >> 24));
    assert(is(p, SIDE, 8, 20, 255, 0, 0, 255) && is(p, SIDE, 32, 20, 0, 0, 255, 255));
    /* a tall one: red on top, blue under it */
    halves(&j, 32, 64);
    assert(!snx_splash_picture(j.data, (size_t)j.size, SIDE, 0, p, SIDE * 4));
    assert(is(p, SIDE, 20, 8, 255, 0, 0, 255) && is(p, SIDE, 20, 32, 0, 0, 255, 255));
    /* none: Wimsy over white, its corners white and opaque, or transparent cut round */
    assert(!snx_splash_picture(NULL, 0, SIDE, 0, p, SIDE * 4));
    assert(is(p, SIDE, 0, 0, 255, 255, 255, 255));
    for (x = 0; x < SIDE * SIDE; ++x) white += (p[x] & 0xFFFFFFu) == 0xFFFFFFu;
    assert(white < SIDE * SIDE * 9 / 10); /* Wimsy is there */
    assert(!snx_splash_picture(NULL, 0, SIDE, 1, p, SIDE * 4) && !(p[0] >> 24));
    /* a JPEG that does not decode, and what is no picture, say so */
    memset(j.data + 2, 0x55, 64);
    assert(snx_splash_picture(j.data, 66, SIDE, 0, p, SIDE * 4));
    assert(snx_splash_picture(NULL, 0, 0, 0, p, 0) && snx_splash_picture(NULL, 0, SIDE, 0, p, SIDE * 4 - 1));
    puts("pictures: a JPEG's middle square, square or round, Wimsy for none");
}

/* the splash's choice by month: in December only the Christmas drawings (a name ending in -xmas), the rest of the
   year every other one and never those, each reached by some r */
static void picks(void) {
    for (int month = 1; month <= 12; month++) {
        int seen[64] = {0}, christmas = 0;
        for (unsigned r = 0; r < 256; r++) {
            const SnxSplashSvg* svg = snx_splash_pick(month, r);
            assert(svg && svg >= snx_splash_svgs && svg < snx_splash_svgs + snx_splash_svg_count);
            const size_t n = strlen(svg->name);
            const int xmas = n >= 5 && !strcmp(svg->name + n - 5, "-xmas");
            if (xmas != (month == 12)) {
                fprintf(stderr, "FAIL: month %d picks %s\n", month, svg->name);
                exit(1);
            }
            seen[svg - snx_splash_svgs] = 1;
            christmas += xmas;
        }
        for (int i = 0; i < snx_splash_svg_count && i < 64; i++) {
            const size_t n = strlen(snx_splash_svgs[i].name);
            const int xmas = n >= 5 && !strcmp(snx_splash_svgs[i].name + n - 5, "-xmas");
            if (xmas == (month == 12) && !seen[i]) {
                fprintf(stderr, "FAIL: month %d never picks %s\n", month, snx_splash_svgs[i].name);
                exit(1);
            }
        }
        assert(month != 12 || christmas == 256);
    }
    assert(!snx_splash_pick(0, 0) || snx_splash_pick(0, 0) == snx_splash_pick(1, 0)); /* no date: as January */
}

int main(void) {
    static const int sides[] = {1, 64, 240, 333, 1024};
    int i, k, x, y;
    SnxSplashDrawing* drawing;
    Uint32* pixels = malloc(1024 * 1024 * 4);
    assert(pixels);
    /* Wimsy first, then dist/splash's, each one once, as the SVG text without its metadata and comments */
    assert(snx_splash_svg_count >= 2 && !strcmp(snx_splash_svgs[0].name, "wimsy"));
    for (i = 0; i < snx_splash_svg_count; ++i) {
        const SnxSplashSvg* svg = &snx_splash_svgs[i];
        char* text = malloc(svg->size + 1);
        assert(text);
        memcpy(text, svg->text, svg->size);
        text[svg->size] = 0;
        assert(!strncmp(text, "<svg ", 5) && !strcmp(text + svg->size - 6, "</svg>"));
        assert(!strstr(text, "<metadata") && !strstr(text, "<!--") && !strstr(text, "\n"));
        for (k = 0; k < i; ++k) assert(strcmp(snx_splash_svgs[k].name, svg->name));
        free(text);
        /* read once, drawn at every size from that */
        drawing = snx_splash_open(svg);
        assert(drawing);
        for (k = 0; k < (int)(sizeof sides / sizeof *sides); ++k) {
            int side = sides[k];
            Coverage c;
            memset(pixels, 0xAB, (size_t)side * side * 4); /* what was there goes */
            assert(!snx_splash_draw(drawing, side, pixels, side * 4));
            c = coverage(pixels, side);
            if (side < 64) continue;
            /* the corners clear, about a third of the square opaque (less when small, its edges blended), and the leaves
               reaching its sides */
            assert(!pixels[0] && !pixels[side - 1] && !pixels[(side - 1) * side] && !pixels[side * side - 1]);
            if (c.opaque < 220 || c.opaque > 450 || c.left > side / 50 || c.right < side - 1 - side / 50 ||
                c.top > side / 40 || c.bottom < side - 1 - side / 40) {
                fprintf(stderr, "%s at %d: %d/1000 opaque, from %d,%d to %d,%d\n", svg->name, side, c.opaque, c.left,
                        c.top, c.right, c.bottom);
                assert(0);
            }
        }
        snx_splash_close(drawing);
    }
    /* Wimsy as dist/icons' 256 PNG has it: the colours (premultiplied) under a step apart on average, a few edge pixels
       more */
    {
        SDL_Surface* png = SDL_LoadPNG(SNX_SPLASH_ICON);
        long long off = 0;
        int far = 0;
        assert(png && png->w == 256 && png->h == 256);
        drawing = snx_splash_open(&snx_splash_svgs[0]);
        assert(drawing && !snx_splash_draw(drawing, 256, pixels, 256 * 4));
        snx_splash_close(drawing);
        for (y = 0; y < 256; ++y)
            for (x = 0; x < 256; ++x) {
                Uint8 r, g, b, a;
                Uint32 p = pixels[y * 256 + x];
                int ours[4], theirs[4], c, worst = 0;
                assert(SDL_ReadSurfacePixel(png, x, y, &r, &g, &b, &a));
                ours[0] = (int)(p >> 16 & 255), ours[1] = (int)(p >> 8 & 255), ours[2] = (int)(p & 255);
                ours[3] = (int)(p >> 24);
                theirs[0] = r * a / 255, theirs[1] = g * a / 255, theirs[2] = b * a / 255, theirs[3] = a;
                for (c = 0; c < 4; ++c) {
                    int d = abs(ours[c] - theirs[c]);
                    off += d;
                    if (d > worst) worst = d;
                }
                far += worst > 24;
            }
        printf("Wimsy against rsvg: %.3f mean, %d of 65536 pixels over 24\n", (double)off / (256 * 256 * 4), far);
        assert(off < 256 * 256 * 4 / 2 && far < 300);
        SDL_DestroySurface(png);
    }
    /* what it cannot draw it says, drawing nothing */
    {
        SnxSplashSvg broken = {"broken", "not a drawing", 13};
        assert(!snx_splash_open(&broken) && !snx_splash_open(NULL));
        assert(snx_splash_draw(NULL, 64, pixels, 64 * 4));
        drawing = snx_splash_open(&snx_splash_svgs[0]);
        assert(drawing && snx_splash_draw(drawing, 0, pixels, 0));
        assert(snx_splash_draw(drawing, 64, pixels, 63 * 4) && snx_splash_draw(drawing, 64, NULL, 64 * 4));
        snx_splash_close(drawing);
        snx_splash_close(NULL);
    }
    pictures(pixels);
    picks();
    free(pixels);
    return 0;
}
