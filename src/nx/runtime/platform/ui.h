/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the renderer draws over the game's picture: the on-screen controls (vpad.c) and the game's menu (overlay.c),
   as quads over the frame: shapes, letters, icons and fills drawn from their own bits and colours, and the user's
   picture and a save state's thumbnail from their images */
#ifndef SNX_UI_H
#define SNX_UI_H
#include "common/cpu/aarch64/cpu.h"

/* A quad of the layer: 20 bytes with no padding, which the renderer reads as they are (gpu/vk/ui.c), an instance each.
   Its place in quarters of the picture's pixels, x and y signed (a box can go past the picture's left or top): 8191
   pixels at most either way. The atlas texels drawn there; uw 0 draws nothing (ui_cells' blank cell). Then one word
   of bits from bit 0 (UI_Q_*): the colour R5 G5 B5 A1, the texels' quarter turns clockwise, UI_BACKED, the sample,
   the ink and the glyph. A quad that draws no glyph (UI_ATLAS, UI_SOLID) has its colour's alpha in the glyph's byte,
   all 8 bits of it, where a blend needs them (the controls' alpha, the menu's panels); a letter, an icon or a bar is
   its colour or clear, A1. ui_quad makes one, ui_view reads it back */
typedef struct {
    i16 x, y;
    u16 w, h;
    u16 u, v, uw, vh;
    u32 bits;
} SnxUiQuad;
#define UI_Q_COLOR  0  /* 16 bits: R5 G5 B5 A1 */
#define UI_Q_TURN   16 /* 2 bits */
#define UI_Q_BACKED 18 /* 1 bit */
#define UI_Q_SAMPLE 19 /* 2 bits */
#define UI_Q_INK    21 /* 3 bits */
#define UI_Q_GLYPH  24 /* 8 bits: the glyph, or the alpha */
#define UI_Q_STEPS  4  /* a pixel's quarters */
/* what a quad holds, in the producers' terms: the place in pixels, the colour R G B A from the low byte (each 5 bits
   widened back to 8, the alpha the glyph byte's or A1's), turn with UI_BACKED in it as ui_quad takes it */
typedef struct {
    float x, y, w, h;
    u16 u, v, uw, vh;
    u32 rgba;
    u8 turn, backed, sample, ink, glyph;
} SnxUiQuadView;
/* a quad of these: turn the quarter turns, UI_BACKED among them; glyph for UI_GLYPH, UI_ICON and UI_RING */
SnxUiQuad ui_quad(float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn, int sample,
                  int ink, int glyph);
SnxUiQuadView ui_view(const SnxUiQuad* q);
/* the field of q's bits from bit at, n bits */
static inline unsigned ui_q_bits(const SnxUiQuad* q, int at, int n) { return q->bits >> at & ((1u << n) - 1); }
/* rgba as a quad keeps it: R, G and B to 5 bits and back; the alpha kept, which a letter's opaque one is */
u32 ui_rgba(u32 rgba);
/* the atlas blended, the atlas's nearest texel (the font at whole steps), or ui_image's picture blended, its texels
   in u, v, uw and vh */
enum { UI_BLEND, UI_SHARP, UI_IMAGE };
/* What the GPU reads for a quad's texels, which are in the atlas all the same (the SDL renderer's fatal panel draws
   them from there): the atlas's or the picture's texels; none, the colour alone (a fill: the atlas's white block);
   or a letter of the 8x16 font or an icon of the menus, from their bits, a bit a pixel, with no atlas read. UI_RING
   is a bar of the ring (ui_ring), drawn as ui_drawn gives it */
enum { UI_ATLAS, UI_SOLID, UI_GLYPH, UI_ICON, UI_RING, UI_SHAPE };
/* UI_SHAPE: a simple shape the fragment shader draws from its place alone, with no texels, hard (each pixel's middle in
   it or not) and in its colour: in u its kind, in v its label (up to two of the 8x16 font's letters, the first in the
   low byte, 0 for none), in uw UI_SHAPE_* flags with bit 15 set, in vh its edge's width in pixels. Unfilled, the edge
   and the label are drawn; filled, the whole shape but the label, which the picture shows through. The label is the
   font's letters at the greatest whole scale that fits half the shape's height and 0.6 of its width, in its middle;
   none on a shape too small for them at their own size (0.8 of its height and width).
   A circle fits the shorter side; a round box has corners a quarter of the shorter side; a cross (the D-pad) has arms
   a third of the shorter side wide; a box is the quad */
enum { UI_SHAPE_CIRCLE, UI_SHAPE_ROUND, UI_SHAPE_CROSS, UI_SHAPE_BOX };
#define UI_SHAPE_FILLED 1 /* the shape filled, its label cut out */
#define UI_SHAPE_BARE   2 /* no edge: the label alone, a mark on a shape drawn beside it */
/* in ui_quad's turn, for a letter or an icon (UI_Q_BACKED): its clear pixels in the ground's colour (ui_ground_set),
   where they are left out otherwise. With an opaque colour and ground, it is an opaque cell, which the renderer draws
   without blending */
#define UI_BACKED 4
/* the ground of every backed letter and icon, R G B A from the low byte: one for all, set by their maker (stats.c)
   each frame it draws them. Under ui_lock */
