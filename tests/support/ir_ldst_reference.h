/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The general-purpose registers' load and store emitters ir_ldst.c wrote by hand, for tests/llvm_sail_ir.c */
#ifndef SNX_TESTS_SUPPORT_IR_LDST_REFERENCE_H
#define SNX_TESTS_SUPPORT_IR_LDST_REFERENCE_H

#include "common/cpu/recompiler/llvm/ir.h"

/* the body of a general-purpose register load or store whose emitter sailgen generates (decoded: its SnxA64Insn), as
   ir_ldst.c wrote it by hand; the shape of SnxIr.ldst_reference */
void snx_ir_ldst_reference(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc);

#endif
