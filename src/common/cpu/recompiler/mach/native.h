/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native back ends' options for a unit of guest functions (mach/mach_codegen.inc, each machine's code generator:
   x64/x64_codegen.h, arm64/arm64_codegen.h) */
#ifndef SNX_RECOMPILER_NATIVE_H
#define SNX_RECOMPILER_NATIVE_H
#include "common/cpu/recompiler/codegen.h"
#include <stddef.h>
typedef struct {
    /* a JIT unit: every function a local symbol, and table_name an array of their addresses in codegen's order, its
       one global (jit_service.c's emit_unit); else the AOT module's F_<address>, global and hidden */
    int jit;
    const char* table_name;
    /* nonzero for a target called through snx_aot_<address> rather than looked up (SnxIr.direct): the AOT module's
       function a JIT unit calls, or another module's function an AOT module imports */
    SnxU64 (*direct)(void* user, SnxU64 target);
    void* direct_user;
    /* the function the BR at address reaches once the program's linker has bound it, 0 for none, with *guard set when
       its slot may hold another (SnxIr.bound): a BL or B to such a PLT stub calls that function in the stub's place,
       on the machine that compares the slot (MG_BOUND: x86-64) */
    SnxU64 (*bound)(void* user, SnxU64 address, int* guard);
    void* bound_user;
    /* a function left to LLVM, 0 for none: the one whose IR the setting ir_dump_pc prints */
    SnxU64 llvm_at;
    /* the AOT module's stale checks (ir_codegen.c's snx_ir_stale_checks): snx_stale[the function's index] read at the
       entry and after each call's return */
    int stale;
} SnxNativeOptions;
#endif
