#version 320 es
// GLSL ES 3.20: out blocks, named and anonymous, with a location on the block or on each member, flat integer
// members, centroid and sample ones
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out Surface {
    vec3 normal;
    centroid vec2 uv;
    flat ivec2 ids;
    mat2 basis;
} surface;
out Light {
    layout(location = 6) vec3 direction;
    layout(location = 7) flat uint index;
    layout(location = 8) sample float weight;
};
void main() {
    surface.normal = normal;
    surface.uv = position.xy;
    surface.ids = ivec2(position.xy);
    surface.basis = mat2(1.0);
    direction = -normal;
    index = uint(position.z);
    weight = 0.5;
    gl_Position = vec4(position, 1.0);
}
