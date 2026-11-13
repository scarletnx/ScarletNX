/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SMDH a 3DS title carries (ExeFS:/icon): the launcher's name for the game and the 48x48 icon it draws. The icon is
   RGB565 in 8x8 tiles, Morton order inside each tile, as 3dbrew and Citra's GetMortonOffset have it */
#include "3ds/loader/smdh.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put_units(unsigned char* smdh, unsigned language, const unsigned* units, size_t count) {
    unsigned char* entry = smdh + 0x8 + 0x200u * language;
    size_t i;
    memset(entry, 0, 0x80);
    for (i = 0; i < count && i < 0x40; ++i) {
        entry[i * 2] = (unsigned char)units[i];
        entry[i * 2 + 1] = (unsigned char)(units[i] >> 8);
    }
}
static void put_title(unsigned char* smdh, unsigned language, const char* text) {
    unsigned char* entry = smdh + 0x8 + 0x200u * language;
    size_t i;
    memset(entry, 0, 0x80);
    for (i = 0; text[i] && i < 0x40; ++i) {
        entry[i * 2] = (unsigned char)text[i];
        entry[i * 2 + 1] = 0;
    }
}
/* the offset of x, y in the large icon: the tile it is in, then its place in that tile's Morton order */
static size_t icon_at(unsigned x, unsigned y) {
    static const unsigned xlut[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const unsigned ylut[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    return 0x24c0 + (size_t)(y & ~7u) * 48 * 2 + (size_t)(xlut[x % 8] + ylut[y % 8] + (x & ~7u) * 8) * 2;
}
static void put_icon(unsigned char* smdh, unsigned x, unsigned y, unsigned color) {
    size_t at = icon_at(x, y);
    smdh[at] = (unsigned char)color;
    smdh[at + 1] = (unsigned char)(color >> 8);
}
int main(void) {
    unsigned char smdh[0x36c0], rgba[48 * 48 * 4];
    char title[192];
    memset(smdh, 0, sizeof smdh);
    assert(!snx_ctr_smdh_valid(smdh, sizeof smdh)); /* no magic */
    memcpy(smdh, "SMDH", 4);
    assert(snx_ctr_smdh_valid(smdh, sizeof smdh));
    assert(!snx_ctr_smdh_valid(smdh, sizeof smdh - 1)); /* the large icon is part of it */
    assert(!snx_ctr_smdh_valid(NULL, sizeof smdh));
    /* the New 3DS exclusive flag: bit 12 of the little-endian flags at 0x2028, whatever the other bits hold */
    assert(!snx_ctr_smdh_new3ds_only(smdh, sizeof smdh));
    smdh[0x2028] = 0xff;
    smdh[0x2029] = 0xef;
    assert(!snx_ctr_smdh_new3ds_only(smdh, sizeof smdh));
    smdh[0x2029] = 0x10;
    assert(snx_ctr_smdh_new3ds_only(smdh, sizeof smdh));
    assert(snx_ctr_smdh_new3ds_only(smdh, 0x202c));  /* the flags read, the icons not needed */
    assert(!snx_ctr_smdh_new3ds_only(smdh, 0x202b)); /* the flags cut */
    assert(!snx_ctr_smdh_new3ds_only(NULL, sizeof smdh));
    smdh[0x2028] = smdh[0x2029] = 0;
    /* English first, then Japanese, then the others in the SMDH's order */
    snx_ctr_smdh_title(smdh, sizeof smdh, title, sizeof title);
    assert(!title[0]);
    put_title(smdh, 0, "Nihongo");
    snx_ctr_smdh_title(smdh, sizeof smdh, title, sizeof title);
    assert(!strcmp(title, "Nihongo"));
    put_title(smdh, 2, "Francais");
    snx_ctr_smdh_title(smdh, sizeof smdh, title, sizeof title);
    assert(!strcmp(title, "Nihongo")); /* Japanese before the other languages */
    put_title(smdh, 1, "English");
    snx_ctr_smdh_title(smdh, sizeof smdh, title, sizeof title);
    assert(!strcmp(title, "English")); /* and English before Japanese */
    /* a title broken over two lines is one in a window's, and its room ends it */
    put_title(smdh, 1, "Two\nLines");
    snx_ctr_smdh_title(smdh, sizeof smdh, title, sizeof title);
    assert(!strcmp(title, "Two Lines"));
    snx_ctr_smdh_title(smdh, sizeof smdh, title, 5);
    assert(!strcmp(title, "Two "));
    snx_ctr_smdh_title(smdh, 0x2000, title, sizeof title);
    assert(!title[0]); /* no SMDH, no title */
    /* a carriage return is a space too, and a surrogate with no pair is U+FFFD; a pair is one character */
    {
        static const unsigned cr[] = {'A', '\r', 'B'}, lone[] = {0xd800, 'X'}, pair[] = {0xd800, 0xdc00};
        put_units(smdh, 1, cr, 3);
        snx_ctr_smdh_title(smdh, sizeof smdh, title, sizeof title);
        assert(!strcmp(title, "A B"));
        put_units(smdh, 1, lone, 2);
        snx_ctr_smdh_title(smdh, sizeof smdh, title, sizeof title);
        assert(!strcmp(title, "\xEF\xBF\xBDX"));
        put_units(smdh, 1, pair, 2);
        snx_ctr_smdh_title(smdh, sizeof smdh, title, sizeof title);
        assert(!strcmp(title, "\xF0\x90\x80\x80"));
    }
    /* the titles end before the icons: an ExeFS icon read in part still names the game, and has no icon */
    put_title(smdh, 1, "Partial");
    snx_ctr_smdh_title(smdh, 0x2020, title, sizeof title);
    assert(!strcmp(title, "Partial"));
    assert(snx_ctr_smdh_icon(smdh, 0x2020, rgba));
    /* the icon: a colour at a known place in the first tile, and one in the second */
    memset(rgba, 0, sizeof rgba);
    assert(snx_ctr_smdh_icon(smdh, 0x2000, rgba)); /* the icon is not there */
    put_icon(smdh, 0, 0, 0xf800);                  /* red */
    put_icon(smdh, 1, 0, 0x07e0);                  /* green */
    put_icon(smdh, 0, 1, 0x001f);                  /* blue */
    put_icon(smdh, 8, 0, 0xffff);                  /* white, in the tile to the right */
    put_icon(smdh, 47, 47, 0x8410);                /* grey, in the last tile */
    assert(!snx_ctr_smdh_icon(smdh, sizeof smdh, rgba));
    assert(rgba[4 * (0 * 48 + 0) + 0] == 255 && rgba[4 * (0 * 48 + 0) + 1] == 0 && rgba[4 * (0 * 48 + 0) + 2] == 0);
    assert(rgba[4 * (0 * 48 + 1) + 0] == 0 && rgba[4 * (0 * 48 + 1) + 1] == 255 && rgba[4 * (0 * 48 + 1) + 2] == 0);
    assert(rgba[4 * (1 * 48 + 0) + 0] == 0 && rgba[4 * (1 * 48 + 0) + 1] == 0 && rgba[4 * (1 * 48 + 0) + 2] == 255);
    assert(rgba[4 * (0 * 48 + 8) + 0] == 255 && rgba[4 * (0 * 48 + 8) + 1] == 255 && rgba[4 * (0 * 48 + 8) + 2] == 255);
    assert(rgba[4 * (47 * 48 + 47) + 0] == 131 && rgba[4 * (47 * 48 + 47) + 1] == 129 &&
           rgba[4 * (47 * 48 + 47) + 2] == 131 && rgba[4 * (47 * 48 + 47) + 3] == 255);
    puts("smdh: title and icon");
    return 0;
}
