#version 310 es
// GLSL ES 3.10: explicit sets and bindings, gather, multisample fetch, gl_HelperInvocation, early fragment tests
precision highp float;
layout(early_fragment_tests) in;
layout(set = 1, binding = 0) uniform highp sampler2D tex;
layout(binding = 1) uniform highp sampler2DMS ms;
layout(binding = 2) uniform highp sampler2DArrayShadow shadows;
layout(binding = 3) uniform highp usampler2D ids;
layout(binding = 4, std140) uniform Scene { vec4 tint; mat2 rot; } scene;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec4 g = textureGather(tex, uv) + textureGather(tex, uv, 2) + textureGatherOffset(tex, uv, ivec2(1, 0), 1);
    vec4 m = texelFetch(ms, ivec2(uv * 8.0), 3);
    float s = texture(shadows, vec4(uv, 1.0, 0.5)) + textureGrad(shadows, vec4(uv, 0.0, 0.5), vec2(0.0), vec2(0.0));
    vec4 gs = textureGather(shadows, vec3(uv, 0.0), 0.25);
    uvec4 id = textureGather(ids, uv, 3);
    vec2 r = scene.rot * uv;
    color = (g + m + gs) * scene.tint * s + vec4(r, float(id.x), gl_HelperInvocation ? 1.0 : 0.0);
}
