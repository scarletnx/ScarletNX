/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The body of shading_rate.comp, which the tests also run on the CPU (tests/glsl/shading_rate_glsl.cpp). An
   invocation for each texel of the device's image. Each texel of the game's image is a palette index, and table gives
   the value that the device reads for that index (vk/shading_rate_table.c). A pixel past the game's image takes
   entry 0, as the console reads it. Who includes it defines GAME_INDEX(x, y), a texel of the game's image, and
   RATE_STORE(x, y, v), a texel of the device's, and declares the push constants of VkVrsPush (vk/shading_rate.h)
   under their names. All of it is integer arithmetic */

/* the value of the game's texel (gx, gy), entry 0 past the image */
uint rate_value(uint gx, uint gy) {
    uint index = gx < game_w && gy < game_h ? GAME_INDEX(gx, gy) & 15u : 0u;
    return (table[index >> 2u] >> ((index & 3u) * 8u)) & 0xFFu;
}

void main() {
    uint x = gl_GlobalInvocationID.x, y = gl_GlobalInvocationID.y;
    if (x >= size_x || y >= size_y) return;
    if (finest == 0u) {
        /* the game's texel under the center pixel of the device's texel: the values are palette indices, with no
           order between them (VK_NV_shading_rate_image, whose texel is the console's) */
        RATE_STORE(x, y, rate_value((x * texel_w + texel_w / 2u) / game_texel, (y * texel_h + texel_h / 2u) / game_texel));
        return;
    }
    /* the finest rate of the game's texels under the device's texel: the values are rates as
       VK_KHR_fragment_shading_rate encodes them (log2 of the width in bits 2-3, of the height in bits 0-1), and the one
       with the fewest pixels shades each of those texels at least as finely as the console does. Of two with as
       many (2x1 and 1x2), the smaller value, so the result does not depend on the order of the texels */
    uint gx0 = x * texel_w / game_texel, gx1 = (x * texel_w + texel_w - 1u) / game_texel;
    uint gy0 = y * texel_h / game_texel, gy1 = (y * texel_h + texel_h - 1u) / game_texel;
    uint best = rate_value(gx0, gy0);
    for (uint gy = gy0; gy <= gy1; gy++)
        for (uint gx = gx0; gx <= gx1; gx++) {
            uint v = rate_value(gx, gy);
            if (((v >> 2u) + (v & 3u)) * 16u + v < ((best >> 2u) + (best & 3u)) * 16u + best) best = v;
        }
    RATE_STORE(x, y, best);
}
