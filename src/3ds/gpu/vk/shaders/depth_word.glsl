// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The word that depth.comp stores for one depth and stencil texel. scaled_depth_texture.comp takes the same word for
// each texel of a scaled image: 16 bits of depth for D16 (format 0), 24 bits for D24 (2) and D24S8 (3, with the stencil
// in the top byte). A float depth (is_float, D32S8) converts to the format's bits. The bytes the store writes are the
// low bytes of the word, least significant first.
// the depth formats, as FB_DEPTH_FORMAT numbers them (render.c's DEPTH_ enum)
#define DEPTH_D16 0u
#define DEPTH_D24 2u
#define DEPTH_D24S8 3u
uint depth_word(uint fmt, uint is_float, uint bits, uint stencil) {
    float depth = is_float != 0u ? uintBitsToFloat(bits) : float(bits & 0xffffffu) / 16777215.0;
    uint value = fmt == DEPTH_D16 ? uint(round(clamp(depth, 0.0, 1.0) * 65535.0))
                 : is_float != 0u ? uint(round(clamp(depth, 0.0, 1.0) * 16777215.0))
                                  : bits & 0xffffffu;
    return fmt == DEPTH_D24S8 ? value | stencil << 24 : value;
}
