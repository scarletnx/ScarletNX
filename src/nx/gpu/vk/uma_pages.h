/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VK_UMA_PAGES_H
#define SNX_VK_UMA_PAGES_H
#include <stdint.h>

/* The bits pages p0 to p1 take of word wi in a table of a bit a page, 64 pages a word (wi from p0 >> 6 to p1 >> 6):
   from p0's bit up in the range's first word, up to p1's in its last, and every bit of a word between them */
static inline uint64_t vk_uma_word_mask(uint64_t wi, uint64_t p0, uint64_t p1) {
    uint64_t a = wi == p0 >> 6 ? p0 & 63 : 0, b = wi == p1 >> 6 ? p1 & 63 : 63;
    return (~0ull << a) & (~0ull >> (63 - b));
}

#endif
