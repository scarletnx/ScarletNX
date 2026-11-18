/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The scalar floating-point instructions' emitters, keyed on the generated decoder's ops (ir_fpu.c), which sailgen
   generates from their Sail */
#ifndef SNX_RECOMPILER_LLVM_IR_FPU_H
#define SNX_RECOMPILER_LLVM_IR_FPU_H

#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/a64_decoder.h"

/* 1 when ir_fpu.c emits op, whatever feature it needs (FEAT_FP16's halves, FEAT_JSCVT, FEAT_FRINTTS and
   FEAT_BF16's BFCVT included) */
int snx_ir_fpu_handles(SnxA64Op op);
/* The body of insn (word at pc), an op snx_ir_fpu_handles */
void snx_ir_fpu(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc);

#endif
