/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AOT module's variant table: what a profile run compiled, linked into the
   module and described for the runtime. The runtime reads it through
   snx_variants(); it never compiles or links code. */
#ifndef SNX_NX_MODULE_VARIANTS_H
#define SNX_NX_MODULE_VARIANTS_H

#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/cpu/recompiler/llvm/ir.h"

/* Adds the variant table and snx_variants() to the descriptor IR. *count is
   the number of functions (0 when the cache has none). 0, or -1 with the
   reason in error. */
int snx_module_variants_emit(SnxIr* ir, const SnxJitCache* cache, size_t* count, char* error, size_t error_size);

#endif
