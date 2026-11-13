// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// nothing drawn: the pass that changes the stencil buffer alone, its color writes masked
#version 450
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_attribute;
void main() {
    o_color = vec4(0.0);
    o_attribute = vec4(0.0);
}
