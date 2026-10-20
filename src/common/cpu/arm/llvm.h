/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ARM_LLVM_H
#define SNX_ARM_LLVM_H
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/checked.h"
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

/* Versioned opt-in code generation. NULL means the original A57/flat ABI.
   Checked memory currently supports integer transfers only; exclusives and
   vectors stop with BACKEND_UNSUPPORTED, never access the flat pointer.
   Every instruction is fetched through the bus before executing, including
   condition-failed instructions. Permission faults report PREFETCH_ABORT;
   changed bytes report CODE_CHANGED without executing the stale instruction.
   The runtime must return to its dispatcher on either exit, never re-enter
   stale code or silently compile at runtime. ARM11 VFPv2 emission is pending.
   ARM7TDMI takes flat memory only: its bus hook reads cpu->io and cpu->mem. */
typedef enum { SNX_ARM_MEMORY_FLAT, SNX_ARM_MEMORY_CHECKED } SnxArmMemoryMode;
#define SNX_ARM_CODEGEN_OPTIONS_ABI 1u
typedef struct {
    uint32_t abi;
    SnxArmProfile profile;
    SnxArmMemoryMode memory;
} SnxArmCodegenOptions;
LLVMValueRef snx_arm_llvm_block_ex(LLVMModuleRef module, const char* name, const uint32_t* words, size_t count,
                                   uint32_t pc, size_t* consumed, const SnxArmCodegenOptions* options);
LLVMValueRef snx_thumb_llvm_block_ex(LLVMModuleRef module, const char* name, const uint16_t* halfwords, size_t count,
                                     uint32_t pc, size_t* consumed, const SnxArmCodegenOptions* options);
#endif
