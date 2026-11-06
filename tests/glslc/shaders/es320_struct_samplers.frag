#version 320 es
// glslang -V: lays a struct's samplers out as a UniformConstant struct, which Vulkan has not; semantics/samplers.frag
// compares glslc's with glslangValidator -R's
// GLSL ES 3.20: a struct's sampler read through an array of structs indexed at run time, beside explicit bindings
precision highp float;
struct Material { sampler2D albedo; float roughness; samplerCube reflection; };
layout(binding = 0) uniform sampler2D first;
uniform Material materials[4];
layout(binding = 1) uniform M { int which; };
layout(location = 0) flat in int v_material;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    color = texture(materials[which].albedo, v_uv) * materials[v_material].roughness +
            texture(materials[which].reflection, vec3(v_uv, 1.0)) + texture(first, v_uv);
}
