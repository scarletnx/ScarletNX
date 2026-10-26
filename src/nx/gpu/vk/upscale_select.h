/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The spatial upscaler the Graphics setting names, and the choice Auto makes without Vulkan: the .ini's names, the GPU
   vendor's filter, the phone list, and the ratio with its hysteresis. Pure C, so the tests hold it to the rules. */
#ifndef SNX_GPU_VK_UPSCALE_SELECT_H
#define SNX_GPU_VK_UPSCALE_SELECT_H
#include <stdint.h>

enum {
    SNX_UPSCALE_AUTO,
    SNX_UPSCALE_OFF,
    SNX_UPSCALE_BILINEAR,
    SNX_UPSCALE_FSR1,
    SNX_UPSCALE_NIS,
    SNX_UPSCALE_SGSR,
    SNX_UPSCALE_COUNT
};

/* the .ini's value (auto, off, bilinear, fsr1, nis, sgsr), SNX_UPSCALE_AUTO for one it doesn't know */
int snx_upscale_parse(const char* value);
/* the .ini's value, and the menu's label */
const char* snx_upscale_key(int filter);
const char* snx_upscale_label(int filter);
/* the filter Auto picks for a device: NIS on NVIDIA, SGSR on a Qualcomm phone on the list, FSR 1 everywhere else */
int snx_upscale_auto_pick(uint32_t vendor, uint32_t device, const char* name);
/* whether a Qualcomm GPU is one Auto may use SGSR on (Adreno 730 and later) */
int snx_upscale_known_phone(uint32_t vendor, uint32_t device, const char* name);
/* Auto's ratio rule: state is 0 (off), 1 (on) or -1 (off after it was on). It turns on at 1.30x per axis, stays on to
   1.25x, and turns on again at 1.35x. Returns whether Auto uses its filter now */
int snx_upscale_ratio(int* state, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h);
/* whether the filter's sharpness applies (Auto may use one; None and Bilinear never do) */
int snx_upscale_uses_sharpness(int filter);
#endif
