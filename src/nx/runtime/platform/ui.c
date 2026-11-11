/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/ui.h"
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
static int g_atlas_h; /* g_atlas's rows */
#define UI_PICTURE_Y SNX_UI_PICTURE_Y
#define UI_ROWS      (UI_PICTURE_Y + SNX_UI_PICTURE) /* the atlas's */
_Static_assert(SNX_UI_ICONS_X + 16 * GUI_ICON_COUNT <= SNX_UI_ATLAS_W, "the icons fit beside the picture");
/* the menu's look's shape for it, GuiPicture's: 0 none, 1 square, 2 round */
#ifndef SNX_GUI_THEME_YORHA_PICTURE
#error "the YoRHa theme names no [head] picture"
#endif

void ui_lock(void) { pm_lock(&g_lock); }
void ui_unlock(void) { pm_unlock(&g_lock); }

_Static_assert(sizeof(SnxUiQuad) == 20, "a quad is the renderer's instance, with no padding");
_Static_assert(GUI_ICON_COUNT <= 256, "an icon is a quad's glyph byte");

/* v in quarters of a pixel, rounded, within lo and hi */
static int quarters(float v, int lo, int hi) {
    float q = v * UI_Q_STEPS + (v < 0 ? -0.5f : 0.5f);
    return q <= (float)lo ? lo : q >= (float)hi ? hi : (int)q;
}
/* an 8-bit channel to 5 bits, rounded, and back as the GPU widens it: the top bits again under the 5 */
static unsigned to5(unsigned c) { return (c * 31 + 127) / 255; }
static unsigned from5(unsigned c) { return c << 3 | c >> 2; }
u32 ui_rgba(u32 rgba) {
    return from5(to5(rgba & 0xFF)) | from5(to5(rgba >> 8 & 0xFF)) << 8 | from5(to5(rgba >> 16 & 0xFF)) << 16 |
           (rgba & 0xFF000000u);
}
SnxUiQuad ui_quad(float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn, int sample,
                  int ink, int glyph) {
    unsigned a = rgba >> 24, color = to5(rgba & 0xFF) | to5(rgba >> 8 & 0xFF) << 5 | to5(rgba >> 16 & 0xFF) << 10 |
                                     (a >= 128 ? 1u : 0u) << 15;
    int alpha_glyph = ink == UI_ATLAS || ink == UI_SOLID; /* no glyph: the alpha's 8 bits in its byte */
    return (SnxUiQuad){.x = (i16)quarters(x, -32768, 32767),
                       .y = (i16)quarters(y, -32768, 32767),
                       .w = (u16)quarters(w, 0, 65535),
                       .h = (u16)quarters(h, 0, 65535),
                       .u = (u16)u,
                       .v = (u16)v,
                       .uw = (u16)uw,
                       .vh = (u16)vh,
                       .bits = color << UI_Q_COLOR | (u32)(turn & 3) << UI_Q_TURN |
                               (u32)(turn & UI_BACKED ? 1 : 0) << UI_Q_BACKED | (u32)(sample & 3) << UI_Q_SAMPLE |
                               (u32)(ink & 7) << UI_Q_INK | (u32)((alpha_glyph ? (int)a : glyph) & 0xFF) << UI_Q_GLYPH};
}
SnxUiQuadView ui_view(const SnxUiQuad* q) {
    unsigned ink = ui_q_bits(q, UI_Q_INK, 3), byte = ui_q_bits(q, UI_Q_GLYPH, 8), alpha_glyph = ink <= UI_SOLID;
    unsigned a = alpha_glyph ? byte : ui_q_bits(q, 15, 1) * 255;
    return (SnxUiQuadView){.x = (float)q->x / UI_Q_STEPS,
                           .y = (float)q->y / UI_Q_STEPS,
                           .w = (float)q->w / UI_Q_STEPS,
                           .h = (float)q->h / UI_Q_STEPS,
                           .u = q->u,
                           .v = q->v,
                           .uw = q->uw,
                           .vh = q->vh,
                           .rgba = from5(ui_q_bits(q, 0, 5)) | from5(ui_q_bits(q, 5, 5)) << 8 |
                                   from5(ui_q_bits(q, 10, 5)) << 16 | a << 24,
                           .turn = (u8)ui_q_bits(q, UI_Q_TURN, 2),
                           .backed = (u8)ui_q_bits(q, UI_Q_BACKED, 1),
                           .sample = (u8)ui_q_bits(q, UI_Q_SAMPLE, 2),
                           .ink = (u8)ink,
                           .glyph = (u8)(alpha_glyph ? 0 : byte)};
}
/* the renderer's list (ui_dirty_begin), the span of it written since, and the parts kept in it (ui_append_kept) this
   frame and the one before */
