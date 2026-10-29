/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from deko3d (source/maxwell/tic_generate.cpp, tsc_generate.cpp, format_traits.inc), whose notice follows:

   Copyright (C) 2018-2020 fincs

   This software is provided 'as-is', without any express or implied
   warranty.  In no event will the authors be held liable for any
   damages arising from the use of this software.

   Permission is granted to anyone to use this software for any
   purpose, including commercial applications, and to alter it and
   redistribute it freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you
      must not claim that you wrote the original software. If you use
      this software in a product, an acknowledgment in the product
      documentation would be appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and
      must not be misrepresented as being the original software.
   3. This notice may not be removed or altered from any source
      distribution.

   Altered: deko3d's C++ for NVN's formats, targets and sampler enums, as C99. */
/* An NVN texture or sampler as the GPU's descriptor: the TIC and TSC entries NVIDIA's driver writes into a texture
   or sampler pool at the ID a game registers it under. NVN's handles keep the hardware's packing (texture ID |
   sampler ID << 20) because the game's shaders are Maxwell code reading handles the game stores in its own buffers,
   so the pools hold what the hardware would read, and the renderer's TIC and TSC code serves NVN as it serves
   nvhost's games.

   The words, from envytools' gm200_texture.xml as deko3d lays them out:
   TIC 0  format word: format 0-6, components r 7-9 g 10-12 b 13-15 a 16-18, swizzle x 19-21 y 22-24 z 25-27 w 28-30
       1  address 0-31
       2  address 32-47, view layer base 3-7 at 16, header version 21-23, view layer base 8-10 at 29
       3  block linear: tile width 0-2, height 3-5, depth 6-8; pitch: pitch >> 5; buffer: width - 1 >> 16 (0-15);
          lod aniso quality 2 at 16, lod aniso quality 17, lod iso quality 18, max levels - 1 at 28-31
       4  width - 1 (0-15), view layer base 0-2 at 16, sRGB 22, texture type 23-26, sector promotion 27-28, border 29-31
       5  height - 1 (0-15), depth - 1 (16-29), normalized coordinates 31
       6  aniso fine spread function 23-24, coarse 25-26
       7  view's first level 0-3, last level 4-7, multisample mode 8-11
   TSC 0  wrap u 0-2 v 3-5 p 6-8, compare 9, compare op 10-12, sRGB conversion 13, font filter 14-16 17-19,
          max anisotropy 20-22
       1  mag filter 0-1, min filter 4-5, mip filter 6-7, cube anisotropy 8, cube interface filtering 9, reduction
          10-11, lod bias 12-24 (fixed 5.8), trilinear optimization 26-30
       2  min lod clamp 0-11, max lod clamp 12-23 (fixed 4.8), sRGB border red 24-31
       3  sRGB border green 12-19, blue 20-27
       4-7 border color r g b a */
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include <math.h>
#include <string.h>

/* NVN format -> the TIC format word (deko3d's traits, its swizzle the format's own channels), whether its "one" is
   an integer, sRGB, depth or stencil, and its render target format (the 3D engine's color or zeta format code, 0 for
   a format no target takes). NVN's 3-channel 8 and 16 bit, I2F and RGB10A2 SN/I formats have no entry */
