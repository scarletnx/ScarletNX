/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include "nx/gpu/vk/range_shape.h"

u64 vk_shape_bucket_floor(u32 bucket) { return bucket ? 256ull << 2 * (bucket - 1) : 0; }
u32 vk_shape_size_bucket(u64 len) {
    u32 b = 0;
    while (b + 1 < VK_SHAPE_SIZES && len >= vk_shape_bucket_floor(b + 1)) b++;
    return b;
}

static int by_start(const void* a, const void* b) {
    const VkRange *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* one joined piece [a, e) into the totals; last[k]: the last granule of size k counted so far, plus one */
static void run_add(u64 a, u64 e, const u64 gran[VK_SHAPE_GRANS], u64 last[VK_SHAPE_GRANS], VkRangeShape* out) {
    out->runs++;
    out->covered += e - a;
    if (e - a > out->run_max) out->run_max = e - a;
    for (u32 k = 0; k < VK_SHAPE_GRANS; k++) {
        u64 first = a / gran[k], end = (e - 1) / gran[k] + 1; /* the piece's granules: [first, end) */
        if (first < last[k]) first = last[k];                 /* the piece before it ended in this one */
        if (end > first) out->granules[k] += end - first;
        if (end > last[k]) last[k] = end;
    }
}

u32 vk_range_join(VkRange* r, u32 n) {
    qsort(r, n, sizeof *r, by_start);
    u32 k = 0;
    for (u32 i = 0; i < n; i++) {
        u64 len = r[i].len;
        if (!len) continue;
        if (len > ~0ull - r[i].start) len = ~0ull - r[i].start;
        if (k && r[i].start <= r[k - 1].start + r[k - 1].len) {
            u64 e = r[i].start + len;
            if (e > r[k - 1].start + r[k - 1].len) r[k - 1].len = e - r[k - 1].start;
        } else
            r[k++] = (VkRange){r[i].start, len};
    }
    return k;
}

u64 vk_range_inside(const VkRange* r, u32 n, u64 start, u64 len) {
    u64 end = len > ~0ull - start ? ~0ull : start + len, bytes = 0;
    u32 lo = 0, hi = n;
    while (lo < hi) { /* the first piece that ends after start */
        u32 mid = lo + (hi - lo) / 2;
        if (r[mid].start + r[mid].len <= start)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (u32 i = lo; i < n && r[i].start < end; i++) {
        u64 a = r[i].start > start ? r[i].start : start, e = r[i].start + r[i].len < end ? r[i].start + r[i].len : end;
        bytes += e - a;
    }
    return bytes;
}

void vk_range_shape(VkRange* r, u32 n, const u64 gran[VK_SHAPE_GRANS], VkRangeShape* out) {
    *out = (VkRangeShape){0};
    qsort(r, n, sizeof *r, by_start);
    u64 last[VK_SHAPE_GRANS] = {0};
    u64 a = 0, e = 0; /* the piece being joined: none while e is 0 */
    for (u32 i = 0; i < n; i++) {
        u64 len = r[i].len;
        if (!len) continue;
        if (len > ~0ull - r[i].start) len = ~0ull - r[i].start;
        u32 b = vk_shape_size_bucket(len);
        out->ranges++;
        out->sum += len;
        out->size_count[b]++;
        out->size_bytes[b] += len;
        if (e && r[i].start <= e) { /* touches or overlaps the piece: joined */
            if (r[i].start + len > e) e = r[i].start + len;
            continue;
        }
        if (e) run_add(a, e, gran, last, out);
        a = r[i].start;
        e = a + len;
    }
    if (e) run_add(a, e, gran, last, out);
}
