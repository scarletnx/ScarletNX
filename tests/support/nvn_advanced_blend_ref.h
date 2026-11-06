/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Independent coverage-weight reference for the published advanced blend equations. */
#ifndef SNX_TEST_NVN_ADVANCED_BLEND_REF_H
#define SNX_TEST_NVN_ADVANCED_BLEND_REF_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/nvn/nvn.h"
#include <math.h>

static inline float nvn_blend_ref_lum(const float c[3]) { return 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
static inline float nvn_blend_ref_sat(const float c[3]) {
    return fmaxf(fmaxf(c[0], c[1]), c[2]) - fminf(fminf(c[0], c[1]), c[2]);
}
static inline void nvn_blend_ref_lum_set(float c[3], const float luminosity[3]) {
    float delta = nvn_blend_ref_lum(luminosity) - nvn_blend_ref_lum(c);
    for (u32 i = 0; i < 3; i++) c[i] += delta;
    float lum = nvn_blend_ref_lum(c);
    float low = fminf(fminf(c[0], c[1]), c[2]), high = fmaxf(fmaxf(c[0], c[1]), c[2]);
    if (low < 0)
        for (u32 i = 0; i < 3; i++) c[i] = lum + (c[i] - lum) * lum / (lum - low);
    if (high > 1)
        for (u32 i = 0; i < 3; i++) c[i] = lum + (c[i] - lum) * (1 - lum) / (high - lum);
}
static inline float nvn_blend_ref_function(u32 mode, float s, float d) {
    switch (mode) {
        case NVN_BLEND_ADVANCED_MODE_MULTIPLY: return s * d;
        case NVN_BLEND_ADVANCED_MODE_SCREEN: return s + d - s * d;
        case NVN_BLEND_ADVANCED_MODE_OVERLAY: return d <= 0.5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
        case NVN_BLEND_ADVANCED_MODE_DARKEN: return fminf(s, d);
        case NVN_BLEND_ADVANCED_MODE_LIGHTEN: return fmaxf(s, d);
        case NVN_BLEND_ADVANCED_MODE_COLORDODGE: return d <= 0 ? 0 : s < 1 ? fminf(1, d / (1 - s)) : 1;
        case NVN_BLEND_ADVANCED_MODE_COLORBURN: return d >= 1 ? 1 : s > 0 ? 1 - fminf(1, (1 - d) / s) : 0;
        case NVN_BLEND_ADVANCED_MODE_HARDLIGHT: return s <= 0.5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
        case NVN_BLEND_ADVANCED_MODE_SOFTLIGHT:
            return s <= 0.5f    ? d - (1 - 2 * s) * d * (1 - d)
                   : d <= 0.25f ? d + (2 * s - 1) * d * ((16 * d - 12) * d + 3)
                                : d + (2 * s - 1) * (sqrtf(d) - d);
        case NVN_BLEND_ADVANCED_MODE_DIFFERENCE: return fabsf(d - s);
        case NVN_BLEND_ADVANCED_MODE_EXCLUSION: return s + d - 2 * s * d;
        case NVN_BLEND_ADVANCED_MODE_INVERT: return 1 - d;
        case NVN_BLEND_ADVANCED_MODE_INVERT_RGB: return s * (1 - d);
        case NVN_BLEND_ADVANCED_MODE_LINEARDODGE: return fminf(1, s + d);
        case NVN_BLEND_ADVANCED_MODE_LINEARBURN: return fmaxf(0, s + d - 1);
        case NVN_BLEND_ADVANCED_MODE_VIVIDLIGHT:
            return s <= 0 ? 0 : s < 0.5f ? 1 - fminf(1, (1 - d) / (2 * s)) : s < 1 ? fminf(1, d / (2 * (1 - s))) : 1;
        case NVN_BLEND_ADVANCED_MODE_LINEARLIGHT: return fminf(1, fmaxf(0, 2 * s + d - 1));
        case NVN_BLEND_ADVANCED_MODE_PINLIGHT: return s < 0.5f ? fminf(d, 2 * s) : fmaxf(d, 2 * s - 1);
        case NVN_BLEND_ADVANCED_MODE_HARDMIX: return s + d < 1 ? 0 : 1;
        default: return 0;
    }
}

static inline void nvn_advanced_blend_reference(u32 mode, int premultiplied, u32 overlap, const float source[4],
                                                const float destination[4], float result[4]) {
    float a = source[3], b = destination[3];
    if ((mode >= NVN_BLEND_ADVANCED_MODE_PLUS && mode <= NVN_BLEND_ADVANCED_MODE_PLUS_DARKER) ||
        mode == NVN_BLEND_ADVANCED_MODE_MINUS || mode == NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED ||
        mode == NVN_BLEND_ADVANCED_MODE_CONTRAST || mode == NVN_BLEND_ADVANCED_MODE_INVERT_OVG ||
        (mode >= NVN_BLEND_ADVANCED_MODE_RED && mode <= NVN_BLEND_ADVANCED_MODE_BLUE)) {
        for (u32 component = 0; component < 4; component++) {
            float s = component < 3 && !premultiplied ? source[component] * a : source[component];
            float d = destination[component], sum = s + d, alpha_sum = fminf(1, a + b);
            result[component] = mode == NVN_BLEND_ADVANCED_MODE_PLUS                 ? sum
                                : mode == NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED       ? fminf(1, sum)
                                : mode == NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA ? fminf(alpha_sum, sum)
                                : mode == NVN_BLEND_ADVANCED_MODE_PLUS_DARKER
                                    ? component == 3 ? alpha_sum : fmaxf(0, alpha_sum - a - b + sum)
                                : mode == NVN_BLEND_ADVANCED_MODE_MINUS         ? d - s
                                : mode == NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED ? fmaxf(0, d - s)
                                : mode == NVN_BLEND_ADVANCED_MODE_CONTRAST
                                    ? component == 3 ? b : b / 2 + 2 * (d - b / 2) * (s - a / 2)
                                : mode == NVN_BLEND_ADVANCED_MODE_INVERT_OVG
                                    ? component == 3 ? a + b - a * b : a * (1 - d) + (1 - a) * d
                                : component == mode - NVN_BLEND_ADVANCED_MODE_RED ? s
                                                                                  : d;
        }
        return;
    }
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
        case NVN_BLEND_ADVANCED_MODE_INVERT:
        case NVN_BLEND_ADVANCED_MODE_INVERT_RGB: y = 0; break;
        default: break;
    }
    float hsl[3], source_base[3], destination_base[3];
    for (u32 i = 0; i < 3; i++) {
        source_base[i] = premultiplied ? (a ? source[i] / a : 0) : source[i];
        destination_base[i] = b ? destination[i] / b : 0;
    }
    if (mode >= NVN_BLEND_ADVANCED_MODE_HSL_HUE) {
        int source_hue = mode == NVN_BLEND_ADVANCED_MODE_HSL_HUE || mode == NVN_BLEND_ADVANCED_MODE_HSL_COLOR;
        for (u32 i = 0; i < 3; i++) hsl[i] = source_hue ? source_base[i] : destination_base[i];
        if (mode == NVN_BLEND_ADVANCED_MODE_HSL_HUE || mode == NVN_BLEND_ADVANCED_MODE_HSL_SATURATION) {
            float saturation = nvn_blend_ref_sat(source_hue ? destination_base : source_base);
            float base_sat = nvn_blend_ref_sat(hsl), base_min = fminf(fminf(hsl[0], hsl[1]), hsl[2]);
            for (u32 i = 0; i < 3; i++) hsl[i] = base_sat ? (hsl[i] - base_min) * saturation / base_sat : 0;
        }
        nvn_blend_ref_lum_set(hsl, mode == NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY ? source_base : destination_base);
    }
    for (u32 component = 0; component < 3; component++) {
        float s = premultiplied ? (a ? source[component] / a : 0) : source[component];
        float d = b ? destination[component] / b : 0;
        float f = blend == 1                                ? s
                  : blend == 2                              ? d
                  : mode >= NVN_BLEND_ADVANCED_MODE_HSL_HUE ? hsl[component]
                                                            : nvn_blend_ref_function(mode, s, d);
        result[component] = f * intersection + y * s * source_only + z * d * destination_only;
    }
    result[3] = x * intersection + y * source_only + z * destination_only;
}
#endif
