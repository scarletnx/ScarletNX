/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hardware's texture and sampler descriptors (TIC and TSC entries) for NVN's objects (gpu/nvn/nvn_tic.c), which
   RegisterTexture and RegisterSampler write into the pools as NVIDIA's driver does */
#ifndef SNX_NVN_TIC_H
#define SNX_NVN_TIC_H
#include "nx/gpu/nvn/nvn_obj.h"

/* t's TIC entry through view v (NULL for the whole texture), its storage at t->gpu: 1, or 0 (out zeroed) for a format
   the table doesn't have. image: for shader loads and stores (RegisterImage), where a cube map is its layers */
int nvn_tic_encode(const NvnTexture* t, const NvnView* v, int image, u32 out[8]);
/* an NVN format's render target code: the 3D engine's color format, or its zeta format with *depth set; 0 for a
   format no target takes */
u32 nvn_rt_format(u32 format, int* depth);
/* the sampler state's TSC entry */
void nvn_tsc_encode(const NvnSamplerPar* p, u32 out[8]);

#endif
