/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The templates' interpreter (snx_a64_interp's C, which NX code modules no longer carry), as this test's miss
   handler: it runs the loaded NRO's code from guest memory */
#include "common/cpu/aarch64/cpu.h"
void snx_interp(CPU* c, u64 entry);
/* Unsupported encodings still decode operands before taking the interpreter's undefined-instruction path. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "ldr_interp.inc"
#pragma GCC diagnostic pop
