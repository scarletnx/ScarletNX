/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A rectangle's one colour, opaque (rects.vert) */
#version 450

layout(location = 0) flat in vec4 c;
layout(location = 0) out vec4 o;

void main() { o = c; }
