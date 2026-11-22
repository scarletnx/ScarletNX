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
    /* the AOT module's function for a target a JIT unit calls directly (snx_aot_<address>), 0 for none */
    SnxU64 (*direct)(void* user, SnxU64 target);
    void* direct_user;
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
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each (an AOT module's
   unit: the others are LLVM's); for a JIT unit all of them or none (0 and no object then). The object's size, or 0
   with none made, -1 with the reason in error */
int snx_x64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options, unsigned char* made,
                   unsigned char** object, size_t* size, char* error, size_t error_size);

#endif
