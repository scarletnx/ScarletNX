/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the renderer draws over the game's picture: the on-screen controls (vpad.c) and the game's menu (overlay.c),
   as quads of one atlas blended over the frame. The atlas is dist/controls/controls.png with the menu's 8x16 font
   and a white block for plain fills in the places its header keeps for them, and under it the picture of the user
   playing, which the menu's head shows by their nickname */
#ifndef SNX_UI_H
#define SNX_UI_H
#include "common/cpu/aarch64/cpu.h"

/* 32 bytes with no padding, which the renderer reads as they are (gpu/vk/ui.c): one instance of its quad each */
typedef struct {
    float x, y, w, h; /* where, in the pixels of the picture drawn on */
    u16 u, v, uw, vh; /* the atlas texels drawn there; uw 0: nothing drawn (ui_cells' blank cell) */
    u32 rgba;         /* each texel times this colour, R G B A from the low byte */
    u8 turn;          /* the texels turned this many quarters clockwise, and UI_BACKED */
    u8 sample;        /* how: UI_BLEND, UI_SHARP or UI_IMAGE */
    u8 ink;           /* what the renderer draws it with: one of UI_ATLAS, UI_SOLID, UI_GLYPH, UI_ICON */
    u8 glyph;         /* UI_GLYPH's letter, UI_ICON's icon from GUI_ICON_FIRST, UI_RING's slot */
} SnxUiQuad;
/* the atlas blended, the atlas's nearest texel (the font at whole steps), or ui_image's picture blended, its texels
   in u, v, uw and vh */
enum { UI_BLEND, UI_SHARP, UI_IMAGE };
/* What the GPU reads for a quad's texels, which are in the atlas all the same (the SDL renderer's fatal panel draws
   them from there): the atlas's or the picture's texels; none, the colour alone (a fill: the atlas's white block);
   or a letter of the 8x16 font or an icon of the menus, from their bits, a bit a pixel, with no atlas read. UI_RING
   is a bar of the ring (ui_ring), drawn as ui_drawn gives it */
enum { UI_ATLAS, UI_SOLID, UI_GLYPH, UI_ICON, UI_RING };
/* in turn, for a letter or an icon: its clear pixels in the ground's colour (ui_ground_set), where they are left out
   otherwise. With an opaque colour and ground, it is an opaque cell, which the renderer draws without blending */
#define UI_BACKED 4
/* the ground of every backed letter and icon, R G B A from the low byte: one for all, set by their maker (stats.c)
   each frame it draws them. Under ui_lock */
void ui_ground_set(u32 rgba);
u32 ui_ground(void);
typedef struct {
    SnxUiQuad* q;
    int n, max; /* quads past max are dropped */
} SnxUiList;
/* The ring of bars, one at most (stats.c's graph): n slots in a row that scrolls left, the newest's at the right, each
   a byte, the bar's height in 255ths of the graph's. A UI_RING quad is a slot's bar, laid out once: x the row's right
   edge, w a bar's width, y and h the graph's top and height, glyph its slot. A new bar writes its byte and moves the
   head, and the quads stay as they are: the renderer sends the byte and the GPU draws the bar (ui.vert). Under
   ui_lock */
#define SNX_UI_RING_MAX 256
typedef struct {
    u8 height[SNX_UI_RING_MAX];
    u16 head, n;  /* the newest's slot and the slots; n 0 for no ring */
    u8 slow_from; /* a height past it draws in the slow colour (a frame past 30 FPS's) */
    u32 slow;     /* that colour, R G B A from the low byte */
} SnxUiRing;
void ui_ring_set(const u8* height, int n, int head, int slow_from, u32 slow);
const SnxUiRing* ui_ring(void);
/* q as it is drawn: a UI_RING quad as its slot's bar, a width left for each slot newer than it, its height's share of
   h up from the graph's foot, in the slow colour past slow_from, and nothing (uw 0) at height 0; any other as it is */
SnxUiQuad ui_drawn(const SnxUiQuad* q);
/* for a backed letter or icon (UI_BACKED), the fill of its ground under it, as one draws it before the letter where it
   can't draw both at once (the SDL renderer, the tests' CPU blend): 1 with it in *fill; 0 for any other quad */
int ui_drawn_ground(const SnxUiQuad* q, SnxUiQuad* fill);

/* a sprite of controls_atlas.h at alpha (0 to 255) */
void ui_sprite(SnxUiList* l, int sprite, float x, float y, float w, float h, u32 alpha, int turn);
/* the atlas's texels u, v, uw x vh at x, y, w x h, blended, times rgba: a skin's image (vpad_skin.h) */
void ui_texels(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba);
/* a plain rectangle */
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba);
/* q, a quad kept from frame to frame, made that rectangle; one with no area draws nothing, and keeps its quad */
void ui_fill_set(SnxUiQuad* q, float x, float y, float w, float h, u32 rgba);
/* the picture of the user playing (common/utilities/users.h), side pixels square at x, y: drawn SNX_UI_PICTURE texels
   square in the YoRHa theme's [head] picture shape, square or round (the rest transparent); 0 when the theme shows
   none, which draws nothing */
