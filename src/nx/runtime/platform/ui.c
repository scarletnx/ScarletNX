/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/platform/controls_atlas.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/endian.h"
#include "common/utilities/font.h"
#include "common/utilities/users.h"
#include "gui/icons.h"
#include "gui/splash.h"
#include "gui_theme_colors.h"
#include <stdlib.h>
#include <string.h>

static PMutex g_lock = PMUTEX_INIT;
static u32* g_atlas;
static u32 g_version;
static u8 g_image[SNX_UI_IMAGE_W * SNX_UI_IMAGE_H * 4];
static int g_image_set;
static u32 g_image_version;
#define UI_PICTURE_Y SNX_CONTROLS_ATLAS_HEIGHT /* the picture's first row, under the sprites' */
_Static_assert(SNX_UI_ICONS_X + SNX_UI_ICON * SNX_UI_ICONS_ROW <= SNX_CONTROLS_ATLAS_WIDTH &&
                   (GUI_ICON_COUNT + SNX_UI_ICONS_ROW - 1) / SNX_UI_ICONS_ROW * SNX_UI_ICON <= SNX_UI_PICTURE,
               "the icons fit beside the picture");
/* the menu's look's shape for it, GuiPicture's: 0 none, 1 square, 2 round */
#ifndef SNX_GUI_THEME_YORHA_PICTURE
#error "the YoRHa theme names no [head] picture"
#endif

void ui_lock(void) { pm_lock(&g_lock); }
void ui_unlock(void) { pm_unlock(&g_lock); }

static void quad(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn,
                 int sample) {
    SnxUiQuad* q;
    if (l->n >= l->max || w <= 0 || h <= 0) return;
    q = &l->q[l->n++];
    q->x = x;
    q->y = y;
    q->w = w;
    q->h = h;
    q->u = (u16)u;
    q->v = (u16)v;
    q->uw = (u16)uw;
    q->vh = (u16)vh;
    q->rgba = rgba;
    q->turn = (u8)(turn & 3);
    q->sample = (u8)sample;
}
void ui_sprite(SnxUiList* l, int sprite, float x, float y, float w, float h, u32 alpha, int turn) {
    const unsigned short* s = snx_controls_sprites[sprite];
    if (alpha) quad(l, x, y, w, h, s[0], s[1], s[2], s[3], 0x00FFFFFFu | alpha << 24, turn, UI_BLEND);
}
int ui_picture(SnxUiList* l, float x, float y, float side) {
    if (!SNX_GUI_THEME_YORHA_PICTURE) return 0;
    quad(l, x, y, side, side, 0, UI_PICTURE_Y, SNX_UI_PICTURE, SNX_UI_PICTURE, 0xFFFFFFFFu, 0, UI_BLEND);
    return 1;
}
/* the picture of the user playing into the atlas's rows from UI_PICTURE_Y, w texels a row; transparent when there is
   no user yet or the theme shows none. A JPEG that does not draw shows Wimsy, said */
static void user_picture(u32* a, int w) {
    const SnxUser* u = g_user_count ? snx_users_selected() : NULL;
    const unsigned char* jpeg;
    uint32_t size = 0;
    int round = SNX_GUI_THEME_YORHA_PICTURE == 2, i, y;
    u32* at = a + (size_t)UI_PICTURE_Y * (size_t)w;
    if (!u || !SNX_GUI_THEME_YORHA_PICTURE) return;
    jpeg = snx_users_picture(u, &size);
    if (snx_splash_picture(jpeg, size, SNX_UI_PICTURE, round, at, w * 4)) {
        WARN("ui: %s's picture could not be drawn, Wimsy shows in the menu\n", u->nickname);
        snx_splash_picture(NULL, 0, SNX_UI_PICTURE, round, at, w * 4);
    }
    for (y = 0; y < SNX_UI_PICTURE; ++y) /* ARGB to R G B A from the low byte, the alpha premultiplied as it was */
        for (i = 0; i < SNX_UI_PICTURE; ++i) {
            u32 p = at[(size_t)y * (size_t)w + (size_t)i];
            at[(size_t)y * (size_t)w + (size_t)i] = (p & 0xFF00FF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16;
        }
}
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba) {
    /* the block's inner texels: a blend at its edge never reaches the transparent ones around it */
    quad(l, x, y, w, h, SNX_CONTROLS_WHITE_X + 1, SNX_CONTROLS_WHITE_Y + 1, 2, 2, rgba, 0, UI_SHARP);
}
float ui_text(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba) {
    float x0 = x;
    for (; *s; ++s, x += 8.0f * (float)scale) {
        u8 c = (u8)*s;
        if (c != ' ')
            quad(l, x, y, 8.0f * (float)scale, 16.0f * (float)scale, SNX_CONTROLS_FONT_X + c % 16 * 8,
                 SNX_CONTROLS_FONT_Y + c / 16 * 16, 8, 16, rgba, 0, UI_SHARP);
    }
    return x - x0;
}
float ui_icon(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba) {
    int i = (int)icon - GUI_ICON_FIRST;
    if (icon < GUI_ICON_FIRST || icon >= GUI_ICON_END) return 0;
    quad(l, x, y, 16.0f * (float)scale, 16.0f * (float)scale, SNX_UI_ICONS_X + i % SNX_UI_ICONS_ROW * SNX_UI_ICON + 1,
         UI_PICTURE_Y + i / SNX_UI_ICONS_ROW * SNX_UI_ICON + 1, SNX_UI_ICON - 2, SNX_UI_ICON - 2, rgba, 0, UI_BLEND);
    return 16.0f * (float)scale;
}
int ui_image(SnxUiList* l, float x, float y, float w, float h) {
    if (!g_image_set) return 0;
    quad(l, x, y, w, h, 0, 0, SNX_UI_IMAGE_W, SNX_UI_IMAGE_H, 0xFFFFFFFFu, 0, UI_IMAGE);
    return 1;
}
void ui_image_set(const u8* rgba) {
    g_image_set = rgba != NULL;
    if (rgba) memcpy(g_image, rgba, sizeof g_image);
    ++g_image_version;
}
const u8* ui_image_get(u32* version) {
    *version = g_image_version;
    return g_image_set ? g_image : NULL;
}

