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
#include <string.h>

typedef SnxNativeOptions SnxX64Options;

/* The MXCSR the FP hot paths assume (tools/sailgen/x64_a64.h, mach_arm.inc's VFP): round to nearest, no FTZ or DAZ,
   every exception masked. The thread's may be another, as what the runtime called before left it, so the entry of an
   A64 function or an A32 block with floating point of the host's (snx_x64_host_fp_helper) writes it where it drifted,
   as the LLVM functions do (cpu.h's snx_fp_env). The frame's first slot holds it: nothing is in it before a region
   spills. stmxcsr [rsp]; and dword [rsp], -64 (the
   sticky flags out); cmp dword [rsp], 0x1F80; je past the write; mov dword [rsp], 0x1F80; ldmxcsr [rsp] */
#define SNX_X64_MXCSR_ENTRY "0F AE 1C 24 83 24 24 C0 81 3C 24 80 1F 00 00 74 0B C7 04 24 80 1F 00 00 0F AE 14 24"
/* 1 for a helper whose arithmetic is the host's, so the MXCSR the code that calls it runs under: the A64 FP ones
   (x64_helpers.c's snx_x64_fp_* and snx_x64_fp8_*, which try the host's arithmetic first, and are the cold path of
   every SSE fast path), the estimates, the halves, BFDOT, and the A32 VFP's snx_arm_fp_x64. Code that calls none has
   no floating point of the host's, and its entry no check (as ir.c's uses_host_fp keeps snx_fp_env out) */
static inline int snx_x64_host_fp_helper(const char* sym) {
    static const char* const exact[] = {"snx_x64_estimate", "snx_x64_half", "snx_x64_bfdot", "snx_arm_fp_x64"};
    if (!sym) return 0;
    if (!strncmp(sym, "snx_x64_fp", 10)) return 1;
    for (size_t k = 0; k < sizeof exact / sizeof *exact; ++k)
        if (!strcmp(sym, exact[k])) return 1;
    return 0;
}

/* 1 where this host runs the x86-64 back end's code and its objects load (x86-64, with what its code needs) */
int snx_x64_available(void);
/* 1 on an x86-64 host without what the back end's code needs (SSE2: a test's alone), with the line of it in why
   (snx_native_floor); 0 and "" elsewhere */
int snx_x64_floor(char* why, size_t size);
/* 1 on an x86-64 host without features the back end does without, through its helpers (SSSE3 to SSE4.2, PCLMULQDQ), with
   line of it in why (snx_native_fallback); 0 and "" elsewhere */
int snx_x64_fallback(char* why, size_t size);
/* the host's processor features the back end's forms may need (sailgen's SGX_F_*: CPUID's BMI1, BMI2, LZCNT, the SSE
   levels, PCLMULQDQ, POPCNT, AES-NI, SHA-NI, GFNI, and AVX, AVX2, FMA and F16C where the system keeps the YMM
   state), and SGX_F_WIN64 where the code is Windows'; 0 where unavailable */
unsigned snx_x64_features(void);
/* the features (SGX_F_*) a processor's CPUID gives, leaf 1's, leaf 7's (subleaf 0) and leaf 0x80000001's registers
   (EAX, EBX, ECX, EDX) and leaf 7 subleaf 1's EAX, the VEX forms' where XCR0 keeps the YMM state: this host's
   (snx_x64_features), and a test's of another processor (0 where the back end is not built) */
unsigned snx_x64_features_of(const unsigned leaf1[4], const unsigned leaf7[4], unsigned leaf7_1a, const unsigned ext1[4],
                             unsigned xcr0);
/* 1 where the code is made for Windows: its calling convention, in COFF objects */
int snx_x64_win64(void);
/* the features (SGX_F_*) the back end goes without from here on though the host has them, for a test of a host without
   them (0 none): one of those its code needs of every host leaves the host without a back end (snx_x64_floor) */
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
