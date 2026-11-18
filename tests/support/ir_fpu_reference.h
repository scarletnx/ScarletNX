/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hand-written scalar floating-point emitters the generated ones replaced (ir_fpu_reference.c), for the tests that
   compare them */
#ifndef SNX_TESTS_SUPPORT_IR_FPU_REFERENCE_H
#define SNX_TESTS_SUPPORT_IR_FPU_REFERENCE_H

#include "common/cpu/recompiler/llvm/ir.h"

/* the body of a scalar floating-point instruction (decoded: its SnxA64Insn), as ir_fpu.c wrote it by hand; the shape
   of SnxIr.fpu_reference */
void snx_ir_fpu_reference(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc);

#endif
