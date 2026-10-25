/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Latin letters the menu draws past its VGA font's CP437: GNU Unifont's 8x16 glyphs of U+00A0 to U+024F that CP437
   lacks (src/gui/unifont-latin.hex, compiled in by scripts/build-glyphs.py; src/gui/FONT-NOTICE.txt) */
#ifndef SNX_GUI_LATIN_H
#define SNX_GUI_LATIN_H
#include <stdint.h>

extern const int gui_latin_count;
extern const uint16_t gui_latin_points[]; /* their code points, ascending */
extern const uint8_t gui_latin_font[];    /* 16 rows of each, as gui_vga_font's */
/* the menu font's glyph of a code point: CP437's (0 to 255, as gui_glyph) when it has one, else 256 and the Latin
   glyph's index, else 256 + gui_latin_count and twice the wide letter's (wide.h: its left half, the right one after),
   else for an icon's (icons.h) 256 + gui_latin_count + 2 * gui_wide_count and twice its place in the list, two
   columns as a wide letter's, which the menu draws as one (lunasvg's), else '?' */
int gui_menu_glyph(uint32_t codepoint);
#endif
