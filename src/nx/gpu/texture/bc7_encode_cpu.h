/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 encoder's fast build (bc7_encode.glsl, SNX_BC7_FAST 1) run on the CPU (bc7_encode_cpu.cpp), for the
   background encodes of vk/astc.c that astc_background=1 runs on worker threads (docs/ASTC.md) */
#ifndef SNX_BC7_ENCODE_CPU_H
#define SNX_BC7_ENCODE_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* a strip as bc7.comp encodes one: slices slices of w x h RGBA8 texels (R first, rows after rows, a slice after
   another) into ((w + 3) / 4) x ((h + 3) / 4) blocks a slice, 16 bytes each, the bytes the GPU's fast build writes.
   rgba and out are 4-byte aligned */
void bc7_encode_cpu_strip(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t slices, uint8_t* out);
#ifdef __cplusplus
}
#endif
#endif
