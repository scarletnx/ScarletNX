/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An AOT module's descriptor without LLVM (module_x64.c), for module_llvm.c where the x86-64 back end made every
   function: snx_module, snx_routes, snx_nros (with nros), snx_imports (with imports) and their data */
#ifndef SNX_NX_LOADER_MODULE_X64_H
#define SNX_NX_LOADER_MODULE_X64_H
#include "nx/loader/module.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/jit/jit_cache.h"
#include <stddef.h>

/* the descriptor's ELF object (malloc'd), its entries' count and its variants' (the profile run's units it links:
   records, their functions' snx_variants table and the snx_aot_ wrappers their calls name); nonzero with error set on
   failure. The host's machine's; snx_x64_descriptor_for a RISC machine's (desc, NULL for the host's x86-64 or
   arm64, &snx_a64_desc arm64's on any host: a module packed for iOS), as the tests run another machine's */
int snx_x64_descriptor(const SnxCodegen* codegen, const SnxCodegenOptions* options, const SnxProgram* loaded,
                       const SnxU64* imports, size_t import_count, const SnxJitCacheRecord* records,
                       size_t record_count, unsigned char** object, size_t* size, size_t* entry_count,
                       size_t* variant_count, char* error, size_t error_size);
int snx_x64_descriptor_for(const SnxNativeDesc* desc, const SnxCodegen* codegen, const SnxCodegenOptions* options,
                           const SnxProgram* loaded, const SnxU64* imports, size_t import_count,
                           const SnxJitCacheRecord* records, size_t record_count, unsigned char** object,
                           size_t* size, size_t* entry_count, size_t* variant_count, char* error, size_t error_size);
/* arm64's descriptor functions, which this file writes: the machine alone, for snx_x64_descriptor_for. An ELF object,
   so not on Windows (the reason in error there) */
extern const SnxNativeDesc snx_a64_desc;

/* a 32-bit program's (module_arm.c): its SnxArmModule and snx_module_arm, the count blocks' keys sorted (the address,
   bit 0 set for Thumb); nonzero with error set on failure */
int snx_x64_arm_descriptor(const SnxProgram* loaded, const SnxCodegenOptions* options, const SnxU32* keys, size_t count,
                           unsigned char** object, size_t* size, char* error, size_t error_size);
int snx_x64_arm_descriptor_for(const SnxNativeDesc* desc, const SnxProgram* loaded, const SnxCodegenOptions* options,
                               const SnxU32* keys, size_t count, unsigned char** object, size_t* size, char* error,
                               size_t error_size);

#endif
