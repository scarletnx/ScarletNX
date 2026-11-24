/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ARM_LLVM_H
#define SNX_ARM_LLVM_H
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/checked.h"
#include "common/cpu/arm/codegen.h"
#include <llvm-c/Core.h>

/* Emit void name(SnxArmCPU*) into a host-target module. No interpreter or runtime
   symbols are needed. words are host integers decoded from little-endian code.
   Stops at a control transfer (including conditional transfers), or count.
   *consumed is the number of words compiled. At return r15 is the next PC, or
   the faulting PC on an exception; SVC/BKPT advance it by four. A dispatcher
   must check exit and CPSR.T before selecting the next block. Blocks are bounded
   so the caller can poll/schedule between them. Invalidate them on code writes.
   Rejects bad input with NULL without changing the module. No A64 ABI changes. */
LLVMValueRef snx_arm_llvm_block(LLVMModuleRef module, const char* name, const uint32_t* words, size_t count,
                                uint32_t pc, size_t* consumed);
/* The same for a Thumb16/Thumb32 block: the halfword stream is decoded as
   Thumb and CPSR.T must be set on entry. IT conditions are applied per
   instruction; interworking branches set/clear CPSR.T before returning. */
LLVMValueRef snx_thumb_llvm_block(LLVMModuleRef module, const char* name, const uint16_t* halfwords, size_t count,
                                  uint32_t pc, size_t* consumed);

/* The same for options (codegen.h): NULL is the original A57 on flat memory. */
LLVMValueRef snx_arm_llvm_block_ex(LLVMModuleRef module, const char* name, const uint32_t* words, size_t count,
                                   uint32_t pc, size_t* consumed, const SnxArmCodegenOptions* options);
LLVMValueRef snx_thumb_llvm_block_ex(LLVMModuleRef module, const char* name, const uint16_t* halfwords, size_t count,
                                     uint32_t pc, size_t* consumed, const SnxArmCodegenOptions* options);
/* snx_arm_llvm_block_ex, its A32 words through the emitters written by hand alone (llvm.c), as the generated ones
   (sg_emit_a32) took them over: the reference tests/arm_sail_ir.c holds the generated ones to */
LLVMValueRef snx_arm_llvm_block_reference(LLVMModuleRef module, const char* name, const uint32_t* words, size_t count,
                                          uint32_t pc, size_t* consumed, const SnxArmCodegenOptions* options);
/* The same, its T16 instructions through the emitters written by hand that the Sail's replaced (llvm_thumb.inc): the
   reference tests/arm_sail_ir.c holds the generated ones to */
LLVMValueRef snx_thumb_llvm_block_t16_reference(LLVMModuleRef module, const char* name, const uint16_t* halfwords,
                                                size_t count, uint32_t pc, size_t* consumed,
                                                const SnxArmCodegenOptions* options);
#endif
