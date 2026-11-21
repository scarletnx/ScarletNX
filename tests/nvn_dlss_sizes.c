/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The render sizes the DLSS stand-in answers (src/nx/gpu/nvn/nvn_dlss_sizes.h): the driver's rule for each method at
   the output sizes games ask, and the DLSS render size setting, whose size must lie between the smallest and the
   largest */
#include "nx/gpu/nvn/nvn_dlss_sizes.h"
#include <stdio.h>

static int failures;
/* one answer: optimal, then largest, then smallest, as nvn_dlss_sizes fills them */
static void check(int32_t w, int32_t h, int32_t method, int32_t setting, int32_t ow, int32_t oh, int32_t xw, int32_t xh,
                  int32_t nw, int32_t nh) {
    int32_t r[3][2];
    nvn_dlss_sizes(w, h, method, setting, r);
    if (r[0][0] != ow || r[0][1] != oh || r[1][0] != xw || r[1][1] != xh || r[2][0] != nw || r[2][1] != nh) {
        failures++;
        printf("FAIL %dx%d method %d setting %d: optimal %dx%d largest %dx%d smallest %dx%d, expected %dx%d %dx%d "
               "%dx%d\n",
               w, h, method, setting, r[0][0], r[0][1], r[1][0], r[1][1], r[2][0], r[2][1], ow, oh, xw, xh, nw, nh);
    }
    if (r[2][0] > r[0][0] || r[0][0] > r[1][0] || r[2][1] > r[0][1] || r[0][1] > r[1][1]) {
        failures++;
        printf("FAIL %dx%d method %d setting %d: the optimal size is outside the range\n", w, h, method, setting);
    }
}

int main(void) {
    /* 0.58 of the output, between a half of it and all of it */
    check(2560, 1440, 0, 0, 1485, 835, 2560, 1440, 1280, 720);
    check(1920, 1080, 0, 0, 1114, 626, 1920, 1080, 960, 540);
    check(3840, 2160, 0, 0, 2227, 1253, 3840, 2160, 1920, 1080);
    /* a method the rule doesn't name is the same */
    check(2560, 1440, 3, 0, 1485, 835, 2560, 1440, 1280, 720);
    check(2560, 1440, -1, 0, 1485, 835, 2560, 1440, 1280, 720);
    /* a third, fixed */
    check(2560, 1440, 1, 0, 853, 480, 853, 480, 853, 480);
    check(3840, 2160, 1, 0, 1280, 720, 1280, 720, 1280, 720);
    /* the output's size, down to 0.99 of it */
    check(3840, 2160, 2, 0, 3840, 2160, 3840, 2160, 3802, 2138);
    check(1920, 1080, 2, 0, 1920, 1080, 1920, 1080, 1901, 1069);
    /* the setting replaces the optimal size and the range holds it */
    check(2560, 1440, 0, 1, 2560, 1440, 2560, 1440, 1280, 720);
    check(2560, 1440, 0, 2, 1707, 960, 2560, 1440, 1280, 720);
    check(2560, 1440, 0, 4, 1280, 720, 2560, 1440, 1280, 720);
    check(2560, 1440, 0, 5, 853, 480, 2560, 1440, 853, 480);
    check(2560, 1440, 1, 4, 1280, 720, 1280, 720, 853, 480);
    check(3840, 2160, 2, 4, 1920, 1080, 3840, 2160, 1920, 1080);
    check(3840, 2160, 2, 6, 3840, 2160, 3840, 2160, 3802, 2138);
    /* no size is ever zero */
    check(1, 1, 1, 0, 1, 1, 1, 1, 1, 1);
    check(0, 0, 0, 0, 1, 1, 1, 1, 1, 1);
    if (failures) return 1;
    printf("nvn_dlss_sizes: ok\n");
    return 0;
}
