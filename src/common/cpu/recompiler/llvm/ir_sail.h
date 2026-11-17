/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A64 instructions' emitters tools/sailgen generates from the Sail of the Arm architecture (ir_sail.c) */
#ifndef SNX_RECOMPILER_LLVM_IR_SAIL_H
#define SNX_RECOMPILER_LLVM_IR_SAIL_H

#include "common/cpu/recompiler/llvm/ir.h"

/* word, at pc, emitted from its Sail when it is an instruction of a generated subset (a64.roots): 1, else 0 */
int snx_ir_sail(SnxIr* ir, SnxU32 word, SnxU64 pc);

#endif
