/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's TIC and TSC encoders (gpu/nvn/nvn_tic.c) read back through the decoders the renderer makes its textures
   and samplers from (gpu/texture/descriptor.h): every NVN texture format two games initialize, the targets, views,
   linear and buffer textures, and the sampler state. The format word is held to a table of the hardware's own format
   codes, written out here from the Maxwell texture formats (envytools), with their texel blocks and component types,
   apart from the encoder's table it checks */
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/texture/descriptor.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
static void check(int ok, const char* what, unsigned format) {
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL: %s (format %#x)\n", what, format);
        failures++;
    }
}

/* the hardware's format codes: bytes per block and block size */
static int code_block(u32 code, u32* bw, u32* bh) {
    static const u8 astc[0x58][2] = {[0x40] = {4, 4},  [0x41] = {5, 5},   [0x42] = {6, 6},   [0x44] = {8, 8},
                                     [0x45] = {10, 10}, [0x46] = {12, 12}, [0x50] = {5, 4},   [0x51] = {6, 5},
                                     [0x52] = {8, 6},  [0x53] = {10, 8},  [0x54] = {12, 10}, [0x55] = {8, 5},
                                     [0x56] = {10, 5}, [0x57] = {10, 6}};
    *bw = *bh = 1;
    if (code < 0x58 && astc[code][0]) {
        *bw = astc[code][0];
        *bh = astc[code][1];
        return 16;
    }
    switch (code) {
        case 0x01: return 16; /* R32_G32_B32_A32 */
        case 0x02: return 12; /* R32_G32_B32 */
        case 0x03:            /* R16_G16_B16_A16 */
        case 0x04: return 8;  /* R32_G32 */
        case 0x07:            /* X8B8G8R8 */
        case 0x08:            /* A8R8G8B8 */
        case 0x09:            /* A2B10G10R10 */
        case 0x0C:            /* R16_G16 */
        case 0x0F:            /* R32 */
        case 0x20:            /* E5B9G9R9 */
        case 0x21:            /* BF10GF11RF11 */
        case 0x29:            /* Z24S8 */
        case 0x2A:            /* X8Z24 */
        case 0x2F: return 4;  /* ZF32 */
        case 0x30: return 8;  /* ZF32_X24S8 */
        case 0x12:            /* A4B4G4R4 */
        case 0x13:            /* A5B5G5R1 */
        case 0x14:            /* A1B5G5R5 */
        case 0x15:            /* B5G6R5 */
        case 0x18:            /* G8R8 */
        case 0x1B:            /* R16 */
        case 0x3A: return 2;  /* Z16 */
        case 0x1D: return 1;  /* R8 */
        case 0x24:            /* DXT1 */
        case 0x27: *bw = *bh = 4; return 8; /* DXN1 */
        case 0x10:            /* BC6H_SF16 */
        case 0x11:            /* BC6H_UF16 */
        case 0x17:            /* BC7U */
        case 0x25:            /* DXT23 */
        case 0x26:            /* DXT45 */
        case 0x28: *bw = *bh = 4; return 16; /* DXN2 */
    }
    return 0;
}

/* the formats two games initialize: NVN's code, the hardware's, and the red component's type (1 snorm, 2 unorm,
   3 sint, 4 uint, 7 float; Z24S8's red is its stencil, an integer) */
