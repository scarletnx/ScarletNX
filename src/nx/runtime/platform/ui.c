/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/platform/controls_atlas.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/endian.h"
#include "common/utilities/font.h"
#include "common/utilities/users.h"
#include "common/utilities/mii_db.h"
#include "common/utilities/mii_model.h"
#include "gui/icons.h"
#include "gui/splash.h"
#include "gui_theme_colors.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static PMutex g_lock = PMUTEX_INIT;
static u32* g_atlas;
static u32 g_version;
static u8 g_image[SNX_UI_IMAGE_W * SNX_UI_IMAGE_H * 4];
static int g_image_set;
static u32 g_image_version;
static u32* g_skin; /* a skin's rows, R G B A bytes, straight alpha */
static int g_skin_rows;
static int g_atlas_h;                          /* g_atlas's rows: the skin's when it was made */
#define UI_PICTURE_Y SNX_CONTROLS_ATLAS_HEIGHT /* the picture's first row, under the sprites' */
#define UI_SKIN_Y    (UI_PICTURE_Y + SNX_UI_PICTURE)
#define UI_ROWS_MAX  4096
_Static_assert(SNX_UI_ICONS_X + 16 * GUI_ICON_COUNT <= SNX_CONTROLS_ATLAS_WIDTH, "the icons fit beside the picture");
/* the menu's look's shape for it, GuiPicture's: 0 none, 1 square, 2 round */
#ifndef SNX_GUI_THEME_YORHA_PICTURE
#error "the YoRHa theme names no [head] picture"
#endif

void ui_lock(void) { pm_lock(&g_lock); }
void ui_unlock(void) { pm_unlock(&g_lock); }

_Static_assert(sizeof(SnxUiQuad) == 32, "a quad is the renderer's instance, with no padding");
_Static_assert(GUI_ICON_COUNT <= 256, "an icon is a quad's glyph byte");

