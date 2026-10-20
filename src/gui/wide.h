/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The wide letters the menu draws, two columns each: GNU Unifont's 16x16 glyphs of what the menu's languages show past
   CP437 and the Latin letters (src/gui/unifont-wide.hex, compiled in by scripts/build-glyphs.py;
   src/gui/FONT-NOTICE.txt) */
#ifndef SNX_GUI_WIDE_H
#define SNX_GUI_WIDE_H
#include <stdint.h>

extern const int gui_wide_count;
extern const uint32_t gui_wide_points[]; /* their code points, ascending */
extern const uint8_t gui_wide_font[];    /* each one's halves, left then right, 16 rows each as gui_vga_font's */
#endif