typedef struct {
    u32 word;
    u8 raw_int, srgb, depth, rt;
} TicFormat;
static const TicFormat tic_formats[0x9d] = {
    [0x01] = {0x7012491Du, 0, 0, 0, 0xF3}, /* R8 */
    [0x02] = {0x7011249Du, 0, 0, 0, 0xF4}, /* R8SN */
    [0x03] = {0x6014921Du, 1, 0, 0, 0xF6}, /* R8UI */
    [0x04] = {0x60136D9Du, 1, 0, 0, 0xF5}, /* R8I */
    [0x05] = {0x7017FF9Bu, 0, 0, 0, 0xF2}, /* R16F */
    [0x06] = {0x7012491Bu, 0, 0, 0, 0xEE}, /* R16 */
    [0x07] = {0x7011249Bu, 0, 0, 0, 0xEF}, /* R16SN */
    [0x08] = {0x6014921Bu, 1, 0, 0, 0xF1}, /* R16UI */
    [0x09] = {0x60136D9Bu, 1, 0, 0, 0xF0}, /* R16I */
    [0x0A] = {0x7017FF8Fu, 0, 0, 0, 0xE5}, /* R32F */
    [0x0B] = {0x6014920Fu, 1, 0, 0, 0xE4}, /* R32UI */
    [0x0C] = {0x60136D8Fu, 1, 0, 0, 0xE3}, /* R32I */
    [0x0D] = {0x70D24918u, 0, 0, 0, 0xEA}, /* RG8 */
    [0x0E] = {0x70D12498u, 0, 0, 0, 0xEB}, /* RG8SN */
    [0x0F] = {0x60D49218u, 1, 0, 0, 0xED}, /* RG8UI */
    [0x10] = {0x60D36D98u, 1, 0, 0, 0xEC}, /* RG8I */
    [0x11] = {0x70D7FF8Cu, 0, 0, 0, 0xDE}, /* RG16F */
    [0x12] = {0x70D2490Cu, 0, 0, 0, 0xDA}, /* RG16 */
    [0x13] = {0x70D1248Cu, 0, 0, 0, 0xDB}, /* RG16SN */
    [0x14] = {0x60D4920Cu, 1, 0, 0, 0xDD}, /* RG16UI */
    [0x15] = {0x60D36D8Cu, 1, 0, 0, 0xDC}, /* RG16I */
    [0x16] = {0x70D7FF84u, 0, 0, 0, 0xCB}, /* RG32F */
    [0x17] = {0x60D49204u, 1, 0, 0, 0xCD}, /* RG32UI */
    [0x18] = {0x60D36D84u, 1, 0, 0, 0xCC}, /* RG32I */
    [0x22] = {0x78D7FF82u, 0, 0, 0, 0x00}, /* RGB32F */
    [0x23] = {0x68D49202u, 1, 0, 0, 0x00}, /* RGB32UI */
    [0x24] = {0x68D36D82u, 1, 0, 0, 0x00}, /* RGB32I */
    [0x25] = {0x58D24908u, 0, 0, 0, 0xD5}, /* RGBA8 */
    [0x26] = {0x58D12488u, 0, 0, 0, 0xD7}, /* RGBA8SN */
    [0x27] = {0x58D49208u, 1, 0, 0, 0xD9}, /* RGBA8UI */
    [0x28] = {0x58D36D88u, 1, 0, 0, 0xD8}, /* RGBA8I */
    [0x29] = {0x58D7FF83u, 0, 0, 0, 0xCA}, /* RGBA16F */
    [0x2A] = {0x58D24903u, 0, 0, 0, 0xC6}, /* RGBA16 */
    [0x2B] = {0x58D12483u, 0, 0, 0, 0xC7}, /* RGBA16SN */
    [0x2C] = {0x58D49203u, 1, 0, 0, 0xC9}, /* RGBA16UI */
    [0x2D] = {0x58D36D83u, 1, 0, 0, 0xC8}, /* RGBA16I */
    [0x2E] = {0x58D7FF81u, 0, 0, 0, 0xC0}, /* RGBA32F */
    [0x2F] = {0x58D49201u, 1, 0, 0, 0xC2}, /* RGBA32UI */
    [0x30] = {0x58D36D81u, 1, 0, 0, 0xC1}, /* RGBA32I */
    [0x31] = {0x6014921Du, 0, 0, 1, 0x17}, /* STENCIL8 */
    [0x32] = {0x76DA493Au, 0, 0, 1, 0x13}, /* DEPTH16 */
    [0x33] = {0x76DA4A29u, 0, 0, 1, 0x14}, /* DEPTH24 */
    [0x34] = {0x7497FFAFu, 0, 0, 1, 0x0A}, /* DEPTH32F */
    [0x35] = {0x76DA4A29u, 0, 0, 1, 0x14}, /* DEPTH24_STENCIL8 */
    [0x36] = {0x749253B0u, 0, 0, 1, 0x19}, /* DEPTH32F_STENCIL8 */
    [0x37] = {0x78D24908u, 0, 1, 0, 0xFA}, /* RGBX8_SRGB */
    [0x38] = {0x58D24908u, 0, 1, 0, 0xD6}, /* RGBA8_SRGB */
    [0x39] = {0x58D24912u, 0, 0, 0, 0x00}, /* RGBA4 */
    [0x3A] = {0x78D24914u, 0, 0, 0, 0x00}, /* RGB5 */
    [0x3B] = {0x58D24914u, 0, 0, 0, 0x00}, /* RGB5A1 */
    [0x3C] = {0x78D24915u, 0, 0, 0, 0x00}, /* RGB565 */
    [0x3D] = {0x58D24909u, 0, 0, 0, 0xD1}, /* RGB10A2 */
    [0x3E] = {0x58D49209u, 1, 0, 0, 0xD2}, /* RGB10A2UI */
    [0x3F] = {0x78D7FFA1u, 0, 0, 0, 0xE0}, /* R11G11B10F */
    [0x40] = {0x78D7FFA0u, 0, 0, 0, 0x00}, /* RGB9E5F */
    [0x41] = {0x78D24924u, 0, 0, 0, 0x00}, /* RGB_DXT1 */
    [0x42] = {0x58D24924u, 0, 0, 0, 0x00}, /* RGBA_DXT1 */
    [0x43] = {0x58D24925u, 0, 0, 0, 0x00}, /* RGBA_DXT3 */
    [0x44] = {0x58D24926u, 0, 0, 0, 0x00}, /* RGBA_DXT5 */
    [0x45] = {0x78D24924u, 0, 1, 0, 0x00}, /* RGB_DXT1_SRGB */
    [0x46] = {0x58D24924u, 0, 1, 0, 0x00}, /* RGBA_DXT1_SRGB */
    [0x47] = {0x58D24925u, 0, 1, 0, 0x00}, /* RGBA_DXT3_SRGB */
    [0x48] = {0x58D24926u, 0, 1, 0, 0x00}, /* RGBA_DXT5_SRGB */
    [0x49] = {0x70124927u, 0, 0, 0, 0x00}, /* RGTC1_UNORM */
    [0x4A] = {0x701124A7u, 0, 0, 0, 0x00}, /* RGTC1_SNORM */
    [0x4B] = {0x70D24928u, 0, 0, 0, 0x00}, /* RGTC2_UNORM */
    [0x4C] = {0x70D124A8u, 0, 0, 0, 0x00}, /* RGTC2_SNORM */
    [0x4D] = {0x58D24917u, 0, 0, 0, 0x00}, /* BPTC_UNORM */
    [0x4E] = {0x58D24917u, 0, 1, 0, 0x00}, /* BPTC_UNORM_SRGB */
    [0x4F] = {0x78D7FF90u, 0, 0, 0, 0x00}, /* BPTC_SFLOAT */
    [0x50] = {0x78D7FF91u, 0, 0, 0, 0x00}, /* BPTC_UFLOAT */
    [0x6D] = {0x78D24908u, 0, 0, 0, 0xF9}, /* RGBX8 */
    [0x6E] = {0x78D12488u, 0, 0, 0, 0xD7}, /* RGBX8SN */
    [0x6F] = {0x68D49208u, 1, 0, 0, 0xD9}, /* RGBX8UI */
    [0x70] = {0x68D36D88u, 1, 0, 0, 0xD8}, /* RGBX8I */
    [0x71] = {0x78D7FF83u, 0, 0, 0, 0xCE}, /* RGBX16F */
    [0x72] = {0x78D24903u, 0, 0, 0, 0xC6}, /* RGBX16 */
    [0x73] = {0x78D12483u, 0, 0, 0, 0xC7}, /* RGBX16SN */
    [0x74] = {0x68D49203u, 1, 0, 0, 0xC9}, /* RGBX16UI */
    [0x75] = {0x68D36D83u, 1, 0, 0, 0xC8}, /* RGBX16I */
    [0x76] = {0x78D7FF81u, 0, 0, 0, 0xC3}, /* RGBX32F */
    [0x77] = {0x68D49201u, 1, 0, 0, 0xC5}, /* RGBX32UI */
    [0x78] = {0x68D36D81u, 1, 0, 0, 0xC4}, /* RGBX32I */
    [0x79] = {0x58D24940u, 0, 0, 0, 0x00}, /* RGBA_ASTC_4x4 */
    [0x7A] = {0x58D24950u, 0, 0, 0, 0x00}, /* RGBA_ASTC_5x4 */
    [0x7B] = {0x58D24941u, 0, 0, 0, 0x00}, /* RGBA_ASTC_5x5 */
    [0x7C] = {0x58D24951u, 0, 0, 0, 0x00}, /* RGBA_ASTC_6x5 */
    [0x7D] = {0x58D24942u, 0, 0, 0, 0x00}, /* RGBA_ASTC_6x6 */
    [0x7E] = {0x58D24955u, 0, 0, 0, 0x00}, /* RGBA_ASTC_8x5 */
    [0x7F] = {0x58D24952u, 0, 0, 0, 0x00}, /* RGBA_ASTC_8x6 */
    [0x80] = {0x58D24944u, 0, 0, 0, 0x00}, /* RGBA_ASTC_8x8 */
    [0x81] = {0x58D24956u, 0, 0, 0, 0x00}, /* RGBA_ASTC_10x5 */
    [0x82] = {0x58D24957u, 0, 0, 0, 0x00}, /* RGBA_ASTC_10x6 */
    [0x83] = {0x58D24953u, 0, 0, 0, 0x00}, /* RGBA_ASTC_10x8 */
    [0x84] = {0x58D24945u, 0, 0, 0, 0x00}, /* RGBA_ASTC_10x10 */
    [0x85] = {0x58D24954u, 0, 0, 0, 0x00}, /* RGBA_ASTC_12x10 */
    [0x86] = {0x58D24946u, 0, 0, 0, 0x00}, /* RGBA_ASTC_12x12 */
    [0x87] = {0x58D24940u, 0, 1, 0, 0x00}, /* RGBA_ASTC_4x4_SRGB */
    [0x88] = {0x58D24950u, 0, 1, 0, 0x00}, /* RGBA_ASTC_5x4_SRGB */
    [0x89] = {0x58D24941u, 0, 1, 0, 0x00}, /* RGBA_ASTC_5x5_SRGB */
    [0x8A] = {0x58D24951u, 0, 1, 0, 0x00}, /* RGBA_ASTC_6x5_SRGB */
    [0x8B] = {0x58D24942u, 0, 1, 0, 0x00}, /* RGBA_ASTC_6x6_SRGB */
    [0x8C] = {0x58D24955u, 0, 1, 0, 0x00}, /* RGBA_ASTC_8x5_SRGB */
    [0x8D] = {0x58D24952u, 0, 1, 0, 0x00}, /* RGBA_ASTC_8x6_SRGB */
    [0x8E] = {0x58D24944u, 0, 1, 0, 0x00}, /* RGBA_ASTC_8x8_SRGB */
    [0x8F] = {0x58D24956u, 0, 1, 0, 0x00}, /* RGBA_ASTC_10x5_SRGB */
    [0x90] = {0x58D24957u, 0, 1, 0, 0x00}, /* RGBA_ASTC_10x6_SRGB */
    [0x91] = {0x58D24953u, 0, 1, 0, 0x00}, /* RGBA_ASTC_10x8_SRGB */
    [0x92] = {0x58D24945u, 0, 1, 0, 0x00}, /* RGBA_ASTC_10x10_SRGB */
    [0x93] = {0x58D24954u, 0, 1, 0, 0x00}, /* RGBA_ASTC_12x10_SRGB */
    [0x94] = {0x58D24946u, 0, 1, 0, 0x00}, /* RGBA_ASTC_12x12_SRGB */
    [0x95] = {0x74E24915u, 0, 0, 0, 0xE8}, /* BGR565 */
    [0x96] = {0x74E24914u, 0, 0, 0, 0xF8}, /* BGR5 */
    [0x97] = {0x54E24914u, 0, 0, 0, 0xE9}, /* BGR5A1 */
    [0x98] = {0x272A4913u, 0, 0, 0, 0x00}, /* A1BGR5 */
    [0x99] = {0x74E24908u, 0, 0, 0, 0xE6}, /* BGRX8 */
    [0x9A] = {0x54E24908u, 0, 0, 0, 0xCF}, /* BGRA8 */
    [0x9B] = {0x74E24908u, 0, 1, 0, 0xE7}, /* BGRX8_SRGB */
    [0x9C] = {0x54E24908u, 0, 1, 0, 0xD0}, /* BGRA8_SRGB */
};

