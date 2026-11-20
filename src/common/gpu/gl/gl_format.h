/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's texture and renderbuffer formats (gl_state.h's layer): what GL says of each, which every platform
   reads the same, by an index a record keeps. A platform's own forms of a format (Vulkan's formats and swizzles, in
   the Android bridge) are a table of its own in the same order */
#ifndef SNX_GL_FORMAT_H
#define SNX_GL_FORMAT_H
#include <stdint.h>

/* what a format's texels hold */
enum { SNX_GL_FLOAT = 0, SNX_GL_INT = 1, SNX_GL_UINT = 2, SNX_GL_DEPTH = 3, SNX_GL_STENCIL = 4, SNX_GL_DS = 5 };

typedef struct {
    uint32_t internal;     /* the sized internal format, or GLES 2's unsized one */
    uint32_t format, type; /* the client data that copies into it as it is; 0 for a compressed one */
    uint8_t bytes;         /* of a texel of the client data, or of a compressed block */
    uint8_t expand;        /* 3 components of 1, 2 or 4 bytes, which a platform may make 4: their size */
    uint8_t kind;          /* SNX_GL_FLOAT (normalized too) and the others */
    uint8_t block;         /* a compressed format's block width (high 4 bits) and height, 0 for others */
} SnxGlFormat;

/* the table's formats (a platform's own table has as many, in the same order), then ASTC's 28: GL's 0x93b0 to
   0x93bd and their sRGB forms at 0x93d0 */
#define SNX_GL_TABLE_FORMATS 109
#define SNX_GL_FORMATS       (SNX_GL_TABLE_FORMATS + 28)

/* the index of the format of internal (and format and type, for GLES 2's unsized ones), -1 for none */
int snx_gl_format(uint32_t internal, uint32_t format, uint32_t type);
/* the format at index (snx_gl_format's), NULL for an index past them */
const SnxGlFormat* snx_gl_format_at(int index);
/* the bytes of a level of w x h x d texels (blocks, for a compressed format) */
uint64_t snx_gl_format_bytes(const SnxGlFormat* f, int32_t w, int32_t h, int32_t d);

#endif
