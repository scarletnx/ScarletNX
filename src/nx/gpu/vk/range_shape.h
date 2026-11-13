/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shape of a set of byte ranges, for the measurements of the buffer cache: how much of them is the same bytes
   held more than once, how many pieces they make once joined, and how much memory a backing in granules would bind
   for them. No state and no Vulkan: a test gives it ranges and checks the numbers. */
#ifndef SNX_VK_RANGE_SHAPE_H
#define SNX_VK_RANGE_SHAPE_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    u64 start, len;
} VkRange;

/* sizes in fours: bucket b holds the ranges of 256 << 2 * (b - 1) bytes up to the next bucket's, bucket 0 those under
   256 bytes and the last those of 64 MiB and more */
#define VK_SHAPE_SIZES 11
#define VK_SHAPE_GRANS 2 /* granule sizes counted at once */

typedef struct {
    u32 ranges;                   /* those with bytes */
    u64 sum;                      /* their bytes, each range's counted */
    u64 covered;                  /* the bytes in one range or more: sum less covered is held more than once */
    u32 runs;                     /* the pieces they make once the ones that touch or overlap are joined */
    u64 run_max;                  /* the longest piece */
    u64 granules[VK_SHAPE_GRANS]; /* the granules of each size with a byte of a range in them */
    u32 size_count[VK_SHAPE_SIZES];
    u64 size_bytes[VK_SHAPE_SIZES];
} VkRangeShape;

/* the shape of r[0..n), which it sorts by start; gran: the granule sizes, each a power of two. A range that would
   wrap past the end of the address space is cut there */
void vk_range_shape(VkRange* r, u32 n, const u64 gran[VK_SHAPE_GRANS], VkRangeShape* out);
/* r[0..n) sorted by start and joined in place: the pieces left, in order, none touching the next. Ranges without
   bytes go */
u32 vk_range_join(VkRange* r, u32 n);
/* the bytes of [start, start + len) inside the joined pieces r[0..n) */
u64 vk_range_inside(const VkRange* r, u32 n, u64 start, u64 len);
/* the bucket of a size, and a bucket's least size */
u32 vk_shape_size_bucket(u64 len);
u64 vk_shape_bucket_floor(u32 bucket);
#endif
