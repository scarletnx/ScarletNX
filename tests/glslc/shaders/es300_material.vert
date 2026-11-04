#version 300 es
// GLSL ES 3.00: locations, a uniform block with matrices and an array, flat integer outputs, matrix constructors
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 tangent;
layout(location = 3) in uvec4 joints;
layout(std140) uniform Frame {
    mat4 view_projection;
    mat4 model;
    vec4 clip_plane;
    float time;
    int frame;
};
uniform Material {
    mat3x4 transform;
    vec4 colors[3];
    bool lit;
} material;
out vec3 v_normal;
out vec3 v_tangent;
centroid out vec2 v_uv;
flat out uint v_joint;
flat out ivec2 v_ids;
invariant gl_Position;
void main() {
    mat3 m3 = mat3(model);
    mat4 m4 = mat4(m3);
    vec4 world = model * vec4(position, 1.0);
    v_normal = normalize(m3 * normal);
    v_tangent = (m4 * vec4(tangent.xyz, 0.0)).xyz * tangent.w;
    vec3 t = vec4(position, 1.0) * material.transform;
    v_uv = material.lit ? t.xy : t.yx + material.colors[2].xy;
    v_joint = joints.x ^ (joints.y << 2u) | uint(frame & 3);
    v_ids = ivec2(gl_VertexID, gl_InstanceID) + ivec2(frame) % 7;
    gl_Position = view_projection * world + vec4(sin(time) * 0.0);
}
