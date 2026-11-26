/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The descriptor a module of the recompiler's library exports (library.c's load_descriptor reads it back): LLVM's
   writes it field by field at these offsets from a function (fallback_llvm.c), a build without LLVM holds it as data
   (library.c's native_descriptor). abi comes first: a module of another contract is recognized before anything else */
#ifndef SNX_RECOMPILER_LIBRARY_DESCRIPTOR_H
#define SNX_RECOMPILER_LIBRARY_DESCRIPTOR_H
#include "common/cpu/aarch64/cpu.h"
#include <stdint.h>

typedef struct {
    uint32_t abi;
    uint32_t reserved;
    uint64_t count;            /* entries: every leader of every function, by address */
    const uint64_t* addresses; /* ascending */
    const GuestFn* functions;  /* the function each leader belongs to (its F_, which dispatches on the entry) */
    GuestFn interp;            /* NULL: there is no interpreter, the module's JIT compiles what it lacks */
    uint8_t* stale;            /* one byte per discovered function; set when its source code changes */
} SnxLibraryDescriptor;
/* the function LLVM's module exports, which returns it */
#define SNX_LIBRARY_DESCRIPTOR_FN   "snx_lib_module"
/* the data a module without LLVM exports, which is it */
#define SNX_LIBRARY_DESCRIPTOR_DATA "snx_lib_module_data"
typedef const SnxLibraryDescriptor* (*SnxLibraryDescriptorFn)(void);

#endif
