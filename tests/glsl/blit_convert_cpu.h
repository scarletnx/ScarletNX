/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 2D engine's blit between pixel formats (texconv.glsl built with SNX_TC_CONVERT, its pixel_convert.glsl) run on
   the CPU (blit_convert_cpu.cpp), for the tests */
#ifndef SNX_BLIT_CONVERT_CPU_H
#define SNX_BLIT_CONVERT_CPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* rows rows of nb bytes of the blit fmt describes (gpu.h's GPU_BLIT_CONVERT, GPU_BLIT_LINEAR_BIT), as vk/texconv.c's
   vk_tc_blit dispatches texconv.comp's conversion build: in holds in_size bytes, the source's rows in_pitch bytes
   apart from byte src_at, the column table from byte col_at and the row table from row_at (both multiples of 4); out
   holds rows rows out_pitch bytes apart (a multiple of 4), whose bytes past nb in a row's last word stay as they are */
void blit_convert_cpu(uint32_t fmt, uint32_t nb, uint32_t rows, const uint8_t* in, uint32_t in_size, uint32_t src_at,
                      uint32_t in_pitch, uint32_t col_at, uint32_t row_at, uint8_t* out, uint32_t out_pitch);
#ifdef __cplusplus
}
#endif
#endif
