/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_SAIL_ARM64_EMIT_H
#define SNX_TEST_SAIL_ARM64_EMIT_H

#include <stddef.h>
#include <stdint.h>

/* arm64.h's SGX_F_*: the host features the code may use */
enum { SNX_ARM64_LSE = 1, SNX_ARM64_CRC32 = 2, SNX_ARM64_CSSC = 4 };

/* word's arm64 code at pc for a guest of these A64 features, as u64 step(CPU*) returning the next pc: its size in
   code, 0 when the back end's roots take no clause of it */
size_t snx_arm64_step(uint32_t word, uint64_t pc, uint64_t features, unsigned host_features, unsigned char* code,
                      size_t cap);
/* words in a row as one function u64 block(CPU*), *count of them taken: its size, 0 as snx_arm64_step's */
size_t snx_arm64_block(const uint32_t* words, size_t* count, uint64_t pc, uint64_t features, unsigned host_features,
                       unsigned char* code, size_t cap);
/* the host registers the back end may give values (0: all sixteen; fewer makes it spill) */
void snx_arm64_set_pool(int registers);
/* a helper's address, where the code calls it (the test's traps) */
void snx_arm64_set_resolve(uint64_t (*resolve)(const char* sym));

#endif
