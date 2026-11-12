/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shape of a set of ranges (gpu/vk/range_shape.c): the bytes held twice, the joined pieces and the granules they
   touch, which the buffer cache's measurements report. */
#include "nx/gpu/vk/range_shape.h"
#include <assert.h>
#include <stdio.h>

#define KIB 1024ull
#define MIB (1024 * KIB)

int main(void) {
    const u64 gran[VK_SHAPE_GRANS] = {64 * KIB, 2 * MIB};
    VkRangeShape s;

    /* no ranges, and ranges without bytes */
    VkRange none[2] = {{100, 0}, {5, 0}};
    vk_range_shape(none, 0, gran, &s);
    assert(!s.ranges && !s.sum && !s.covered && !s.runs && !s.granules[0] && !s.granules[1]);
    vk_range_shape(none, 2, gran, &s);
    assert(!s.ranges && !s.runs);

    /* one range inside the next, a third that only touches, a fourth apart: given out of order */
    VkRange a[4] = {{3 * MIB, 4 * KIB}, {0x1000, 0x1000}, {0x1800, 0x400}, {0x2000, 0x1000}};
    vk_range_shape(a, 4, gran, &s);
    assert(s.ranges == 4);
    assert(s.sum == 0x1000 + 0x400 + 0x1000 + 4 * KIB);
    assert(s.covered == 0x2000 + 4 * KIB); /* [0x1000, 0x3000) and the one apart */
    assert(s.sum - s.covered == 0x400);    /* the inner range's bytes are held twice */
    assert(s.runs == 2 && s.run_max == 0x2000);
    assert(s.granules[0] == 2);                            /* 64 KiB: granule 0 and the one at 3 MiB */
    assert(s.granules[1] == 2);                            /* 2 MiB: granules 0 and 1 */
    assert(a[0].start == 0x1000 && a[3].start == 3 * MIB); /* sorted */

    /* two pieces in one granule count it once, and a piece over a granule's end counts both */
    VkRange b[3] = {{10, 10}, {40 * KIB, 10}, {64 * KIB - 5, 10}};
    vk_range_shape(b, 3, gran, &s);
    assert(s.runs == 3 && s.covered == 30);
    assert(s.granules[0] == 2 && s.granules[1] == 1);

    /* identical ranges: all but one copy is held again */
    VkRange c[3] = {{MIB, 256}, {MIB, 256}, {MIB, 256}};
    vk_range_shape(c, 3, gran, &s);
    assert(s.sum == 768 && s.covered == 256 && s.runs == 1 && s.granules[0] == 1);

    /* a range at the end of the address space is cut there */
    VkRange d[1] = {{~0ull - 16, 64}};
    vk_range_shape(d, 1, gran, &s);
    assert(s.sum == 16 && s.covered == 16 && s.runs == 1);

    /* the buckets: under 256 bytes, then in fours, the last open */
    assert(vk_shape_size_bucket(0) == 0 && vk_shape_size_bucket(255) == 0);
    assert(vk_shape_size_bucket(256) == 1 && vk_shape_size_bucket(1023) == 1 && vk_shape_size_bucket(1024) == 2);
    assert(vk_shape_size_bucket(64 * MIB - 1) == VK_SHAPE_SIZES - 2);
    assert(vk_shape_size_bucket(64 * MIB) == VK_SHAPE_SIZES - 1 && vk_shape_size_bucket(~0ull) == VK_SHAPE_SIZES - 1);
    assert(vk_shape_bucket_floor(0) == 0 && vk_shape_bucket_floor(1) == 256);
    assert(vk_shape_bucket_floor(VK_SHAPE_SIZES - 1) == 64 * MIB);
    VkRange e[2] = {{0, 100}, {MIB, 5000}};
    vk_range_shape(e, 2, gran, &s);
    assert(s.size_count[0] == 1 && s.size_bytes[0] == 100 && s.size_count[3] == 1 && s.size_bytes[3] == 5000);

    /* joined in place: sorted, the ones that touch or overlap made one, the empty one gone */
    VkRange j[5] = {{300, 50}, {100, 50}, {150, 10}, {120, 5}, {500, 0}};
    u32 nj = vk_range_join(j, 5);
    assert(nj == 2 && j[0].start == 100 && j[0].len == 60 && j[1].start == 300 && j[1].len == 50);
    assert(vk_range_inside(j, nj, 0, 1000) == 110);
    assert(vk_range_inside(j, nj, 0, 100) == 0 && vk_range_inside(j, nj, 160, 140) == 0);
    assert(vk_range_inside(j, nj, 130, 200) == 30 + 30); /* [130, 160) and [300, 330) */
    assert(vk_range_inside(j, nj, 340, ~0ull) == 10 && vk_range_inside(j, nj, 350, 5) == 0);
    assert(vk_range_join(j, 0) == 0 && vk_range_inside(j, 0, 0, 100) == 0);

    puts("vk_range_shape: ok");
    return 0;
}
