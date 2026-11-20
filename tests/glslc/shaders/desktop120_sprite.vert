#version 120
// desktop GLSL 1.20 (a desktop GL context's): attribute and varying, the f suffix, a matrix uniform
attribute vec4 a_position;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform mat4 u_transform;
varying vec2 v_uv;
varying vec4 v_color;

void main() {
    v_uv = a_uv;
    v_color = a_color * 1.0f;
    gl_Position = u_transform * a_position;
}
