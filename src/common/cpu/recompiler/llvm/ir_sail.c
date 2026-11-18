/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The generated emitters (a64_sail.inc, cmake/sailgen.cmake): sailgen's LLVM back end and one function per decode
   function of src/common/cpu/aarch64/sail/a64.roots. The back end's per-block cache of the registers and flags is
   every emitter's: ir.h's accessors for the ones written by hand are its functions, below */
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

int snx_ir_sail(SnxIr* ir, SnxU32 word, SnxU64 pc) { return sg_emit(ir, word, pc, NULL, NULL); }
int snx_ir_sail_branch(SnxIr* ir, SnxU32 word, SnxU64 pc, LLVMValueRef* taken, LLVMValueRef* target) {
    return sg_emit(ir, word, pc, taken, target);
}

/* ir.h's register and flag accessors, for the emitters written by hand: the back end's cache, the one the generated
   emitters keep their state in */
void ir_registers_invalidate(SnxIr* ir) { sg_registers_forget(ir); }
void ir_flags_invalidate(SnxIr* ir) { sg_flags_forget(ir); }
LLVMValueRef ir_x(SnxIr* ir, unsigned n) { return sg_x(ir, n); }
void ir_set_x(SnxIr* ir, unsigned n, LLVMValueRef value) { sg_set_x(ir, n, ir_u64(ir, value)); }
LLVMValueRef ir_flag_read(SnxIr* ir, unsigned index) { return sg_flag(ir, index); }
void ir_flags_commit(SnxIr* ir, LLVMValueRef n, LLVMValueRef z, LLVMValueRef c, LLVMValueRef v) {
    LLVMValueRef values[4];
    values[0] = n;
    values[1] = z;
    values[2] = c;
    values[3] = v;
    sg_flags_store(ir, values);
}
void ir_flags_origin(SnxIr* ir, unsigned origin, LLVMValueRef left, LLVMValueRef right) {
    sg_flags_origin(ir, origin, left, right);
}
LLVMValueRef snx_ir_sail_origin(SnxIr* ir, unsigned code) { return sg_origin_cond(ir, code); }
