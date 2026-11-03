#version 310 es
// GLSL ES 3.10: an out block through GL_EXT_shader_io_blocks
#extension GL_EXT_shader_io_blocks : require
layout(location = 0) in vec4 position;
out Varyings {
    layout(location = 0) vec4 color;
    layout(location = 1) flat int id;
};
void main() {
    color = position;
    id = int(position.w);
    gl_Position = position;
}
