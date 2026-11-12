/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The icon's 8x8 tiles are read in Morton order as Azahar (Citra)'s video_core/utils.h GetMortonOffset does it:
   copyright 2014 to 2025 Citra Emulator Project / Azahar Emulator Project, GPLv2 or later (docs/CREDITS.md) */
#include "3ds/loader/smdh.h"
#include <stdint.h>
#include <string.h>

#define SMDH_SIZE       0x36c0 /* magic to the end of the large icon */
#define SMDH_TITLES     0x8    /* the 16 titles' start, 0x200 bytes each */
#define SMDH_TITLE_SIZE 0x200
#define SMDH_LARGE      0x24c0 /* the 48x48 icon's start, RGB565 */
#define SMDH_SIDE       48

int snx_ctr_smdh_valid(const unsigned char* smdh, size_t size) {
    return smdh && size >= SMDH_SIZE && !memcmp(smdh, "SMDH", 4);
}
/* one title entry's short description, 0x40 UTF-16LE units at its start, as UTF-8; the bytes written, 0 for none */
static size_t title_of(const unsigned char* entry, char* out, size_t out_size) {
    size_t n = 0;
    for (unsigned i = 0; i < 0x40; ++i) {
        uint32_t cp = (uint32_t)entry[i * 2] | (uint32_t)entry[i * 2 + 1] << 8;
        unsigned char u[4];
        unsigned len;
        if (!cp) break;
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < 0x40) { /* a surrogate pair */
            uint32_t low = (uint32_t)entry[i * 2 + 2] | (uint32_t)entry[i * 2 + 3] << 8;
            if (low >= 0xdc00 && low < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                ++i;
            }
        }
        if (cp == '\n' || cp == '\r') cp = ' '; /* a title broken over two lines is one in a window's */
        if (cp >= 0xd800 && cp < 0xe000) cp = 0xfffd; /* a surrogate with no pair is not UTF-8 */
        len = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (n + len >= out_size) break;
        if (len == 1)
            u[0] = (unsigned char)cp;
        else {
            unsigned k;
            for (k = len - 1; k > 0; --k, cp >>= 6) u[k] = (unsigned char)(0x80 | (cp & 0x3f));
            u[0] = (unsigned char)((len == 2 ? 0xc0 : len == 3 ? 0xe0 : 0xf0) | cp);
        }
        memcpy(out + n, u, len);
        n += len;
    }
    out[n] = 0;
    return n;
}
void snx_ctr_smdh_title(const unsigned char* smdh, size_t size, char* out, size_t out_size) {
    /* the titles alone end before the icons: an ExeFS icon read in part still names the game */
    if (out_size) out[0] = 0;
    if (!smdh || size < SNX_CTR_SMDH_TITLES_SIZE || memcmp(smdh, "SMDH", 4) || out_size < 2) return;
    for (unsigned order = 0; order < 16; ++order) {
        /* English (1) first, then Japanese (0), then the others as the SMDH lists them */
        unsigned language = order == 0 ? 1 : order == 1 ? 0 : order;
        const unsigned char* entry = smdh + SMDH_TITLES + (size_t)SMDH_TITLE_SIZE * language;
        if (title_of(entry, out, out_size)) return;
    }
}
int snx_ctr_smdh_icon(const unsigned char* smdh, size_t size, unsigned char* rgba) {
    /* 8x8 Z-order, the coordinates inside one tile, as Citra's video_core GetMortonOffset has them */
    static const unsigned xlut[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const unsigned ylut[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    if (!snx_ctr_smdh_valid(smdh, size) || !rgba) return 1;
    const unsigned char* data = smdh + SMDH_LARGE;
    for (unsigned y = 0; y < SMDH_SIDE; ++y) {
        size_t row = (size_t)(y & ~7u) * SMDH_SIDE * 2; /* the tiles of the rows above, linear */
        for (unsigned x = 0; x < SMDH_SIDE; ++x) {
            unsigned i = xlut[x % 8] + ylut[y % 8];
            size_t at = row + (size_t)(i + (x & ~7u) * 8) * 2;
            unsigned c = (unsigned)data[at] | (unsigned)data[at + 1] << 8;
            unsigned char* p = rgba + 4 * ((size_t)y * SMDH_SIDE + x);
            p[0] = (unsigned char)(((c >> 11) & 0x1f) * 255 / 31); /* R */
            p[1] = (unsigned char)(((c >> 5) & 0x3f) * 255 / 63);  /* G */
            p[2] = (unsigned char)((c & 0x1f) * 255 / 31);         /* B */
            p[3] = 255;
        }
    }
    return 0;
}
