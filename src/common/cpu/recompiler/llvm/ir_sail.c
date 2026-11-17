/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The generated emitters (a64_sail.inc, cmake/sailgen.cmake): sailgen's LLVM back end and one function per decode
   function of src/common/cpu/aarch64/sail/a64.roots, which reach the CPU only through ir.h's state model */
#include "common/cpu/recompiler/llvm/ir_sail.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the generated code binds every parameter and local of the Sail, used or not */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

#include "a64_sail.inc"

int snx_ir_sail(SnxIr* ir, SnxU32 word, SnxU64 pc) { return sg_emit(ir, word, pc); }
