/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's guest functions (x64_codegen.c): the recompiler's code without LLVM, from the A64 emitters
   sailgen generates with its x86-64 back end (tools/sailgen/x64.h, its instruction selection derived from the Intel
   SDM), written as an ELF object the linker and the JIT's loader take as they take LLVM's (link_elf.c,
   code_loader.c). The default code generator on x86-64 hosts that load ELF objects; a function it can't make (one
   with an instruction its roots don't take: floating point, SIMD, a system register) is left to LLVM */
#ifndef SNX_RECOMPILER_X64_CODEGEN_H
#define SNX_RECOMPILER_X64_CODEGEN_H

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
       its slot may hold another (SnxIr.bound): a BL or B to such a PLT stub calls that function in the stub's place */
    SnxU64 (*bound)(void* user, SnxU64 address, int* guard);
    void* bound_user;
    /* a function left to LLVM, 0 for none: the one whose IR the setting ir_dump_pc prints */
    SnxU64 llvm_at;
    /* the AOT module's stale checks (ir_codegen.c's snx_ir_stale_checks): snx_stale[the function's index] read at the
       entry and after each call's return */
    int stale;
} SnxX64Options;

/* 1 where this host runs the x86-64 back end's code and its objects load (x86-64, ELF: not Windows, whose objects are
   COFF) */
int snx_x64_available(void);
/* the host's processor features the back end's forms may need (sailgen's SGX_F_*: CPUID's BMI1, BMI2, LZCNT, the SSE
   levels, PCLMULQDQ, POPCNT, and AVX, FMA and F16C where the system keeps the YMM state); 0 where unavailable */
unsigned snx_x64_features(void);
/* the features (SGX_F_*) the back end goes without from here on though the host has them, for a test of a host without
   them (0 none); those every host it runs on has stay */
void snx_x64_without(unsigned features);
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each (an AOT module's
   unit: the others are LLVM's); for a JIT unit all of them or none (0 and no object then). The object's size, or 0
   with none made, -1 with the reason in error */
int snx_x64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options, unsigned char* made,
                   unsigned char** object, size_t* size, char* error, size_t error_size);

/* Where a unit's object comes from, put through put: what snx_x64_object makes of the functions [first, last) of
   codegen under options depends on these alone. The host's and the guest's features, the options and codegen fields
   that change the code (a JIT unit's table name too), each function's index (its stale check reads snx_stale there),
   addresses, words and leaders, then for each address its code calls or branches to and each fall-through out of it
   what the call there asks of codegen and options (a function, routed, imported, a bound PLT stub with its words and
   index), and each BR's binding. The key a unit is held in memory under (x64_cache.h): a new question the back end
   asks of codegen or the options goes here too, or a unit made under another answer is reused */
void snx_x64_unit_facts(const SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options,
                        void (*put)(void* user, const void* data, size_t size), void* user);

#endif
