// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2016-2026 melonDS team
// Copyright 2026 the ScarletNX contributors
// A texel of the 3D engine's texture memory, decoded where it lies: the texture space's 512 KiB and the texture
// palettes' 96 KiB as the board had them when the frame was drawn, copied whole (render3d.c). The seven formats are
// read as melonDS's software renderer reads them (GPU3D_Soft.cpp's TextureLookup, GPL-3.0-or-later).
layout(std430, set = 0, binding = 1) readonly buffer TextureMemory { uint T[]; };
const uint TEXPAL = 0x80000u;
uint tex8(uint a) {
    a &= 0x7FFFFu;
    return (T[a >> 2] >> ((a & 3u) * 8u)) & 0xFFu;
}
uint tex16(uint a) {
    a &= 0x7FFFEu;
    return (T[a >> 2] >> ((a & 2u) * 8u)) & 0xFFFFu;
}
uint pal16(uint a) {
    a &= 0x1FFFEu;
    if (a >= 0x18000u) return 0u;
    a += TEXPAL;
    return (T[a >> 2] >> ((a & 2u) * 8u)) & 0xFFFFu;
}
// a coordinate into its texture: repeated, mirrored or held at the edge
int wrap(int c, int size, bool repeat, bool flip) {
    if (repeat) {
        if (flip && (c & size) != 0) return (size - 1) - (c & (size - 1));
        return c & (size - 1);
    }
    return clamp(c, 0, size - 1);
}
// the texel at (s, t), in texels: its 15-bit color and its alpha, 0 to 31
uint texel(uint param, uint palette, ivec2 st, out uint alpha) {
    int width = 8 << ((param >> 20) & 7u), height = 8 << ((param >> 23) & 7u);
    uint s = uint(wrap(st.x, width, (param & 0x10000u) != 0u, (param & 0x40000u) != 0u));
    uint t = uint(wrap(st.y, height, (param & 0x20000u) != 0u, (param & 0x80000u) != 0u));
    uint address = (param & 0xFFFFu) << 3, w = uint(width);
    uint alpha0 = (param & 0x20000000u) != 0u ? 0u : 31u;
    switch ((param >> 26) & 7u) {
        case 1u: { // A3I5
            uint pixel = tex8(address + t * w + s);
            alpha = ((pixel >> 3) & 0x1Cu) + (pixel >> 6);
            return pal16((palette << 4) + ((pixel & 0x1Fu) << 1));
        }
        case 2u: { // 4 colors
            uint pixel = (tex8(address + ((t * w + s) >> 2)) >> ((s & 3u) << 1)) & 3u;
            alpha = pixel == 0u ? alpha0 : 31u;
            return pal16((palette << 3) + (pixel << 1));
        }
        case 3u: { // 16 colors
            uint pixel = (tex8(address + ((t * w + s) >> 1)) >> ((s & 1u) << 2)) & 15u;
            alpha = pixel == 0u ? alpha0 : 31u;
            return pal16((palette << 4) + (pixel << 1));
        }
        case 4u: { // 256 colors
            uint pixel = tex8(address + t * w + s);
            alpha = pixel == 0u ? alpha0 : 31u;
            return pal16((palette << 4) + (pixel << 1));
        }
        case 5u: { // 4x4 blocks of four colors, each block's palette named in slot 1
            uint block = (address + (t & 0x3FCu) * (w >> 2) + (s & 0x3FCu) + (t & 3u)) & 0x7FFFFu;
            uint info_at = 0x20000u + ((block & 0x1FFFCu) >> 1);
            if (block >= 0x40000u) info_at += 0x10000u;
            uint value = block >= 0x20000u && block < 0x40000u ? 0u : (tex8(block) >> (2u * (s & 3u))) & 3u;
            uint info = tex16(info_at), base = (palette << 4) + ((info & 0x3FFFu) << 2), mode = info >> 14;
            alpha = 31u;
            if (value == 0u) return pal16(base);
            if (value == 1u) return pal16(base + 2u);
            uint c0 = pal16(base), c1 = pal16(base + 2u);
            uvec3 a = uvec3(c0 & 0x1Fu, c0 & 0x3E0u, c0 & 0x7C00u), b = uvec3(c1 & 0x1Fu, c1 & 0x3E0u, c1 & 0x7C00u);
            if (value == 2u) {
                if (mode == 1u) {
                    uvec3 m = ((a + b) >> 1) & uvec3(0x1Fu, 0x3E0u, 0x7C00u);
                    return m.r | m.g | m.b;
                }
                if (mode == 3u) {
                    uvec3 m = ((a * 5u + b * 3u) >> 3) & uvec3(0x1Fu, 0x3E0u, 0x7C00u);
                    return m.r | m.g | m.b;
                }
                return pal16(base + 4u);
            }
            if (mode == 2u) return pal16(base + 6u);
            if (mode == 3u) {
                uvec3 m = ((a * 3u + b * 5u) >> 3) & uvec3(0x1Fu, 0x3E0u, 0x7C00u);
                return m.r | m.g | m.b;
            }
            alpha = 0u;
            return 0u;
        }
        case 6u: { // A5I3
            uint pixel = tex8(address + t * w + s);
            alpha = pixel >> 3;
            return pal16((palette << 4) + ((pixel & 7u) << 1));
        }
        case 7u: { // 16-bit colors
            uint color = tex16(address + ((t * w + s) << 1));
            alpha = (color & 0x8000u) != 0u ? 31u : 0u;
            return color;
        }
        default: break;
    }
    alpha = 31u;
    return 0x7FFFu;
}
