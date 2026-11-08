/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The boot splash's drawings: Wimsy and dist/splash's, compiled in as SVG text (cmake/splash.cmake), and drawn at the
   splash's size by lunasvg (splash.cpp) */
#ifndef SNX_GUI_SPLASH_H
#define SNX_GUI_SPLASH_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* name; /* its file's, without .svg */
    const char* text;
    size_t size;
} SnxSplashSvg;
extern const SnxSplashSvg snx_splash_svgs[]; /* Wimsy's own first */
extern const int snx_splash_svg_count;

/* svg's drawing, read once to be drawn at any size; NULL when its text is not one */
typedef struct SnxSplashDrawing SnxSplashDrawing;
SnxSplashDrawing* snx_splash_open(const SnxSplashSvg* svg);
/* the drawing filling a side x side square of pixels, ARGB8888 with the alpha premultiplied, stride bytes a row, the
   rest of it transparent; 0 when drawn */
int snx_splash_draw(const SnxSplashDrawing* drawing, int side, void* pixels, int stride);
void snx_splash_close(SnxSplashDrawing* drawing);

/* the drawing named name (its file's, without .svg) filling a side x side square over white, as a JPEG of the
   quality given (1 to 100), handed out through write in pieces; 0 when written */
typedef void (*SnxSplashWrite)(void* closure, void* data, int size);
int snx_splash_jpeg(const char* name, int side, int quality, SnxSplashWrite write, void* closure);
/* width x height pixels of RGBA8, a row after another, their alpha left out, as a JPEG of the quality given (1 to 100)
   handed out through write in pieces; 0 when written */
int snx_splash_jpeg_rgba(const void* rgba, int width, int height, int quality, SnxSplashWrite write, void* closure);

/* a user's picture filling a side x side square of pixels, ARGB8888 with the alpha premultiplied, stride bytes a row:
   the middle square of the JPEG (size bytes), or with none Wimsy in Fuddles' colours over white, as acc gives a game.
   round: cut to the circle the square holds, the rest transparent. 0 when drawn; 1 for a JPEG that does not decode */
int snx_splash_picture(const void* jpeg, size_t size, int side, int round, void* pixels, int stride);
#ifdef __cplusplus
}
#endif
#endif
