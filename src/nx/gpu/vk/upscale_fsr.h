/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* FSR 1's constants (AMD's ffx_fsr1.h, MIT), made on the CPU for the shaders' push constants. */
#ifndef SNX_GPU_VK_UPSCALE_FSR_H
#define SNX_GPU_VK_UPSCALE_FSR_H
#include <stdint.h>

/* EASU: the shown crop (input_w x input_h at offset_x, offset_y in an image of image_w x image_h) to output_w x
   output_h */
void vk_upscale_fsr_easu_con(uint32_t con[4][4], float input_w, float input_h, float image_w, float image_h,
                             float output_w, float output_h, float offset_x, float offset_y);
/* RCAS: sharpness in stops, 0 the sharpest */
void vk_upscale_fsr_rcas_con(uint32_t con[4], float sharpness);
#endif
