/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The notice of the windows with no menu of their own (common/utilities/notice.c), the Switch's notice drawn in the
   3DS's, the Vita's and the GBA's windows (tests/overlay.c holds it to the Switch's band): how long it shows, the
   letters' scale on pictures and displays of several sizes, the band's place and its cut by a small picture, and its
   pixels, the font's lit ones in the theme's page colour and the rest its ink, in either byte order */
#include "common/utilities/notice.h"
#include "common/utilities/font.h"
#include "gui_theme_colors.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the band's pixel at x, y of rect's, as the font and the theme draw it: page where a letter's pixel is lit */
static unsigned expected(const SnxNotice* n, int scale, int x, int y) {
    int cell = x / (8 * scale) - 1;
    unsigned char c = cell >= 0 && (size_t)cell < strlen(n->text) ? (unsigned char)n->text[cell] : ' ';
    return gui_vga_font[c * 16 + y / scale] & (0x80 >> (x / scale % 8)) ? SNX_GUI_THEME_YORHA_PAGE
                                                                        : SNX_GUI_THEME_YORHA_INK;
}

int main(void) {
    SnxNotice n = {0};
    char long_text[100];
    int rect[4], lit = 0;
    unsigned char* pixels;
    /* nothing before a text; a text for SNX_NOTICE_MS from when it was given; a longer one cut to the widest panel */
    assert(!snx_notice_shown(&n, 0));
    snx_notice_show(&n, "Press Esc again within 3 seconds to quit", 1000);
    assert(snx_notice_shown(&n, 1000) && snx_notice_shown(&n, 1000 + (uint64_t)SNX_NOTICE_MS * 1000000u - 1));
    assert(!snx_notice_shown(&n, 1000 + (uint64_t)SNX_NOTICE_MS * 1000000u) && SNX_NOTICE_MS == 3000);
    memset(long_text, 'x', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = 0;
    snx_notice_show(&n, long_text, 0);
    assert(strlen(n.text) == SNX_NOTICE_COLS && SNX_NOTICE_COLS == 72);
    snx_notice_show(&n, "", 0);
    assert(!snx_notice_shown(&n, 0));

    /* the menu's letters: half the picture for a page of 40 by 16 cells, twice the display's scale where that is
       more, three quarters of the picture at most; the notice's a step under, 1 at least */
    assert(snx_menu_scale(1280, 720, 100) == 2 && snx_notice_scale(1280, 720, 100) == 1);
    assert(snx_menu_scale(1920, 1080, 100) == 2 && snx_notice_scale(1920, 1080, 100) == 1);
    assert(snx_menu_scale(3840, 2160, 100) == 4 && snx_notice_scale(3840, 2160, 100) == 3);
    assert(snx_menu_scale(1920, 1080, 200) == 3 && snx_notice_scale(1920, 1080, 200) == 2);
    assert(snx_menu_scale(400, 240, 100) == 1 && snx_notice_scale(400, 240, 100) == 1);
    assert(snx_display_percent(1.5f) == 150 && snx_display_percent(0.0f) == 100);

    /* the band a cell in and a row down, its letters and a blank cell at either end; a small picture cuts it at its
       edges, and one the corner's cell does not fit in has none */
    snx_notice_show(&n, "Press Esc again within 3 seconds to quit", 0);
    assert(snx_notice_rect(&n, 3840, 2160, 3, rect));
    assert(rect[0] == 24 && rect[1] == 48 && rect[2] == 24 * 42 && rect[3] == 48);
    assert(snx_notice_rect(&n, 100, 40, 1, rect) && rect[0] == 8 && rect[1] == 16 && rect[2] == 92 && rect[3] == 16);
    assert(snx_notice_rect(&n, 100, 20, 1, rect) && rect[3] == 4);
    assert(!snx_notice_rect(&n, 8, 100, 1, rect) && !snx_notice_rect(&n, 100, 16, 1, rect));

    /* its pixels at a scale of 2, opaque: the font's lit ones the page, the rest the ink, the end cells blank; the
       same with the red and blue bytes swapped for a BGRA window */
    for (int bgra = 0; bgra < 2; ++bgra) {
        assert(snx_notice_rect(&n, 1920, 1080, 2, rect));
        pixels = malloc((size_t)rect[2] * (size_t)rect[3] * 4u);
        assert(pixels);
        snx_notice_pixels(&n, 2, bgra, rect, pixels, (size_t)rect[2] * 4u);
        lit = 0;
        for (int y = 0; y < rect[3]; ++y)
            for (int x = 0; x < rect[2]; ++x) {
                const unsigned char* p = pixels + ((size_t)y * (size_t)rect[2] + (size_t)x) * 4u;
                unsigned rgb = expected(&n, 2, x, y);
                unsigned got = bgra ? (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0]
                                    : (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
                assert(got == rgb && p[3] == 255);
                if (x < 16 || x >= rect[2] - 16) assert(rgb == SNX_GUI_THEME_YORHA_INK);
                lit += rgb == SNX_GUI_THEME_YORHA_PAGE;
            }
        assert(lit > 0 && SNX_GUI_THEME_YORHA_PAGE != SNX_GUI_THEME_YORHA_INK);
        free(pixels);
    }

    printf("notice: the band and its pixels, as the Switch's\n");
    return 0;
}
