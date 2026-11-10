/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of every game module the backends don't generate per game: the helpers (cpu_helpers.c) and the C library
   functions LLVM lowers memory intrinsics to. The build embeds its LLVM bitcode. ir.c removes the bitcode's CPU
   attributes and compiles it for the module's target CPU, then link_elf.c, link_coff.c or link_macho.c links that
   object into the module. The modules import nothing, so they need no dynamic symbol resolution.
   It has no writable globals (a miss goes to RtAPI.miss), so the runtime JIT loads a copy of its own into the code
   arena (jit_service.c) without anything being kept twice */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/cpu_helpers.c"

/* No interpreter here: what a module lacks is compiled by the runtime JIT, and every decoded op has an IR emitter */

#include "common/cpu/recompiler/llvm/libc.inc"

/* GCC calls libm's round and roundf for ties-away rounding (FCVTA*, FRINTA) unless told -fno-trapping-math, and a
   module links no libm. On the bits, with integers: Clang may not turn them back into a libm call */
double round(double x);
float roundf(float x);
double round(double x) {
    union {
        double d;
        u64 u;
    } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x; /* whole already, or infinite or NaN */
    if (e < 0) {           /* under 1: 0 below a half, else 1, with the sign */
        v.u = (v.u & ((u64)1 << 63)) | (e == -1 ? (u64)0x3FF << 52 : 0);
        return v.d;
    }
    v.u += (u64)1 << (51 - e);          /* a half at this exponent, carrying into it */
    v.u &= ~(((u64)1 << (52 - e)) - 1); /* the fraction dropped */
    return v.d;
}
float roundf(float x) {
    union {
        float f;
        u32 u;
    } v;
    v.f = x;
    int e = (int)((v.u >> 23) & 0xFF) - 127;
    if (e >= 23) return x;
    if (e < 0) {
        v.u = (v.u & ((u32)1 << 31)) | (e == -1 ? (u32)0x7F << 23 : 0);
        return v.f;
    }
    v.u += (u32)1 << (22 - e);
    v.u &= ~(((u32)1 << (23 - e)) - 1);
    return v.f;
}
#ifdef _WIN32
/* GNU ld enters a DLL at DllMainCRTStartup, which the C runtime the gcc backend leaves out (-nostdlib) defines: without
   it the DLL would be entered at the start of its code. link_coff.c gives modules no entry */
int DllMainCRTStartup(void* module, unsigned long reason, void* reserved);
int DllMainCRTStartup(void* module, unsigned long reason, void* reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return 1;
}
#endif
#if defined(_WIN32) && defined(__x86_64__)
/* The stack probe of generated functions (SNX_IR_STACK_PROBE, ir_codegen.c). Windows commits a thread's stack as it
   grows, through the guard page below the part in use: a frame of more than a page that nothing touches on the way
   down skips that page, and its first store lands in reserved memory. The system then cannot even write the
   exception's record (the stack pointer is in that memory) and ends the process with no handler run. A prologue whose
   frame is a page or more calls this with the frame's size in rax and subtracts rax from rsp after it, so every
   register but the flags comes back as it was. It reads one byte in each page from the caller's stack pointer down to
   the new one, nearest first, so each read meets the guard page or a page already committed. The compiler runtime's
   ___chkstk_ms does the same, and a module links no compiler runtime. Naked, with the unwind codes of its two pushes */
__attribute__((naked)) void snx_stack_probe(void);
__attribute__((naked)) void snx_stack_probe(void) {
    __asm__(".seh_proc snx_stack_probe\n\t"
            "pushq %rcx\n\t"
            ".seh_pushreg %rcx\n\t"
            "pushq %rax\n\t"
            ".seh_pushreg %rax\n\t"
            ".seh_endprologue\n\t"
            "leaq 24(%rsp), %rcx\n" /* the caller's stack pointer: above the return address and the two pushes */
            "1:\n\t"
            "cmpq $0x1000, %rax\n\t"
            "jb 2f\n\t"
            "subq $0x1000, %rcx\n\t"
            "testb %cl, (%rcx)\n\t"
            "subq $0x1000, %rax\n\t"
            "jmp 1b\n"
            "2:\n\t"
            "subq %rax, %rcx\n\t" /* the new stack pointer, less than a page below the last page read */
            "testb %cl, (%rcx)\n\t"
            "popq %rax\n\t"
            "popq %rcx\n\t"
            "retq\n\t"
            ".seh_endproc");
}
#endif
