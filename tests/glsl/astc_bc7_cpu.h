/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_bc7.comp's body (astc_decode.glsl, bc7_encode.glsl and astc_bc7.glsl) run on the CPU (astc_bc7_cpu.cpp), for
   the tests: a strip of depth slices of nx x ny ASTC blocks of bw x bh texels, w x h texels a slice, into
   ((w + 3) / 4) x ((h + 3) / 4) BC7 blocks a slice, in the encoder's build `build` (vk_bc7_build: 0 the full one, 1
   the fast one, 2 mode 6 alone, 3 the quick one) as vk/astc.c runs it: what a GPU's blocks are held to. The fast
   build takes the ASTC blocks' hints (astc_bc7.glsl's SNX_BC7_HINTS), so its blocks are not the encoder's of the
   decoded texels alone; the others' are */
#ifndef SNX_ASTC_BC7_CPU_H
#define SNX_ASTC_BC7_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* or'd into the fast build (1), for --measure: without the hints, and with mode 6's quick line kept under an error
   of 64 (bc7_encode.glsl's SNX_BC7_QUICK), which no build of the shader takes */
#define ASTC_BC7_PLAIN 16u
#define ASTC_BC7_QUICK 32u
void astc_bc7_cpu(uint32_t build, const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw,
                  uint32_t bh, int srgb, uint8_t* out);
/* the full (0) or the fast (1) build in the two passes vk/astc.c runs it in (astc_bc7.glsl's SNX_BC7_PASS): the same
   blocks. listed, when not NULL: the blocks the first pass left to the second */
void astc_bc7_cpu_passes(uint32_t build, const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw,
                         uint32_t bh, int srgb, uint8_t* out, uint32_t* listed);
#ifdef __cplusplus
}
#endif
#endif
