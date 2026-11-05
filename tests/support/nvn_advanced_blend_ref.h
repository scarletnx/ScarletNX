/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Independent coverage-weight reference for the fixed-function subset of NVN advanced blend equations. */
#ifndef SNX_TEST_NVN_ADVANCED_BLEND_REF_H
#define SNX_TEST_NVN_ADVANCED_BLEND_REF_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/nvn/nvn.h"
#include <math.h>

static inline void nvn_advanced_blend_reference(u32 mode, int premultiplied, u32 overlap, const float source[4],
                                                const float destination[4], float result[4]) {
    float a = source[3], b = destination[3];
    float intersection = overlap == 0 ? a * b : overlap == 1 ? fmaxf(a + b - 1, 0) : fminf(a, b);
    float source_only = a - intersection, destination_only = b - intersection;
    int x = 1, y = 1, z = 1, blend = 0;
    switch (mode) {
        case NVN_BLEND_ADVANCED_MODE_ZERO: x = y = z = 0; break;
        case NVN_BLEND_ADVANCED_MODE_SRC:
            z = 0;
            blend = 1;
            break;
        case NVN_BLEND_ADVANCED_MODE_DST:
            y = 0;
            blend = 2;
            break;
        case NVN_BLEND_ADVANCED_MODE_SRC_OVER: blend = 1; break;
        case NVN_BLEND_ADVANCED_MODE_DST_OVER: blend = 2; break;
        case NVN_BLEND_ADVANCED_MODE_SRC_IN:
            y = z = 0;
            blend = 1;
            break;
        case NVN_BLEND_ADVANCED_MODE_DST_IN:
            y = z = 0;
            blend = 2;
            break;
        case NVN_BLEND_ADVANCED_MODE_SRC_OUT: x = z = 0; break;
        case NVN_BLEND_ADVANCED_MODE_DST_OUT: x = y = 0; break;
        case NVN_BLEND_ADVANCED_MODE_SRC_ATOP:
            y = 0;
            blend = 1;
            break;
        case NVN_BLEND_ADVANCED_MODE_DST_ATOP:
            z = 0;
            blend = 2;
            break;
        case NVN_BLEND_ADVANCED_MODE_XOR: x = 0; break;
        case NVN_BLEND_ADVANCED_MODE_SCREEN: blend = 3; break;
        default: x = y = z = 0; break;
    }
    for (u32 component = 0; component < 3; component++) {
        float s = premultiplied ? (a ? source[component] / a : 0) : source[component];
        float d = b ? destination[component] / b : 0;
        float f = blend == 1 ? s : blend == 2 ? d : blend == 3 ? s + d - s * d : 0;
        result[component] = f * intersection + y * s * source_only + z * d * destination_only;
    }
    result[3] = x * intersection + y * source_only + z * destination_only;
}
#endif
