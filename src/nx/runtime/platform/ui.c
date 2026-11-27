/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/ui.h"
#include "common/utilities/ui_colors.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/endian.h"
#include "common/utilities/font.h"
#include "common/utilities/users.h"
#include "common/utilities/mii_db.h"
#include "common/utilities/mii_model.h"
#include "gui/icons.h"
#include "gui/splash.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static u32* g_atlas;
static u32 g_version;
static u8 g_image[SNX_UI_IMAGE_W * SNX_UI_IMAGE_H * 4];
static int g_image_set;
static u32 g_image_version;
static int g_atlas_h; /* g_atlas's rows */
#define UI_PICTURE_Y SNX_UI_PICTURE_Y
#define UI_ROWS      (UI_PICTURE_Y + SNX_UI_PICTURE) /* the atlas's */
_Static_assert(SNX_UI_ICONS_X + 16 * GUI_ICON_COUNT <= SNX_UI_ATLAS_W, "the icons fit beside the picture");
/* the menu's look's shape for it, GuiPicture's: 0 none, 1 square, 2 round */
#ifndef SNX_GUI_THEME_YORHA_PICTURE
#error "the YoRHa theme names no [head] picture"
#endif

_Static_assert(GUI_ICON_COUNT <= 256, "an icon is a quad's glyph byte");

int ui_picture(SnxUiList* l, float x, float y, float side) {
    if (!SNX_GUI_THEME_YORHA_PICTURE) return 0;
    ui_quad_add(l, x, y, side, side, 0, UI_PICTURE_Y, SNX_UI_PICTURE, SNX_UI_PICTURE, 0xFFFFFFFFu, 0, UI_BLEND,
                UI_ATLAS, 0);
    return 1;
}
/* the user's Mii (users.ini's picture=mii) as its picture at at, w texels a row, R G B A from the low byte with the
   alpha premultiplied, cut to the circle the square holds when round: 0, or -1 when it could not be drawn */
static int user_mii_picture(const SnxUser* u, u32* at, int w, int round) {
    u8 charinfo[SNX_MII_CHARINFO], *rgba = (u8*)malloc((size_t)SNX_UI_PICTURE * SNX_UI_PICTURE * 4);
    SnxMiiLook look;
    int x, y;
    if (!rgba) return -1;
    snx_mii_db_charinfo(u->mii, charinfo);
    snx_mii_db_look(u->mii, &look);
    if (snx_mii_picture(charinfo, &look, SNX_UI_PICTURE, rgba)) {
        free(rgba);
        return -1;
    }
    for (y = 0; y < SNX_UI_PICTURE; ++y)
        for (x = 0; x < SNX_UI_PICTURE; ++x) {
            const u8* p = rgba + 4 * ((size_t)y * SNX_UI_PICTURE + (size_t)x);
            float dx = (float)x + 0.5f - SNX_UI_PICTURE / 2.0f, dy = (float)y + 0.5f - SNX_UI_PICTURE / 2.0f;
            float c = round ? SNX_UI_PICTURE / 2.0f - sqrtf(dx * dx + dy * dy) + 0.5f : 1.0f;
            c = c < 0 ? 0 : c > 1 ? 1 : c;
            at[(size_t)y * (size_t)w + (size_t)x] =
                (u32)(p[0] * c) | (u32)(p[1] * c) << 8 | (u32)(p[2] * c) << 16 | (u32)(255 * c) << 24;
        }
    free(rgba);
    return 0;
}
/* the picture of the user playing into the atlas's rows from UI_PICTURE_Y, w texels a row; transparent when there is
   no user yet or the theme shows none. Its Mii's with picture=mii; a JPEG (or Mii) that does not draw shows Wimsy,
   said */
