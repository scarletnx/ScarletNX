#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 color;
void main() {
    color = v_color;
}
