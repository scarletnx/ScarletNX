/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The render sizes the DLSS stand-in answers for an output size and a method (gpu/nvn/nvn_dlss.c), alone in one
   function so that tests pin its numbers, and so that a driver whose rule differs can be told apart here */
#ifndef SNX_NVN_DLSS_SIZES_H
#define SNX_NVN_DLSS_SIZES_H
#include <math.h>
#include <stdint.h>

/* a size times a scale as the driver computes it: a single precision product rounded to the nearest whole number,
   halves away from zero */
static inline int32_t nvn_dlss_scaled(int32_t size, float scale) {
    const float product = scale * (float)size;
    return (int32_t)roundf(product);
}

/* out[0] the optimal render size, out[1] the largest and out[2] the smallest a game may give an evaluation, each
   {width, height}.
   The driver's rule: method 1 renders at a third of the output and method 2 at the output's size, any other method at
   0.58 of it. A scale from a half up to the output's size may vary between a half of the output and all of it. A
   scale below a half is fixed, so the smallest and the largest are the optimal size. At the output's size the
   smallest is 0.99 of it.
   setting is the DLSS render size setting, which replaces the optimal size when it is 1 to 5 (the output's size,
   1/1.5, 1/1.7, 1/2 and 1/3 of it). The smallest and the largest then widen to hold that size, since a game may
   evaluate only a render size between the two */
static inline void nvn_dlss_sizes(int32_t width, int32_t height, int32_t method, int32_t setting, int32_t out[3][2]) {
    static const int32_t set[6][2] = {{0, 0}, {1, 1}, {2, 3}, {10, 17}, {1, 2}, {1, 3}};
    const int32_t size[2] = {width, height};
    const float scale = method == 1 ? 1.0f / 3.0f : method == 2 ? 1.0f : 0.58f;
    for (int a = 0; a < 2; a++) {
        const int32_t optimal = nvn_dlss_scaled(size[a], scale);
        out[0][a] = optimal;
        if (scale == 1.0f) {
            out[1][a] = size[a];
            out[2][a] = nvn_dlss_scaled(size[a], 0.99f);
        } else if (scale >= 0.5f) {
            out[1][a] = size[a];
            out[2][a] = nvn_dlss_scaled(size[a], 0.5f);
        } else {
            out[1][a] = out[2][a] = optimal;
        }
        if (setting > 0 && setting < 6) {
            const int32_t* s = set[setting];
            out[0][a] = (int32_t)(((int64_t)size[a] * s[0] + s[1] / 2) / s[1]);
            if (out[1][a] < out[0][a]) out[1][a] = out[0][a];
            if (out[2][a] > out[0][a]) out[2][a] = out[0][a];
        }
        for (int i = 0; i < 3; i++)
            if (out[i][a] < 1) out[i][a] = 1;
    }
}
#endif
