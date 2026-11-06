/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The wide letters the menu draws, two columns each: GNU Unifont's 16x16 glyphs of what the menu's languages show past
   CP437, the Latin letters and the titles' blocks (src/gui/unifont-wide.hex, compiled in by scripts/build-glyphs.py;
   src/gui/FONT-NOTICE.txt), and the titles' own (titles.h). And the columns text takes as the menu draws it, which
   every width, cut and slide of it counts in */
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
/* the menu font's glyphs, gui_menu_glyph's numbers from 0 to one under this, a wide letter's or an icon's halves two */
int gui_glyph_count(void);
/* the 16 rows of glyph c, as gui_vga_font's: of a wide letter or an icon, the half c is */
const uint8_t* gui_glyph_rows(int c);
/* the wide letters, the languages' then the titles' 16x16 ones, and glyph c's place among them (its left half's): -1
   for a glyph that is none, CP437's, a Latin letter's, an icon's or a title's 8x16 one */
int gui_wide_letters(void);
int gui_wide_letter(int c);
/* the code point of the wide letter at place */
uint32_t gui_wide_letter_point(int place);
/* the code point of the UTF-8 character at *s, *s moved past it; U+FFFD and a byte for one that is not UTF-8, and 0
   at the end, *s left there */
uint32_t gui_utf8_step(const char** s);
/* the columns text takes */
int gui_text_cols(const char* text);
/* the bytes of text's first characters that fit in cols columns: a wide one that would cross the last is left out */
size_t gui_cols_fit(const char* text, int cols);
/* text past its first characters that take cols columns or more, or its end */
const char* gui_cols_skip(const char* text, int cols);
/* text in cols columns at most, into out (size bytes): whole when it fits, else its first characters and "..." after
   them, a wide one that would cross into the dots and the spaces before them left out; with under 4 columns, its
   first characters alone */
void gui_text_fit(const char* text, int cols, char* out, size_t size);
/* path in cols columns at most, into out (size bytes): whole when it fits, else its start and its end around "...",
   cut at its separators where they allow: the end first, as many of the last folders as leave room for the root and
   its first folder ("/home/.../scarletnx/recompiled"); with under 5 columns, its start */
void gui_path_fit(const char* path, int cols, char* out, size_t size);
#endif
