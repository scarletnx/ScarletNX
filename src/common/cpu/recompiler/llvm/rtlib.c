/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of every game module the backends don't generate per game: the helpers (cpu_helpers.c) and the C library
   functions LLVM lowers memory intrinsics to. It is compiled once, when the recompiler is built, with the project's Clang and the flags
   toolchain.c gives generated C (-march=native: build == host == target), and linked into each
   module by link_elf.c / link_coff.c. The modules import nothing, so they need no dynamic symbol resolution.
   It has no writable globals (a miss goes to RtAPI.miss), so the runtime JIT loads a copy of its own into the code
   arena (jit_service.c) without anything being kept twice */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/cpu_helpers.c"

/* No interpreter here any more: what a module lacks is compiled by the runtime JIT, and every pattern has an IR
   emitter. The tests that compare against the templates' C generate it themselves (snx_a64_interp) */

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
    int e;
    v.d = x;
    e = (int)((v.u >> 52) & 0x7FF) - 1023;
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
    int e;
    v.f = x;
    e = (int)((v.u >> 23) & 0xFF) - 127;
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
