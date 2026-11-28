/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_CODEGEN_VIEW_H
#define SNX_NX_CODEGEN_VIEW_H
#include "nx/loader/loader.h"
#include "common/cpu/recompiler/codegen.h"
int snx_nx_codegen_analyze(SnxCodegen* codegen, const SnxProgram* program, const SnxU64* hints, size_t hint_count,
                           int quiet, SnxU64 features);
/* how much of the code module holds: the functions that start in its mapping, and their instructions */
void snx_nx_module_share(const SnxCodegen* codegen, const SnxModuleImage* module, size_t* functions,
                         SnxU64* instructions);
/* the functions whose words are another function's, word for word (each group's first not counted), and their
   instructions: same[0] and same[1]. And of those, alike[0] and alike[1], the ones that are the same code wherever
   they sit: every branch of theirs lands inside them, and no instruction takes an address from where it is (ADR, ADRP,
   a literal's load) */
void snx_nx_repeated_functions(const SnxCodegen* codegen, SnxU64 same[2], SnxU64 alike[2]);
#endif
