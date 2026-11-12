/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A masked clear's full-screen triangle (render.c clear_masked, clear_stencil_masked), on the layer the push constants
   name. Compiled for Vulkan 1.2, SPIR-V 1.5, whose ShaderLayer writes gl_Layer from a vertex shader */
#version 450
#extension GL_ARB_shader_viewport_layer_array : require
layout(push_constant) uniform PC { int layer; };

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    gl_Layer = layer;
}