static void user_picture(u32* a, int w) {
    const SnxUser* u = g_user_count ? snx_users_selected() : NULL;
    const unsigned char* jpeg;
    uint32_t size = 0;
    int round = SNX_GUI_THEME_YORHA_PICTURE == 2, i, y;
    u32* at = a + (size_t)UI_PICTURE_Y * (size_t)w;
    if (!u || !SNX_GUI_THEME_YORHA_PICTURE) return;
    if (u->picture_mii) {
        if (!user_mii_picture(u, at, w, round)) return;
        WARN("ui: user %d's Mii could not be drawn, Wimsy shows in the menu\n", snx_users_selected_number());
    }
    jpeg = u->picture_mii ? NULL : snx_users_picture(u, &size);
    if (snx_splash_picture(jpeg, size, SNX_UI_PICTURE, round, at, w * 4)) {
        /* the user by number: a nickname can name the player, and players attach the log to public reports */
        WARN("ui: user %d's picture could not be drawn, Wimsy shows in the menu\n", snx_users_selected_number());
        snx_splash_picture(NULL, 0, SNX_UI_PICTURE, round, at, w * 4);
    }
    for (y = 0; y < SNX_UI_PICTURE; ++y) /* ARGB to R G B A from the low byte, the alpha premultiplied as it was */
        for (i = 0; i < SNX_UI_PICTURE; ++i) {
            u32 p = at[(size_t)y * (size_t)w + (size_t)i];
            at[(size_t)y * (size_t)w + (size_t)i] = (p & 0xFF00FF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16;
        }
}
void ui_loading_ring(SnxUiList* l, float cx, float cy, int scale, u64 now_ms, int still, u32 dim, u32 soft, u32 ink) {
    /* the launcher's strokes are a line a pixel in from each whole pixel of their width: 1.5 at 1x takes two */
    const float s = (float)scale, r = 7.0f * s, inner = r * 0.62f, thick = ceilf(s * 1.5f);
    const float mid = inner - thick / 2, from = still ? 0.0f : (float)(now_ms / 4 % 360);
    const int edge = (int)ceilf(s * 0.5f);
    ui_shape(l, cx - r, cy - r, 2 * r, 2 * r, UI_SHAPE_CIRCLE, NULL, 0, edge, dim);
    ui_shape(l, cx - inner, cy - inner, 2 * inner, 2 * inner, UI_SHAPE_CIRCLE, NULL, 0, (int)thick, soft);
    for (int i = 0; i < UI_RING_BAR_DOTS; ++i) { /* dots a stroke wide, overlapping along the bar */
        const float a = (from + 70.0f * (float)i / (UI_RING_BAR_DOTS - 1)) * 3.14159265f / 180.0f;
        ui_shape(l, cx + mid * cosf(a) - thick / 2, cy + mid * sinf(a) - thick / 2, thick, thick, UI_SHAPE_CIRCLE, NULL,
                 UI_SHAPE_FILLED, 1, ink);
    }
}
int ui_image(SnxUiList* l, float x, float y, float w, float h) {
    return ui_image_part(l, x, y, w, h, 0, 0, SNX_UI_IMAGE_W, SNX_UI_IMAGE_H);
}
int ui_image_part(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh) {
    if (!g_image_set) return 0;
    ui_quad_add(l, x, y, w, h, u, v, uw, vh, 0xFFFFFFFFu, 0, UI_IMAGE, UI_ATLAS, 0);
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

u32* ui_atlas_make(int* out_rows) {
    int x, y, c, w = SNX_UI_ATLAS_W, rows = UI_ROWS;
    u32* a = (u32*)calloc((size_t)w * (size_t)rows, 4);
    if (!a) return NULL;
    for (c = 0; c < 256; ++c)
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 8; ++x)
                a[(SNX_UI_FONT_Y + c / 16 * 16 + y) * w + SNX_UI_FONT_X + c % 16 * 8 + x] =
                    gui_vga_font[c * 16 + y] & (0x80 >> x) ? 0xFFFFFFFFu : 0;
    for (y = 0; y < 4; ++y)
        for (x = 0; x < 4; ++x) a[(SNX_UI_WHITE_Y + y) * w + SNX_UI_WHITE_X + x] = 0xFFFFFFFFu;
    for (c = 0; c < GUI_ICON_COUNT; ++c) /* each icon's two halves side by side */
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 16; ++x)
                a[(UI_PICTURE_Y + y) * w + SNX_UI_ICONS_X + c * 16 + x] =
                    gui_icon_font[(c * 2 + x / 8) * 16 + y] & (0x80 >> x % 8) ? 0xFFFFFFFFu : 0;
    user_picture(a, w);
    /* the texels' bytes R G B A, as the GPU reads them */
    snx_le32_array(a, (size_t)w * (size_t)rows);
    *out_rows = rows;
    return a;
}
void ui_atlas_put(u32* atlas, int rows) {
    free(g_atlas);
    g_atlas = atlas;
    g_atlas_h = rows;
    ++g_version;
}
void ui_atlas_build(void) {
    int rows = 0;
    u32* a = ui_atlas_make(&rows);
    if (!a) return;
    ui_lock();
    ui_atlas_put(a, rows);
    ui_unlock();
}
const u32* ui_atlas(int* w, int* h, u32* version) {
    *w = SNX_UI_ATLAS_W;
    *h = g_atlas_h ? g_atlas_h : UI_ROWS;
    *version = g_version;
    return g_atlas;
}
void ui_atlas_done(void) {
    free(g_atlas);
    g_atlas = NULL;
}