static const struct {
    u16 nvn;
    u8 code, comp;
} formats[] = {
    {0x01, 0x1D, 2}, {0x05, 0x1B, 7}, {0x06, 0x1B, 2}, {0x0A, 0x0F, 7}, {0x0B, 0x0F, 4}, {0x0D, 0x18, 2},
    {0x11, 0x0C, 7}, {0x12, 0x0C, 2}, {0x17, 0x04, 4}, {0x25, 0x08, 2}, {0x26, 0x08, 1}, {0x29, 0x03, 7},
    {0x2A, 0x03, 2}, {0x2E, 0x01, 7}, {0x32, 0x3A, 2}, {0x34, 0x2F, 7}, {0x35, 0x29, 4}, {0x38, 0x08, 2},
    {0x3B, 0x14, 2}, {0x3C, 0x15, 2}, {0x3D, 0x09, 2}, {0x3F, 0x21, 7}, {0x42, 0x24, 2}, {0x44, 0x26, 2},
    {0x46, 0x24, 2}, {0x48, 0x26, 2}, {0x49, 0x27, 2}, {0x4B, 0x28, 2}, {0x4C, 0x28, 1}, {0x4D, 0x17, 2},
    {0x4E, 0x17, 2}, {0x50, 0x11, 7}, {0x6D, 0x08, 2}, {0x79, 0x40, 2}, {0x7F, 0x52, 2}, {0x80, 0x44, 2},
    {0x82, 0x57, 2}, {0x83, 0x53, 2}, {0x84, 0x45, 2}, {0x85, 0x54, 2}, {0x86, 0x46, 2}, {0x87, 0x40, 2},
    {0x88, 0x50, 2}, {0x8C, 0x55, 2}, {0x8D, 0x52, 2}, {0x8E, 0x44, 2},
};
static int is_srgb(u32 f) { return f == 0x38 || f == 0x46 || f == 0x48 || f == 0x4E || (f >= 0x87 && f <= 0x94); }
static int is_depth(u32 f) { return f >= 0x31 && f <= 0x36; }

static NvnTexture texture(u32 format, u32 target, u32 w, u32 h, u32 d, u32 levels) {
    NvnTexture t;
    memset(&t, 0, sizeof t);
    t.d.format = (u16)format;
    t.d.target = (u8)target;
    t.d.width = w;
    t.d.height = h;
    t.d.depth = d;
    t.d.levels = (u8)levels;
    t.gpu = 0x4084a0000ull;
    return t;
}

static void check_formats(void) {
    unsigned i;
    for (i = 0; i < sizeof formats / sizeof *formats; i++) {
        u32 f = formats[i].nvn, tic[8], bw, bh, cbw, cbh;
        int bpb, nbw, nbh;
        TicFields d;
        NvnTexture t = texture(f, NVN_TEXTURE_TARGET_2D, 300, 200, 1, 5);
        NvnTexLayout l;
        check(nvn_tic_encode(&t, NULL, 0, tic), "a descriptor for the format", f);
        tic_decode(tic, &d);
        nvn_tex_layout(&t.d, &l);
        check((d.format & 0x7F) == formats[i].code, "the hardware's format code", f);
        check(((d.format >> 7) & 7) == formats[i].comp, "the component type", f);
        bpb = code_block(d.format & 0x7F, &cbw, &cbh);
        nbw = nbh = 0;
        check(bpb && bpb == nvn_fmt_bpb((int)f, &nbw, &nbh), "the texel block's bytes", f);
        bw = (u32)nbw;
        bh = (u32)nbh;
        check(cbw == bw && cbh == bh, "the texel block's size", f);
        check(d.srgb == (u32)is_srgb(f), "sRGB", f);
        check(d.address == t.gpu && d.header == 3 && d.type == 1, "a 2D block-linear texture at its address", f);
        check(d.width == 300 && d.height == 200 && d.depth == 1, "its size", f);
        check(d.levels == 5 && d.base_level == 0 && d.max_level == 4, "its levels", f);
        check(d.block_h == l.tile_h && d.block_d == l.tile_d, "the block height the storage size assumed", f);
        if (!is_depth(f)) /* R G B A shows the format's own channels */
            check(((d.format >> 19) & 7) == ((tic[0] >> 19) & 7) && ((d.format >> 19) & 7) == 2,
                  "red reads the red channel", f);
    }
}

