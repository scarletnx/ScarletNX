/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The body of shading_rate.comp, which the tests also run on the CPU (tests/glsl/shading_rate_glsl.cpp). An
   invocation for each texel of the device's image: the center of its pixels falls in a texel of the game's image, a
   palette index, and table gives the value that the device reads for that index (vk/shading_rate_table.c). A texel
   past the game's image takes entry 0, as the console reads it. Who includes it defines GAME_INDEX(x, y), a texel
   of the game's image, and RATE_STORE(x, y, v), a texel of the device's, and declares the push constants of
   VkVrsPush (vk/shading_rate.h) under their names. All of it is integer arithmetic */
void main() {
    uint x = gl_GlobalInvocationID.x, y = gl_GlobalInvocationID.y;
    if (x >= size_x || y >= size_y) return;
    /* the center pixel of the device's texel, in texels of the game's (game_w and game_h count them) */
    uint gx = (x * texel_w + texel_w / 2u) / game_texel, gy = (y * texel_h + texel_h / 2u) / game_texel;
    uint index = gx < game_w && gy < game_h ? GAME_INDEX(gx, gy) & 15u : 0u;
    RATE_STORE(x, y, (table[index >> 2u] >> ((index & 3u) * 8u)) & 0xFFu);
}
