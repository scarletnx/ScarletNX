/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/latin.h"
#include "common/utilities/font.h"
#include "gui/icons.h"
#include "gui/wide.h"

int gui_menu_glyph(uint32_t codepoint) {
    int low = 0, high = gui_latin_count - 1;
    unsigned char c = gui_glyph(codepoint);
    if (c != '?' || codepoint == '?') return c;
    while (low <= high) {
        int middle = (low + high) / 2;
        if (gui_latin_points[middle] == codepoint) return 256 + middle;
        if (gui_latin_points[middle] < codepoint)
            low = middle + 1;
        else
            high = middle - 1;
    }
    for (low = 0, high = gui_wide_count - 1; low <= high;) {
        int middle = (low + high) / 2;
        if (gui_wide_points[middle] == codepoint) return 256 + gui_latin_count + 2 * middle;
        if (gui_wide_points[middle] < codepoint)
            low = middle + 1;
        else
            high = middle - 1;
    }
    if (codepoint >= GUI_ICON_FIRST && codepoint < GUI_ICON_END)
        return 256 + gui_latin_count + 2 * gui_wide_count + 2 * (int)(codepoint - GUI_ICON_FIRST);
    return '?';
}
