/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's guest functions (x64_codegen.c): the recompiler's code without LLVM, from the A64 emitters
   sailgen generates with its x86-64 back end (tools/sailgen/x64.h, its instruction selection derived from the Intel
   SDM), written as an ELF object, or on Windows a COFF one of its own calling convention, which the linker and the JIT's
   loader take as they take LLVM's (link_elf.c, link_coff.c, code_loader.c). The default code generator on x86-64 hosts;
   a function it can't make (one with an instruction its roots don't take: floating point, SIMD, a system register) is
   left to LLVM */
#ifndef SNX_RECOMPILER_X64_CODEGEN_H
#define SNX_RECOMPILER_X64_CODEGEN_H

#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>

typedef SnxNativeOptions SnxX64Options;

/* 1 where this host runs the x86-64 back end's code and its objects load (x86-64) */
int snx_x64_available(void);
/* the host's processor features the back end's forms may need (sailgen's SGX_F_*: CPUID's BMI1, BMI2, LZCNT, the SSE
   levels, PCLMULQDQ, POPCNT, AES-NI, SHA-NI, GFNI, and AVX, AVX2, FMA and F16C where the system keeps the YMM
   state), and SGX_F_WIN64 where the code is Windows'; 0 where unavailable */
unsigned snx_x64_features(void);
/* 1 where the code is made for Windows: its calling convention, in COFF objects */
int snx_x64_win64(void);
/* the features (SGX_F_*) the back end goes without from here on though the host has them, for a test of a host without
   them (0 none); those every host it runs on has stay */
void snx_x64_without(unsigned features);
/* the code made from here on as a Windows host's, Microsoft's x64 calling convention in COFF objects (1), or as this
   host's own (0), for a test of it elsewhere; a Windows host makes its own alone */
void snx_x64_set_win64(int on);
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
