/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The bands a block linear target is published or written back in (vk/rt_bands.h, surface.c's publish_bands and
   flush_parts), for every shape of a grid: rows of blocks a band, bands that meet with no gap and no overlap from
   the span's first byte to its last, the last one partial, and each band's bytes in the guest's layout within the cap
   (no band where a row of blocks is over it). A target one byte wide under a cap of 128 bytes took a band of 512 */
#include <stdio.h>
#include "nx/gpu/vk/rt_bands.h"

static unsigned checks, failures;
static void check(int ok, const char* what, uint64_t cap, uint32_t row, uint32_t h, uint32_t bh, uint64_t a,
                  uint64_t b) {
    checks++;
    if (ok) return;
    if (failures++ < 8)
        fprintf(stderr, "FAIL: %s: cap %llu, row %u bytes, %u rows, blocks of %u GOBs: %llu against %llu\n", what,
                (unsigned long long)cap, row, h, 1u << bh, (unsigned long long)a, (unsigned long long)b);
}

int main(void) {
    static const uint32_t rows_bytes[] = {1, 2, 15, 16, 17, 48, 63, 64, 65, 100, 127, 128, 1000, 4096, 4097, 16384,
                                          65536, 65537, 262144, 1u << 20};
    static const uint32_t heights[] = {1, 7, 8, 9, 15, 16, 17, 100, 127, 128, 129, 1000, 4095, 4096, 4097, 8192, 16383,
                                       16384, 65536};
    static const uint64_t caps[] = {64,       128,       200,         512,         4096,       65536, 1u << 20,
                                    3u << 20, 64u << 20, 256u << 20, 0xFFFFFFFCu, 1ull << 30, 3ull << 30, 1ull << 34};
    for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++)
        for (size_t r = 0; r < sizeof rows_bytes / sizeof rows_bytes[0]; r++)
            for (size_t k = 0; k < sizeof heights / sizeof heights[0]; k++)
                for (uint32_t bh = 0; bh <= 5; bh++) {
                    uint64_t cap = caps[c];
                    uint32_t row = rows_bytes[r], h = heights[k];
                    uint64_t band = (uint64_t)8 << bh, pitch = ((uint64_t)row + 15) & ~(uint64_t)15;
                    uint64_t block_row = (((uint64_t)row + 63) >> 6) << (9 + bh);
                    uint64_t size = (((((uint64_t)h + 7) >> 3) + (1u << bh) - 1) >> bh) * block_row;
                    uint32_t rows = vk_rt_band_rows(cap, 4, row, h, bh);
                    if (!rows) { /* none: not even a row of blocks fits */
                        check(block_row > cap, "no band though a row of blocks fits", cap, row, h, bh, block_row, cap);
                        continue;
                    }
                    check(rows % band == 0, "a band is not whole rows of blocks", cap, row, h, bh, rows, band);
                    uint64_t prev = 0;
                    for (uint64_t y = 0; y < h; y += rows) {
                        uint64_t n = h - y < rows ? h - y : rows;
                        uint64_t lo = ((y >> 3) >> bh) * block_row, hi = ((((y + n - 1) >> 3) >> bh) + 1) * block_row;
                        check(lo == prev, "a band does not start where the last one ended", cap, row, h, bh, lo, prev);
                        check(hi - lo <= cap, "a band is over the cap", cap, row, h, bh, hi - lo, cap);
                        check(n * pitch <= cap / 4 || rows == band, "a band's linear rows are over the cap's share",
                              cap, row, h, bh, n * pitch, cap / 4);
                        prev = hi;
                    }
                    check(prev == size, "the last band does not end at the span's end", cap, row, h, bh, prev, size);
                }
    if (failures) {
        printf("%u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("rt_bands: %u checks: the bands tile each span, whole rows of blocks within the cap\n", checks);
    return 0;
}
