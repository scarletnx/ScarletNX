/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The letters a game's title is written in that the menu draws whatever its languages show: GNU Unifont's glyphs of
   every code point of scripts/build-glyphs.py's TITLE_RANGES, the CJK and Hangul blocks (src/gui/unifont-titles.hex,
   compiled in by that script; src/gui/FONT-NOTICE.txt). Most are 16x16, two columns wide as wide.h's; the halfwidth
   forms are 8x16 */
#ifndef SNX_GUI_TITLES_H
#define SNX_GUI_TITLES_H
#include <stdint.h>

/* code points first to last, each of one width: 16x16 (wide) or 8x16, the first the at-th glyph of its width */
typedef struct {
    uint32_t first, last;
    int wide, at;
} GuiTitleRun;
extern const int gui_title_run_count;
extern const GuiTitleRun gui_title_runs[]; /* ascending, every code point of TITLE_RANGES in one */
extern const int gui_title_narrow_count, gui_title_wide_count;
extern const uint8_t gui_title_narrow_font[]; /* the 8x16 ones, 16 rows each as gui_vga_font's */
extern const uint8_t gui_title_wide_font[];   /* the 16x16 ones' halves, left then right, 16 rows each */
#endif
