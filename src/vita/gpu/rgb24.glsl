/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The body of rgb24.comp: a 24-bit color surface (SceGxm's U8U8U8, three bytes a pixel) between its memory and the
   R8G8B8A8 rows its image is copied from and into, as a host's attachment can't be three bytes. GLSL that is also C++
   (tests/glsl/glsl.hpp): tests/glsl/vita_rgb24_glsl.cpp runs it against the layout written out per byte.

   The surface's rows are stride bytes apart in the source (unpack), stride pixels apart in the destination (pack), its
   first byte src_at bytes into the bound source (unpack) or dst_at into the bound destination (pack). BGR (bgr 0) has
   R in the first byte of a pixel, as ABGR has it for four bytes; RGB (bgr 1) has B there. Unpack: one invocation a
   pixel of the rows (width by height, alpha 1). Pack: one a word of the destination from its bound start, its bytes
   the surface's taken from the rows and the others (the bytes before the surface, and a row's pixels past the width)
   kept, so no two invocations write a word. */

uint source_byte(uint at) { return (src[at >> 2u] >> ((at & 3u) << 3u)) & 0xffu; }

void main() {
    uint w = gl_GlobalInvocationID.x;
    if (w >= words) return;
    if (pack == 0u) {
        uint y = w / width, x = w - y * width;
        uint at = src_at + y * stride + x * 3u;
        uint c0 = source_byte(at), c1 = source_byte(at + 1u), c2 = source_byte(at + 2u);
        dst[w] = (bgr != 0u ? (c2 | (c1 << 8u) | (c0 << 16u)) : (c0 | (c1 << 8u) | (c2 << 16u))) | 0xff000000u;
        return;
    }
    uint v = dst[w];
    for (uint b = 0u; b < 4u; b++) {
        uint at = (w << 2u) + b;
        if (at < dst_at) continue;
        uint s = at - dst_at, p = s / 3u, y = p / stride, x = p - y * stride;
        if (y >= height) break;
        if (x >= width) continue;
        uint c = s - p * 3u, component = bgr != 0u ? 2u - c : c, shift = b << 3u;
        v = (v & ~(0xffu << shift)) | (((src[y * width + x] >> (component << 3u)) & 0xffu) << shift);
    }
    dst[w] = v;
}
