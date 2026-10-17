/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of every 32-bit game module the block compiler doesn't generate: the VFP, Advanced SIMD and crypto helpers
   its blocks call (cpu/arm/simd.h), the calls through which they leave the rest to the backend (arm.h's SnxArmRt), and the C library functions LLVM lowers memory intrinsics to. Compiled like
   rtlib.c, when the recompiler is built, and linked into each 32-bit module (nx/loader/module_arm.c) */
#include "common/cpu/arm/fp_helpers.c"
#include "common/cpu/arm/crypto_helpers.c"
#include "common/cpu/arm/rt_helpers.c"

#include "common/cpu/recompiler/llvm/libc.inc"
