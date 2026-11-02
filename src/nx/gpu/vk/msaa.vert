#version 450
/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The multisample moves' triangle (vk/msaa.c): vertices 0, 1 and 2 at (-1, -1), (3, -1) and (-1, 3), over the whole
   render area. CMake compiles it into SPIR-V at build time (cmake/glsl.cmake). */
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
