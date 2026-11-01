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
   only on x86-64 and AArch64, so every other host has nothing to normalize. */
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
#endif
}

#include "helpers/fp.c"
#include "helpers/fp_host.c"
#include "helpers/integer.c"
#include "helpers/crypto.c"
#include "helpers/vector.c"
