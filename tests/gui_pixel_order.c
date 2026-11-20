/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's pictures (gui/main.c) as the renderer shows them, whichever byte order the host has: a database Mii's,
   square and round, Wimsy's, the 3DS driver's RGBA icon, Show More's cover and the dots of an icon being asked for,
   each the colours of its source. A big-endian host (CI's ppc64, under qemu) puts a 32-bit format's bytes in memory
   the other way round from a little-endian one */
#define SNX_GUI_NO_MAIN
#include "gui/main.c"
#include <assert.h>

/* texture drawn as it is at the corner of a black render, its side pixels read back; owned, the texture with it
   (mine) or kept (the app's own) */
static SDL_Surface* shown(App* app, SDL_Texture* texture, int side, bool mine) {
    SDL_Surface* image;
    SDL_FRect at = {0, 0, (float)side, (float)side};
    SDL_Rect part = {0, 0, side, side};
    assert(texture);
    assert(SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255) && SDL_RenderClear(app->renderer));
    assert(SDL_RenderTexture(app->renderer, texture, NULL, &at));
    image = SDL_RenderReadPixels(app->renderer, &part);
    if (mine) SDL_DestroyTexture(texture);
    return image;
}
/* the pixel at x, y of image within one step of r, g, b; the colours read and wanted said where it is not */
static void pixel_is(const char* what, SDL_Surface* image, int x, int y, int r, int g, int b) {
    Uint8 got[4];
    assert(SDL_ReadSurfacePixel(image, x, y, &got[0], &got[1], &got[2], &got[3]));
    if (abs(got[0] - r) > 1 || abs(got[1] - g) > 1 || abs(got[2] - b) > 1) {
        fprintf(stderr, "%s at %d,%d shows %d,%d,%d, not %d,%d,%d\n", what, x, y, got[0], got[1], got[2], r, g, b);
        assert(0);
    }
}

/* database Mii mii in a square theme: each pixel the colour snx_mii_picture gives it (its R, G and B bytes); in a round
   one the middle the same and the corner clear, so black */
static void mii(App* app, int mii) {
    enum { SIDE = 48 };
    uint8_t charinfo[SNX_MII_CHARINFO], rgba[SIDE * SIDE * 4];
    SnxMiiLook look;
    SDL_Surface* image;
    bool failed = true;
    int x, y, distinct = 0;
    snx_mii_db_charinfo((uint32_t)mii, charinfo);
    snx_mii_db_look((uint32_t)mii, &look);
    assert(!snx_mii_picture(charinfo, &look, SIDE, rgba));
    app->theme.picture = GUI_PICTURE_SQUARE;
    image = shown(app, mii_texture(app, mii, SIDE, &failed), SIDE, true);
    assert(image && !failed);
    for (y = 0; y < SIDE; ++y)
        for (x = 0; x < SIDE; ++x) {
            const uint8_t* p = rgba + 4 * (y * SIDE + x);
            pixel_is("a square Mii", image, x, y, p[0], p[1], p[2]);
            distinct += p[0] != p[2]; /* red and blue apart: a swap of them shows */
        }
    assert(distinct > SIDE);
    SDL_DestroySurface(image);
    app->theme.picture = GUI_PICTURE_ROUND;
    image = shown(app, mii_texture(app, mii, SIDE, &failed), SIDE, true);
    assert(image && !failed);
    pixel_is("a round Mii's middle", image, SIDE / 2, SIDE / 2, rgba[4 * (SIDE / 2 * SIDE + SIDE / 2)],
             rgba[4 * (SIDE / 2 * SIDE + SIDE / 2) + 1], rgba[4 * (SIDE / 2 * SIDE + SIDE / 2) + 2]);
    pixel_is("a round Mii's corner", image, 0, 0, 0, 0, 0);
    SDL_DestroySurface(image);
}
/* Wimsy as a user without a picture shows: snx_splash_picture's 0xAARRGGBB words, premultiplied, so over black their
   R, G and B */
