/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hand-written SIMD emitters the generated ones replaced (ir_vec_reference.c), for the tests that compare them */
#ifndef SNX_TESTS_SUPPORT_IR_VEC_REFERENCE_H
#define SNX_TESTS_SUPPORT_IR_VEC_REFERENCE_H

#include "common/cpu/recompiler/llvm/ir.h"

/* the body of a SIMD instruction whose emitter sailgen generates (decoded: its SnxA64Insn), as ir_vec.c wrote it by
   hand; the shape of SnxIr.vec_reference */
void snx_ir_vec_reference(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc);

#endif
