/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* fermi2d_ref.c: a 2D engine blit (maxwell/fermi2d.c) done on the CPU in guest memory, the reference the tests hold
   the renderer's blit_mem to */
#ifndef SNX_FERMI2D_REF_H
#define SNX_FERMI2D_REF_H
#include "nx/gpu/gpu.h"

void blit_mem_ref(const GpuBlit* b);

#endif
