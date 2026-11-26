/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* No fallback for a game module (module_fallback.h): a build without LLVM makes its modules of the native back end's
   code alone, and what that leaves is refused here by name */
#include "nx/loader/module_fallback.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stdio.h>

#define NO_LLVM "this build has no LLVM to make it (SNX_WITH_LLVM=OFF)"

char* snx_module_fallback_cache_key(const char* path, char opt, int hook, SnxU64 features, char* error,
                                    size_t error_size) {
    (void)hook;
    (void)features;
    snprintf(error, error_size,
             "--jit-cache %s: the key of a profile cache made at -O%c names the LLVM that made its units, and " NO_LLVM,
             path, opt);
    return NULL;
}

int snx_module_fallback_descriptor(const SnxCodegen* codegen, const SnxCodegenOptions* options,
                                   const SnxProgram* loaded, const SnxLlvmBuild* build, const SnxU64* imports,
                                   size_t import_count, const SnxJitCache* cache, unsigned char** object, size_t* size,
                                   size_t* entry_count, size_t* wrapped, size_t* variant_count, char* error,
                                   size_t error_size) {
    (void)options;
    (void)loaded;
    (void)build;
    (void)imports;
    (void)import_count;
    (void)cache;
    *object = NULL;
    *size = *entry_count = *wrapped = *variant_count = 0;
    snprintf(error, error_size,
             "the descriptor of a module of %zu functions, which the native back end (%s) did not "
             "make whole: " NO_LLVM,
             codegen->function_count, snx_native_name());
    return -1;
}

int snx_module_fallback_arm_start(char* error, size_t error_size) {
    snprintf(error, error_size,
             "this host has no native back end for AArch32 blocks, and this build has no LLVM to make them "
             "(SNX_WITH_LLVM=OFF)");
    return -1;
}

/* none is made: no block is measured */
struct SnxModuleArmScratch {
    char unused;
};
int snx_module_fallback_arm_measure(SnxModuleArmScratch** scratch, const SnxModuleArmCode* block, const char* why,
                                    size_t* units, char* error, size_t error_size) {
    const int thumb = (int)(block->key & 1);
    const unsigned char* c = block->code;
    (void)scratch;
    *units = 0;
    if (!block->count)
        snprintf(error, error_size, "the %s block at 0x%08x, of no instructions: %s, and " NO_LLVM,
                 thumb ? "Thumb" : "ARM", (unsigned)(block->key & ~1u), why);
    else if (thumb)
        snprintf(error, error_size, "the Thumb block at 0x%08x, which starts with %04x: %s, and " NO_LLVM,
                 (unsigned)(block->key & ~1u), (unsigned)(c[0] | c[1] << 8), why);
    else
        snprintf(error, error_size, "the ARM block at 0x%08x, which starts with %08x: %s, and " NO_LLVM,
                 (unsigned)(block->key & ~1u), (unsigned)(c[0] | c[1] << 8 | c[2] << 16 | (SnxU32)c[3] << 24), why);
    return -1;
}
void snx_module_fallback_arm_scratch_free(SnxModuleArmScratch* scratch) { (void)scratch; }

int snx_module_fallback_arm_unit(const SnxLlvmBuild* build, const char* name, const SnxModuleArmBlock* blocks,
                                 size_t count, unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)build;
    *object = NULL;
    *size = 0;
    snprintf(error, error_size, "%s: %zu AArch32 blocks, the first at 0x%08x, which the native back end left: " NO_LLVM,
             name, count, count ? (unsigned)(blocks[0].code.key & ~1u) : 0u);
    return -1;
}

int snx_module_fallback_arm_descriptor(const SnxProgram* loaded, const SnxCodegenOptions* options,
                                       const SnxLlvmBuild* build, const SnxU32* keys, size_t count,
                                       unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)loaded;
    (void)options;
    (void)build;
    (void)keys;
    *object = NULL;
    *size = 0;
    snprintf(error, error_size,
             "the descriptor of a module of %zu AArch32 blocks, which the native back end (%s) did "
             "not make: " NO_LLVM,
             count, snx_native_name());
    return -1;
}
