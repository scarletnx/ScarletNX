/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TOOL_CODEGEN_H
#define SNX_TOOL_CODEGEN_H

#include "common/cpu/recompiler/image.h"
#include "common/cpu/aarch64/a64.h"

typedef struct {
    SnxU64 start, end;
    SnxU64* addresses;
    size_t count;
    unsigned char* leaders;
    size_t leader_count;
} SnxFunction;

typedef struct {
    SnxCodeImage* modules;
    size_t module_count;
    int check_fetch;
    SnxFunction* functions;
    size_t function_count, function_capacity, instruction_count;
    SnxCodeImage* last_module;
} SnxCodegen;

int snx_codegen_analyze(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* hints,
                        size_t hint_count, int quiet);
int snx_codegen_analyze_from(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* hints,
                             size_t hint_count, size_t limit, int (*skip)(void* context, SnxU64 address),
                             void* context);
void snx_codegen_free(SnxCodegen* codegen);
SnxCodeImage* snx_codegen_module(SnxCodegen* codegen, SnxU64 address);
int snx_codegen_word(SnxCodegen* codegen, SnxU64 address, SnxU32* word);
SnxU64 snx_function_address(const SnxFunction* function, size_t index);
int snx_function_contains(const SnxFunction* function, SnxU64 address);
const SnxFunction* snx_codegen_function(const SnxCodegen* codegen, SnxU64 address);
void snx_codegen_emit(FILE* out, SnxCodegen* codegen, const SnxFunction* function);

#endif
