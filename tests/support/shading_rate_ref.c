/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/shading_rate_ref.h"
#include <stdlib.h>

/* By pixels, not by the shader's arithmetic: each pixel of the device's image's area gets the index of the game's
   texel that holds it, and each texel of the device's image reads its center pixel from that map */
void shading_rate_ref(const uint8_t* game, uint32_t game_w, uint32_t game_h, uint32_t game_texel, uint32_t texel_w,
                      uint32_t texel_h, uint32_t w, uint32_t h, const uint8_t value[16], uint8_t* out) {
    uint32_t pw = w * texel_w, ph = h * texel_h;
    int* index = malloc((size_t)pw * ph * sizeof *index); /* -1: past the game's image */
    if (!index) abort();
    for (uint32_t py = 0; py < ph; py++)
        for (uint32_t px = 0; px < pw; px++) {
            uint32_t tx = 0, ty = 0;
            /* the game's texel that holds the pixel: the count of whole texels before it */
            while ((tx + 1) * game_texel <= px) tx++;
            while ((ty + 1) * game_texel <= py) ty++;
            index[(size_t)py * pw + px] = tx < game_w && ty < game_h ? game[(size_t)ty * game_w + tx] & 15 : -1;
        }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            int i = index[(size_t)(y * texel_h + texel_h / 2) * pw + x * texel_w + texel_w / 2];
            out[(size_t)y * w + x] = value[i < 0 ? 0 : i];
        }
    free(index);
}
