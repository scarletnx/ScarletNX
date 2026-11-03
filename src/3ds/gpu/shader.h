/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200 shader unit's interpreter: the program and swizzle patterns a command list uploaded (pica.h's
   SnxCtrShaderSetup) run over one unit's registers. Ported from Azahar video_core/shader/shader_interpreter.cpp
   and video_core/pica/shader_unit.cpp (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project / Azahar
   Emulator Project) at azahar-emu/azahar a4b335e, with the instruction encoding of nihstro's shader_bytecode.h
   (BSD-3-Clause, Copyright 2014 Tony Wasserka). Values are host floats, multiplication PICA's (pica.h). */
#ifndef SNX_CTR_SHADER_H
#define SNX_CTR_SHADER_H
#include "3ds/gpu/pica.h"
#include <stdbool.h>
#include <stdint.h>

/* the geometry emitter (EMIT, SETEMIT); NULL for vertex shaders, where both are ignored */
typedef struct {
    void* context;
    void (*emit)(void* context, unsigned vertex_id, bool prim_emit, bool winding, float (*output)[4]);
    unsigned vertex_id;
    bool prim_emit, winding;
} SnxCtrEmitter;

/* runs the program from entry until END (or the last word of program memory) */
void snx_ctr_shader_run(const SnxCtrShaderSetup* setup, SnxCtrShaderUnit* unit, unsigned entry, SnxCtrEmitter* emitter);
/* ShaderUnit::LoadInput: attributes 0..max_input_attribute_index of buffer into the input registers the block's
   attribute map names (block: SNX_PICA_VS or SNX_PICA_GS) */
void snx_ctr_shader_load(const uint32_t* regs, unsigned block, SnxCtrShaderUnit* unit, const float (*buffer)[4]);
/* ShaderUnit::WriteOutput: the output registers the block's output mask enables, in order, into buffer */
void snx_ctr_shader_store(const uint32_t* regs, unsigned block, const SnxCtrShaderUnit* unit, float (*buffer)[4]);
#endif
