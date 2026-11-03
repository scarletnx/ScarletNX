#version 320 es
// GLSL ES 3.20: the array of in blocks es320_io_block_arrays.vert writes, indexed by a constant
precision highp float;
layout(location = 0) in Ray {
    vec3 origin;
    vec2 cone;
} rays[3];
layout(location = 6) in Hit {
    flat int id;
    float t;
} hit;
layout(location = 0) out vec4 color;
void main() {
    color = vec4(rays[2].origin + vec3(rays[0].cone, rays[1].cone.x), hit.t) * float(hit.id);
}
