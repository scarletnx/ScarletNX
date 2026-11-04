/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The out-of-line helpers of the generated-code ABI (cpu.h), compiled into every game module's runtime library
   (llvm/rtlib.c) and into the tests, after cpu.h. LLVM IR calls snx_fp_*, snx_vec_*, the crypto rounds, snx_cas128
   and snx_mrs/snx_msr by name. They are plain portable C: the library is built for no particular host CPU. */
#include "cpu.h"

/* ---- the host FP environment the fast paths need (cpu.h) ----
   ir_fpu.c and ir_vec_fp.c use the host's arithmetic behind guards that prove it gives the helper's result and
   flags, which they do only in round to nearest, with no flush to zero and no denormals-are-zero, and with every
   exception masked (a trap would leave the generated function where it cannot continue). A new host thread starts
   in that state, but generated code shares its thread with whatever the runtime calls into, so every generated
   function calls this on entry, and this writes the control word only when it has drifted. The fast paths exist
   on x86-64, AArch64, PowerPC64, RISC-V and LoongArch (ir_fp.h's gates); MIPS64 and s390x keep the helpers, so they
   have nothing to normalize. */
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif
void snx_fp_env(void) {
#if defined(_M_X64) || defined(__x86_64__)
    unsigned csr = _mm_getcsr();
    if ((csr & 0xFFC0u) != 0x1F80u) _mm_setcsr(0x1F80u); /* FZ, DAZ, RMode and the masks: the default word */
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    u64 fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    if (fpcr & 0x07C8FF00ull) /* FZ, DN, AHP, RMode, FZ16 and the trap enables */
        __asm__ volatile("msr fpcr, %0" : : "r"(0ull));
#elif defined(__powerpc64__) && (defined(__GNUC__) || defined(__clang__))
    /* FPSCR's low byte: the enables (VE, OE, UE, ZE, XE), NI and RN. The fast paths' vectors are VSX's, which
       VSCR's NJ does not flush (ir_fp.h's host_fp_vec asks for VSX) */
    union {
        double d;
        u64 u;
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

#include "helpers/fp.c"
#include "helpers/fp_host.c"
#include "helpers/integer.c"
#include "helpers/crypto.c"
#include "helpers/vector.c"