static void quad(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn,
                 int sample, int ink, int glyph) {
    if (l->n >= l->max || w <= 0 || h <= 0) return;
    l->q[l->n++] = (SnxUiQuad){.x = x,
                               .y = y,
                               .w = w,
                               .h = h,
                               .u = (u16)u,
                               .v = (u16)v,
                               .uw = (u16)uw,
                               .vh = (u16)vh,
                               .rgba = rgba,
                               .turn = (u8)(turn & 3),
                               .sample = (u8)sample,
                               .ink = (u8)ink,
                               .glyph = (u8)glyph};
}
void ui_sprite(SnxUiList* l, int sprite, float x, float y, float w, float h, u32 alpha, int turn) {
    const unsigned short* s = snx_controls_sprites[sprite];
    if (alpha) quad(l, x, y, w, h, s[0], s[1], s[2], s[3], 0x00FFFFFFu | alpha << 24, turn, UI_BLEND, UI_ATLAS, 0);
}
void ui_texels(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba) {
    if (rgba >> 24) quad(l, x, y, w, h, u, v, uw, vh, rgba, 0, UI_BLEND, UI_ATLAS, 0);
}
int ui_picture(SnxUiList* l, float x, float y, float side) {
    if (!SNX_GUI_THEME_YORHA_PICTURE) return 0;
    quad(l, x, y, side, side, 0, UI_PICTURE_Y, SNX_UI_PICTURE, SNX_UI_PICTURE, 0xFFFFFFFFu, 0, UI_BLEND, UI_ATLAS, 0);
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
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba) {
    /* the block's inner texels: a blend at its edge never reaches the transparent ones around it */
    quad(l, x, y, w, h, SNX_CONTROLS_WHITE_X + 1, SNX_CONTROLS_WHITE_Y + 1, 2, 2, rgba, 0, UI_SHARP, UI_SOLID,
         0);
}
void ui_fill_set(SnxUiQuad* q, float x, float y, float w, float h, u32 rgba) {
    int shown = w > 0 && h > 0;
    *q = (SnxUiQuad){.x = x,
                     .y = y,
                     .w = w,
                     .h = h,
                     .u = SNX_CONTROLS_WHITE_X + 1,
                     .v = SNX_CONTROLS_WHITE_Y + 1,
                     .uw = (u16)(shown ? 2 : 0),
                     .vh = (u16)(shown ? 2 : 0),
                     .rgba = rgba,
                     .sample = UI_SHARP,
                     .ink = UI_SOLID};
}
float ui_text(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba) {
    float x0 = x;
    for (; *s; ++s, x += 8.0f * (float)scale) {
        u8 c = (u8)*s;
        if (c != ' ')
            quad(l, x, y, 8.0f * (float)scale, 16.0f * (float)scale, SNX_CONTROLS_FONT_X + c % 16 * 8,
                 SNX_CONTROLS_FONT_Y + c / 16 * 16, 8, 16, rgba, 0, UI_SHARP, UI_GLYPH, c);
    }
    return x - x0;
}
int ui_cells(SnxUiList* l, float x, float y, int scale, int n, u32 rgba) {
    int first = l->n, k;
    if (n < 0 || n > l->max - l->n) return -1;
    for (k = 0; k < n; ++k)
        l->q[l->n++] = (SnxUiQuad){.x = x + 8.0f * (float)(scale * k),
                                   .y = y,
                                   .w = 8.0f * (float)scale,
                                   .h = 16.0f * (float)scale,
                                   .rgba = rgba,
                                   .sample = UI_SHARP,
                                   .ink = UI_GLYPH};
    return first;
}
int ui_cells_set(SnxUiQuad* q, int n, const char* s) {
    int k, changed = 0;
    for (k = 0; k < n; ++k) {
        u8 c = *s ? (u8)*s++ : ' ';
        int blank = c == ' ';
        /* a blank's texels are none: the renderer draws nothing for it */
        u16 u = blank ? 0 : (u16)(SNX_CONTROLS_FONT_X + c % 16 * 8);
        u16 v = blank ? 0 : (u16)(SNX_CONTROLS_FONT_Y + c / 16 * 16);
        if (q[k].u == u && q[k].v == v && q[k].uw == (blank ? 0 : 8)) continue;
        q[k].glyph = c;
        q[k].u = u;
        q[k].v = v;
        q[k].uw = blank ? 0 : 8;
        q[k].vh = blank ? 0 : 16;
        ++changed;
    }
    return changed;
}
char* ui_put(char* at, const char* s) {
    while (*s) *at++ = *s++;
    return at;
}
char* ui_put_u(char* at, u64 v) {
    char d[20];
    int n = 0;
    do d[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n) *at++ = d[--n];
    return at;
}
char* ui_put_tenths(char* at, u64 tenths) {
    at = ui_put_u(at, tenths / 10);
    *at++ = '.';
    *at++ = (char)('0' + tenths % 10);
    return at;
}
void ui_append(SnxUiList* l, const SnxUiList* from) {
    int n = from->n < l->max - l->n ? from->n : l->max - l->n;
    if (n <= 0) return;
    memcpy(l->q + l->n, from->q, (size_t)n * sizeof *l->q);
    l->n += n;
}
float ui_icon(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba) {
    if (icon < GUI_ICON_FIRST || icon >= GUI_ICON_END) return 0;
    quad(l, x, y, 16.0f * (float)scale, 16.0f * (float)scale, SNX_UI_ICONS_X + 16 * (int)(icon - GUI_ICON_FIRST),
         UI_PICTURE_Y, 16, 16, rgba, 0, UI_SHARP, UI_ICON, (int)(icon - GUI_ICON_FIRST));
    return 16.0f * (float)scale;
}
int ui_image(SnxUiList* l, float x, float y, float w, float h) {
    if (!g_image_set) return 0;
    quad(l, x, y, w, h, 0, 0, SNX_UI_IMAGE_W, SNX_UI_IMAGE_H, 0xFFFFFFFFu, 0, UI_IMAGE, UI_ATLAS, 0);
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

int ui_skin_y(void) { return UI_SKIN_Y; }
int ui_skin_room(void) { return UI_ROWS_MAX - UI_SKIN_Y; }
void ui_skin_set(u32* texels, int rows) {
    free(g_skin);
    g_skin = texels && rows > 0 && rows <= ui_skin_room() ? texels : NULL;
    g_skin_rows = g_skin ? rows : 0;
    if (texels && !g_skin) free(texels);
}
/* premultiplied: a blend between a sprite's edge and the clear texels around it */
static void premultiply(u32* to, const u32* from, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        u32 p = snx_le32(from[i]), al = p >> 24;
        to[i] = ((p & 0xFFu) * al / 255) | ((p >> 8 & 0xFFu) * al / 255) << 8 | ((p >> 16 & 0xFFu) * al / 255) << 16 |
                al << 24;
    }
}
u32* ui_atlas_make(const u32* sprites, int w, int h, int* out_rows) {
    int x, y, c, rows = h + SNX_UI_PICTURE + g_skin_rows;
    u32* a;
    if (!sprites || w != SNX_CONTROLS_ATLAS_WIDTH || h != SNX_CONTROLS_ATLAS_HEIGHT) return NULL; /* not the header's */
    a = (u32*)calloc((size_t)w * (size_t)rows, 4);
    if (!a) return NULL;
    premultiply(a, sprites, (size_t)w * (size_t)h);
    if (g_skin) premultiply(a + (size_t)w * UI_SKIN_Y, g_skin, (size_t)w * (size_t)g_skin_rows);
    for (c = 0; c < 256; ++c)
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 8; ++x)
                a[(SNX_CONTROLS_FONT_Y + c / 16 * 16 + y) * w + SNX_CONTROLS_FONT_X + c % 16 * 8 + x] =
                    gui_vga_font[c * 16 + y] & (0x80 >> x) ? 0xFFFFFFFFu : 0;
    for (y = 0; y < 4; ++y)
        for (x = 0; x < 4; ++x) a[(SNX_CONTROLS_WHITE_Y + y) * w + SNX_CONTROLS_WHITE_X + x] = 0xFFFFFFFFu;
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
void ui_atlas_set(const u32* sprites, int w, int h) {
    int rows = 0;
    u32* a = ui_atlas_make(sprites, w, h, &rows);
    if (!a) return;
    ui_lock();
    ui_atlas_put(a, rows);
    ui_unlock();
}
const u32* ui_atlas(int* w, int* h, u32* version) {
    *w = SNX_CONTROLS_ATLAS_WIDTH;
    *h = g_atlas_h ? g_atlas_h : UI_SKIN_Y;
    *version = g_version;
    return g_atlas;
}
void ui_atlas_done(void) {
    free(g_atlas);
    g_atlas = NULL;
}
