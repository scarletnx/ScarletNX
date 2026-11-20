/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's linked programs as the renderer's host programs (Renderer.host_program): where the console's link
   puts their resources (SnxGlLayout: each stage's set as the renderer's translations have it, the default block at
   constant buffer 0, the uniform blocks after it, the samplers in the texture slots), and each stage's description
   as a translation's ShaderInfo */
#ifndef SNX_GL_HOST_PROGRAM_H
#define SNX_GL_HOST_PROGRAM_H
#include "common/gpu/gl/gl_program.h"
#include "nx/gpu/draw_state.h"
#include "nx/gpu/shader/shader.h"

/* the constant buffer slot a draw's texture handles are in (DsBindings.tex_bank), past the program's blocks */
#define GL_HOST_TEX_BANK (DS_CBUFS - 1)

/* the console's layout, with each stage's set from shader_vk_set */
SnxGlLayout gl_host_program_layout(void);
/* stage (0 vertex, 1 fragment) of program p, linked in layout y, as the renderer's ShaderInfo: the attributes the
   vertex stage reads, the fragment stage's output map, the constant buffers (the default block's and the blocks')
   and texture slots (each sampler's handle at its slot's word of GL_HOST_TEX_BANK) it reads. 0, or -1 for a stage
   the program has no module of */
int gl_host_shader_info(const SnxGlProgram* p, const SnxGlLayout* y, int stage, ShaderInfo* si);
#endif