static void check_targets(void) {
    u32 tic[8];
    TicFields d;
    NvnTexture t;
    NvnView v;
    NvnTexLayout l;
    /* a 2D array: its layers in the depth; a view's layers move the address by whole layers */
    t = texture(0x25, NVN_TEXTURE_TARGET_2D_ARRAY, 256, 128, 6, 3);
    nvn_tex_layout(&t.d, &l);
    nvn_tic_encode(&t, NULL, 0, tic);
    tic_decode(tic, &d);
    check(d.type == 5 && d.depth == 6 && d.width == 256 && d.height == 128, "a 2D array's layers", 0x25);
    memset(&v, 0, sizeof v);
    v.set = NVN_VIEW_LAYERS | NVN_VIEW_LEVELS;
    v.min_layer = 2;
    v.layers = 3;
    v.base_level = 1;
    v.levels = 1;
    nvn_tic_encode(&t, &v, 0, tic);
    tic_decode(tic, &d);
    check(d.address == t.gpu + 2 * l.layer_size && d.depth == 3, "a view of layers 2 to 4", 0x25);
    check(d.base_level == 1 && d.max_level == 1 && d.levels == 3, "a view of level 1", 0x25);
    /* a view as another target and format */
    v.set = NVN_VIEW_TARGET | NVN_VIEW_FORMAT;
    v.target = NVN_TEXTURE_TARGET_2D;
    v.format = 0x38;
    nvn_tic_encode(&t, &v, 0, tic);
    tic_decode(tic, &d);
    check(d.type == 1 && d.srgb && d.depth == 1, "a view as 2D in sRGB", 0x25);
    /* a cube map: 6 faces, one cube; a cube array counts its faces; as images, layers */
    t = texture(0x29, NVN_TEXTURE_TARGET_CUBEMAP, 64, 64, 6, 1);
    nvn_tic_encode(&t, NULL, 0, tic);
    tic_decode(tic, &d);
    check(d.type == 3 && d.depth == 1, "a cube map", 0x29);
    nvn_tic_encode(&t, NULL, 1, tic);
    tic_decode(tic, &d);
    check(d.type == 5 && d.depth == 6, "a cube map as an image: its 6 layers", 0x29);
    t = texture(0x29, NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 64, 64, 12, 1);
    nvn_tic_encode(&t, NULL, 0, tic);
    tic_decode(tic, &d);
    check(d.type == 8 && d.depth == 2, "a cube array of 12 faces: 2 cubes", 0x29);
    /* 3D: its depth, and its block depth */
    t = texture(0x25, NVN_TEXTURE_TARGET_3D, 32, 32, 32, 1);
    nvn_tex_layout(&t.d, &l);
    nvn_tic_encode(&t, NULL, 0, tic);
    tic_decode(tic, &d);
    check(d.type == 2 && d.depth == 32 && d.block_d == l.tile_d, "a 3D texture", 0x25);
    /* a pitch-linear texture: its stride */
    t = texture(0x25, NVN_TEXTURE_TARGET_2D, 100, 50, 1, 1);
    t.d.flags = NVN_TEXTURE_FLAGS_LINEAR;
    t.d.stride = 512;
    nvn_tic_encode(&t, NULL, 0, tic);
    tic_decode(tic, &d);
    check(d.header == 2 && d.type == 7 && d.pitch == 512 && d.width == 100, "a pitch-linear texture", 0x25);
    /* a buffer texture: its texels across words 3 and 4 */
    t = texture(0x0A, NVN_TEXTURE_TARGET_BUFFER, 100000, 1, 1, 1);
    nvn_tic_encode(&t, NULL, 0, tic);
    tic_decode(tic, &d);
    check(d.header == 0 && d.type == 6 && d.buffer_width == 100000, "a buffer texture", 0x0A);
    /* depth reads red (Z24S8's in green) */
    t = texture(0x35, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 1);
    nvn_tic_encode(&t, NULL, 0, tic);
    check(((tic[0] >> 19) & 7) == 3 && ((tic[0] >> 28) & 7) == 7, "Z24S8's depth in green, alpha one", 0x35);
    t.d.bits |= NVN_TEX_STENCIL;
    nvn_tic_encode(&t, NULL, 0, tic);
    /* its stencil as the game's driver describes it: R8G24 (0x0E), an integer R, (stencil, 0, 0, 1) */
    check((tic[0] & 0x7fffffffu) == 0x60124a0eu, "Z24S8's stencil as R8G24", 0x35);
    /* a swizzle: B G R ONE of RGBA8 */
    t = texture(0x25, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 1);
    t.d.swizzle = (u16)(NVN_TEX_SWIZZLE_SET | 4 | 3 << 3 | 2 << 6 | 1 << 9);
    nvn_tic_encode(&t, NULL, 0, tic);
    check(((tic[0] >> 19) & 7) == 4 && ((tic[0] >> 22) & 7) == 3 && ((tic[0] >> 25) & 7) == 2 &&
              ((tic[0] >> 28) & 7) == 7,
          "a swizzle B G R ONE", 0x25);
    /* a format the encoder has no entry for: an empty descriptor */
    t = texture(0x19, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 1);
    check(!nvn_tic_encode(&t, NULL, 0, tic) && !tic[0] && !tic[1], "RGB8: no descriptor", 0x19);
}

