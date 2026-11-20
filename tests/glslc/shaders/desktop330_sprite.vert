#version 330 core
// desktop GLSL 3.30 core: in and out, the f suffix, no GL_ES
#ifdef GL_ES
#error GL_ES is GLSL ES's
#endif
layout(location = 0) in vec4 position;
layout(location = 1) in vec2 uv;
uniform mat4 transform;
uniform vec4 tint[2];
out vec2 f_uv;
out vec4 f_tint;

void main() {
    f_uv = uv;
    f_tint = tint[0] * 0.5f + tint[1];
    gl_Position = transform * position;
}
