/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TOOL_A64_H
#define SNX_TOOL_A64_H

#include "common/utilities/common.h"
#include <stddef.h>
#include <stdio.h>

enum SnxFlow {
    SNX_FLOW_UNDEF,
    SNX_FLOW_PLAIN,
    SNX_FLOW_B,
    SNX_FLOW_BL,
    SNX_FLOW_BC,
    SNX_FLOW_BR,
    SNX_FLOW_BLR,
    SNX_FLOW_RET,
    SNX_FLOW_SVC,
    SNX_FLOW_BRK
};

typedef struct {
    enum SnxFlow kind;
    const char *decl, *first, *second;
} SnxA64Case;

typedef struct {
    unsigned lo, width;
} SnxA64Field;

typedef struct {
    const char* name;
    SnxU32 mask, value;
    unsigned validation, field_count;
    SnxA64Field fields[4];
    const char* index_expr;
    const SnxA64Case* cases;
    size_t case_count;
} SnxA64Pattern;

const SnxA64Case* snx_a64_decode(SnxU32 word);
/* The pattern a decoded case belongs to and its index in the pattern's case table, for backends that emit each
   specialization themselves instead of printing the case's C (NULL for the shared undefined case) */
const SnxA64Pattern* snx_a64_case_pattern(const SnxA64Case* instruction, size_t* index);
const SnxA64Pattern* snx_a64_patterns(size_t* count);
SnxU64 snx_a64_target(SnxU32 word, SnxU64 pc);
void snx_a64_literal(FILE* out, SnxU64 value);
void snx_a64_hex(FILE* out, SnxU64 value);
void snx_a64_scope(FILE* out, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction);
void snx_a64_body(FILE* out, const char* body, const char* undefined);
void snx_a64_interp(FILE* out, int step_mode);
void snx_a64_interp_checked(FILE* out);
void snx_a64_static_step(FILE* out, SnxU32 word, SnxU64 pc, const char* name);
int snx_a64_fast_simd(FILE* out, SnxU32 word);

#endif