static void wimsy(App* app) {
    enum { SIDE = 48 };
    static Uint32 words[SIDE * SIDE];
    SDL_Surface* image;
    bool failed = true;
    int i;
    app->theme.picture = GUI_PICTURE_SQUARE;
    assert(!snx_splash_picture(NULL, 0, SIDE, 0, words, SIDE * 4));
    image = shown(app, picture_texture(app, NULL, 0, SIDE, &failed), SIDE, true);
    assert(image && !failed);
    for (i = 0; i < SIDE * SIDE; ++i)
        pixel_is("Wimsy's picture", image, i % SIDE, i / SIDE, (int)(words[i] >> 16 & 255), (int)(words[i] >> 8 & 255),
                 (int)(words[i] & 255));
    SDL_DestroySurface(image);
}
/* an icon.rgba of distinct colours (R by column, G by row, B fixed): its pixels, read as the ARGB8888 the row's
   texture takes them as, the same colours, scaled nearest */
static void ctr_icon(void) {
    enum { SIDE = 96 };
    static unsigned char icon[48 * 48 * 4];
    static Uint32 pixels[SIDE * SIDE];
    SDL_Surface* read;
    int x, y;
    for (y = 0; y < 48; ++y)
        for (x = 0; x < 48; ++x) {
            unsigned char* p = icon + 4 * (y * 48 + x);
            p[0] = (unsigned char)(x * 5), p[1] = (unsigned char)(y * 5), p[2] = 0x80, p[3] = 255;
        }
    assert(rgba_icon(icon, sizeof icon, SIDE, pixels) == 0);
    assert(rgba_icon(icon, sizeof icon - 1, SIDE, pixels) == 1);
    read = SDL_CreateSurfaceFrom(SIDE, SIDE, SDL_PIXELFORMAT_ARGB8888, pixels, SIDE * 4);
    assert(read);
    for (y = 0; y < SIDE; ++y)
        for (x = 0; x < SIDE; ++x) pixel_is("a 3DS icon", read, x, y, x / 2 * 5, y / 2 * 5, 0x80);
    SDL_DestroySurface(read);
}
/* Show More's cover, white with a grey +, and the grey dots of an icon being asked for, on a clear square */
static void drawn_covers(App* app) {
    SDL_Surface* image = shown(app, icon_more(app), GUI_ICON_SIDE, false);
    assert(image);
    pixel_is("Show More's cover", image, 1, 1, 255, 255, 255);
    pixel_is("Show More's +", image, GUI_ICON_SIDE / 2, GUI_ICON_SIDE / 2, 0x62, 0x62, 0x62);
    SDL_DestroySurface(image);
    image = shown(app, icon_loading(app), GUI_ICON_SIDE, false);
    assert(image);
    pixel_is("the loading dots' page", image, 1, 1, 0, 0, 0);
    pixel_is("a loading dot", image, 64, 64, 0x8a, 0x8a, 0x8a);
    SDL_DestroySurface(image);
}

int main(void) {
    App app;
    memset(&app, 0, sizeof app);
    assert(SDL_Init(SDL_INIT_VIDEO));
    assert(SDL_CreateWindowAndRenderer("pixel order", 256, 256, 0, &app.window, &app.renderer));
    app.theme = *gui_theme_builtin("yorha");
    for (int i = 0; i < SNX_MII_DB_COUNT; ++i) mii(&app, i);
    wimsy(&app);
    ctr_icon();
    drawn_covers(&app);
    SDL_DestroyTexture(app.icon_more);
    SDL_DestroyTexture(app.icon_loading);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    printf("GUI pixel order: Miis, Wimsy, a 3DS icon and the covers show their sources' colours on a %s host\n",
           SDL_BYTEORDER == SDL_BIG_ENDIAN ? "big-endian" : "little-endian");
    return 0;
}
