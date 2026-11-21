/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_SAIL_X64_EMIT_H
#define SNX_TEST_SAIL_X64_EMIT_H

#include <stddef.h>
#include <stdint.h>

/* the host's features the x86-64 back end may use (x64.h's SGX_F_*) */
enum { SNX_X64_BMI1 = 1, SNX_X64_BMI2 = 2, SNX_X64_LZCNT = 4 };

/* word's x86-64 code at pc for a guest of these A64 features, as u64 step(CPU*) returning the next pc: its size in
   code, 0 when the x86-64 back end's roots take no clause of it */
size_t snx_x64_step(uint32_t word, uint64_t pc, uint64_t features, unsigned host_features, unsigned char* code,
                    size_t cap);
/* the SSA instructions the last step appended, dropped ones counted */
unsigned snx_x64_instructions(void);

#endif
