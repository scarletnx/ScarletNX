/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a game module takes of the fallback (recompiler/fallback.h, which makes the functions its units' native back end
   leaves) that is the module's own: the profile cache's key, the descriptor of a 64-bit module whose code the fallback
   made some of, and a 32-bit program's blocks the native back end leaves, chained as the module's are.
   module_fallback_llvm.c makes them with LLVM where the build has it (SNX_WITH_LLVM=ON); module_fallback_none.c refuses
   each, naming it. module_llvm.c and module_arm.c take no header of LLVM's */
#ifndef SNX_NX_MODULE_FALLBACK_H
#define SNX_NX_MODULE_FALLBACK_H
#include "nx/loader/module_llvm.h"
#include "common/cpu/recompiler/jit/jit_cache.h"
#include <stddef.h>

/* the key of the profile cache at path (--jit-cache), whose units the module links, as the runtime's JIT made them at
   -O opt (jit_cache.h): malloc'd, NULL with the reason in error */
char* snx_module_fallback_cache_key(const char* path, char opt, int hook, SnxU64 features, char* error,
                                    size_t error_size);

/* ---- a 64-bit program ---- */

/* The descriptor of build's module (module_abi.h) where the fallback made some of its code: snx_module, snx_routes,
   snx_imports with imports (ascending), snx_nros with options' NROs and snx_variants with cache's units (NULL: none),
   whose calls name the snx_aot_ wrappers it then gives, one per entry. Its object (malloc'd), how many entries,
   wrappers and variants it has; 0, or -1 with the reason in error */
int snx_module_fallback_descriptor(const SnxCodegen* codegen, const SnxCodegenOptions* options,
                                   const SnxProgram* loaded, const SnxLlvmBuild* build, const SnxU64* imports,
                                   size_t import_count, const SnxJitCache* cache, unsigned char** object, size_t* size,
                                   size_t* entry_count, size_t* wrapped, size_t* variant_count, char* error,
                                   size_t error_size);

/* ---- a 32-bit program ---- */

/* 0 where the fallback makes the blocks of a program the host's native back end has none for, -1 with why not */
int snx_module_fallback_arm_start(char* error, size_t error_size);

/* A block the fallback makes: at key (its address, bit 0 set for Thumb), the guest's little-endian code at code, of
   count words or halfwords at most */
typedef struct {
    SnxU32 key;
    const unsigned char* code;
    size_t count;
} SnxModuleArmCode;

/* where a block discovery found ends as the fallback makes it, in a thread's scratch (*scratch, made at its first
   block; NULL before it): 0 with *units the words or halfwords it covers, or -1 with the reason in error. why is the
   reason the native back end left it, "" where there is none */
typedef struct SnxModuleArmScratch SnxModuleArmScratch;
int snx_module_fallback_arm_measure(SnxModuleArmScratch** scratch, const SnxModuleArmCode* block, const char* why,
                                    size_t* units, char* error, size_t error_size);
void snx_module_fallback_arm_scratch_free(SnxModuleArmScratch* scratch);

/* A block of a unit: its code, the units of it discovery found it covers, and the blocks of the module it goes on to
   (block chaining: each a tail call, a loop's back edge after the thread's poll) */
typedef struct {
    SnxModuleArmCode code;
    size_t units;
    SnxU32 next[2];
    size_t nnext;
} SnxModuleArmBlock;
/* the unit name of count blocks, each a function named by its key (A_ or T_ and the address), into an object
   (malloc'd) at build's opt and LLVM settings, its IR kept where build says: 0, or -1 with the reason in error */
int snx_module_fallback_arm_unit(const SnxLlvmBuild* build, const char* name, const SnxModuleArmBlock* blocks,
                                 size_t count, unsigned char** object, size_t* size, char* error, size_t error_size);
/* the descriptor (SnxArmModule, snx_module_arm) of a module whose blocks the fallback made, the count blocks' keys
   sorted: its object (malloc'd); 0, or -1 with the reason in error */
int snx_module_fallback_arm_descriptor(const SnxProgram* loaded, const SnxCodegenOptions* options,
                                       const SnxLlvmBuild* build, const SnxU32* keys, size_t count,
                                       unsigned char** object, size_t* size, char* error, size_t error_size);

#endif
