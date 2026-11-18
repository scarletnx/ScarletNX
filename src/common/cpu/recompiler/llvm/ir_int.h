/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The integer, branch, hint and system instructions' emitters, keyed on the generated decoder's ops (ir_int.c, which
   sends the data-processing ones, the hints, the barriers and the branches' operands to their emitters generated from
   the Sail, ir_sail.c) */
#ifndef SNX_RECOMPILER_LLVM_IR_INT_H
#define SNX_RECOMPILER_LLVM_IR_INT_H

#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/a64_decoder.h"

/* 1 when ir_int.c emits op: its PLAIN body, or its flow operand */
int snx_ir_int_handles(SnxA64Op op);
/* The body of a PLAIN instruction insn (word at pc) whose op snx_ir_int_handles. A system register or operation EL0
   can't reach on the Switch raises UNDEF at run time (c->rt->undef), as the console does */
void snx_ir_int(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc);
/* The operand of a flow instruction insn (word at pc): the i1 condition of B.cond, CBZ and TBZ, the i64 target of BR,
   BLR and RET and their PAuth forms (BLR's link register written as it goes), from their Sail (ir_sail.c); the i32
   immediate of SVC and BRK; NULL for another op */
LLVMValueRef snx_ir_int_operand(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc);

#endif
