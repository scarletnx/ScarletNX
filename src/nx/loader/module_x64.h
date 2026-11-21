/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An AOT module's descriptor without LLVM (module_x64.c), for module_llvm.c where the x86-64 back end made every
   function: snx_module, snx_routes, snx_nros (with nros), snx_imports (with imports) and their data */
#ifndef SNX_NX_LOADER_MODULE_X64_H
#define SNX_NX_LOADER_MODULE_X64_H
#include "nx/loader/module.h"
#include "common/cpu/recompiler/codegen.h"
#include <stddef.h>

/* the descriptor's ELF object (malloc'd) and its entries' count; nonzero with error set on failure */
int snx_x64_descriptor(const SnxCodegen* codegen, const SnxCodegenOptions* options, const SnxProgram* loaded,
                       const SnxU64* imports, size_t import_count, unsigned char** object, size_t* size,
                       size_t* entry_count, char* error, size_t error_size);

#endif
