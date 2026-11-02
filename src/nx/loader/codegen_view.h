/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_CODEGEN_VIEW_H
#define SNX_NX_CODEGEN_VIEW_H
#include "nx/loader/loader.h"
#include "common/cpu/recompiler/codegen.h"
/* The A64 features of the console's CPU, as SnxCodegen.features takes them: 0 for the Switch's (what every caller
   passed before there were two), the switch2 target of a64.profile for the Switch 2's */
static inline SnxU64 snx_hw_a64(SnxHw hw) { return hw == SNX_HW_SWITCH2 ? SNX_A64_TARGET_SWITCH2 : 0; }
/* the codegen of program's modules, for the console's CPU its NPDM names */
int snx_nx_codegen_analyze(SnxCodegen* codegen, const SnxProgram* program, const SnxU64* hints, size_t hint_count,
                           int quiet);
#endif
