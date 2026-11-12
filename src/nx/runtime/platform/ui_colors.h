/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen layer's colours (ui.h), one set for all that draws on it: the menu's, from the YoRHa theme's
   theme.ini (gui_theme_colors.h), opaque, as nothing in the layer is translucent */
#ifndef SNX_UI_COLORS_H
#define SNX_UI_COLORS_H
#include "nx/runtime/platform/ui.h"
#include "gui_theme_colors.h"

/* a theme's 0xRRGGBB as a quad's colour: R G B A from the low byte, opaque */
#define UI_RGB(rgb) (0xFF000000u | ((u32)(rgb) & 0xFFu) << 16 | ((u32)(rgb) & 0xFF00u) | ((u32)(rgb) >> 16 & 0xFFu))
/* the ink: the bands' fill, the letters on the page, and the ground of the letters over the game (UI_BACKED) */
#define UI_INK      UI_RGB(SNX_GUI_THEME_YORHA_INK)
/* the page: the menu's fill, and the letters on the ink */
#define UI_PAGE     UI_RGB(SNX_GUI_THEME_YORHA_PAGE)
#define UI_SOFT     UI_RGB(SNX_GUI_THEME_YORHA_SOFT)    /* what matters less: a hint, a guide, a part at rest */
#define UI_FAILURE  UI_RGB(SNX_GUI_THEME_YORHA_FAILURE) /* what went wrong or is too slow, and a part held */
#define UI_WARNING  UI_RGB(SNX_GUI_THEME_YORHA_WARNING) /* a choice to take with care */
/* a panel on the page, a little darker: the soft colour at 0x40 over it */
#define UI_PANEL    UI_RGB(ui_over(SNX_GUI_THEME_YORHA_SOFT, SNX_GUI_THEME_YORHA_PAGE, 0x40u))
#define UI_WHITE    0xFFFFFFFFu /* the on-screen controls, whatever the theme */
/* the control picked in edit mode, which has to show over any picture and beside the white controls */
#define UI_PICKED   0xFF00C8FFu
#endif
