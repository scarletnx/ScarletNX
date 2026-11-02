/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The spatial filter the Graphics setting names. FSR 1 is the default and the filter that runs on every device. The
   other filters are explicit choices. Pure C, so the tests hold it to the names. */
#ifndef SNX_GPU_VK_UPSCALE_SELECT_H
#define SNX_GPU_VK_UPSCALE_SELECT_H

enum { SNX_UPSCALE_OFF, SNX_UPSCALE_BILINEAR, SNX_UPSCALE_FSR1, SNX_UPSCALE_NIS, SNX_UPSCALE_SGSR, SNX_UPSCALE_COUNT };

/* the .ini's value (off, bilinear, fsr1, nis, sgsr), FSR 1 for one it doesn't know */
int snx_upscale_parse(const char* value);
/* the .ini's value, and the menu's label */
const char* snx_upscale_key(int filter);
const char* snx_upscale_label(int filter);
/* whether the filter's sharpness applies (None and Bilinear never do) */
int snx_upscale_uses_sharpness(int filter);
#endif
