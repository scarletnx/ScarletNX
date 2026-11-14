/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The rows a band of a block linear target takes when it is published or written back a band at a time (surface.c's
   publish_bands and flush_parts), apart so tests/rt_bands.c holds them to their rules for every shape */
#ifndef SNX_VK_RT_BANDS_H
#define SNX_VK_RT_BANDS_H
#include <stdint.h>

/* For a target of h rows, row_bytes a row in the guest's layout, blocks of 2^bh GOBs: whole rows of blocks (8 << bh
   rows), as many as keep the band's linear rows (row_bytes up to a multiple of 16) within cap / share, the part of the
   cap a scratch buffer may stay at; else one row of blocks, when its bytes in the guest's layout (whole GOBs of 64
   bytes a row) are within cap; else 0. No more than the rows of blocks h takes. With share 4 or more every band is
   within cap: its GOBs' 64 bytes a row are at most 4 times its 16 */
static inline uint32_t vk_rt_band_rows(uint64_t cap, uint64_t share, uint32_t row_bytes, uint32_t h, uint32_t bh) {
    uint64_t pitch = ((uint64_t)row_bytes + 15) & ~(uint64_t)15, band = (uint64_t)8 << bh;
    uint64_t all = ((uint64_t)h + band - 1) / band * band, rows = cap / share / pitch / band * band;
    if (!rows) rows = ((((uint64_t)row_bytes + 63) >> 6) << (9 + bh)) <= cap ? band : 0;
    return (uint32_t)(rows < all ? rows : all);
}
#endif
