/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The body of yuv.comp: a YUV420 texture (SceGxm's YUV420P2 and YUV420P3, in either chroma order) to R8G8B8A8 rows, a
   pixel an invocation. GLSL that is also C++ (tests/glsl/glsl.hpp): tests/glsl/vita_yuv_glsl.cpp runs it against the
   same arithmetic written plainly.

   Y in rows of stride bytes from y_at; the chroma from c_at, height/2 rows: interleaved pairs in rows of stride bytes
   (first_v: V first), or two planes of rows of stride/2 (first_v: V's first). Limited range, the matrix the host
   hands in as 8-bit fixed point (BT.601 for CSC0, BT.709 for CSC1): with c = 298 (Y - 16), d = U - 128, e = V - 128,
   R = (c + rv e + 128) >> 8, G = (c - gu d - gv e + 128) >> 8, B = (c + bu d + 128) >> 8, each clamped to a byte; A is
   255. The sums stay within 32 signed bits (|298 x 239| + |541 x 128| < 2^17). */

uint byte_at(uint at) { return (src[at >> 2u] >> ((at & 3u) << 3u)) & 0xffu; }
uint clamp_byte(int v) { return uint(clamp(v, 0, 255)); }

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pixels) return;
    uint y = i / width, x = i - y * width, cy = y >> 1u, cx = x >> 1u;
    uint u_byte, v_byte;
    if (packed != 0u) {
        uint pair = c_at + cy * stride + (cx << 1u);
        u_byte = byte_at(pair + first_v), v_byte = byte_at(pair + (first_v ^ 1u));
    } else {
        uint cstride = stride >> 1u, plane = cstride * (height >> 1u), at = c_at + cy * cstride + cx;
        u_byte = byte_at(at + first_v * plane), v_byte = byte_at(at + (first_v ^ 1u) * plane);
    }
    int c = 298 * (int(byte_at(y_at + y * stride + x)) - 16), d = int(u_byte) - 128, e = int(v_byte) - 128;
    uint r = clamp_byte((c + int(rv) * e + 128) >> 8);
    uint g = clamp_byte((c - int(gu) * d - int(gv) * e + 128) >> 8);
    uint b = clamp_byte((c + int(bu) * d + 128) >> 8);
    dst[i] = r | (g << 8u) | (b << 16u) | 0xff000000u;
}
