/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TOOL_CODEGEN_H
#define SNX_TOOL_CODEGEN_H

#include "common/cpu/recompiler/image.h"
#include "common/cpu/aarch64/a64_decoder.h"

typedef struct {
    SnxU64 start, end;
    SnxU64* addresses;
    size_t count;
    unsigned char* leaders;
    size_t leader_count;
} SnxFunction;

typedef struct {
    /* the guest's A64 features (SNX_A64_FEAT_* or a SNX_A64_TARGET_* of a64_decoder.h), what decodes: 0 is the
       Switch's. The analyses' features argument: emitting decodes with them too */
    SnxU64 features;
    SnxCodeImage* modules;
    size_t module_count;
    /* SVC #imm below SNX_SVC_COUNT calls RtAPI.svcs[imm], the SVC's own entry, instead of RtAPI.svc (the NX modules) */
    int direct_svc;
    SnxFunction* functions;
    size_t function_count, function_capacity, instruction_count;
    SnxCodeImage* last_module;
    /* emit options, set after snx_codegen_analyze (which clears the struct). check_fetch: each instruction compares its
       word with memory and interprets it when the code changed. hook_direct_calls (--hook-direct-calls): calls and
       branches between functions go through CPU.disp too, so wrappers installed there (gpu/nvn_trace.c) see them */
    int check_fetch;
    int hook_direct_calls;
} SnxCodegen;

int snx_codegen_analyze(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* hints,
                        size_t hint_count, int quiet, SnxU64 features);
/* The runtime JIT's discovery: the functions at roots[0..root_count), a batch of them, and those they call, found as
   snx_codegen_analyze finds functions. Every root is searched, and its callees until the functions hold budget
   instructions. known(user, address) says code exists already at an address, or will without this batch (the AOT
   module, an earlier compile, a pc another batch compiles): a function stops there and isn't searched from there,
   unless it is a root. region_limit bounds one explored branch graph (0: the original limit), so an external branch
   or fallthrough resumes through dispatch. With overflow_limit, a graph larger than region_limit is explored again
   with that smaller limit instead, keeping short functions together. A root inside another root's region is a leader
   of it. 0 when at least one root has code (snx_codegen_entry), -1 when none has (unaligned, outside the modules).
   The codegen is snx_codegen_free'd either way */
typedef int (*SnxCodegenKnown)(void* user, SnxU64 address);
int snx_codegen_discover(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* roots,
                         size_t root_count, SnxCodegenKnown known, void* user, size_t budget, SnxU64 features,
                         size_t region_limit, size_t overflow_limit);
int snx_codegen_analyze_from(SnxCodegen* codegen, const SnxCodeImage* modules, size_t module_count, const SnxU64* hints,
                             size_t hint_count, size_t limit, int (*skip)(void* context, SnxU64 address),
                             void* context, SnxU64 features);
void snx_codegen_free(SnxCodegen* codegen);
SnxCodeImage* snx_codegen_module(SnxCodegen* codegen, SnxU64 address);
int snx_codegen_word(SnxCodegen* codegen, SnxU64 address, SnxU32* word);
SnxU64 snx_function_address(const SnxFunction* function, size_t index);
int snx_function_contains(const SnxFunction* function, SnxU64 address);
const SnxFunction* snx_codegen_function(const SnxCodegen* codegen, SnxU64 address);
/* whether a module routes the function at address (SnxCodeImage.routed): a call to it goes through its dispatch slot
   even when it is compiled (CALLDYN), so a hook in the slot (nx/hle/hook/hook.h) sees every call that isn't inlined */
int snx_codegen_routed(const SnxCodegen* codegen, SnxU64 address);
/* the function that starts at address, or else one that has a leader there: what GuestFn(c, address) enters. NULL when
   none has */
const SnxFunction* snx_codegen_entry(const SnxCodegen* codegen, SnxU64 address);

#endif
