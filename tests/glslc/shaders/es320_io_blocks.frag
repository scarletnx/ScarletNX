#version 320 es
// GLSL ES 3.20: the in blocks es320_io_blocks.vert writes, read through instance names
precision highp float;
layout(location = 0) in Surface {
    vec3 normal;
    centroid vec2 uv;
    flat ivec2 ids;
    mat2 basis;
} surface;
in Light {
    layout(location = 6) vec3 direction;
    layout(location = 7) flat uint index;
    layout(location = 8) sample float weight;
} light;
layout(location = 0) out vec4 color;
void main() {
    float k = max(dot(normalize(surface.normal), light.direction), 0.0) * light.weight;
    color = vec4(surface.uv, float(surface.ids.x + int(light.index)), k) + vec4(surface.basis[1], 0.0, 0.0);
}
