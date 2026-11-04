#version 300 es
// GLSL ES 3.00: two outputs, texture functions of every shape, switch, integer and bit operations
precision highp float;
precision highp int;
precision mediump sampler3D;
precision mediump sampler2DShadow;
precision mediump sampler2DArray;
precision mediump isampler2D;
in vec3 v_normal;
in vec3 v_tangent;
centroid in vec2 v_uv;
flat in uint v_joint;
flat in ivec2 v_ids;
uniform sampler2D albedo;
uniform sampler3D volume;
uniform samplerCube sky;
uniform sampler2DShadow shadow_map;
uniform sampler2DArray layers;
uniform isampler2D ids;
uniform vec4 shadow_coord;
layout(location = 0) out vec4 color;
layout(location = 1) out uvec2 extra;

vec4 sample_all(vec2 uv) {
    vec4 c = texture(albedo, uv);
    c += textureLod(albedo, uv, 2.0);
    c += textureOffset(albedo, uv, ivec2(1, -1));
    c += textureGrad(albedo, uv, dFdx(uv), dFdy(uv));
    c += textureProj(albedo, vec3(uv, 2.0));
    c += textureProj(albedo, vec4(uv, 0.0, 2.0));
    c += texelFetch(albedo, ivec2(uv * 64.0), 0);
    c += texelFetchOffset(albedo, ivec2(uv * 64.0), 1, ivec2(-1, 0));
    c += texture(volume, vec3(uv, 0.5));
    c += texture(sky, normalize(v_normal));
    c += texture(layers, vec3(uv, float(v_ids.x)));
    c.r += texture(shadow_map, shadow_coord.xyz);
    c.g += textureProj(shadow_map, shadow_coord);
    c.b += textureLod(shadow_map, shadow_coord.xyz, 0.0);
    ivec2 size = textureSize(albedo, 0) + textureSize(layers, 0).xy;
    c.a += float(size.x + size.y) + float(texture(ids, uv).x);
    return c;
}

void main() {
    vec4 c = sample_all(v_uv);
    uint bits = v_joint;
    int mode = int(bits & 3u);
    switch (mode) {
        case 0:
            c *= 0.5;
            break;
        case 1:
        case 2:
            c.rgb = c.bgr;
            // falls through
        default:
            c = clamp(c, 0.0, 1.0);
    }
    uvec2 e = uvec2(bits >> 4u, ~bits) + uvec2(floatBitsToUint(c.x), packHalf2x16(c.yz));
    extra = e % uvec2(1000u);
    color = vec4(normalize(v_normal + v_tangent) * 0.5 + 0.5, 1.0) * c;
}
