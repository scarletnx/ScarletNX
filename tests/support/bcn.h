/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC1 to BC7 decoding on the CPU (bcn.c), for the tests only: the Khronos Data Format Specification's S3TC, RGTC and BPTC chapters, 4x4
   blocks. The reference the GPU's decoder (texconv.glsl, for GPUs without textureCompressionBC) is tested against */
#ifndef SNX_BCN_H
#define SNX_BCN_H
#include "common/cpu/aarch64/cpu.h"

enum {
    BCN_BC1,       /* 8 bytes a block: RGB565 endpoints, a block whose first is the smaller has a transparent index */
    BCN_BC2,       /* 16 bytes: explicit 4 bit alpha, then a BC1 color block */
    BCN_BC3,       /* 16 bytes: a BC4 alpha block, then a BC1 color block */
    BCN_BC4,       /* 8 bytes: one channel, unsigned */
    BCN_BC4_SNORM, /* the same, signed */
    BCN_BC5,       /* 16 bytes: two BC4 blocks, red then green, unsigned */
    BCN_BC5_SNORM, /* the same, signed */
    BCN_BC6H_UF,   /* 16 bytes: HDR RGB, unsigned half floats */
    BCN_BC6H_SF,   /* the same, signed */
    BCN_BC7,       /* 16 bytes: RGBA */
};

/* a w x h image of 4x4 blocks of kind (rows of whole blocks, row after row) to pixels, little-endian as the GPU reads
   them: RGBA8 for BC1 to BC3 and BC7, R16 and R16G16 (unorm or snorm as the kind says) for BC4 and BC5, RGBA16F with
   alpha 1.0 for BC6H. BC1 to BC5's interpolants are those of NVIDIA's GPUs, the Switch's among them (bcn.c) */
void bcn_decode_image(const u8* src, u32 w, u32 h, int kind, u8* dst);
/* the same with BC1 to BC5's interpolants exact and rounded to nearest, as the specification's other GPUs come near */
void bcn_decode_image_spec(const u8* src, u32 w, u32 h, int kind, u8* dst);
/* the bytes of one block of kind, and of one output pixel */
u32 bcn_block_bytes(int kind);
u32 bcn_pixel_bytes(int kind);

#endif
