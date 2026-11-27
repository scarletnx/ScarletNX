/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The out-of-line helpers of the generated-code ABI (cpu.h), compiled into every game module's runtime library
   (rtlib/rtlib.c) and into the tests, after cpu.h. The generated code calls snx_fp_*, snx_vec_*, the crypto rounds,
   snx_cas128 and snx_mrs/snx_msr by name. They are plain portable C: the library is built for no particular host CPU.
   */
#include "cpu.h"

/* ---- the host FP environment the fast paths need (cpu.h) ----
   The generated code uses the host's arithmetic behind guards that prove it gives the helper's result and
   flags, which they do only in round to nearest, with no flush to zero and no denormals-are-zero, and with every
   exception masked (a trap would leave the generated function where it cannot continue). A new host thread starts
   in that state, but generated code shares its thread with whatever the runtime calls into, so every generated
   function calls this on entry, and this writes the control word only when it has drifted. It does so on x86-64,
   AArch64, PowerPC64, RISC-V and LoongArch (fp_env.h); MIPS64 and s390x keep the helpers, so they have nothing to
   normalize. */
#include "helpers/fp_env.h"
void snx_fp_env(void) { snx_host_fp_env(); }

#include "helpers/fp.c"
#include "helpers/fp_host.c"
#include "helpers/integer.c"
#include "helpers/crypto.c"
#include "helpers/vector.c"
