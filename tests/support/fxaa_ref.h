/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_FXAA_REF_H
#define SNX_TEST_FXAA_REF_H
#include <stdint.h>
/* src/common/gpu/vk/fxaa.glsl on the CPU, at each pixel's centre of an RGBA8 picture of width x height (rows top
   down, gamma-encoded as fxaa.comp reads it), into out: the colour as floats 0 to 1, four a pixel. edge (one a pixel,
   may be NULL) is 1 where the pixel's taps spanned an edge, 0 where the picture there was kept. other (may be NULL),
   four a pixel as out: at an edge the colour of the pair of taps the shader did not keep, which a GPU whose bilinear
   weights are coarser may keep where the wider pair's luma is at the neighbourhood's range; elsewhere out's */
void fxaa_ref(const uint8_t* rgba, unsigned width, unsigned height, float* out, float* other, uint8_t* edge);
/* fxaa_ref with the taps held inside bounds, the part of the picture a window shows from its first pixel's centre to
   its last's (x0, y0, x1, y1), as the shader's bounds hold them */
void fxaa_ref_bounds(const uint8_t* rgba, unsigned width, unsigned height, const float bounds[4], float* out,
                     float* other, uint8_t* edge);
#endif