#define UI_PARTS 16
typedef struct {
    const void* who;
    u32 version;
    int at, n;
} UiPart;
static struct {
    const SnxUiQuad* list;
    const SnxUiQuad* was; /* the list of the frame before */
    int first, end;
    UiPart part[2][UI_PARTS];
    int parts[2], now;
} g_dirty;
void ui_dirty_begin(const SnxUiQuad* list) {
    g_dirty.now ^= 1;
    g_dirty.parts[g_dirty.now] = 0;
    if (list != g_dirty.was) g_dirty.parts[g_dirty.now ^ 1] = 0; /* another list: nothing in it is known */
    g_dirty.list = g_dirty.was = list;
    g_dirty.first = g_dirty.end = 0;
}
void ui_dirty_end(int* first, int* end) {
    *first = g_dirty.first;
    *end = g_dirty.end;
    g_dirty.list = NULL;
}
/* l's quads first to end written */
static void dirty(const SnxUiList* l, int first, int end) {
    if (l->q != g_dirty.list || first >= end) return;
    if (g_dirty.first >= g_dirty.end || first < g_dirty.first) g_dirty.first = first;
    if (end > g_dirty.end) g_dirty.end = end;
}
static void quad(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn,
                 int sample, int ink, int glyph) {
    if (l->n >= l->max || w <= 0 || h <= 0) return;
    l->q[l->n] = ui_quad(x, y, w, h, u, v, uw, vh, rgba, turn, sample, ink, glyph);
    dirty(l, l->n, l->n + 1);
    ++l->n;
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
    quad(l, x, y, w, h, SNX_UI_WHITE_X + 1, SNX_UI_WHITE_Y + 1, 2, 2, rgba, 0, UI_SHARP, UI_SOLID,
         0);
}
void ui_shape(SnxUiList* l, float x, float y, float w, float h, int kind, const char* label, int flags, int edge,
              u32 rgba) {
    unsigned c0 = label && label[0] ? (u8)label[0] : 0, c1 = c0 && label[1] ? (u8)label[1] : 0;
    quad(l, x, y, w, h, kind, (int)(c0 | c1 << 8), 0x8000 | flags, edge < 1 ? 1 : edge, rgba, 0, UI_SHARP, UI_SHAPE,
         0);
}
float ui_text(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba) {
    float x0 = x;
    for (; *s; ++s, x += 8.0f * (float)scale) {
        u8 c = (u8)*s;
        if (c != ' ')
            quad(l, x, y, 8.0f * (float)scale, 16.0f * (float)scale, SNX_UI_FONT_X + c % 16 * 8,
                 SNX_UI_FONT_Y + c / 16 * 16, 8, 16, rgba, 0, UI_SHARP, UI_GLYPH, c);
    }
    return x - x0;
}
static u32 g_ground;
void ui_ground_set(u32 rgba) { g_ground = rgba; }
u32 ui_ground(void) { return g_ground; }
int ui_drawn_ground(const SnxUiQuad* q, SnxUiQuadView* fill) {
    SnxUiQuad ground;
    unsigned ink = ui_q_bits(q, UI_Q_INK, 3);
    if ((ink != UI_GLYPH && ink != UI_ICON) || !ui_q_bits(q, UI_Q_BACKED, 1) || !q->uw) return 0;
    ground = *q; /* its place as it is, in quarters */
    ground.u = SNX_UI_WHITE_X + 1;
    ground.v = SNX_UI_WHITE_Y + 1;
    ground.uw = ground.vh = 2;
    ground.bits = ui_quad(0, 0, 0, 0, 0, 0, 0, 0, g_ground, 0, UI_SHARP, UI_SOLID, 0).bits;
    *fill = ui_view(&ground);
    fill->rgba = g_ground; /* as the GPU takes it, from a push constant: all 8 bits */
    return 1;
}
float ui_text_backed(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba) {
    float x0 = x;
    for (; *s; ++s, x += 8.0f * (float)scale) {
        u8 c = (u8)*s;
        int n = l->n;
        quad(l, x, y, 8.0f * (float)scale, 16.0f * (float)scale, SNX_UI_FONT_X + c % 16 * 8,
             SNX_UI_FONT_Y + c / 16 * 16, 8, 16, rgba, 0, UI_SHARP, UI_GLYPH, c);
        if (l->n > n) l->q[n].bits |= 1u << UI_Q_BACKED;
    }
    return x - x0;
}
float ui_icon_backed(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba) {
    int n = l->n;
    float w = ui_icon(l, x, y, scale, icon, rgba);
    if (l->n > n) l->q[n].bits |= 1u << UI_Q_BACKED;
    return w;
}
int ui_cells_backed(SnxUiList* l, float x, float y, int scale, int n, u32 rgba) {
    int first = ui_cells(l, x, y, scale, n, rgba), k;
    if (first < 0) return -1;
    for (k = 0; k < n; ++k) l->q[first + k].bits |= 1u << UI_Q_BACKED;
    ui_cells_set(&l->q[first], n, ""); /* blank: the ground */
    return first;
}
int ui_cells(SnxUiList* l, float x, float y, int scale, int n, u32 rgba) {
    int first = l->n, k;
    if (n < 0 || n > l->max - l->n) return -1;
    for (k = 0; k < n; ++k)
        l->q[l->n++] = ui_quad(x + 8.0f * (float)(scale * k), y, 8.0f * (float)scale, 16.0f * (float)scale, 0, 0, 0, 0,
                               rgba, 0, UI_SHARP, UI_GLYPH, ' ');
    dirty(l, first, l->n);
    return first;
}
int ui_cells_set(SnxUiQuad* q, int n, const char* s) {
    int k, changed = 0;
    for (k = 0; k < n; ++k) {
        u8 c = *s ? (u8)*s++ : ' ';
        int blank = c == ' ' && !ui_q_bits(&q[k], UI_Q_BACKED, 1);
        /* a blank's texels are none: the renderer draws nothing for it; a backed one is the space, the ground */
        u16 u = blank ? 0 : (u16)(SNX_UI_FONT_X + c % 16 * 8);
        u16 v = blank ? 0 : (u16)(SNX_UI_FONT_Y + c / 16 * 16);
        if (q[k].u == u && q[k].v == v && q[k].uw == (blank ? 0 : 8)) continue;
        q[k].bits = (q[k].bits & ~(0xFFu << UI_Q_GLYPH)) | (u32)c << UI_Q_GLYPH;
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
static SnxUiRing g_ring;
void ui_ring_set(const u8* height, int n, int head, int slow_from, u32 slow, u32 version) {
    n = n < 0 ? 0 : n > SNX_UI_RING_MAX ? SNX_UI_RING_MAX : n;
    if (version != g_ring.version || n != g_ring.n) memcpy(g_ring.height, height, (size_t)n);
    g_ring.version = version;
    g_ring.n = (u16)n;
    g_ring.head = (u16)(n ? head % n : 0);
    g_ring.slow_from = (u8)slow_from;
    g_ring.slow = slow;
}
const SnxUiRing* ui_ring(void) { return &g_ring; }
void ui_ring_bars(SnxUiList* l, float x, float y, float w, float h, int n, u32 rgba) {
    int k, first = l->n;
    for (k = 0; k < n && l->n < l->max; ++k)
        l->q[l->n++] = ui_quad(x, y, w, h, SNX_UI_WHITE_X + 1, SNX_UI_WHITE_Y + 1, 2, 2, rgba, 0, UI_SHARP,
                               UI_RING, k);
    dirty(l, first, l->n);
}
SnxUiQuadView ui_drawn(const SnxUiQuad* q) {
    SnxUiQuadView d = ui_view(q);
    unsigned age, height, slot = ui_q_bits(q, UI_Q_GLYPH, 8);
    float bw;
    if (d.ink != UI_RING) return d;
    height = g_ring.n && slot < g_ring.n ? g_ring.height[slot] : 0;
    if (!height) {
        d.uw = d.vh = 0;
        return d;
    }
    /* as ui.vert places it: a bar's width over from the graph's left for each slot older than the newest */
    age = ((unsigned)g_ring.head + g_ring.n - slot) % g_ring.n;
    bw = d.w / (float)g_ring.n;
    d.x += bw * (float)(g_ring.n - 1 - age);
    d.w = bw;
    d.h = d.h * (float)height / 255.0f;
    d.y = (float)q->y / UI_Q_STEPS + (float)q->h / UI_Q_STEPS - d.h;
    if (height > g_ring.slow_from) d.rgba = g_ring.slow;
    return d;
}
void ui_append(SnxUiList* l, const SnxUiList* from) {
    int n = from->n < l->max - l->n ? from->n : l->max - l->n;
    if (n <= 0) return;
    memcpy(l->q + l->n, from->q, (size_t)n * sizeof *l->q);
    dirty(l, l->n, l->n + n);
    l->n += n;
}
void ui_span_add(SnxUiSpan* span, int first, int end) {
    if (first >= end) return;
    if (span->first >= span->end || first < span->first) span->first = first;
    if (end > span->end) span->end = end;
}
void ui_append_kept(SnxUiList* l, const SnxUiList* from, u32 version, const void* who, SnxUiSpan* changed) {
    int n = from->n < l->max - l->n ? from->n : l->max - l->n, k, was_here = 0, same = 0, first = 0, end = n;
    if (n <= 0) return;
    if (l->q == g_dirty.list) { /* where the frame before had them: the same part, place and length, and version */
        const UiPart* was = g_dirty.part[g_dirty.now ^ 1];
        for (k = 0; k < g_dirty.parts[g_dirty.now ^ 1] && !was_here; ++k)
            if (was[k].who == who && was[k].at == l->n && was[k].n == n) {
                was_here = 1;
                same = was[k].version == version;
            }
        if (g_dirty.parts[g_dirty.now] < UI_PARTS)
            g_dirty.part[g_dirty.now][g_dirty.parts[g_dirty.now]++] = (UiPart){who, version, l->n, n};
        if (was_here && changed) { /* only what changed since */
            first = changed->first < 0 ? 0 : changed->first;
            end = changed->end > n ? n : changed->end;
        }
        if (changed) *changed = (SnxUiSpan){0, 0};
    }
    if (!same && first < end) {
        memcpy(l->q + l->n + first, from->q + first, (size_t)(end - first) * sizeof *l->q);
        dirty(l, l->n + first, l->n + end);
    }
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
