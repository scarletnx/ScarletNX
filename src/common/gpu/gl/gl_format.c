/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's formats (gl_format.h) */
#include "common/gpu/gl/gl_format.h"
#include <stddef.h>

enum {
    GL_ALPHA_ = 0x1906,
    GL_BGRA_EXT_ = 0x80e1,
    GL_BYTE_ = 0x1400,
    GL_DEPTH_COMPONENT_ = 0x1902,
    GL_DEPTH_STENCIL_ = 0x84f9,
    GL_F32_UI_24_8_ = 0x8dad,
    GL_FLOAT_ = 0x1406,
    GL_HALF_FLOAT_ = 0x140b,
    GL_HALF_FLOAT_OES_ = 0x8d61,
    GL_INT_ = 0x1404,
    GL_LUMINANCE_ = 0x1909,
    GL_LUMINANCE_ALPHA_ = 0x190a,
    GL_RED_ = 0x1903,
    GL_RED_INTEGER_ = 0x8d94,
    GL_RGBA_ = 0x1908,
    GL_RGBA_INTEGER_ = 0x8d99,
    GL_RGB_ = 0x1907,
    GL_RGB_INTEGER_ = 0x8d98,
    GL_RG_ = 0x8227,
    GL_RG_INTEGER_ = 0x8228,
    GL_SHORT_ = 0x1402,
    GL_UI_10F11F11F_ = 0x8c3b,
    GL_UI_2101010_ = 0x8368,
    GL_UI_24_8_ = 0x84fa,
    GL_UI_5999_ = 0x8c3e,
    GL_UNSIGNED_BYTE_ = 0x1401,
    GL_UNSIGNED_INT_ = 0x1405,
    GL_UNSIGNED_SHORT_ = 0x1403,
    GL_US_4444_ = 0x8033,
    GL_US_5551_ = 0x8034,
    GL_US_565_ = 0x8363,
};

