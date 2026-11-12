/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A masked clear's color (render.c clear_masked): the register bits from the push constants, read in the target's
   number type, float, or with CLEAR_UINT or CLEAR_SINT defined. vk_module_spv_location moves its one output's
   Location to the slot being cleared */
#version 450
#if defined(CLEAR_UINT)
#define CLEAR_TYPE uvec4
#elif defined(CLEAR_SINT)
#define CLEAR_TYPE ivec4
#else
#define CLEAR_TYPE vec4
#endif
layout(push_constant) uniform PC { layout(offset = 16) CLEAR_TYPE c; };
layout(location = 0) out CLEAR_TYPE o;

void main() { o = c; }
