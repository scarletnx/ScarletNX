/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The body of swizzle.comp: a swizzled or tiled SceGxm texture's level 0 to linear rows, one 32-bit word of the rows a
   invocation. GLSL that is also C++ (tests/glsl/glsl.hpp): tests/glsl/vita_swizzle_glsl.cpp runs it against Vita3K's
   loops (renderer/src/texture/format.cpp, GPL-2.0-or-later: swizzled_texture_to_linear_texture,
   resolve_z_order_compressed_image, tiled_texture_to_linear_texture), whose layouts these are.

   An element is a texel, or a 4x4 block of a block-compressed format, usually 2^es bytes. U8U8U8 elements use es 5
   and take three bytes. Element (x, y) of a swizzled texture, w by h elements (powers of two), is at the Morton index
   of x and y below min(w, h) = 2^k, interleaved y in bit 0, with the larger side's bits above k after them. A tiled
   texture's 32x32 tiles are each 1024 elements in rows, the tiles in rows of tiles_x. Elements of four bytes or more
   give a word from one load (a uniform branch on es); smaller ones a byte at a time from up to four elements.
   Reversed (reverse nonzero, elements of four bytes or more only), linear rows become the texture: each word of the
   rows goes to its place in dst from dst_base, as a swizzled or tiled color surface's store writes its memory. */

uint part1by1(uint v) {
    v &= 0x0000ffffu;
    v = (v ^ (v << 8u)) & 0x00ff00ffu;
    v = (v ^ (v << 4u)) & 0x0f0f0f0fu;
    v = (v ^ (v << 2u)) & 0x33333333u;
    v = (v ^ (v << 1u)) & 0x55555555u;
    return v;
}
/* the element of the source holding linear element e */
uint source_element(uint e) {
    if (tiled != 0u) {
        uint y = e / width, x = e - y * width;
        return (((x >> 5u) + tiles_x * (y >> 5u)) << 10u) | (x & 31u) | ((y & 31u) << 5u);
    }
    uint x = e & (width - 1u), y = e >> wl, m = (1u << k) - 1u;
    return (((x >> k) | (y >> k)) << (k << 1u)) | (part1by1(x & m) << 1u) | part1by1(y & m);
}

void main() {
    uint w = gl_GlobalInvocationID.x;
    if (w >= words) return;
    uint d = w << 2u;
    if (es == 5u) {
        /* the word's bytes are of two elements, or three: each one's place is found once, as its first byte comes */
        uint v = 0u, e = d / 3u, c = d - e * 3u, from = 0u;
        if (e < elements) from = src_at + source_element(e) * 3u;
        for (uint b = 0u; b < 4u; b++) {
            if (e >= elements) break;
            uint s = from + c;
            v |= ((src[src_base + (s >> 2u)] >> ((s & 3u) << 3u)) & 0xffu) << (b << 3u);
            if (++c == 3u) {
                c = 0u;
                if (++e < elements) from = src_at + source_element(e) * 3u;
            }
        }
        dst[w] = v;
        return;
    }
    if (reverse != 0u) {
        uint shift = es - 2u;
        dst[dst_base + (source_element(d >> es) << shift) + ((d >> 2u) & ((1u << shift) - 1u))] = src[src_base + w];
        return;
    }
    if (es >= 2u) {
        uint shift = es - 2u;
        dst[w] = src[src_base + (source_element(d >> es) << shift) + ((d >> 2u) & ((1u << shift) - 1u))];
        return;
    }
    uint v = 0u;
    for (uint b = 0u; b < 4u; b++) {
        uint at = d + b, e = at >> es;
        if (e >= elements) break; /* the last word's bytes past the texture */
        uint s = (source_element(e) << es) + (at & ((1u << es) - 1u));
        v |= ((src[src_base + (s >> 2u)] >> ((s & 3u) << 3u)) & 0xffu) << (b << 3u);
    }
    dst[w] = v;
}