/* internal format, client format and type, client bytes, expand, kind, block */
static const SnxGlFormat formats[] = {
    /* GLES 2's unsized formats, as their format and type */
    {GL_RGBA_, GL_RGBA_, GL_UNSIGNED_BYTE_, 4, 0, SNX_GL_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_UNSIGNED_BYTE_, 3, 1, SNX_GL_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_US_4444_, 2, 0, SNX_GL_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_US_5551_, 2, 0, SNX_GL_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_US_565_, 2, 0, SNX_GL_FLOAT, 0},
    {GL_LUMINANCE_, GL_LUMINANCE_, GL_UNSIGNED_BYTE_, 1, 0, SNX_GL_FLOAT, 0},
    {GL_ALPHA_, GL_ALPHA_, GL_UNSIGNED_BYTE_, 1, 0, SNX_GL_FLOAT, 0},
    {GL_LUMINANCE_ALPHA_, GL_LUMINANCE_ALPHA_, GL_UNSIGNED_BYTE_, 2, 0, SNX_GL_FLOAT, 0},
    {GL_BGRA_EXT_, GL_BGRA_EXT_, GL_UNSIGNED_BYTE_, 4, 0, SNX_GL_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_HALF_FLOAT_OES_, 8, 0, SNX_GL_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_HALF_FLOAT_, 8, 0, SNX_GL_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_FLOAT_, 16, 0, SNX_GL_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_HALF_FLOAT_OES_, 6, 2, SNX_GL_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_HALF_FLOAT_, 6, 2, SNX_GL_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_FLOAT_, 12, 4, SNX_GL_FLOAT, 0},
    {GL_LUMINANCE_, GL_LUMINANCE_, GL_HALF_FLOAT_OES_, 2, 0, SNX_GL_FLOAT, 0},
    {GL_LUMINANCE_, GL_LUMINANCE_, GL_FLOAT_, 4, 0, SNX_GL_FLOAT, 0},
    {GL_ALPHA_, GL_ALPHA_, GL_HALF_FLOAT_OES_, 2, 0, SNX_GL_FLOAT, 0},
    {GL_ALPHA_, GL_ALPHA_, GL_FLOAT_, 4, 0, SNX_GL_FLOAT, 0},
    {GL_LUMINANCE_ALPHA_, GL_LUMINANCE_ALPHA_, GL_HALF_FLOAT_OES_, 4, 0, SNX_GL_FLOAT, 0},
    {GL_LUMINANCE_ALPHA_, GL_LUMINANCE_ALPHA_, GL_FLOAT_, 8, 0, SNX_GL_FLOAT, 0},
    {GL_RED_, GL_RED_, GL_UNSIGNED_BYTE_, 1, 0, SNX_GL_FLOAT, 0},
    {GL_RG_, GL_RG_, GL_UNSIGNED_BYTE_, 2, 0, SNX_GL_FLOAT, 0},
    {GL_DEPTH_COMPONENT_, GL_DEPTH_COMPONENT_, GL_UNSIGNED_SHORT_, 2, 0, SNX_GL_DEPTH, 0},
    {GL_DEPTH_COMPONENT_, GL_DEPTH_COMPONENT_, GL_UNSIGNED_INT_, 4, 0, SNX_GL_DEPTH, 0},
    {GL_DEPTH_STENCIL_, GL_DEPTH_STENCIL_, GL_UI_24_8_, 4, 0, SNX_GL_DS, 0},
    /* GLES 3's sized formats */
    {0x8229, GL_RED_, GL_UNSIGNED_BYTE_, 1, 0, SNX_GL_FLOAT, 0},
    {0x8f94, GL_RED_, GL_BYTE_, 1, 0, SNX_GL_FLOAT, 0},
    {0x822d, GL_RED_, GL_HALF_FLOAT_, 2, 0, SNX_GL_FLOAT, 0},
    {0x822e, GL_RED_, GL_FLOAT_, 4, 0, SNX_GL_FLOAT, 0},
    {0x8232, GL_RED_INTEGER_, GL_UNSIGNED_BYTE_, 1, 0, SNX_GL_UINT, 0},
    {0x8231, GL_RED_INTEGER_, GL_BYTE_, 1, 0, SNX_GL_INT, 0},
    {0x8234, GL_RED_INTEGER_, GL_UNSIGNED_SHORT_, 2, 0, SNX_GL_UINT, 0},
    {0x8233, GL_RED_INTEGER_, GL_SHORT_, 2, 0, SNX_GL_INT, 0},
    {0x8236, GL_RED_INTEGER_, GL_UNSIGNED_INT_, 4, 0, SNX_GL_UINT, 0},
    {0x8235, GL_RED_INTEGER_, GL_INT_, 4, 0, SNX_GL_INT, 0},
    {0x822b, GL_RG_, GL_UNSIGNED_BYTE_, 2, 0, SNX_GL_FLOAT, 0},
    {0x8f95, GL_RG_, GL_BYTE_, 2, 0, SNX_GL_FLOAT, 0},
    {0x822f, GL_RG_, GL_HALF_FLOAT_, 4, 0, SNX_GL_FLOAT, 0},
    {0x8230, GL_RG_, GL_FLOAT_, 8, 0, SNX_GL_FLOAT, 0},
    {0x8238, GL_RG_INTEGER_, GL_UNSIGNED_BYTE_, 2, 0, SNX_GL_UINT, 0},
    {0x8237, GL_RG_INTEGER_, GL_BYTE_, 2, 0, SNX_GL_INT, 0},
    {0x823a, GL_RG_INTEGER_, GL_UNSIGNED_SHORT_, 4, 0, SNX_GL_UINT, 0},
    {0x8239, GL_RG_INTEGER_, GL_SHORT_, 4, 0, SNX_GL_INT, 0},
    {0x823c, GL_RG_INTEGER_, GL_UNSIGNED_INT_, 8, 0, SNX_GL_UINT, 0},
    {0x823b, GL_RG_INTEGER_, GL_INT_, 8, 0, SNX_GL_INT, 0},
    {0x8051, GL_RGB_, GL_UNSIGNED_BYTE_, 3, 1, SNX_GL_FLOAT, 0},
    {0x8c41, GL_RGB_, GL_UNSIGNED_BYTE_, 3, 1, SNX_GL_FLOAT, 0},
    {0x8d62, GL_RGB_, GL_US_565_, 2, 0, SNX_GL_FLOAT, 0},
    {0x8f96, GL_RGB_, GL_BYTE_, 3, 1, SNX_GL_FLOAT, 0},
    {0x8c3a, GL_RGB_, GL_UI_10F11F11F_, 4, 0, SNX_GL_FLOAT, 0},
    {0x8c3d, GL_RGB_, GL_UI_5999_, 4, 0, SNX_GL_FLOAT, 0},
    {0x881b, GL_RGB_, GL_HALF_FLOAT_, 6, 2, SNX_GL_FLOAT, 0},
    {0x8815, GL_RGB_, GL_FLOAT_, 12, 4, SNX_GL_FLOAT, 0},
    {0x8d7d, GL_RGB_INTEGER_, GL_UNSIGNED_BYTE_, 3, 1, SNX_GL_UINT, 0},
    {0x8d8f, GL_RGB_INTEGER_, GL_BYTE_, 3, 1, SNX_GL_INT, 0},
    {0x8d77, GL_RGB_INTEGER_, GL_UNSIGNED_SHORT_, 6, 2, SNX_GL_UINT, 0},
    {0x8d89, GL_RGB_INTEGER_, GL_SHORT_, 6, 2, SNX_GL_INT, 0},
    {0x8d71, GL_RGB_INTEGER_, GL_UNSIGNED_INT_, 12, 4, SNX_GL_UINT, 0},
    {0x8d83, GL_RGB_INTEGER_, GL_INT_, 12, 4, SNX_GL_INT, 0},
    {0x8058, GL_RGBA_, GL_UNSIGNED_BYTE_, 4, 0, SNX_GL_FLOAT, 0},
    {0x8c43, GL_RGBA_, GL_UNSIGNED_BYTE_, 4, 0, SNX_GL_FLOAT, 0},
    {0x8f97, GL_RGBA_, GL_BYTE_, 4, 0, SNX_GL_FLOAT, 0},
    {0x8057, GL_RGBA_, GL_US_5551_, 2, 0, SNX_GL_FLOAT, 0},
    {0x8056, GL_RGBA_, GL_US_4444_, 2, 0, SNX_GL_FLOAT, 0},
    {0x8059, GL_RGBA_, GL_UI_2101010_, 4, 0, SNX_GL_FLOAT, 0},
    {0x881a, GL_RGBA_, GL_HALF_FLOAT_, 8, 0, SNX_GL_FLOAT, 0},
    {0x8814, GL_RGBA_, GL_FLOAT_, 16, 0, SNX_GL_FLOAT, 0},
    {0x8d7c, GL_RGBA_INTEGER_, GL_UNSIGNED_BYTE_, 4, 0, SNX_GL_UINT, 0},
    {0x8d8e, GL_RGBA_INTEGER_, GL_BYTE_, 4, 0, SNX_GL_INT, 0},
    {0x906f, GL_RGBA_INTEGER_, GL_UI_2101010_, 4, 0, SNX_GL_UINT, 0},
    {0x8d76, GL_RGBA_INTEGER_, GL_UNSIGNED_SHORT_, 8, 0, SNX_GL_UINT, 0},
    {0x8d88, GL_RGBA_INTEGER_, GL_SHORT_, 8, 0, SNX_GL_INT, 0},
    {0x8d70, GL_RGBA_INTEGER_, GL_UNSIGNED_INT_, 16, 0, SNX_GL_UINT, 0},
    {0x8d82, GL_RGBA_INTEGER_, GL_INT_, 16, 0, SNX_GL_INT, 0},
    {0x93a1, GL_BGRA_EXT_, GL_UNSIGNED_BYTE_, 4, 0, SNX_GL_FLOAT, 0},
    {0x81a5, GL_DEPTH_COMPONENT_, GL_UNSIGNED_SHORT_, 2, 0, SNX_GL_DEPTH, 0},
    {0x81a6, GL_DEPTH_COMPONENT_, GL_UNSIGNED_INT_, 4, 0, SNX_GL_DEPTH, 0},
    {0x8cac, GL_DEPTH_COMPONENT_, GL_FLOAT_, 4, 0, SNX_GL_DEPTH, 0},
    {0x88f0, GL_DEPTH_STENCIL_, GL_UI_24_8_, 4, 0, SNX_GL_DS, 0},
    {0x8cad, GL_DEPTH_STENCIL_, GL_F32_UI_24_8_, 8, 0, SNX_GL_DS, 0},
    {0x8d48, 0x1901, GL_UNSIGNED_BYTE_, 1, 0, SNX_GL_STENCIL, 0},
    /* compressed: the device's own formats, the bytes of a block, its width and height */
    {0x8d64, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x9274, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x9275, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x9276, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x9277, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x9278, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44},
    {0x9279, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44},
    {0x9270, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x9271, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x9272, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44},
    {0x9273, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44},
    /* the BCn formats (S3TC, RGTC, BPTC): a desktop GL context's, as a console game compresses its textures */
    {0x83f0, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},  /* RGB S3TC DXT1, BC1 */
    {0x83f1, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},  /* RGBA DXT1 */
    {0x83f2, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44}, /* DXT3, BC2 */
    {0x83f3, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44}, /* DXT5, BC3 */
    {0x8c4c, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},  /* their sRGB forms */
    {0x8c4d, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x8c4e, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44},
    {0x8c4f, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44},
    {0x8dbb, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44}, /* RGTC1, BC4 */
    {0x8dbc, 0, 0, 8, 0, SNX_GL_FLOAT, 0x44},
    {0x8dbd, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44}, /* RGTC2, BC5 */
    {0x8dbe, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44},
    {0x8e8c, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44}, /* BPTC, BC7 */
    {0x8e8d, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44},
    {0x8e8e, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44}, /* BC6H signed */
    {0x8e8f, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44}, /* BC6H unsigned */
};
typedef char snx_gl_formats_counted[sizeof formats / sizeof *formats == SNX_GL_TABLE_FORMATS ? 1 : -1];

