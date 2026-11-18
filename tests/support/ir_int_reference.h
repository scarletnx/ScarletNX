/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hand-written integer, hint and barrier emitters the generated ones replaced (ir_int_reference.c), for the tests
   that compare them */
#ifndef SNX_TESTS_SUPPORT_IR_INT_REFERENCE_H
#define SNX_TESTS_SUPPORT_IR_INT_REFERENCE_H

#include "common/cpu/recompiler/llvm/ir.h"

/* the body of a plain integer instruction whose emitter sailgen generates (decoded: its SnxA64Insn), as ir_int.c
   wrote it by hand; the shape of SnxIr.int_reference */
void snx_ir_int_reference(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc);
/* the operand of a branch whose operand sailgen generates, as ir_int.c wrote it by hand; the shape of
   SnxIr.operand_reference */
LLVMValueRef snx_ir_int_operand_reference(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc);

#endif
