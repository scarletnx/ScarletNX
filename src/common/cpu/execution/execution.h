/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_EXECUTION_H
#define SNX_EXECUTION_H
#include "common/cpu/aarch64/cpu.h"
typedef struct {
    u64 address;
    const u8* data;
    size_t size;
} SnxExecRange;
typedef void (*SnxFetchFn)(CPU* cpu, u64 pc, u32* word);
typedef struct {
    void* module;
    GuestFn entry;
    GuestFn* dispatch;
    u64 code_lo, code_hi;
    char* temporary;
    SnxFetchFn fetch;
    void (*set_fetch)(SnxFetchFn fetch);
} SnxExecution;
/* Translate executable ranges with the existing ARM64-to-C compiler and load the
   resulting host module. cc=NULL uses the configured build compiler. */
int snx_execution_compile(SnxExecution* execution, const SnxExecRange* ranges, size_t count, u64 entry, const char* cc,
                          char* error, size_t error_size);
void snx_execution_run(SnxExecution* execution, CPU* cpu, u64 entry);
void snx_execution_free(SnxExecution* execution);
#endif