u32 nvn_rt_format(u32 format, int* depth) {
    const TicFormat* f = format < sizeof tic_formats / sizeof *tic_formats ? &tic_formats[format] : NULL;
    *depth = f && f->depth;
    return f ? f->rt : 0;
}

enum { SWZ_ZERO = 0, SWZ_R = 2, SWZ_G = 3, SWZ_B = 4, SWZ_A = 5, SWZ_ONE_INT = 6, SWZ_ONE_FLOAT = 7 };
enum { TIC_HDR_BUFFER = 0, TIC_HDR_PITCH = 2, TIC_HDR_BLOCK_LINEAR = 3 };
enum {
    TT_1D = 0, TT_2D = 1, TT_3D = 2, TT_CUBE = 3, TT_1D_ARRAY = 4, TT_2D_ARRAY = 5, TT_1D_BUFFER = 6,
    TT_2D_NO_MIPMAP = 7, TT_CUBE_ARRAY = 8
};

/* an NVN channel (ZERO, ONE, R, G, B, A) as the hardware's source: the format's own channel for R to A */
static u32 swizzle(const TicFormat* f, u32 s) {
    switch (s) {
        case 0: return SWZ_ZERO;
        case 1: return f->raw_int ? SWZ_ONE_INT : SWZ_ONE_FLOAT;
        case 2: return (f->word >> 19) & 7;
        case 3: return (f->word >> 22) & 7;
        case 4: return (f->word >> 25) & 7;
        case 5: return (f->word >> 28) & 7;
    }
    return SWZ_ZERO;
}