/* ASTC's, GL's 0x93b0 (4x4) to 0x93bd (12x12), then the same in sRGB at 0x93d0: 16-byte blocks */
static const SnxGlFormat astc[28] = {
    {0x93b0, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44}, {0x93b1, 0, 0, 16, 0, SNX_GL_FLOAT, 0x54},
    {0x93b2, 0, 0, 16, 0, SNX_GL_FLOAT, 0x55}, {0x93b3, 0, 0, 16, 0, SNX_GL_FLOAT, 0x65},
    {0x93b4, 0, 0, 16, 0, SNX_GL_FLOAT, 0x66}, {0x93b5, 0, 0, 16, 0, SNX_GL_FLOAT, 0x85},
    {0x93b6, 0, 0, 16, 0, SNX_GL_FLOAT, 0x86}, {0x93b7, 0, 0, 16, 0, SNX_GL_FLOAT, 0x88},
    {0x93b8, 0, 0, 16, 0, SNX_GL_FLOAT, 0xa5}, {0x93b9, 0, 0, 16, 0, SNX_GL_FLOAT, 0xa6},
    {0x93ba, 0, 0, 16, 0, SNX_GL_FLOAT, 0xa8}, {0x93bb, 0, 0, 16, 0, SNX_GL_FLOAT, 0xaa},
    {0x93bc, 0, 0, 16, 0, SNX_GL_FLOAT, 0xca}, {0x93bd, 0, 0, 16, 0, SNX_GL_FLOAT, 0xcc},
    {0x93d0, 0, 0, 16, 0, SNX_GL_FLOAT, 0x44}, {0x93d1, 0, 0, 16, 0, SNX_GL_FLOAT, 0x54},
    {0x93d2, 0, 0, 16, 0, SNX_GL_FLOAT, 0x55}, {0x93d3, 0, 0, 16, 0, SNX_GL_FLOAT, 0x65},
    {0x93d4, 0, 0, 16, 0, SNX_GL_FLOAT, 0x66}, {0x93d5, 0, 0, 16, 0, SNX_GL_FLOAT, 0x85},
    {0x93d6, 0, 0, 16, 0, SNX_GL_FLOAT, 0x86}, {0x93d7, 0, 0, 16, 0, SNX_GL_FLOAT, 0x88},
    {0x93d8, 0, 0, 16, 0, SNX_GL_FLOAT, 0xa5}, {0x93d9, 0, 0, 16, 0, SNX_GL_FLOAT, 0xa6},
    {0x93da, 0, 0, 16, 0, SNX_GL_FLOAT, 0xa8}, {0x93db, 0, 0, 16, 0, SNX_GL_FLOAT, 0xaa},
    {0x93dc, 0, 0, 16, 0, SNX_GL_FLOAT, 0xca}, {0x93dd, 0, 0, 16, 0, SNX_GL_FLOAT, 0xcc},
};

