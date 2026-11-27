/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The quads of a layer drawn over a game's picture (the on-screen controls, vpad.h; the Switch game's menu and stats,
   nx/runtime/platform/ui.h): made, packed and listed here, drawn by whoever has the picture: the Switch renderer's
   pipeline (nx/gpu/vk/ui.c) reads them as they are */
#ifndef SNX_UI_QUADS_H
#define SNX_UI_QUADS_H
#include "common/cpu/aarch64/cpu.h"

/* A quad of the layer: 20 bytes with no padding, which the renderer reads as they are (nx/gpu/vk/ui.c), an instance
   each. Its place in quarters of the picture's pixels, x and y signed (a box can go past the picture's left or top): 8191
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
/* in ui_quad's turn, for a letter or an icon (UI_Q_BACKED): its clear pixels in the ground's colour (ui_ground),
   where they are left out otherwise. With an opaque colour and ground, it is an opaque cell, which the renderer draws
   without blending */
#define UI_BACKED       4
typedef struct {
    SnxUiQuad* q;
    int n, max; /* quads past max are dropped */
} SnxUiList;
/* a UI_SHAPE quad of kind (UI_SHAPE_*), its label (NULL or "" for none) and flags, its edge's width in pixels */
void ui_shape(SnxUiList* l, float x, float y, float w, float h, int kind, const char* label, int flags, int edge,
              u32 rgba);
/* a quad of l from what ui_quad takes, dropped when l is full or it has no size */
void ui_quad_add(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn,
                 int sample, int ink, int glyph);
/* l's quads first to end were written: marked when l is the renderer's list (ui_dirty_begin). For a writer that puts
   its quads in by itself (ui_ring_bars) */
void ui_written(const SnxUiList* l, int first, int end);

/* top, a theme's 0xRRGGBB, at a (0 to 255) over under, as a blend would have made it: the opaque colour that stands
   for it, nothing in the layer being translucent */
static inline u32 ui_over(u32 top, u32 under, unsigned a) {
    u32 rgb = 0;
    int k;
    for (k = 0; k < 24; k += 8) rgb |= ((top >> k & 0xFFu) * a + (under >> k & 0xFFu) * (255 - a) + 127) / 255 << k;
    return rgb;
}
/* a plain rectangle */
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba);
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
/* A span of a list, first to end, drawn at alpha (0 to 254, of 255) over what is under it: the one thing in the layer
   that is translucent, a notification's line fading in or out (overlay.c), and only while it fades. Its quads are
   fills, bars and backed letters and icons, which the renderer otherwise draws opaque with no blending (gpu/vk/ui.c),
   unchanged from frame to frame while alpha moves. Up to SNX_UI_FADES spans a list, each its own alpha, in the list's
   order and apart; each list starts with none (ui_dirty_begin). An empty span, or alpha 255, is none, and so is one
   past SNX_UI_FADES, which draws opaque: the notification gives no more (overlay.c's OVERLAY_TOASTS). Under ui_lock */
#define SNX_UI_FADES 4
typedef struct {
    int first, end;
    unsigned alpha;
} SnxUiFade;
void ui_fade_set(int first, int end, unsigned alpha);
/* those spans into fades, the rest of it zeroed: how many */
int ui_fades(SnxUiFade fades[SNX_UI_FADES]);

/* where the font's letters (16 a row) and the white block for fills are in the layer's atlas (nx/runtime/platform/ui.h
   makes it), which a quad's texels name */
#define SNX_UI_FONT_X  0
#define SNX_UI_FONT_Y  0
#define SNX_UI_WHITE_X 128
#define SNX_UI_WHITE_Y 0
/* the picture of the user playing (nx/runtime/platform/ui.h's ui_picture), SNX_UI_PICTURE texels square from row
   SNX_UI_PICTURE_Y, and the icons beside it */
#define SNX_UI_PICTURE   96
#define SNX_UI_PICTURE_Y 256

/* the controls, the stats, the showcase and the Switch's menu change on the main thread while the renderer draws them on
   its own: all under this */
void ui_lock(void);
void ui_unlock(void);
#endif
