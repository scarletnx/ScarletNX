// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The framebuffer and transfer pixel formats (Pica::PixelFormat) as common/color.h decodes and encodes them, and the
// Morton order of video_core/utils.h: the CPU reference is src/3ds/gpu/transfer.c (ported from Azahar, GPL-2.0-or-
// later, Copyright 2014-2023 Citra Emulator Project). Components are stored most significant first.
// The including shader declares SRC and DST, the byte arrays pixels are read from and written to (NO_ENCODE: no DST,
// for a shader that only decodes).
#define PIX_RGBA8  0u
#define PIX_RGB8   1u
#define PIX_RGB565 2u
#define PIX_RGB5A1 3u
#define PIX_RGBA4  4u

uint pixel_bytes(uint format) { return format == PIX_RGBA8 ? 4u : format == PIX_RGB8 ? 3u : 2u; }
uint c5to8(uint v) { return (v << 3) | (v >> 2); }
uint c6to8(uint v) { return (v << 2) | (v >> 4); }
uint c4to8(uint v) { return (v << 4) | v; }
uvec4 decode(uint format, uint at) {
    uint b0 = uint(SRC[at]), b1 = uint(SRC[at + 1u]);
    uint v = b0 | (b1 << 8); // 16 bits: its top field needs no mask
    switch (format) {
    case PIX_RGBA8: return uvec4(uint(SRC[at + 3u]), uint(SRC[at + 2u]), b1, b0);
    case PIX_RGB8: return uvec4(uint(SRC[at + 2u]), b1, b0, 255u);
    case PIX_RGB565: return uvec4(c5to8(v >> 11), c6to8((v >> 5) & 63u), c5to8(v & 31u), 255u);
    case PIX_RGB5A1: return uvec4(c5to8(v >> 11), c5to8((v >> 6) & 31u), c5to8((v >> 1) & 31u), (v & 1u) * 255u);
    default: return uvec4(c4to8(v >> 12), c4to8((v >> 8) & 15u), c4to8((v >> 4) & 15u), c4to8(v & 15u));
    }
}
#ifndef NO_ENCODE
void encode(uint format, uvec4 c, uint at) {
    uint v;
    switch (format) {
    case PIX_RGBA8:
        DST[at] = uint8_t(c.a), DST[at + 1u] = uint8_t(c.b), DST[at + 2u] = uint8_t(c.g), DST[at + 3u] = uint8_t(c.r);
        return;
    case PIX_RGB8: DST[at] = uint8_t(c.b), DST[at + 1u] = uint8_t(c.g), DST[at + 2u] = uint8_t(c.r); return;
    case PIX_RGB565: v = ((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3); break;
    case PIX_RGB5A1: v = ((c.r >> 3) << 11) | ((c.g >> 3) << 6) | ((c.b >> 3) << 1) | (c.a >> 7); break;
    default: v = ((c.r >> 4) << 12) | ((c.g >> 4) << 8) | ((c.b >> 4) << 4) | (c.a >> 4); break;
    }
    DST[at] = uint8_t(v), DST[at + 1u] = uint8_t(v >> 8);
}
#endif
// the place of pixel (x, y) in its 8x8 tile: the low three bits of each, interleaved
uint tile_index(uint x, uint y) {
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) | ((y & 4u) << 3);
}
// the byte offset of pixel (x, y) in its 8-pixel-high tile row, as VideoCore::GetMortonOffset
uint morton(uint x, uint y, uint bpp) { return (tile_index(x, y) + (x & ~7u) * 8u) * bpp; }
