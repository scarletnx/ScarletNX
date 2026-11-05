/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The memory blit's pixel descriptor, shared by C and GLSL. Ordinary copies keep fmt zero and move exact bytes.
   Converted copies describe component width/count, number type, BGR order and an unused alpha channel. */
#ifndef SNX_GPU_PIXEL_FORMAT_H
#define SNX_GPU_PIXEL_FORMAT_H
#define GPU_BLIT_CONVERT_BIT 0x80000000u
#define GPU_BLIT_LINEAR_BIT  0x40000000u
#define GPU_PIXEL_UNORM      0u
#define GPU_PIXEL_SNORM      1u
#define GPU_PIXEL_FLOAT      2u
#define GPU_PIXEL_565        3u
#define GPU_PIXEL_5551       4u
#define GPU_PIXEL_1010102    5u
#define GPU_PIXEL_111110     6u
#define GPU_PIXEL_BGR        0x80u
#define GPU_PIXEL_X          0x100u
#define GPU_PIXEL_SRGB       0x200u
#define GPU_PIXEL_VALID      0x400u
#define GPU_PIXEL_TYPE(p)    (((p) >> 4) & 7u)
#define GPU_PIXEL_DESC(log_bytes, components, type)                                                                    \
    (GPU_PIXEL_VALID | (log_bytes) | (((components) - 1u) << 2) | ((type) << 4))
#define GPU_PIXEL_BYTES(p)                                                                                             \
    (GPU_PIXEL_TYPE(p) >= GPU_PIXEL_565 ? (GPU_PIXEL_TYPE(p) <= GPU_PIXEL_5551 ? 2u : 4u)                              \
                                        : (1u << ((p) & 3u)) * ((((p) >> 2) & 3u) + 1u))
#define GPU_BLIT_CONVERT(source, destination) (GPU_BLIT_CONVERT_BIT | (source) | ((destination) << 16))
#endif
