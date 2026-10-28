/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The out-of-line helpers of the generated-code ABI (cpu.h), compiled into every game module's runtime library
   (llvm/rtlib.c) and into the tests, after cpu.h. LLVM IR calls snx_fp_*, snx_vec_*, the crypto rounds, snx_cas128
   and snx_mrs/snx_msr by name. They are plain portable C: the library is built for no particular host CPU. */
#include "cpu.h"

#include "helpers/fp.c"
#include "helpers/fp_host.c"
#include "helpers/integer.c"
#include "helpers/crypto.c"
#include "helpers/vector.c"
