/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/shading_rate_ref.h"
#include <stdlib.h>

/* By pixels, not by the shader's arithmetic: each pixel of the device's image's area gets the value of the game's
   texel that holds it, and each texel of the device's image reads its center pixel, or the finest of its pixels */
void shading_rate_ref(const uint8_t* game, uint32_t game_w, uint32_t game_h, uint32_t game_texel, uint32_t texel_w,
                      uint32_t texel_h, uint32_t w, uint32_t h, const uint8_t value[16], int finest, uint8_t* out) {
    uint32_t pw = w * texel_w, ph = h * texel_h;
    uint8_t* pixel = malloc((size_t)pw * ph);
    if (!pixel) abort();
    for (uint32_t py = 0; py < ph; py++)
        for (uint32_t px = 0; px < pw; px++) {
            uint32_t tx = 0, ty = 0;
            /* the game's texel that holds the pixel: the count of whole texels before it */
            while ((tx + 1) * game_texel <= px) tx++;
            while ((ty + 1) * game_texel <= py) ty++;
            int index = tx < game_w && ty < game_h ? game[(size_t)ty * game_w + tx] & 15 : 0;
            pixel[(size_t)py * pw + px] = value[index];
        }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t v = pixel[(size_t)(y * texel_h + texel_h / 2) * pw + x * texel_w + texel_w / 2];
            /* the rate of the fewest pixels: the smallest sum of the two log2 sizes, then the smaller value */
            for (uint32_t py = y * texel_h; finest && py < (y + 1) * texel_h; py++)
                for (uint32_t px = x * texel_w; px < (x + 1) * texel_w; px++) {
                    uint8_t p = pixel[(size_t)py * pw + px];
                    int pk = ((p >> 2) + (p & 3)) * 16 + p, vk = ((v >> 2) + (v & 3)) * 16 + v;
                    if (pk < vk) v = p;
                }
            out[(size_t)y * w + x] = v;
        }
    free(pixel);
}
