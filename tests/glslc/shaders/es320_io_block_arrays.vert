#version 320 es
// GLSL ES 3.20: an array of out blocks, each element after the one before, and an out block glslc places itself
layout(location = 0) in vec4 position;
layout(location = 0) out Ray {
    vec3 origin;
    vec2 cone;
} rays[3];
layout(location = 6) out Hit {
    flat int id;
    float t;
} hit;
void main() {
    for (int i = 0; i < 3; i++) {
        rays[i].origin = position.xyz * float(i);
        rays[i].cone = position.zw;
    }
    hit.id = int(position.x);
    hit.t = position.w;
    gl_Position = position;
}
