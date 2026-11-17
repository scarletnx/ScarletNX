/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors

   snx_mo_lookup is musl's src/locale/__mo_lookup.c (https://git.musl-libc.org/cgit/musl/tree/src/locale/__mo_lookup.c),
   in ISO C, reading the catalog a byte at a time (it may lie at any address) and checking its whole tables are in it.
   musl's notice:

   Copyright (c) 2005-2020 Rich Felker, et al.

   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
   the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE. */
#include "gui/i18n.h"
#include "common/utilities/language_tag.h"
#include <string.h>

#define SNX_MO_MAGIC   0x950412deUL
#define SNX_MO_SWAPPED 0xde120495UL
#define SNX_MO_HEADER  28 /* magic, revision, count, the two tables' offsets, the hash table's size and offset */

static const SnxLanguage* tr_list;
static int tr_count, tr_current;

/* the catalog's 32-bit word at byte at, little-endian, or big-endian when swapped */
static unsigned long mo_word(const unsigned char* mo, size_t at, int swapped) {
    if (swapped)
        return (unsigned long)mo[at] << 24 | (unsigned long)mo[at + 1] << 16 | (unsigned long)mo[at + 2] << 8 |
               (unsigned long)mo[at + 3];
    return (unsigned long)mo[at + 3] << 24 | (unsigned long)mo[at + 2] << 16 | (unsigned long)mo[at + 1] << 8 |
           (unsigned long)mo[at];
}
/* the string of entry i of the table at table, NULL when it is not within size, NUL-ended */
static const char* mo_string(const unsigned char* mo, size_t size, size_t table, size_t i, int swapped) {
    unsigned long length = mo_word(mo, table + 8 * i, swapped), at = mo_word(mo, table + 8 * i + 4, swapped);
    if (at >= size || length >= size - at || mo[at + length]) return NULL;
    return (const char*)mo + at;
}
const char* snx_mo_lookup(const unsigned char* mo, size_t size, const char* s) {
    unsigned long magic, n, o, t;
    size_t b = 0, half;
    int swapped, sign;
    const char* original;
    if (!mo || size < SNX_MO_HEADER) return NULL;
    magic = mo_word(mo, 0, 0);
    if (magic != SNX_MO_MAGIC && magic != SNX_MO_SWAPPED) return NULL;
    swapped = magic == SNX_MO_SWAPPED;
    n = mo_word(mo, 8, swapped);
    o = mo_word(mo, 12, swapped);
    t = mo_word(mo, 16, swapped);
    /* n entries of 8 bytes in either table, each table whole in the catalog */
    if (!n || n >= size / 8 || o > size - 8 * n || t > size - 8 * n || (o | t) % 4) return NULL;
    for (;;) {
        half = b + n / 2;
        original = mo_string(mo, size, o, half, swapped);
        if (!original) return NULL;
        sign = strcmp(s, original);
        if (!sign) return mo_string(mo, size, t, half, swapped);
        if (n == 1) return NULL;
        if (sign < 0)
            n /= 2;
        else {
            b += n / 2;
            n -= n / 2;
        }
    }
}
void snx_tr_languages(const SnxLanguage* list, int count) {
    tr_list = list;
    tr_count = count;
    tr_current = 0;
}
void snx_tr_use(int language) { tr_current = language > 0 && language < tr_count ? language : 0; }
int snx_tr_language(void) { return tr_current; }
const char* snx_tr(const char* english) {
    const char* s;
    if (!tr_current || !*english) return english; /* "" names the catalog's header */
    s = snx_mo_lookup(tr_list[tr_current].mo, tr_list[tr_current].size, english);
    return s && *s ? s : english;
}
int snx_tr_find(const char* code) {
    int i;
    for (i = 0; i < tr_count; ++i)
        if (!strcmp(tr_list[i].code, code)) return i;
    for (i = 0; i < tr_count; ++i)
        if (snx_language_fit(tr_list[i].code, code) == 3) return i;
    return -1;
}
int snx_tr_pick(const char* const* locales, int count) {
    int l, i, best, how;
    for (l = 0; l < count; ++l) {
        best = -1;
        how = 0;
        for (i = 0; i < tr_count; ++i) {
            int fit = snx_language_fit(tr_list[i].code, locales[l]);
            if (fit > how) {
                best = i;
                how = fit;
            }
        }
        if (best >= 0) return best;
    }
    return 0;
}
