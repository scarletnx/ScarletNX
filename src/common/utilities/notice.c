/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/notice.h"
#include "common/utilities/font.h"
#include "gui_theme_colors.h"
#include <string.h>

void snx_notice_show(SnxNotice* n, const char* text, uint64_t now_ns) {
    size_t len = strlen(text);
    if (len > SNX_NOTICE_COLS) len = SNX_NOTICE_COLS;
    memcpy(n->text, text, len);
    n->text[len] = 0;
    n->until_ns = now_ns + (uint64_t)SNX_NOTICE_MS * 1000000u;
}

int snx_notice_shown(const SnxNotice* n, uint64_t now_ns) { return n->text[0] && now_ns < n->until_ns; }

int snx_menu_scale(int sw, int sh, int percent) {
    int w = SNX_MENU_COLS * 8, h = SNX_MENU_ROWS * 16;
    int s = sw / 2 / w < sh / 2 / h ? sw / 2 / w : sh / 2 / h;
    int dense = 2 * percent / 100;
    if (dense > s) s = dense;
    if (s > sw * 3 / 4 / w) s = sw * 3 / 4 / w;
    if (s > sh * 3 / 4 / h) s = sh * 3 / 4 / h;
    return s < 1 ? 1 : s;
}

int snx_notice_scale(int sw, int sh, int percent) {
    int s = snx_menu_scale(sw, sh, percent);
    return s > 1 ? s - 1 : 1;
}

int snx_notice_rect(const SnxNotice* n, int sw, int sh, int scale, int rect[4]) {
    int x = 8 * scale, y = 16 * scale, w = 8 * scale * ((int)strlen(n->text) + 2), h = 16 * scale;
    if (x >= sw || y >= sh) return 0;
    rect[0] = x;
    rect[1] = y;
    rect[2] = w < sw - x ? w : sw - x;
    rect[3] = h < sh - y ? h : sh - y;
    return 1;
}

void snx_notice_pixels(const SnxNotice* n, int scale, int bgra, const int rect[4], unsigned char* out, size_t stride) {
    const uint32_t page = SNX_GUI_THEME_YORHA_PAGE, ink = SNX_GUI_THEME_YORHA_INK;
    size_t len = strlen(n->text);
    for (int y = 0; y < rect[3]; ++y) {
        unsigned char* row = out + (size_t)y * stride;
        int fy = y / scale;
        for (int x = 0; x < rect[2]; ++x) {
            int cell = x / (8 * scale) - 1, fx = x / scale % 8; /* the blank cell before the letters is -1 */
            unsigned char c = cell >= 0 && (size_t)cell < len ? (unsigned char)n->text[cell] : ' ';
            uint32_t rgb = gui_vga_font[c * 16 + fy] & (0x80 >> fx) ? page : ink;
            unsigned char r = (unsigned char)(rgb >> 16), g = (unsigned char)(rgb >> 8), b = (unsigned char)rgb;
            row[x * 4 + 0] = bgra ? b : r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = bgra ? r : b;
            row[x * 4 + 3] = 255;
        }
    }
}
