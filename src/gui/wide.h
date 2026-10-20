/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The wide letters the menu draws, two columns each: GNU Unifont's 16x16 glyphs of what the menu's languages show past
   CP437 and the Latin letters (src/gui/unifont-wide.hex, compiled in by scripts/build-glyphs.py;
   src/gui/FONT-NOTICE.txt). And the columns text takes as the menu draws it, which every width, cut and slide of it
   counts in */
#ifndef SNX_GUI_WIDE_H
#define SNX_GUI_WIDE_H
#include <stddef.h>
#include <stdint.h>

extern const int gui_wide_count;
extern const uint32_t gui_wide_points[]; /* their code points, ascending */
extern const uint8_t gui_wide_font[];    /* each one's halves, left then right, 16 rows each as gui_vga_font's */
/* the columns the menu font's glyph c takes (gui_menu_glyph's): 2 for a wide letter, whose halves are c and c + 1,
   else 1 */
int gui_glyph_cols(int c);
/* the code point of the UTF-8 character at *s, *s moved past it; U+FFFD and a byte for one that is not UTF-8, and 0
   at the end, *s left there */
uint32_t gui_utf8_step(const char** s);
/* the columns text takes */
int gui_text_cols(const char* text);
/* the bytes of text's first characters that fit in cols columns: a wide one that would cross the last is left out */
size_t gui_cols_fit(const char* text, int cols);
/* text past its first characters that take cols columns or more, or its end */
const char* gui_cols_skip(const char* text, int cols);
#endif
