/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The mask of a range of pages in one word of the page model's bit tables (gpu/vk/uma_pages.h), which says whether a
   range read in place has a page whose newer bytes are elsewhere (uma.c's rg_blk): every range of the first 300
   pages against its pages one by one. A range that starts inside a word and ends in a later one takes that word from
   its first page's bit, and the words after it whole: pages 70 to 200 are bits 6 to 63 of word 1, all of word 2 and
   bits 0 to 8 of word 3 */
#include <assert.h>
#include "nx/gpu/vk/uma_pages.h"
int main(void) {
    assert(vk_uma_word_mask(1, 70, 200) == ~0ull << 6 && vk_uma_word_mask(2, 70, 200) == ~0ull);
    assert(vk_uma_word_mask(3, 70, 200) == 0x1ffull && vk_uma_word_mask(1, 70, 72) == 0x1c0ull);
    for (uint64_t p0 = 0; p0 < 300; p0++)
        for (uint64_t p1 = p0; p1 < 300; p1++)
            for (uint64_t wi = p0 >> 6; wi <= p1 >> 6; wi++) {
                uint64_t pages = 0;
                for (uint64_t p = wi << 6; p <= (wi << 6) + 63; p++)
                    if (p >= p0 && p <= p1) pages |= 1ull << (p & 63);
                assert(vk_uma_word_mask(wi, p0, p1) == pages);
            }
    return 0;
}
