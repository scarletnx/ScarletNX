/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_SAIL_X64_EMIT_H
#define SNX_TEST_SAIL_X64_EMIT_H

#include <stddef.h>
#include <stdint.h>

/* the host's features the x86-64 back end may use (x64.h's SGX_F_*) */
/* x64.h's SGX_F_*: the host features the code may use */
enum {
    SNX_X64_BMI1 = 1, SNX_X64_BMI2 = 2, SNX_X64_LZCNT = 4, SNX_X64_SSE3 = 8, SNX_X64_SSSE3 = 16, SNX_X64_SSE41 = 32,
    SNX_X64_SSE42 = 64, SNX_X64_PCLMUL = 128, SNX_X64_F16C = 256, SNX_X64_POPCNT = 512, SNX_X64_AVX = 1024,
    SNX_X64_FMA = 2048
};
/* what the back end needs of every host (snx_x64_available): the 'plain' host of the tests */
#define SNX_X64_BASELINE \
    (SNX_X64_SSE3 | SNX_X64_SSSE3 | SNX_X64_SSE41 | SNX_X64_SSE42 | SNX_X64_PCLMUL | SNX_X64_POPCNT)

/* word's x86-64 code at pc for a guest of these A64 features, as u64 step(CPU*) returning the next pc: its size in
   code, 0 when the x86-64 back end's roots take no clause of it */
size_t snx_x64_step(uint32_t word, uint64_t pc, uint64_t features, unsigned host_features, unsigned char* code,
                    size_t cap);
/* words in a row as one function u64 block(CPU*), *count of them taken (a branch or UNDEF ends it): its size, 0 as
   snx_x64_step's */
size_t snx_x64_block(const uint32_t* words, size_t* count, uint64_t pc, uint64_t features, unsigned host_features,
                     unsigned char* code, size_t cap);
/* the host registers the back end may give values (0: all nine; fewer makes it spill, for the test of spilling), and
   the vector ones (0: all sixteen) */
void snx_x64_set_pool(int registers);
void snx_x64_set_xpool(int registers);
/* the pools those set last, 0 for every register */
int snx_x64_pool(void);
int snx_x64_xpool(void);
/* the SSA instructions the last step appended, dropped ones counted */
unsigned snx_x64_instructions(void);
/* the calls of the floating-point helpers the code made so far */
unsigned long snx_x64_fp_calls(void);

#endif
