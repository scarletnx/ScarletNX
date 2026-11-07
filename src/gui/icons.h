/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The icons drawn beside a menu item's label, the launcher's and the game's menu's, before the stats box's lines and
   before the keys of F1's box: each a letter of the menu's font two columns wide at a code point of Unicode's private
   use area, U+E000 on in this order, named by the Lucide icon (lucide.dev) whose symbol it shows. Their 16x16 glyphs
   are src/gui/icons.hex, a line each in this order, compiled in by cmake/gui_icons.cmake, which refuses one missing,
   blank or out of its place, naming it */
#ifndef SNX_GUI_ICONS_H
#define SNX_GUI_ICONS_H
#include <stdint.h>

enum {
    GUI_ICON_NONE, /* no icon: an item's, a row's */
    GUI_ICON_GAMEPAD_2 = 0xE000,
    GUI_ICON_SQUARE_TERMINAL,
    GUI_ICON_BOX,
    GUI_ICON_PACKAGE,
    GUI_ICON_GAMEPAD,
    GUI_ICON_KEY_ROUND,
    GUI_ICON_SETTINGS,
    GUI_ICON_SHARE_2,
    GUI_ICON_FOLDER_OPEN,
    GUI_ICON_LOG_OUT,
    GUI_ICON_APP_WINDOW,
    GUI_ICON_ROCKET,
    GUI_ICON_JOYSTICK,
    GUI_ICON_MONITOR,
    GUI_ICON_IMAGE,
    GUI_ICON_GAUGE,
    GUI_ICON_POINTER,
    GUI_ICON_WIFI,
    GUI_ICON_BUG,
    GUI_ICON_USER,
    GUI_ICON_PAINTBRUSH,
    GUI_ICON_FOLDER,
    GUI_ICON_ERASER,
    GUI_ICON_FOLDER_INPUT,
    GUI_ICON_ROTATE_CCW,
    GUI_ICON_CPU,
    GUI_ICON_LAYOUT_GRID,
    GUI_ICON_GAMEPAD_DIRECTIONAL,
    GUI_ICON_FOLDER_PLUS,
    GUI_ICON_PACKAGE_PLUS,
    GUI_ICON_FILE_PLUS,
    GUI_ICON_SAVE,
    GUI_ICON_PLAY,
    GUI_ICON_HAMMER,
    GUI_ICON_PALETTE,
    GUI_ICON_HARD_DRIVE_DOWNLOAD,
    GUI_ICON_ACTIVITY,
    GUI_ICON_INFO,
    GUI_ICON_GPU,
    GUI_ICON_MEMORY_STICK,
    GUI_ICON_LAYERS,
    GUI_ICON_MOVE,
    GUI_ICON_SLIDERS_HORIZONTAL,
    GUI_ICON_REFRESH_CW,
    GUI_ICON_ARROW_LEFT,
    GUI_ICON_X,
    GUI_ICON_CHECK,
    GUI_ICON_TRASH,
    GUI_ICON_PENCIL,
    GUI_ICON_CLIPBOARD_PASTE,
    GUI_ICON_PANEL_RIGHT,
    GUI_ICON_EYE,
    GUI_ICON_KEYBOARD,
    GUI_ICON_END
};
#define GUI_ICON_FIRST GUI_ICON_GAMEPAD_2
#define GUI_ICON_COUNT (GUI_ICON_END - GUI_ICON_FIRST)
/* each icon's halves, left then right, 16 rows each as gui_vga_font's: GUI_ICON_COUNT of them */
extern const uint8_t gui_icon_font[];
/* icon as UTF-8 into out, "" for GUI_ICON_NONE; out. A value past the list is drawn as the font draws a code point it
   has no glyph for, a '?' */
static inline char* gui_icon_utf8(unsigned icon, char out[4]) {
    out[0] = 0;
    if (icon == GUI_ICON_NONE || icon > 0xFFFF) return out;
    out[0] = (char)(0xE0 | icon >> 12);
    out[1] = (char)(0x80 | (icon >> 6 & 0x3F));
    out[2] = (char)(0x80 | (icon & 0x3F));
    out[3] = 0;
    return out;
}
#endif