static void check_samplers(void) {
    NvnSamplerPar p;
    u32 tsc[8];
    TscFields s;
    float b;
    memset(&p, 0, sizeof p);
    p.min_filter = NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR;
    p.mag_filter = NVN_MAG_FILTER_LINEAR;
    p.wrap_s = NVN_WRAP_MODE_REPEAT;
    p.wrap_t = NVN_WRAP_MODE_CLAMP_TO_EDGE;
    p.wrap_r = NVN_WRAP_MODE_MIRRORED_REPEAT;
    p.compare_mode = NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE;
    p.compare_func = NVN_COMPARE_FUNC_LEQUAL;
    p.reduction = NVN_SAMPLER_REDUCTION_MAX;
    p.lod_min = 0.5f;
    p.lod_max = 9.25f;
    p.lod_bias = -1.5f;
    p.anisotropy = 8.0f;
    p.border[0] = 1.0f;
    p.border[3] = 0.25f;
    nvn_tsc_encode(&p, tsc);
    tsc_decode(tsc, &s);
    check(s.mag == 2 && s.min == 2 && s.mip == 3, "trilinear filtering", 0);
    check(s.wrap_u == 0 && s.wrap_v == 2 && s.wrap_p == 1, "repeat, clamp to edge, mirrored repeat", 0);
    check(s.compare == 1 && s.compare_op == 3, "a LEQUAL compare (VkCompareOp 3)", 0);
    check(s.reduction == 2, "a max reduction", 0);
    check(s.min_lod == 128 && s.max_lod == 9 * 256 + 64 && s.lod_bias == -384, "the LOD clamp and bias", 0);
    check(s.anisotropy == 4, "8x anisotropy", 0);
    memcpy(&b, &s.border[3], 4);
    check(b == 0.25f && s.border[0] == 0x3f800000u, "the border color", 0);
    p.min_filter = NVN_MIN_FILTER_NEAREST;
    p.mag_filter = NVN_MAG_FILTER_NEAREST;
    p.wrap_s = NVN_WRAP_MODE_CLAMP_TO_BORDER;
    p.wrap_t = NVN_WRAP_MODE_CLAMP;
    p.wrap_r = NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE;
    p.compare_mode = NVN_COMPARE_MODE_NONE;
    nvn_tsc_encode(&p, tsc);
    tsc_decode(tsc, &s);
    check(s.mag == 1 && s.min == 1 && s.mip == 1 && !s.compare, "nearest without mipmaps", 0);
    check(s.wrap_u == 3 && s.wrap_v == 4 && s.wrap_p == 5, "border, clamp, mirror once to edge", 0);
}

int main(void) {
    check_formats();
    check_targets();
    check_samplers();
    if (!failures) printf("NVN's TIC and TSC entries decode as the renderer reads them (%d checks)\n", checks);
    return failures ? 1 : 0;
}
