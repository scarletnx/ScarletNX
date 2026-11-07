/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ranges of a capture by stores (vk/render.c xfb_bind), apart from the device so that a test can check them */
#ifndef SNX_VK_XFB_STORES_H
#define SNX_VK_XFB_STORES_H
#include <stdint.h>

typedef struct {
    uint32_t limit;    /* the vertices the draw captures */
    uint32_t declared; /* the buffers the program stores into */
    uint32_t bound;    /* those of them that get bytes */
    uint32_t start[4]; /* in bytes from each buffer's start offset */
    uint32_t used[4];
    uint32_t end[4]; /* where the next capture of each bound buffer resumes */
} SnxXfbRange;

/* the whole primitives of want that fit in each enabled buffer (size 0: off) from where it resumes, as the GPU stops */
static inline void snx_xfb_range(uint32_t want, uint32_t per, const uint32_t stride[4], const uint32_t words[4],
                                 const uint32_t size[4], const uint32_t from[4], SnxXfbRange* r) {
    uint64_t limit = want;
    uint32_t last[4] = {0};
    *r = (SnxXfbRange){0};
    for (uint32_t b = 0; b < 4; b++) {
        if (!words[b] || stride[b] < 4 || (stride[b] & 3)) continue; /* xfb_write stores nothing there */
        r->declared |= 1u << b;
        if ((int32_t)size[b] <= 0) continue;
        r->start[b] = from[b] < size[b] ? from[b] : size[b];
        uint32_t room = size[b] - r->start[b];
        last[b] = words[b] * 4 > stride[b] ? words[b] * 4 : stride[b];
        uint64_t fit = room >= last[b] ? (room - last[b]) / stride[b] + 1 : 0;
        fit -= per ? fit % per : 0;
        if (fit < limit) limit = fit;
        r->bound |= 1u << b;
    }
    r->limit = (uint32_t)limit;
    for (uint32_t b = 0; b < 4; b++) {
        uint64_t used = r->limit ? (uint64_t)(r->limit - 1) * stride[b] + last[b] : 0;
        if (!(r->bound >> b & 1) || !used) {
            r->bound &= ~(1u << b);
            continue;
        }
        r->used[b] = (uint32_t)used;
        r->end[b] = r->start[b] + r->limit * stride[b];
    }
}

#endif
