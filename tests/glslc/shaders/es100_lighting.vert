// GLSL ES 1.00: attributes, varyings, loose uniforms, a struct, a loop with a constant bound
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec2 a_uv;
uniform mat4 u_model;
uniform mat4 u_view_projection;
uniform mat3 u_normal_matrix;
struct PointLight {
    vec3 position;
    vec3 color;
    float radius;
};
uniform PointLight u_lights[4];
uniform int u_light_count;
varying vec3 v_color;
varying vec2 v_uv;

vec3 shade(PointLight light, vec3 p, vec3 n) {
    vec3 d = light.position - p;
    float k = max(dot(normalize(d), n), 0.0);
    float fall = clamp(1.0 - length(d) / light.radius, 0.0, 1.0);
    return light.color * k * fall * fall;
}

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    vec3 n = normalize(u_normal_matrix * a_normal);
    vec3 c = vec3(0.05);
    for (int i = 0; i < 4; i++) {
        if (i >= u_light_count) break;
        c += shade(u_lights[i], world.xyz, n);
    }
    v_color = c;
    v_uv = a_uv;
    gl_Position = u_view_projection * world;
    gl_PointSize = 1.0;
}