/* icon i of icons.h's list into its cell of the atlas a, w texels a row: lunasvg's drawing of it, white premultiplied
   by its coverage, or the font's '?' where it cannot be drawn, said */
static void icon_draw(u32* a, int w, int i) {
    enum { SIDE = SNX_UI_ICON - 2 };
    u32 argb[SIDE * SIDE];
    SnxSplashDrawing* drawing = snx_splash_open(&gui_icon_svgs[i]);
    u32* at = a + (size_t)(UI_PICTURE_Y + i / SNX_UI_ICONS_ROW * SNX_UI_ICON + 1) * (size_t)w + SNX_UI_ICONS_X +
              i % SNX_UI_ICONS_ROW * SNX_UI_ICON + 1;
    int x, y, failed = !drawing || snx_splash_draw(drawing, SIDE, argb, SIDE * 4);
    snx_splash_close(drawing);
    if (failed) {
        WARN("ui: the menu's icon %s.svg could not be drawn, a ? shows in its place\n", gui_icon_svgs[i].name);
        for (y = 0; y < 32; ++y) /* the font's '?' twice its size, in the middle */
            for (x = 0; x < 16; ++x)
                at[(size_t)(y + (SIDE - 32) / 2) * (size_t)w + (size_t)(x + (SIDE - 16) / 2)] =
                    gui_vga_font['?' * 16 + y / 2] & (0x80 >> x / 2) ? 0xFFFFFFFFu : 0;
        return;
    }
    for (y = 0; y < SIDE; ++y)
        for (x = 0; x < SIDE; ++x) {
            u32 cover = argb[y * SIDE + x] >> 24; /* premultiplied ARGB: whatever the colour, the alpha is the cover */
            at[(size_t)y * (size_t)w + (size_t)x] = cover | cover << 8 | cover << 16 | cover << 24;
        }
}
void ui_atlas_set(const u32* sprites, int w, int h) {
    int x, y, c, i;
    u32* a;
    if (w != SNX_CONTROLS_ATLAS_WIDTH || h != SNX_CONTROLS_ATLAS_HEIGHT) return; /* not the image the header is for */
    a = (u32*)calloc((size_t)w * (size_t)(h + SNX_UI_PICTURE), 4);
    if (!a) return;
    for (i = 0; i < w * h; ++i) { /* premultiplied: a blend between a sprite's edge and the clear texels around it */
        u32 p = snx_le32(sprites[i]), al = p >> 24;
        a[i] = ((p & 0xFFu) * al / 255) | ((p >> 8 & 0xFFu) * al / 255) << 8 | ((p >> 16 & 0xFFu) * al / 255) << 16 |
               al << 24;
    }
    for (c = 0; c < 256; ++c)
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 8; ++x)
                a[(SNX_CONTROLS_FONT_Y + c / 16 * 16 + y) * w + SNX_CONTROLS_FONT_X + c % 16 * 8 + x] =
                    gui_vga_font[c * 16 + y] & (0x80 >> x) ? 0xFFFFFFFFu : 0;
    for (y = 0; y < 4; ++y)
        for (x = 0; x < 4; ++x) a[(SNX_CONTROLS_WHITE_Y + y) * w + SNX_CONTROLS_WHITE_X + x] = 0xFFFFFFFFu;
    for (c = 0; c < GUI_ICON_COUNT; ++c) icon_draw(a, w, c);
    user_picture(a, w);
    snx_le32_array(a, (size_t)w * (size_t)(h + SNX_UI_PICTURE)); /* the texels' bytes R G B A, as the GPU reads them */
    ui_lock();
    free(g_atlas);
    g_atlas = a;
    ++g_version;
    ui_unlock();
}
const u32* ui_atlas(int* w, int* h, u32* version) {
    *w = SNX_CONTROLS_ATLAS_WIDTH;
    *h = SNX_CONTROLS_ATLAS_HEIGHT + SNX_UI_PICTURE;
    *version = g_version;
    return g_atlas;
}
void ui_atlas_done(void) {
    free(g_atlas);
    g_atlas = NULL;
}
