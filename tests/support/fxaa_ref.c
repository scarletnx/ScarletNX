/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The CPU reference of the FXAA that the Switch's and the 3DS's presenters share (src/common/gpu/vk/fxaa.glsl), step
   for step: its taps filtered bilinearly in floats, clamped to the edge, as the shader's linear sampler does. */
#include "fxaa_ref.h"
#include <math.h>
#include <stddef.h>

typedef struct {
    float c[4];
} Colour;

static const uint8_t* g_rgba;
static unsigned g_width, g_height;

static Colour texel(int x, int y) {
    x = x < 0 ? 0 : x >= (int)g_width ? (int)g_width - 1 : x;
    y = y < 0 ? 0 : y >= (int)g_height ? (int)g_height - 1 : y;
    const uint8_t* p = g_rgba + ((size_t)y * g_width + (size_t)x) * 4;
    return (Colour){{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f}};
}
/* the picture at a point in pixels, a pixel's centre half a pixel in from its corner */
static Colour tap(float x, float y) {
    float qx = x - 0.5f, qy = y - 0.5f, ix = floorf(qx), iy = floorf(qy), fx = qx - ix, fy = qy - iy;
    Colour a = texel((int)ix, (int)iy), b = texel((int)ix + 1, (int)iy), c = texel((int)ix, (int)iy + 1),
           d = texel((int)ix + 1, (int)iy + 1), r;
    for (int i = 0; i < 4; ++i) {
        float top = a.c[i] + (b.c[i] - a.c[i]) * fx, bottom = c.c[i] + (d.c[i] - c.c[i]) * fx;
        r.c[i] = top + (bottom - top) * fy;
    }
    return r;
}
static float luma(Colour c) { return c.c[0] * 0.299f + c.c[1] * 0.587f + c.c[2] * 0.114f; }
static float lesser(float a, float b) { return a < b ? a : b; }
static float greater(float a, float b) { return a > b ? a : b; }
static float clamp_span(float v) { return v < -8.0f ? -8.0f : v > 8.0f ? 8.0f : v; }

void fxaa_ref(const uint8_t* rgba, unsigned width, unsigned height, float* out, float* other, uint8_t* edge) {
    g_rgba = rgba, g_width = width, g_height = height;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            Colour middle = tap(px, py), result = middle, rest = middle;
            float lm = luma(middle), nw = luma(tap(px - 1, py - 1)), ne = luma(tap(px + 1, py - 1)),
                  sw = luma(tap(px - 1, py + 1)), se = luma(tap(px + 1, py + 1));
            float low = lesser(lm, lesser(lesser(nw, ne), lesser(sw, se)));
            float high = greater(lm, greater(greater(nw, ne), greater(sw, se)));
            int is_edge = !(high - low < greater(1.0f / 24.0f, high * 0.125f));
            if (is_edge) {
                float dx = (sw + se) - (nw + ne), dy = (nw + sw) - (ne + se);
                float reduce = greater((nw + ne + sw + se) * 0.25f * (1.0f / 8.0f), 1.0f / 128.0f);
                float scale = 1.0f / (lesser(fabsf(dx), fabsf(dy)) + reduce);
                dx = clamp_span(dx * scale), dy = clamp_span(dy * scale);
                Colour n1 = tap(px + dx * (1.0f / 3.0f - 0.5f), py + dy * (1.0f / 3.0f - 0.5f));
                Colour n2 = tap(px + dx * (2.0f / 3.0f - 0.5f), py + dy * (2.0f / 3.0f - 0.5f));
                Colour w1 = tap(px - dx * 0.5f, py - dy * 0.5f), w2 = tap(px + dx * 0.5f, py + dy * 0.5f);
                Colour narrow, wide;
                for (int i = 0; i < 3; ++i) {
                    narrow.c[i] = 0.5f * (n1.c[i] + n2.c[i]);
                    wide.c[i] = 0.5f * narrow.c[i] + 0.25f * (w1.c[i] + w2.c[i]);
                }
                float l = luma(wide);
                int keep_narrow = l < low || l > high;
                for (int i = 0; i < 3; ++i) {
                    result.c[i] = keep_narrow ? narrow.c[i] : wide.c[i];
                    rest.c[i] = keep_narrow ? wide.c[i] : narrow.c[i];
                }
            }
            for (int i = 0; i < 4; ++i) {
                out[((size_t)y * width + x) * 4 + (size_t)i] = result.c[i];
                if (other) other[((size_t)y * width + x) * 4 + (size_t)i] = rest.c[i];
            }
            if (edge) edge[(size_t)y * width + x] = (uint8_t)is_edge;
        }
}
