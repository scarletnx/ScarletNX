#version 330 core
// desktop GLSL 3.30 core: float highp without a precision statement, texture(), an out variable at location 0
uniform sampler2D tex;
in vec2 f_uv;
in vec4 f_tint;
layout(location = 0) out vec4 color;

void main() {
    color = texture(tex, f_uv) * f_tint;
}
