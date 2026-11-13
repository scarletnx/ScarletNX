/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SPIR-V of FSR 2.2.1's passes, which the build compiles from AMD's GLSL (cmake/fsr2_shaders.cmake): a module for
   each FfxFsr2Pass and each value of the four options a context's flags choose (bit 0 HDR color input, bit 1 low
   resolution motion vectors, bit 2 jittered motion vectors, bit 3 inverted depth) */
#ifndef SNX_NX_GPU_VK_FSR2_SPV_H
#define SNX_NX_GPU_VK_FSR2_SPV_H
#include <stdint.h>
typedef struct {
    const uint32_t* words;
    uint32_t size; /* in bytes */
} SnxFsr2Spv;
extern const SnxFsr2Spv snx_fsr2_spv[9][16];
#endif
