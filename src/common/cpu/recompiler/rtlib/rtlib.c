/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of every game module the back ends don't generate per game: the helpers (cpu_helpers.c) and the C library
   functions Clang lowers memory intrinsics to. The build compiles it once into an object for the host (rtlib_object.h),
   which link_elf.c, link_coff.c or link_macho.c links into each module. The modules import nothing, so they need no
   dynamic symbol resolution. It has no writable globals (a miss goes to RtAPI.miss), so the runtime JIT loads a copy of
   its own into the code arena (jit_service.c) without anything being kept twice */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/cpu_helpers.c"
/* what the x86-64 back end's code calls (its exclusives, atomics, CRC32): every module links it, whichever made it */
#include "common/cpu/recompiler/x64/x64_helpers.c"

/* No interpreter here: what a module lacks is compiled by the runtime JIT, and a decoded op the back end has no code
   for is undefined where it runs */

#include "common/cpu/recompiler/rtlib/libc.inc"

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
/* The stack probe of generated functions. Windows commits a thread's stack as it
   grows, through the guard page below the part in use: a frame of more than a page that nothing touches on the way
   down skips that page, and its first store lands in reserved memory. The system then cannot even write the
   exception's record (the stack pointer is in that memory) and ends the process with no handler run. A prologue whose
   frame is a page or more calls this with the frame's size in rax and subtracts rax from rsp after it, so every
   register but the flags comes back as it was. It reads one byte in each page from the caller's stack pointer down to
   the new one, nearest first, so each read meets the guard page or a page already committed. The compiler runtime's
   ___chkstk_ms does the same, and a module links no compiler runtime. Naked, with the unwind codes of its two pushes */
[[gnu::naked]] void snx_stack_probe(void);
[[gnu::naked]] void snx_stack_probe(void) {
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
#if defined(_WIN32) && defined(__aarch64__)
/* The same probe on Windows on Arm, where Clang's AArch64 code calls __chkstk and no other name: a prologue whose
   frame is a page or more calls it with the frame's size in 16-byte units in x15, and subtracts x15 << 4 from sp after
   it, so the probe may change only x16, x17 (the procedure call standard's scratch registers) and the flags. It reads
   one word in each page from the caller's stack pointer down past the new one, nearest first, as the compiler
   runtime's __chkstk does, which a module does not link. A leaf that keeps sp and the link register needs no unwind
   codes */
[[gnu::naked]] void __chkstk(void);
[[gnu::naked]] void __chkstk(void) {
    __asm__("lsl x16, x15, #4\n\t" /* the frame's size in bytes */
            "mov x17, sp\n"
            "1:\n\t"
            "sub x17, x17, #0x1000\n\t"
            "subs x16, x16, #0x1000\n\t"
            "ldr xzr, [x17]\n\t"
            "b.gt 1b\n\t"
            "ret");
}
#endif
