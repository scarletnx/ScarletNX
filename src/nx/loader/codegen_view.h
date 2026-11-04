/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_CODEGEN_VIEW_H
#define SNX_NX_CODEGEN_VIEW_H
#include "nx/loader/loader.h"
#include "common/cpu/recompiler/codegen.h"
int snx_nx_codegen_analyze(SnxCodegen* codegen, const SnxProgram* program, const SnxU64* hints, size_t hint_count,
                           int quiet);
#endif
