#version 310 es
// compare: samples
// Samplers inside structs, through uniforms, arrays of them, function parameters and a comma: spirv-opt inlines both
// glslc's module and glslangValidator -R's, and tests/glslc compares the sampler each texture call reads, with its
// coordinate, and the uniform members read (tests/glslc/glslc_test.py semantics)
precision mediump float;
precision mediump sampler2DArray;
struct Material { sampler2D albedo; float scale; sampler2D normal; vec2 offset; };
struct Pair { sampler2D a; samplerCube b; };
struct Layers { vec4 tint; sampler2DArray layers[3]; };
uniform Material mat;
uniform Material mats[3];
uniform Pair pair;
uniform Pair pairs[2];
uniform Layers stack;
uniform vec4 after;
layout(location = 0) out vec4 color;

vec4 shade(Material m, vec2 uv) { return texture(m.albedo, uv + m.offset) * m.scale + texture(m.normal, uv); }
vec4 both(Pair p, vec3 d) { return texture(p.a, d.xy) + texture(p.b, d); }
vec4 every(Material ms[3]) {
    return texture(ms[0].normal, vec2(0.11)) + texture(ms[2].albedo, vec2(0.12)) * ms[1].scale +
           shade(ms[1], vec2(0.13));
}
vec4 forward(Pair p) { return both(p, vec3(0.14)); }
vec4 one(sampler2D s, vec2 uv) { return texture(s, uv); }

void main() {
    color = texture(mat.albedo, vec2(0.21)) * mat.scale + texture(mat.normal, mat.offset);
    color += shade(mats[2], vec2(0.22)) + every(mats);
    color += both(pair, vec3(0.23)) + forward(pairs[1]) + texture(pairs[0].b, vec3(0.24));
    color += texture(stack.layers[2], vec3(0.25)) * stack.tint + one(mats[1].normal, vec2(0.26)) * after;
    color += texture(stack.layers[0], vec3(mats[0].offset, 0.27));
    color.xy += (color.x, mat).offset;
}
