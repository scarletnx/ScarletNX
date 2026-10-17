/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Decoders for the MME macro calls NVN command buffers are made of, checked against packets captured from Tears of the
   Kingdom (tests/nvn_control.c). Macro slots are assigned by the SDK's macro upload, so a runtime user has to identify
   the macro by its uploaded code first: these only define the argument layouts once it has. */
#ifndef SNX_NVN_CONTROL_H
#define SNX_NVN_CONTROL_H
#include "common/cpu/aarch64/cpu.h"

typedef struct {
    u64 index_address;
    u32 index_type, primitive, count, instances;
    u32 base_instance, base_vertex;
    u32 macro_args[6]; /* preserve SDK-specific arguments until their effects are proven */
} NvnControlMacro38Draw;

/* Decode the observed 12-word secondary-list indexed draw. This extracts draw
   arguments but does not establish the surrounding render state or execute it. */
int nvn_control_macro38_draw(const u32* words, u32 count, NvnControlMacro38Draw* out);

typedef struct {
    u32 selector, code_offset;
    u32 macro_args[5];
} NvnControlProgramBind;

/* Extract complete MME macro-12 program binds from a validated GPFIFO entry.
   The observed selector 1 writes VertexB's code offset and 5 writes fragment's.
   Returns zero for malformed packets or insufficient output capacity. */
int nvn_control_program_binds(const u32* words, u32 count, NvnControlProgramBind* out, u32 capacity, u32* bind_count);
#endif