int nvn_tic_encode(const NvnTexture* t, const NvnView* v, int image, u32 out[8]) {
    u32 format = v && (v->set & NVN_VIEW_FORMAT) ? v->format : t->d.format;
    u32 target = v && (v->set & NVN_VIEW_TARGET) ? v->target : t->d.target, orig = t->d.target;
    const TicFormat* f;
    u32 sw[4], w = t->d.width, h = t->d.height, d = t->d.depth, layer = 0, i;
    u64 address = t->gpu;
    NvnTexLayout l;
    memset(out, 0, 8 * sizeof *out);
    if (format >= sizeof tic_formats / sizeof *tic_formats || !tic_formats[format].word) return 0;
    f = &tic_formats[format];
    nvn_tex_layout(&t->d, &l);
    /* the channels: the view's, else the texture's, else R G B A */
    if (v && (v->set & NVN_VIEW_SWIZZLE))
        for (i = 0; i < 4; i++) sw[i] = v->swizzle[i];
    else if (t->d.swizzle & NVN_TEX_SWIZZLE_SET)
        for (i = 0; i < 4; i++) sw[i] = (t->d.swizzle >> (3 * i)) & 7;
    else
        for (i = 0; i < 4; i++) sw[i] = 2 + i;
    out[0] = f->word & 0x7ffff;
    if (!f->depth)
        out[0] |= swizzle(f, sw[0]) << 19 | swizzle(f, sw[1]) << 22 | swizzle(f, sw[2]) << 25 | swizzle(f, sw[3]) << 28;
    else {
        /* depth in R (stencil in G) but for Z24 formats, which keep depth in G: the view's depth-stencil mode, else
           the texture's */
        int d24 = format == NVN_FORMAT_DEPTH24 || format == NVN_FORMAT_DEPTH24_STENCIL8;
        int stencil = v && (v->set & NVN_VIEW_DSMODE) ? (v->set >> 6) == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL
                                                      : (t->d.bits & NVN_TEX_STENCIL) != 0;
        u32 s = (d24 ^ stencil) ? SWZ_G : SWZ_R;
        /* depth as (d, 0, 0, 1), as the game's driver describes a depth texture; stencil in every channel, but a
           DEPTH24_STENCIL8 texture's stencil, which the driver describes as R8G24 with an integer R (stencil, 0, 0,
           1): the renderer reads a target's stencil through that format */
        if (stencil && format == NVN_FORMAT_DEPTH24_STENCIL8)
            out[0] = 0x24a0eu | (u32)SWZ_R << 19 | (u32)SWZ_ZERO << 22 | (u32)SWZ_ZERO << 25 | (u32)SWZ_ONE_INT << 28;
        else if (stencil)
            out[0] |= s << 19 | s << 22 | s << 25 | s << 28;
        else
            out[0] |= s << 19 | (u32)SWZ_ZERO << 22 | (u32)SWZ_ZERO << 25 | (u32)SWZ_ONE_FLOAT << 28;
    }
    out[4] |= (u32)f->srgb << 22;
    if (target == NVN_TEXTURE_TARGET_BUFFER) {
        out[2] |= (u32)TIC_HDR_BUFFER << 21;
        out[4] |= (u32)TT_1D_BUFFER << 23;
        out[3] |= ((w - 1) >> 16) & 0xffff;
        out[4] |= (w - 1) & 0xffff;
    } else {
        int rect = target == NVN_TEXTURE_TARGET_RECTANGLE;
        if (t->d.flags & NVN_TEX_LINEAR) {
            int bw, bh, bpb = nvn_fmt_bpb((int)t->d.format, &bw, &bh);
            u32 align = (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) ? 128 : 32;
            u32 pitch = t->d.stride ? t->d.stride : ((u32)bpb * ((w + (u32)bw - 1) / (u32)bw) + align - 1) & ~(align - 1);
            out[2] |= (u32)TIC_HDR_PITCH << 21;
            out[4] |= (u32)TT_2D_NO_MIPMAP << 23 | 7u << 29; /* border: the sampler's color */
            out[3] |= (pitch >> 5) & 0xffff;
        } else {
            static const u8 type[11] = {TT_1D,       TT_2D, TT_3D,   TT_1D_ARRAY, TT_2D_ARRAY,  TT_2D,
                                        TT_2D_ARRAY, TT_2D, TT_CUBE, TT_CUBE_ARRAY, TT_1D};
            u32 tt = target < 11 ? type[target] : TT_2D;
            if (image && (tt == TT_CUBE || tt == TT_CUBE_ARRAY)) tt = TT_2D_ARRAY;
            out[2] |= (u32)TIC_HDR_BLOCK_LINEAR << 21 | (u32)(image != 0) << 24;
            out[4] |= tt << 23 | 1u << 27 | 7u << 29;
            out[3] |= (l.tile_h & 7) << 3 | (l.tile_d & 7) << 6 | 1u << 16 | 1u << 17 | 1u << 18;
            out[6] |= 2u << 23 | 1u << 25; /* aniso fine spread: two, coarse: one */
        }
        out[5] |= (u32)!rect << 31;
        out[7] |= nvn_tex_msaa(t->d.samples) << 8;
        /* the layers: an array's count is its depth (a 1D array's its height, a cube array's in faces) */
        if (orig == NVN_TEXTURE_TARGET_1D_ARRAY) d = h;
        if (orig == NVN_TEXTURE_TARGET_1D_ARRAY || orig == NVN_TEXTURE_TARGET_2D_ARRAY ||
            orig == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY || orig == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
            if (v && (v->set & NVN_VIEW_LAYERS)) layer = v->min_layer;
            address += (u64)layer * l.layer_size;
        }
        if (target == NVN_TEXTURE_TARGET_1D || target == NVN_TEXTURE_TARGET_1D_ARRAY) h = 1;
        if (target == NVN_TEXTURE_TARGET_1D_ARRAY || target == NVN_TEXTURE_TARGET_2D_ARRAY ||
            target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
            u32 layers = v && (v->set & NVN_VIEW_LAYERS) && v->layers ? v->layers : d - layer;
            d = target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && !image ? layers / 6 : layers;
        } else if (target == NVN_TEXTURE_TARGET_CUBEMAP)
            d = image ? 6 : 1;
        else if (target != NVN_TEXTURE_TARGET_3D)
            d = 1;
        out[4] |= (w - 1) & 0xffff;
        out[5] |= ((h - 1) & 0xffff) | ((d - 1) & 0x3fff) << 16;
        {
            u32 maxl = t->d.levels >= 16 ? 15 : t->d.levels ? t->d.levels - 1u : 0, lo = 0, hi;
            if (v && (v->set & NVN_VIEW_LEVELS)) lo = v->base_level > maxl ? maxl : v->base_level;
            hi = v && (v->set & NVN_VIEW_LEVELS) && v->levels ? lo + v->levels - 1u : maxl;
            if (hi > maxl) hi = maxl;
            out[3] |= maxl << 28;
            out[7] |= lo | hi << 4;
        }
    }
    out[1] = (u32)address;
    out[2] |= (u32)(address >> 32) & 0xffff;
    out[2] |= ((layer >> 3) & 0x1f) << 16 | ((layer >> 8) & 7) << 29;
    out[4] |= (layer & 7) << 16;
    return 1;
}

