/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef AGB_CPU_RUNTIME_H
#define AGB_CPU_RUNTIME_H
#include <stddef.h>
/* The ARM backend's runtime library (snx_arm_rt: snx_arm_undefined, snx_arm_coproc and the VFP helpers), the
   archive the ROM's module links, embedded at build time (cmake/ctr-runtime.cmake) */
extern const unsigned char agb_arm_runtime[];
extern const size_t agb_arm_runtime_size;
#endif
