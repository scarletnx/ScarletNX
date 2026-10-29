/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module from the LLVM backend: what module.c and toolchain.c produce through C and Clang, written directly
   (LLVM IR per chunk, compiled in parallel, linked with the runtime library by recompiler/llvm/link.c) */
#ifndef SNX_NX_MODULE_LLVM_H
#define SNX_NX_MODULE_LLVM_H

#include "nx/loader/module.h"

struct SnxIrTuning; /* recompiler/llvm/ir.h's */
typedef struct {
    const char* output; /* <Game>_code.so or .dll, replaced only by a complete module */
    /* The target triple of a module made for another machine than this one, which the JIT loader loads instead of
       dlopen (iOS: arm64-apple-ios16.0-elf, the object format the loader reads): then output is a pack of the
       module's objects without the runtime library (recompiler/llvm/objects_pack.h), which the loading app has for its
       own machine. NULL: a module for this host */
    const char* triple;
    char opt;           /* -O letter */
    unsigned jobs;
    int quiet;
    const char* keep_ir; /* when set, each unit's IR is saved there as .ll */
    const struct SnxIrTuning* tuning; /* the game code's LLVM settings (snx_llvm_tuning), NULL for LLVM's own */
    /* the code units' cache (unit_cache.h), a folder every game shares: a unit kept there is not compiled again. NULL:
       every unit is compiled, and none kept */
    const char* unit_cache;
    SnxU64 unit_cache_limit; /* the bytes the folder is held to after the build, its oldest objects removed; 0: none */
    /* the room on the unit cache's disk, as unit_cache.h's SnxUnitDisk reads it: NULL for snx_disk_room, a test's own */
    int (*unit_cache_disk)(const char* folder, SnxU64* free_bytes, SnxU64* total);
    /* when set, the snx_aot_ wrappers the descriptor got: one per entry when the profile cache's units link (they call
       them), else none */
    size_t* wrappers;
    /* when set, the instructions the analysis found and the module makes ahead of time (0 with --jit-only) */
    size_t* found;
    /* --layout: the guest addresses of functions in the order their code takes at the start of the module, each in a
       code section of its own (link.h's SNX_LINK_HOT_TEXT). A profile's hot functions, the others after them in
       address order. Addresses that start no function are skipped. An ELF module for this host only */
    const SnxU64* layout;
    size_t layout_count;
} SnxLlvmBuild;
/* g_settings' LLVM Internals (settings.txt's llvm table) for -O<opt>: the pipeline with the passes turned off cut
   out, the options set. Static, made again at each call. NULL with the reason in error when nothing at all is left
   (a level's pipeline of passes only, every one of them off) */
const struct SnxIrTuning* snx_llvm_tuning(char opt, char* error, size_t error_size);

/* SNX_ABI as this build has it: the layout its modules are written with, which the runtime of the same build checks */
unsigned snx_module_abi(void);
/* 0 on success. LLVM is a required recompiler dependency. */
int snx_build_module_llvm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build);
/* A 32-bit program's module (module_arm.c): its AArch32 blocks and an SnxArmModule. LLVM only */
int snx_build_module_arm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build);

#endif