void ui_ground_set(u32 rgba);
u32 ui_ground(void);
typedef struct {
    SnxUiQuad* q;
    int n, max; /* quads past max are dropped */
} SnxUiList;
/* a UI_SHAPE quad of kind (UI_SHAPE_*), its label (NULL or "" for none) and flags, its edge's width in pixels */
void ui_shape(SnxUiList* l, float x, float y, float w, float h, int kind, const char* label, int flags, int edge,
              u32 rgba);
/* The ring of bars, one at most (stats.c's graph): n slots in a row that scrolls left, the newest's at the right, each
   a byte, the bar's height in 255ths of the graph's. A UI_RING quad is a slot's bar, laid out once over the whole
   graph (x, y, w, h), glyph its slot: the bar is w / n wide. A new bar writes its byte and moves the head, and the
   quads stay as they are: the renderer sends the byte and the GPU draws the bar (ui.vert). Under ui_lock */
#define SNX_UI_RING_MAX 256
typedef struct {
    u8 height[SNX_UI_RING_MAX];
    u16 head, n;  /* the newest's slot and the slots; n 0 for no ring */
    u8 slow_from; /* a height past it draws in the slow colour (a frame past 30 FPS's) */
    u32 slow;     /* that colour, R G B A from the low byte */
    u32 version;  /* the ring's maker's count of the changes to its bytes: the same, they are not copied again */
} SnxUiRing;
void ui_ring_set(const u8* height, int n, int head, int slow_from, u32 slow, u32 version);
/* the ring's n bars' quads over the graph at x, y, w x h, a slot each from 0, in rgba below slow_from */
void ui_ring_bars(SnxUiList* l, float x, float y, float w, float h, int n, u32 rgba);
const SnxUiRing* ui_ring(void);
/* q as it is drawn (ui_view's): a UI_RING quad as its slot's bar, w / n wide, a bar's width left of the graph's right
   edge for each slot newer than it, its height's share of h up from the graph's foot, in the slow colour past
   slow_from, and nothing (uw 0) at height 0; any other as it is */
SnxUiQuadView ui_drawn(const SnxUiQuad* q);
/* for a backed letter or icon (UI_BACKED), the fill of its ground under it, as one draws it before the letter where it
   can't draw both at once (the SDL renderer, the tests' CPU blend): 1 with it in *fill; 0 for any other quad */
int ui_drawn_ground(const SnxUiQuad* q, SnxUiQuadView* fill);

/* top, a theme's 0xRRGGBB, at a (0 to 255) over under, as a blend would have made it: the opaque colour that stands
   for it, nothing in the layer being translucent */
static inline u32 ui_over(u32 top, u32 under, unsigned a) {
    u32 rgb = 0;
    int k;
    for (k = 0; k < 24; k += 8)
        rgb |= ((top >> k & 0xFFu) * a + (under >> k & 0xFFu) * (255 - a) + 127) / 255 << k;
    return rgb;
}
/* a plain rectangle */
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba);
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
/* A part's quads kept from frame to frame (stats.c's box, vpad.c's controls; who any address of its own), appended as
   ui_append does, but not copied into the renderer's list (ui_dirty_begin's) where its last frame had them, the same
   version (which the part counts up at each change), at the same place: as each frame's list is made from its start,
   they are still there. There, of a new version only the quads changed (NULL: all) are copied, which the part marks
   from its last append to the renderer's list, and which this then empties. Into any other list they are copied */
typedef struct {
    int first, end; /* the part's quads changed, an empty span for none */
} SnxUiSpan;
/* first to end into span, which grows to take it */
void ui_span_add(SnxUiSpan* span, int first, int end);
void ui_append_kept(SnxUiList* l, const SnxUiList* from, u32 version, const void* who, SnxUiSpan* changed);
/* The quads written into one list (the renderer's) from ui_dirty_begin to ui_dirty_end: its first and one past its
   last, an empty span (first >= end) for none, so that the renderer looks at no other. Every ui_ function that writes
   a quad marks it; under ui_lock */
void ui_dirty_begin(const SnxUiQuad* list);
void ui_dirty_end(int* first, int* end);
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

/* The atlas, each texel the bytes R G B A (a little-endian u32) with alpha premultiplied, made when the window opens,
   the users read by then, and freed by ui_atlas_done once the renderer has it, both under ui_lock: the 8x16 font's 256
   letters (16 a row from SNX_UI_FONT_X, SNX_UI_FONT_Y), a white block for fills (SNX_UI_WHITE_X, SNX_UI_WHITE_Y, 4 x 4),
   and from row SNX_UI_PICTURE_Y the picture of the user playing and the icons. The GPU draws the letters, the icons and
   the fills from their bits and colours (ui.frag); the SDL renderer's fatal panel and the tests read the texels.
   version counts the atlases made, 0 for none yet */
#define SNX_UI_ATLAS_W    1024
#define SNX_UI_FONT_X     0
#define SNX_UI_FONT_Y     0
#define SNX_UI_WHITE_X    128
#define SNX_UI_WHITE_Y    0
#define SNX_UI_PICTURE_Y  256
void ui_atlas_build(void);
/* ui_atlas_build in two: the atlas made (NULL without memory), its rows into *rows; then put in under ui_lock, which the
   caller holds (with what is drawn from it changing at once) */
u32* ui_atlas_make(int* rows);
void ui_atlas_put(u32* atlas, int rows);
const u32* ui_atlas(int* w, int* h, u32* version);
void ui_atlas_done(void);

/* the controls and the menu change on the main thread while the renderer draws them on its own: both under this */
void ui_lock(void);
void ui_unlock(void);
#endif
