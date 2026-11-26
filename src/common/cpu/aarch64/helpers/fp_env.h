/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host FP environment the generated code's FP fast paths assume, which both runtime libraries' entries put back
   (cpu.h's snx_fp_env for A64, simd.h's snx_arm_fp_env for A32): round to nearest, no flush to zero and no
   denormals-are-zero, every exception masked. Inlined, so the A64 helper stays one body LLVM can take into a function
   (ir.c) */
#ifndef SNX_CPU_HELPERS_FP_ENV_H
#define SNX_CPU_HELPERS_FP_ENV_H

#include <stdint.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif
[[gnu::always_inline]] static inline void snx_host_fp_env(void) {
#if defined(_M_X64) || defined(__x86_64__)
    unsigned csr = _mm_getcsr();
    if ((csr & 0xFFC0u) != 0x1F80u) _mm_setcsr(0x1F80u); /* FZ, DAZ, RMode and the masks: the default word */
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    if (fpcr & 0x07C8FF00ull) /* FZ, DN, AHP, RMode, FZ16 and the trap enables */
        __asm__ volatile("msr fpcr, %0" : : "r"(0ull));
#elif defined(__powerpc64__) && (defined(__GNUC__) || defined(__clang__))
    /* FPSCR's low byte: the enables (VE, OE, UE, ZE, XE), NI and RN. The fast paths' vectors are VSX's, which
       VSCR's NJ does not flush (ir_fp.h's host_fp_vec asks for VSX) */
    union {
        double d;
        uint64_t u;
    } fpscr;
    __asm__ volatile("mffs %0" : "=f"(fpscr.d));
    if (fpscr.u & 0xFFull) {
        fpscr.u &= ~0xFFull;
        __asm__ volatile("mtfsf 0xff, %0" : : "f"(fpscr.d));
    }
#elif defined(__riscv) && defined(__riscv_flen)
    /* frm, the dynamic rounding mode every FP instruction LLVM emits uses; RISC-V has no traps and no flush */
    unsigned long frm;
    __asm__ volatile("frrm %0" : "=r"(frm));
    if (frm) __asm__ volatile("fsrm zero");
#elif defined(__loongarch64)
    /* FCSR0's enables (bits 0-4) and RM (bits 8-9) */
    unsigned fcsr;
    __asm__ volatile("movfcsr2gr %0, $fcsr0" : "=r"(fcsr));
    if (fcsr & 0x31Fu) __asm__ volatile("movgr2fcsr $fcsr0, %0" : : "r"(fcsr & ~0x31Fu));
#endif
}

#endif
