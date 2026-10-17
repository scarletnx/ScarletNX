/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp module.py, Copyright 2026 MZ Labs, Apache-2.0.
   Retain LICENSE.apache and NOTICE; the fork's changes are GPL-3.0-or-later. */
#ifndef SNX_TOOL_MODULE_H
#define SNX_TOOL_MODULE_H

#include "nx/loader/loader.h"
#include "common/cpu/recompiler/codegen.h"

typedef struct {
    const char* title;
    size_t chunk_instrs;
    int interp_only, quiet, hook_direct_calls;
    const SnxU64* hints;
    size_t hint_count;
    unsigned abi;
} SnxCodegenOptions;

/* Instructions per code unit (a C file, an LLVM module): whole functions are batched until a unit reaches it. Fewer
   on Android, whose allocator gives each allocation size a region per process: a CPU's worth of large units fills it */
#ifdef __ANDROID__
#define SNX_CHUNK_INSTRUCTIONS 20000
#else
#define SNX_CHUNK_INSTRUCTIONS 90000
#endif

/* the dispatch entries: every leader of every function by address, the first function's when two share one */
typedef struct {
    SnxU64 address;
    size_t function;
} SnxModuleEntry;
SnxModuleEntry* snx_module_entries(const SnxCodegen* codegen, size_t* count);
/* "0x8012340-0x8019ABC, 512 functions, 90120 instructions": the guest code of functions [first, last) */
char* snx_chunk_label(const SnxCodegen* codegen, size_t first, size_t last);
/* one line per finished unit: "  compiled block 3/40: code_002, <label> (4.2 s)" */
void snx_report_unit(size_t done, size_t count, const char* name, const char* label, int cached, double seconds);

#endif
