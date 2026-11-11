/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the game module's backends (module_llvm.c, module_arm.c) share: their options, how code is
   cut into units, the dispatch entry table and the progress lines */
#ifndef SNX_TOOL_MODULE_H
#define SNX_TOOL_MODULE_H

#include "nx/loader/loader.h"
#include "common/cpu/recompiler/codegen.h"

typedef struct {
    const char* title;
    size_t chunk_instrs; /* instructions per unit, 0 for the setting unit_instructions' */
    int interp_only, quiet, hook_direct_calls;
    const SnxU64* hints; /* guest addresses a run found code at (the game's .hints file) */
    size_t hint_count;
    const char* jit_cache; /* the profile run's .jitcache, linked into the module; NULL for none */
    unsigned abi;
    /* the NROs the program maps while it runs, each placed (snx_nro_slots_place), behind its modules up to nro_end:
       the module's snx_nros table (module_abi.h). None: no table */
    const SnxNroSlot* nros;
    size_t nro_count;
    SnxU64 nro_end;
    /* the module's bound branches, in the order of their addresses: where the register holds the function, the code
       calls it directly, and goes through CPU.disp where it holds anything else. None: every BR through CPU.disp */
    const SnxBoundBranch* bound;
    size_t bound_count;
} SnxCodegenOptions;

/* An address the runtime can enter: a leader of a function, and that function's index */
typedef struct {
    SnxU64 address;
    size_t function;
} SnxModuleEntry;
/* Every leader of every function, sorted by address; a leader two functions share is the lower-indexed one's. The
   array is the caller's to free, NULL for none */
SnxModuleEntry* snx_module_entries(const SnxCodegen* codegen, size_t* count);
/* A unit's description for progress lines, for functions [first, last), allocated, e.g.
   "0x8012340-0x8019ABC, 512 functions, 90120 instructions" */
char* snx_chunk_label(const SnxCodegen* codegen, size_t first, size_t last);
/* Prints that unit `done` of `count` finished: "  compiled block 3/40: code_002, <label> (4.2 s)", or "(cached)" */
void snx_report_unit(size_t done, size_t count, const char* name, const char* label, int cached, double seconds);
/* snx_report_unit's line, into out: the unit's place, name and label, then cached, or its time and the memory the
   process holds when used tells it (0: the system does not) */
void snx_unit_line(char* out, size_t size, size_t done, size_t count, const char* name, const char* label, int cached,
                   double seconds, SnxU64 used);

#endif
