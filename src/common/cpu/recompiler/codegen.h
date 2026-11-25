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

/* what a function's flow gives of the jump table of the BR at address: its base (0 where the code before its load does,
   ~0 where two ways to the BR take two); bound, the entries a compare of the index bounds it to, or else max, those the
   index's making lets it reach, each 0 for none; and the unwind range its cases are in, count instructions from first,
   count 0 for none */
typedef struct {
    SnxU64 address, base, first;
    size_t count;
    unsigned bound, max;
} SnxFlowTable;

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
       branches between functions go through CPU.disp too, so handlers installed there (gpu/nvn/nvn_loader.c) see
       them */
    int check_fetch;
    int hook_direct_calls;
    /* the words read from a module whose copy can change while it is read (SnxCodeImage.generation), each taken from
       the module once, so discovery, the jump tables, the emitter and the JIT cache's hash agree on one version of
       every word: the one the code is built from. One block: read_capacity addresses, as many words, as many marks */
    unsigned char* read_block;
    size_t read_count, read_capacity;
    /* the jump tables whose base or end came from their function's flow (explore's, the leaders'), not from the code
       before the load, by BR address, so every later snx_codegen_jump_table (the leaders, the back ends) finds the same
       cases; and every table's base the leaders know of, ascending: where the table before it ends */
    SnxFlowTable* flow_tables;
    size_t flow_table_count, flow_table_capacity;
    SnxU64* table_bases;
    size_t table_base_count;
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
/* the instruction word at address: 1, or 0 outside the text. From a module whose copy can change, the word this codegen
   read there first (read_block) */
int snx_codegen_word(SnxCodegen* codegen, SnxU64 address, SnxU32* word);
/* 1 when every word in read_block is what its module holds now */
int snx_codegen_words_current(const SnxCodegen* codegen);
/* conservative discovery targets for a BR's jump table, up to capacity; emitted BR instructions still use their
   register and dispatch at run time (the native back ends' through a table of the cases, the others by the dispatch
   block). A switch of more cases is followed to SNX_JUMP_TABLE_MAX of them, a table a compare bounds to as many entries
   as its immediate says (cases repeat: a switch of a byte or an opcode has more entries than cases) */
#define SNX_JUMP_TABLE_MAX     2048
#define SNX_JUMP_TABLE_ENTRIES 4096
size_t snx_codegen_jump_table(SnxCodegen* codegen, SnxU64 address, SnxU64* targets, size_t capacity);
SnxU64 snx_function_address(const SnxFunction* function, size_t index);
int snx_function_contains(const SnxFunction* function, SnxU64 address);
const SnxFunction* snx_codegen_function(const SnxCodegen* codegen, SnxU64 address);
/* whether a module routes the function at address (SnxCodeImage.routed): a call to it goes through its dispatch slot
   even when it is compiled (CALLDYN), so a hook in the slot (nx/nn/hook.h) sees every call that isn't inlined */
int snx_codegen_routed(const SnxCodegen* codegen, SnxU64 address);
/* the function that starts at address, or else one that has a leader there: what GuestFn(c, address) enters. NULL when
   none has */
const SnxFunction* snx_codegen_entry(const SnxCodegen* codegen, SnxU64 address);

#endif