#define SNX_UI_PICTURE 96
int ui_picture(SnxUiList* l, float x, float y, float side);
/* ASCII text, 0xFE the font's square, in 8x16 glyphs at a whole scale; returns the width drawn */
float ui_text(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba);
/* the same, backed (UI_BACKED): a quad for each letter, a space too, which shows the ground */
float ui_text_backed(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba);
/* A field of n letter cells from x, y in 8x16 glyphs at a whole scale, all blank: a text that changes in a box kept
   from frame to frame (stats.c). ui_cells_set writes its letters later and moves nothing, so the field is laid out
   once. A blank cell (a space, or past the text) keeps its quad, which draws nothing: the field takes the same quads
   and the same time to draw whatever it shows. The first cell's index in l, -1 when l has no room for all n */
int ui_cells(SnxUiList* l, float x, float y, int scale, int n, u32 rgba);
/* the same, backed (UI_BACKED): a blank cell shows the ground, so the field is a band of it whatever it says */
int ui_cells_backed(SnxUiList* l, float x, float y, int scale, int n, u32 rgba);
/* the letters of s into the n cells from q, blank after its end and the end cut off past n: the cells changed */
int ui_cells_set(SnxUiQuad* q, int n, const char* s);
/* letters for the cells without printf: s, the decimal digits of v, and v / 10 with its tenth (12.3), each at at; the
   letter after them, where a NUL goes */
char* ui_put(char* at, const char* s);
char* ui_put_u(char* at, u64 v);
char* ui_put_tenths(char* at, u64 tenths);
/* the quads of from after l's, as many as fit */
void ui_append(SnxUiList* l, const SnxUiList* from);
/* an icon of the menus' (gui/icons.h), two of the font's cells at a whole scale, as the launcher draws it beside a
   label; returns the width drawn, 0 for GUI_ICON_NONE. Its texels are in the atlas beside the picture, on its rows:
   icon i's 16x16 from x SNX_UI_ICONS_X + 16 * i */
#define SNX_UI_ICONS_X SNX_UI_PICTURE
float ui_icon(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba);
float ui_icon_backed(SnxUiList* l, float x, float y, int scale, unsigned icon, u32 rgba); /* backed (UI_BACKED) */
/* One picture apart from the atlas, SNX_UI_IMAGE_W x SNX_UI_IMAGE_H: the menu's save state thumbnail. ui_image draws it
   at x, y, w x h: 0 while there is none, which draws nothing */
#define SNX_UI_IMAGE_W 320
#define SNX_UI_IMAGE_H 180
int ui_image(SnxUiList* l, float x, float y, float w, float h);
/* the picture's texels, R G B A bytes, opaque, copied (NULL: none), under ui_lock */
void ui_image_set(const u8* rgba);
/* the picture for the renderer, under ui_lock: its texels, NULL for none, and version counting the pictures given */
const u8* ui_image_get(u32* version);

/* The atlas, each texel the bytes R G B A (a little-endian u32) with alpha premultiplied, made from the sprites'
   image when the window opens (sdl3.c decodes it to the same bytes, SDL's RGBA32), the users read by then, and freed
   by ui_atlas_done once the renderer has it, both under ui_lock. It is the image's width and SNX_UI_PICTURE rows
   taller. version counts the images given, 0 for none yet */
void ui_atlas_set(const u32* sprites, int w, int h);
/* ui_atlas_set in two: the atlas made (NULL for an image of another size), its rows into *rows; then put in under
   ui_lock, which the caller holds (with what is drawn from it changing at once) */
u32* ui_atlas_make(const u32* sprites, int w, int h, int* rows);
void ui_atlas_put(u32* atlas, int rows);
/* A skin's images (vpad_skin.h), rows of the atlas's width under the picture's: from row ui_skin_y, at most
   ui_skin_room rows, as the atlas is at most 4096 rows (Vulkan's least maxImageDimension2D). ui_skin_set takes texels
   (malloc'd, R G B A bytes, straight alpha), NULL for none, for the next atlas made; on the main thread, which makes
   the atlas (the renderer reads only what ui_atlas_put put in) */
int ui_skin_y(void);
int ui_skin_room(void);
void ui_skin_set(u32* texels, int rows);
const u32* ui_atlas(int* w, int* h, u32* version);
void ui_atlas_done(void);

/* the controls and the menu change on the main thread while the renderer draws them on its own: both under this */
void ui_lock(void);
void ui_unlock(void);
#endif
