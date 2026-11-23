/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The window icon built in: dist/icons' 128 PNG byte for byte, decoded to the file's pixels, and a window takes it;
   in December the Christmas Wimsy's 128 PNG, the same way */
#include "common/utilities/window_icon.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char snx_window_icon_png[];
extern const size_t snx_window_icon_png_size;

int main(int argc, char** argv) {
    size_t size;
    void* bytes;
    SDL_Surface* icon;
    SDL_Surface* file;
    int drawn, y, x;
    Uint8 r, g, b, a;
    SDL_Window* window;
    (void)argc;
    (void)argv;
    assert(SDL_Init(SDL_INIT_VIDEO));
    size = 0;
    bytes = SDL_LoadFile(SNX_WINDOW_ICON_FILE, &size);
    assert(bytes && size == snx_window_icon_png_size && !memcmp(bytes, snx_window_icon_png, size));
    SDL_free(bytes);
    icon = snx_window_icon_surface_for(1);
    assert(icon && icon->w == 128 && icon->h == 128);
    file = SDL_LoadPNG(SNX_WINDOW_ICON_FILE);
    assert(file && file->w == icon->w && file->h == icon->h);
    drawn = 0;
    for (y = 0; y < icon->h; ++y)
        for (x = 0; x < icon->w; ++x) {
            Uint8 r = 0, g = 0, b = 0, a = 0, fr = 0, fg = 0, fb = 0, fa = 0;
            assert(SDL_ReadSurfacePixel(icon, x, y, &r, &g, &b, &a));
            assert(SDL_ReadSurfacePixel(file, x, y, &fr, &fg, &fb, &fa));
            assert(r == fr && g == fg && b == fb && a == fa);
            drawn += a == 255;
        }
    /* a drawing, not an empty or a filled square: the corners clear, a good part of it opaque */
    r = g = b = 0;
    a = 255;
    assert(SDL_ReadSurfacePixel(icon, 0, 0, &r, &g, &b, &a) && a == 0);
    assert(drawn > 128 * 128 / 5 && drawn < 128 * 128 * 4 / 5);
    SDL_DestroySurface(file);
    /* December's: the Christmas PNG's pixels, not the others'; no month known, the usual one */
    {
        SDL_Surface* xmas = snx_window_icon_surface_for(12);
        SDL_Surface* xmas_file = SDL_LoadPNG(SNX_WINDOW_ICON_XMAS_FILE);
        SDL_Surface* unknown = snx_window_icon_surface_for(0);
        SDL_Surface* now = snx_window_icon_surface();
        int same_xmas = 1, differs = 0, same_unknown = 1;
        assert(xmas && xmas_file && unknown && now && xmas->w == 128 && xmas->h == 128);
        for (y = 0; y < 128; ++y)
            for (x = 0; x < 128; ++x) {
                Uint8 p[4] = {0}, q[4] = {0}, m[4] = {0}, u[4] = {0};
                assert(SDL_ReadSurfacePixel(xmas, x, y, &p[0], &p[1], &p[2], &p[3]));
                assert(SDL_ReadSurfacePixel(xmas_file, x, y, &q[0], &q[1], &q[2], &q[3]));
                assert(SDL_ReadSurfacePixel(icon, x, y, &m[0], &m[1], &m[2], &m[3]));
                assert(SDL_ReadSurfacePixel(unknown, x, y, &u[0], &u[1], &u[2], &u[3]));
                same_xmas &= !memcmp(p, q, 4);
                differs |= memcmp(p, m, 4) != 0;
                same_unknown &= !memcmp(u, m, 4);
            }
        assert(same_xmas && differs && same_unknown);
        SDL_DestroySurface(now);
        SDL_DestroySurface(unknown);
        SDL_DestroySurface(xmas_file);
        SDL_DestroySurface(xmas);
    }
    SDL_DestroySurface(icon);
    window = SDL_CreateWindow("icon", 64, 64, SDL_WINDOW_HIDDEN);
    assert(window);
    snx_window_icon(window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("window icon: 128x128, %d of %d pixels opaque, the file's pixels\n", drawn, 128 * 128);
    return 0;
}
