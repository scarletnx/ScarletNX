/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The programs a front end gives the renderer as SPIR-V (Renderer.host_program: the GL layer's, compiled from GLSL),
   which a draw names in DsPrograms.code[] as DS_HOST_PROGRAM | id instead of a guest address. Caller holds vk_lock */
#ifndef SNX_VK_HOST_PROGRAM_H
#define SNX_VK_HOST_PROGRAM_H
#include "nx/gpu/vk/vk.h"
/* spirv (words of it), a vertex (ST_VS) or fragment (ST_FS) module, as a shader object with si's description: its id,
   the same for the same module, 0 when the device refuses the module or every id is taken (said in the log) */
u32 vk_host_program(int stage, const u32* spirv, size_t words, const ShaderInfo* si);
/* the shader object of a DsPrograms.code[] word that names a host program of stage, NULL for one it doesn't name */
VkSh* vk_host_shader(u64 code, int stage);
/* whether a draw's programs are host programs: its vertex program is one */
static inline int vk_host_programs(const DsPrograms* p) { return (p->code[1] & DS_HOST_PROGRAM) != 0; }
#endif
