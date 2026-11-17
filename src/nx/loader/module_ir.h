/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What module_llvm.c's descriptor and module_arm.c's share, in LLVM IR (GNU C23 and the LLVM-C headers: not for the ISO C
   driver, which includes module_llvm.h) */
#ifndef SNX_NX_MODULE_IR_H
#define SNX_NX_MODULE_IR_H

#include "nx/loader/module_llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"

/* SnxModInfo as an LLVM struct (NULL with the reason when its layout differs from C's), MODS (const SnxModInfo[], one
   per loaded module), and a constant string */
LLVMTypeRef snx_module_info_type(SnxIr* ir, char* error, size_t error_size);
LLVMValueRef snx_module_mods(SnxIr* ir, LLVMTypeRef info, const SnxProgram* loaded);
LLVMValueRef snx_module_string(SnxIr* ir, const char* text);

#endif
