/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module ahead of time: its code in units, compiled in parallel by the host's native back end, linked with the
   runtime library by recompiler/object/link.c */
#ifndef SNX_NX_MODULE_A64_H
#define SNX_NX_MODULE_A64_H

#include "nx/loader/module.h"

typedef struct {
    const char* output; /* <Game>_code.so or .dll, replaced only by a complete module */
    /* The target triple of a module made for another machine than this one, which the JIT loader loads instead of
       dlopen (iOS: arm64-apple-ios16.0-elf, the object format the loader reads): then output is a pack of the
       module's objects without the runtime library (recompiler/object/objects_pack.h), which the loading app has for its
       own machine. NULL: a module for this host */
    const char* triple;
    char opt;           /* -O letter */
    unsigned jobs;      /* the threads compiling the units */
    /* jobs is the most: the units start on 1 thread and more open while the memory holds them, as measured from the
       units themselves (the setting aot_threads left at 0, Auto; common.h's SnxAotGovernor) */
    int jobs_open;
    int quiet;
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
} SnxModuleBuild;

/* SNX_ABI as this build has it: the layout its modules are written with, which the runtime of the same build checks */
unsigned snx_module_abi(void);
/* A 64-bit program's module (module_a64.c): 0, or nonzero with the error said */
int snx_build_module_a64(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxModuleBuild* build);
/* A 32-bit program's module (module_arm.c): its AArch32 blocks and an SnxArmModule. 0, or nonzero with the error said */
int snx_build_module_arm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxModuleBuild* build);

#endif
