/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A64 instructions' emitters tools/sailgen generates from the Sail of the Arm architecture (ir_sail.c) */
#ifndef SNX_RECOMPILER_LLVM_IR_SAIL_H
#define SNX_RECOMPILER_LLVM_IR_SAIL_H

#include "common/cpu/recompiler/llvm/ir.h"

/* word, at pc, emitted from its Sail when it is an instruction of a generated subset (a64.roots): 1, else 0 */
int snx_ir_sail(SnxIr* ir, SnxU32 word, SnxU64 pc);
/* the same for a branch, with its operand: whether it branches (*taken, an i1) and where (*target, an i64) */
int snx_ir_sail_branch(SnxIr* ir, SnxU32 word, SnxU64 pc, LLVMValueRef* taken, LLVMValueRef* target);

/* the condition code (cond >> 1, but AL) on flags a compare or a logical result made in this block, as that compare's
   i1, by the back end's laws (ConditionHolds's); NULL when none did, or the condition needs the flags themselves.
   ir_state.c's ir_flags_cond reads the flags then. ir.h's register and flag accessors are ir_sail.c's too */
LLVMValueRef snx_ir_sail_origin(SnxIr* ir, unsigned code);

#endif
