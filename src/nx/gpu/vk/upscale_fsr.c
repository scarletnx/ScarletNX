/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* FSR 1's constant setup, AMD's MIT ffx_fsr1.h with its CPU path. The shaders include the same header on the GPU. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define A_CPU    1
#define A_STATIC static inline
#include "nx/gpu/vk/upscale/ffx_a.h"
#include "nx/gpu/vk/upscale/ffx_fsr1.h"

#include "nx/gpu/vk/upscale_fsr.h"

void vk_upscale_fsr_easu_con(uint32_t con[4][4], float input_w, float input_h, float image_w, float image_h,
                             float output_w, float output_h, float offset_x, float offset_y) {
    AU1 c0[4], c1[4], c2[4], c3[4];
    int i;
    FsrEasuConOffset(c0, c1, c2, c3, input_w, input_h, image_w, image_h, output_w, output_h, offset_x, offset_y);
    for (i = 0; i < 4; i++) {
        con[0][i] = c0[i];
        con[1][i] = c1[i];
        con[2][i] = c2[i];
        con[3][i] = c3[i];
    }
}

void vk_upscale_fsr_rcas_con(uint32_t con[4], float sharpness) {
    AU1 c[4];
    FsrRcasCon(c, sharpness);
    for (int i = 0; i < 4; i++) con[i] = c[i];
}
