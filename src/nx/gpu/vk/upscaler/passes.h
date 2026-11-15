/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* upscaler.h's compute backends, passes.c: the filtered copy (up_copy.comp) and the spatial upscaler (up_easu.comp,
   then up_rcas.comp when sharpening). upscaler.c's contexts call these, and nothing else does */
#ifndef SNX_NX_GPU_VK_UPSCALER_PASSES_H
#define SNX_NX_GPU_VK_UPSCALER_PASSES_H
#include "nx/gpu/vk/upscaler/upscaler.h"

typedef struct SnxUpPasses SnxUpPasses;
/* spatial: EASU and RCAS, else the copy. NULL and why when the device refuses something */
SnxUpPasses* snx_up_passes_create(const SnxUpscalerInfo* info, int spatial, const char** why);
int snx_up_passes_record(SnxUpPasses* p, const SnxUpscaleEval* e, const char** why);
void snx_up_passes_destroy(SnxUpPasses* p);
#endif