int snx_gl_format(uint32_t internal, uint32_t format, uint32_t type) {
    int unsized = internal == GL_RGBA_ || internal == GL_RGB_ || internal == GL_LUMINANCE_ || internal == GL_ALPHA_ ||
                  internal == GL_LUMINANCE_ALPHA_ || internal == GL_BGRA_EXT_ || internal == GL_DEPTH_COMPONENT_ ||
                  internal == GL_DEPTH_STENCIL_ || internal == GL_RED_ || internal == GL_RG_;
    for (int i = 0; i < SNX_GL_TABLE_FORMATS; ++i) {
        const SnxGlFormat* f = &formats[i];
        if (f->internal != internal) continue;
        if (!unsized || (f->format == format && f->type == type)) return i;
    }
    if ((internal >= 0x93b0 && internal <= 0x93bd) || (internal >= 0x93d0 && internal <= 0x93dd))
        return SNX_GL_TABLE_FORMATS + (int)(internal & 0xf) + (internal >= 0x93d0 ? 14 : 0);
    return -1;
}
const SnxGlFormat* snx_gl_format_at(int index) {
    if (index < 0) return NULL;
    if (index < SNX_GL_TABLE_FORMATS) return &formats[index];
    return index < SNX_GL_FORMATS ? &astc[index - SNX_GL_TABLE_FORMATS] : NULL;
}
uint64_t snx_gl_format_bytes(const SnxGlFormat* f, int32_t w, int32_t h, int32_t d) {
    uint32_t bw = f->block >> 4, bh = f->block & 15;
    if (f->block)
        return (uint64_t)((w + (int32_t)bw - 1) / (int32_t)bw) * (uint64_t)((h + (int32_t)bh - 1) / (int32_t)bh) *
               (uint64_t)d * f->bytes;
    return (uint64_t)w * (uint64_t)h * (uint64_t)d * f->bytes;
}