/* ---- samplers ---- */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* sRGB's encoding of a linear value, 8 bits (deko3d's, after rygorous) */
static u32 srgb8(float f) {
    float s;
    if (!(f > 0.0f))
        s = 0.0f;
    else if (f <= 0.0031308f)
        s = 12.92f * f;
    else if (f < 1.0f)
        s = 1.055f * powf(f, 1.0f / 2.4f) - 0.055f;
    else
        s = 1.0f;
    return (u32)(s * 255.0f + 0.5f);
}
void nvn_tsc_encode(const NvnSamplerPar* p, u32 out[8]) {
    /* NVN's wrap modes (CLAMP, REPEAT, MIRROR_CLAMP, MIRROR_CLAMP_TO_EDGE, MIRROR_CLAMP_TO_BORDER, CLAMP_TO_BORDER,
       MIRRORED_REPEAT, CLAMP_TO_EDGE) as the hardware's */
    static const u8 wrap[8] = {4, 0, 7, 5, 6, 3, 1, 2};
    /* NVN's min filters (NEAREST, LINEAR, then NEAREST and LINEAR with MIPMAP_NEAREST and MIPMAP_LINEAR) as the
       hardware's min filter (1 nearest, 2 linear) and mip filter (1 none, 2 nearest, 3 linear) */
    static const u8 minf[6] = {1, 2, 1, 2, 1, 2}, mipf[6] = {1, 1, 2, 2, 3, 3};
    static const u8 aniso[16] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7};
    float lo = clampf(p->lod_min, 0.0f, 15.0f), hi = clampf(p->lod_max, 0.0f, 15.0f);
    u32 bias = (u32)(i32)(clampf(p->lod_bias, -15.0f, 15.0f) * 256.0f) & 0x1fff, i;
    u32 a = (u32)(clampf(p->anisotropy, 1.0f, 16.0f) + 0.5f) - 1;
    u32 m = p->min_filter < 6 ? p->min_filter : 0;
    if (hi < lo) hi = lo;
    memset(out, 0, 8 * sizeof *out);
    out[0] = (u32)wrap[p->wrap_s & 7] | (u32)wrap[p->wrap_t & 7] << 3 | (u32)wrap[p->wrap_r & 7] << 6 |
             (u32)(p->compare_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) << 9 |
             (u32)((p->compare_func ? p->compare_func - 1u : 0) & 7) << 10 | 1u << 13 | 1u << 14 | 1u << 17 |
             (u32)aniso[a & 15] << 20;
    out[1] = (p->mag_filter == NVN_MAG_FILTER_LINEAR ? 2u : 1u) | (u32)minf[m] << 4 | (u32)mipf[m] << 6 | 1u << 8 |
             1u << 9 | ((u32)p->reduction & 3) << 10 | bias << 12;
    out[2] = ((u32)(lo * 256.0f) & 0xfff) | ((u32)(hi * 256.0f) & 0xfff) << 12 | srgb8(p->border[0]) << 24;
    out[3] = srgb8(p->border[1]) << 12 | srgb8(p->border[2]) << 20;
    for (i = 0; i < 4; i++) memcpy(&out[4 + i], &p->border[i], 4);
}
