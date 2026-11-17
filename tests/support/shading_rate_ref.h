/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The CPU reference of shading_rate.comp (src/nx/gpu/vk/shading_rate.glsl): the device's image of a shading rate
   image, made from the pixels that each texel covers */
#ifndef SNX_TEST_SHADING_RATE_REF_H
#define SNX_TEST_SHADING_RATE_REF_H
#include <stdint.h>

/* out (w x h texels of texel_w x texel_h pixels) from the game's image (game_w x game_h texels of game_texel x
   game_texel pixels, an index a byte). A pixel takes value[] of the index of its texel, or value[0] past the game's
   image. Without finest, each texel of out takes its center pixel's value. With finest, the values are KHR rates
   (log2 of the width in bits 2-3, of the height in bits 0-1), and each texel takes the finest of its pixels' */
void shading_rate_ref(const uint8_t* game, uint32_t game_w, uint32_t game_h, uint32_t game_texel, uint32_t texel_w,
                      uint32_t texel_h, uint32_t w, uint32_t h, const uint8_t value[16], int finest, uint8_t* out);

#endif
